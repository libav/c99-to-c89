/*
 * Boring copyright header.
 */

#include <assert.h>
#include <stdio.h>
#include <clang-c/Index.h>
#include <string.h>
#include <stdlib.h>

/*
 * The basic idea of the token parser is to "stack" ordered tokens
 * (i.e. ordering is done by libclang) in such a way that we can
 * re-arrange them on the fly before printing it back out to an
 * output file.
 *
 * Example:
 *
 *   x = (AVRational) { y, z };
 * becomes
 *   { AVRational temp = { y, z }; x = temp; }
 *
 *   x = function((AVRational) { y, z });
 * becomes
 *   { AVRational temp = { y, z }; x = function(temp); }
 *
 *   return function((AVRational) { y, z });
 * becomes
 *   { AVRational temp = { y, z }; return function(temp); }
 *
 *   int var = ((int[2]) { 1, 2 })[1];
 * becomes
 *   int var; { int temp[2] = { 1, 2 }; var = temp[1]; { [..] } }
 *
 * Note in the above, that the [..] indeed means the whole rest
 * of the statements in the same context needs to be within the
 * brackets, otherwise the resulting code could contain mixed
 * variable declarations and statements, which c89 does not allow.
 *
 * Like for compound literals, c89 does not support designated
 * initializers, thus we attempt to replace them. The basic idea
 * is to parse the layout of structs and enums, and then to parse
 * expressions like:
 *   {
 *     [index1] = val1,
 *     [index2] = val2,
 *   }
 * or
 *   {
 *     .member1 = val1,
 *     .member2 = val2,
 *   }
 * and convert these to ordered struct/array initializers without
 * designation, i.e.:
 *   {
 *     val1,
 *     val2,
 *   }
 * Note that in cases where the indexes or members are not ordered,
 * i.e. their order in the struct (for members) is different from
 * the order of initialization in the expression, or their numeric
 * values are not linearly ascending in the same way as they are
 * presented in the expression, then we have to reorder the expressions
 * and, in some cases, insert gap fillers. For example,
 *   {
 *     [index3] = val3,
 *     [index1] = val1,
 *   }
 * becomes
 *   {
 *     val1, 0,
 *     val3,
 *   }
 * (assuming index1 is the first value and index3 is the third value
 * in an enum, and in between these two is a value index2 which is
 * not used in this designated initializer expression. If the values
 * themselves are structs, we use {} instead of 0 as a gap filler.
 */

typedef struct {
    char *type;
    char *name;
    unsigned n_ptrs; // 0 if not a pointer
    unsigned array_size; // 0 if no array
    CXCursor cursor;
} StructMember;

typedef struct {
    StructMember *entries;
    unsigned n_entries;
    unsigned n_allocated_entries;
    char *name;
    CXCursor cursor;
} StructDeclaration;
static StructDeclaration *structs = NULL;
static unsigned n_structs = 0;
static unsigned n_allocated_structs = 0;

typedef struct {
    char *name;
    int value;
    CXCursor cursor;
} EnumMember;

typedef struct {
    EnumMember *entries;
    unsigned n_entries;
    unsigned n_allocated_entries;
    char *name;
    CXCursor cursor;
} EnumDeclaration;
static EnumDeclaration *enums = NULL;
static unsigned n_enums = 0;
static unsigned n_allocated_enums = 0;

/* FIXME we're not taking pointers or array sizes into account here,
 * in large part because Libav doesn't use those in combination with
 * typedefs. */
typedef struct {
    char *proxy;
    char *name;
    StructDeclaration *struct_decl;
    EnumDeclaration *enum_decl;
    CXCursor cursor;
} TypedefDeclaration;
static TypedefDeclaration *typedefs = NULL;
static unsigned n_typedefs = 0;
static unsigned n_allocated_typedefs = 0;

enum StructArrayType {
    TYPE_IRRELEVANT = 0,
    TYPE_STRUCT     = 1,
    TYPE_ARRAY      = 2,
};
typedef struct {
    unsigned index;
    struct {
        unsigned start, end;
    } value_offset, expression_offset;
} StructArrayItem;

typedef struct {
    enum StructArrayType type;
    StructDeclaration *str_decl;
    StructArrayItem *entries;
    unsigned level;
    unsigned n_entries;
    unsigned n_allocated_entries;
    struct {
        unsigned start, end;
    } value_offset;
} StructArrayList;
static StructArrayList *struct_array_lists = NULL;
static unsigned n_struct_array_lists = 0;
static unsigned n_allocated_struct_array_lists = 0;

static CXTranslationUnit TU;

#define DEBUG 0
#define dprintf(...) \
    if (DEBUG) \
        printf(__VA_ARGS__)

static unsigned find_token_index(CXToken *tokens, unsigned n_tokens,
                                 const char *str)
{
    unsigned n;

    for (n = 0; n < n_tokens; n++) {
        CXString tstr = clang_getTokenSpelling(TU, tokens[n]);
        const char *cstr = clang_getCString(tstr);
        int res = strcmp(str, cstr);
        clang_disposeString(tstr);
        if (!res)
            return n;
    }

    fprintf(stderr, "Could not find token %s in set\n", str);
    exit(1);
}

static char *concat_name(CXToken *tokens, unsigned int from, unsigned to)
{
    unsigned int cnt = 0, n;
    char *str;

    for (n = from; n <= to; n++) {
        CXString tstr = clang_getTokenSpelling(TU, tokens[n]);
        const char *cstr = clang_getCString(tstr);
        cnt += strlen(cstr) + 1;
    }

    str = (char *) malloc(cnt);
    if (!str) {
        fprintf(stderr, "Out of memory\n");
        exit(1);
    }

    for (cnt = 0, n = from; n <= to; n++) {
        CXString tstr = clang_getTokenSpelling(TU, tokens[n]);
        const char *cstr = clang_getCString(tstr);
        int len = strlen(cstr);
        memcpy(&str[cnt], cstr, len);
        if (n == to) {
            str[cnt + len] = 0;
        } else {
            str[cnt + len] = ' ';
        }
        cnt += len + 1;
    }

    return str;
}

static enum CXChildVisitResult fill_struct_members(CXCursor cursor,
                                                   CXCursor parent,
                                                   CXClientData client_data)
{
    StructDeclaration *decl = (StructDeclaration *) client_data;

    // FIXME what happens when an anonymous struct is declared within
    // another?

    if (cursor.kind == CXCursor_FieldDecl) {
        CXString cstr = clang_getCursorSpelling(cursor);
        const char *str = clang_getCString(cstr);
        unsigned n = decl->n_entries, idx;
        CXToken *tokens = 0;
        unsigned int n_tokens = 0;
        CXSourceRange range = clang_getCursorExtent(cursor);

        clang_tokenize(TU, range, &tokens, &n_tokens);

        if (decl->n_entries == decl->n_allocated_entries) {
            unsigned num = decl->n_allocated_entries + 16;
            void *mem = realloc(decl->entries,
                                sizeof(*decl->entries) * num);
            if (!mem) {
                fprintf(stderr,
                        "Ran out of memory while declaring field %s in %s\n",
                        str, decl->name);
                exit(1);
            }
            decl->entries = (StructMember *) mem;
            decl->n_allocated_entries = num;
        }

        decl->entries[n].name = strdup(str);
        decl->entries[n].cursor = cursor;
        decl->n_entries++;

        idx = find_token_index(tokens, n_tokens, str);
        decl->entries[n].n_ptrs = 0;
        do {
            CXString tstr = clang_getTokenSpelling(TU, tokens[idx + 1]);
            const char *cstr = clang_getCString(tstr);
            int res = strcmp(cstr, "[");
            clang_disposeString(tstr);
            if (!res) {
                CXString tstr = clang_getTokenSpelling(TU, tokens[idx + 2]);
                const char *cstr = clang_getCString(tstr);
                decl->entries[n].array_size = atoi(cstr);
                clang_disposeString(tstr);
            } else {
                decl->entries[n].array_size = 0;
            }
        } while (0);

        for (;;) {
            unsigned im1 = idx - 1 - decl->entries[n].n_ptrs;
            CXString tstr = clang_getTokenSpelling(TU, tokens[im1]);
            const char *cstr = clang_getCString(tstr);
            int res = strcmp(cstr, "*");
            clang_disposeString(tstr);
            if (!res) {
                decl->entries[n].n_ptrs++;
            } else {
                break;
            }
        }

        do {
            unsigned im1 = idx - 1 - decl->entries[n].n_ptrs;
            CXString tstr = clang_getTokenSpelling(TU, tokens[im1]);
            const char *cstr = clang_getCString(tstr);
            if (!strcmp(cstr, ",")) {
                decl->entries[n].type = strdup(decl->entries[n - 1].type);
            } else {
                decl->entries[n].type = concat_name(tokens, 0, im1);
            }
            clang_disposeString(tstr);
        } while (0);

        // FIXME it's not hard to find the struct name (either because
        // tokens[idx-2-n_ptrs] == 'struct', or because tokens[idx-1-n_ptrs]
        // is a typedef for the struct name), and then we can use
        // find_struct_decl() to find the StructDeclaration belonging to
        // that type.

        clang_disposeString(cstr);
        clang_disposeTokens(TU, tokens, n_tokens);
    }

    return CXChildVisit_Continue;
}

static void register_struct(const char *str, CXCursor cursor,
                            TypedefDeclaration *decl_ptr)
{
    unsigned n;
    StructDeclaration *decl;

    for (n = 0; n < n_structs; n++) {
        if (!strcmp(structs[n].name, str) &&
            memcmp(&cursor, &structs[n].cursor, sizeof(cursor))) {
            /* already exists */
            if (decl_ptr)
                decl_ptr->struct_decl = &structs[n];
            return;
        }
    }

    if (n_structs == n_allocated_structs) {
        unsigned num = n_allocated_structs + 16;
        void *mem = realloc(structs, sizeof(*structs) * num);
        if (!mem) {
            fprintf(stderr, "Out of memory while registering struct %s\n", str);
            exit(1);
        }
        structs = (StructDeclaration *) mem;
        n_allocated_structs = num;
    }

    decl = &structs[n_structs++];
    decl->name = strdup(str);
    decl->cursor = cursor;
    decl->n_entries = 0;
    decl->n_allocated_entries = 0;
    decl->entries = NULL;

    clang_visitChildren(cursor, fill_struct_members, decl);

    if (decl_ptr)
        decl_ptr->struct_decl = decl;
}

static int arithmetic_expression(int val1, const char *expr, int val2)
{
    assert(expr[1] == 0);

    switch (expr[0]) {
    case '^': return val1 ^ val2;
    case '|': return val1 | val2;
    case '&': return val1 & val2;
    case '+': return val1 + val2;
    case '-': return val1 - val2;
    case '*': return val1 * val2;
    case '/': return val1 / val2;
    case '%': return val1 % val2;
    }

    fprintf(stderr, "Unknown arithmetic expression %s\n", expr);
    exit(1);
}

static int find_enum_value(const char *str)
{
    unsigned n, m;

    for (n = 0; n < n_enums; n++) {
        for (m = 0; m < enums[n].n_entries; m++) {
            if (!strcmp(enums[n].entries[m].name, str))
                return enums[n].entries[m].value;
        }
    }

    fprintf(stderr, "Unknown enum value %s\n", str);
    exit(1);
}

static enum CXChildVisitResult fill_enum_value(CXCursor cursor,
                                               CXCursor parent,
                                               CXClientData client_data)
{
    int *ptr = (int *) client_data;
    CXToken *tokens = 0;
    unsigned int n_tokens = 0;
    CXSourceRange range = clang_getCursorExtent(cursor);

    clang_tokenize(TU, range, &tokens, &n_tokens);

    switch (cursor.kind) {
    case CXCursor_BinaryOperator: {
        int cache[3] = { 0 };
        CXString tsp;

        assert(n_tokens == 4);
        tsp = clang_getTokenSpelling(TU, tokens[1]);
        clang_visitChildren(cursor, fill_enum_value, cache);
        assert(cache[0] == 2);
        ptr[1 + ptr[0]++] = arithmetic_expression(cache[1],
                                                  clang_getCString(tsp),
                                                  cache[2]);
        clang_disposeString(tsp);
        break;
    }
    case CXCursor_IntegerLiteral: {
        CXString tsp;

        assert(n_tokens == 2);
        tsp = clang_getTokenSpelling(TU, tokens[0]);
        ptr[1 + ptr[0]++] = atoi(clang_getCString(tsp));
        clang_disposeString(tsp);
        break;
    }
    case CXCursor_DeclRefExpr: {
        CXString tsp;

        assert(n_tokens == 2);
        tsp = clang_getTokenSpelling(TU, tokens[0]);
        ptr[1 + ptr[0]++] = find_enum_value(clang_getCString(tsp));
        clang_disposeString(tsp);
        break;
    }
    default:
        break;
    }

    clang_disposeTokens(TU, tokens, n_tokens);

    return CXChildVisit_Continue;
}

static enum CXChildVisitResult fill_enum_members(CXCursor cursor,
                                                 CXCursor parent,
                                                 CXClientData client_data)
{
    EnumDeclaration *decl = (EnumDeclaration *) client_data;

    if (cursor.kind == CXCursor_EnumConstantDecl) {
        CXString cstr = clang_getCursorSpelling(cursor);
        const char *str = clang_getCString(cstr);
        unsigned n = decl->n_entries;
        int cache[3] = { 0 };

        if (decl->n_entries == decl->n_allocated_entries) {
            unsigned num = decl->n_allocated_entries + 16;
            void *mem = realloc(decl->entries,
                                sizeof(*decl->entries) * num);
            if (!mem) {
                fprintf(stderr,
                        "Ran out of memory while declaring field %s in %s\n",
                        str, decl->name);
                exit(1);
            }
            decl->entries = (EnumMember *) mem;
            decl->n_allocated_entries = num;
        }

        decl->entries[n].name = strdup(str);
        decl->entries[n].cursor = cursor;
        clang_visitChildren(cursor, fill_enum_value, cache);
        assert(cache[0] <= 1);
        if (cache[0] == 1) {
            decl->entries[n].value = cache[1];
        } else if (n == 0) {
            decl->entries[n].value = 0;
        } else {
            decl->entries[n].value = decl->entries[n - 1].value + 1;
        }
        decl->n_entries++;

        clang_disposeString(cstr);
    }

    return CXChildVisit_Continue;
}

static void register_enum(const char *str, CXCursor cursor,
                          TypedefDeclaration *decl_ptr)
{
    unsigned n;
    EnumDeclaration *decl;

    for (n = 0; n < n_enums; n++) {
        if (!strcmp(enums[n].name, str) &&
            memcmp(&cursor, &enums[n].cursor, sizeof(cursor))) {
            /* already exists */
            if (decl_ptr)
                decl_ptr->enum_decl = &enums[n];
            return;
        }
    }

    if (n_enums == n_allocated_enums) {
        unsigned num = n_allocated_enums + 16;
        void *mem = realloc(enums, sizeof(*enums) * num);
        if (!mem) {
            fprintf(stderr, "Out of memory while registering enum %s\n", str);
            exit(1);
        }
        enums = (EnumDeclaration *) mem;
        n_allocated_enums = num;
    }

    decl = &enums[n_enums++];
    decl->name = strdup(str);
    decl->cursor = cursor;
    decl->n_entries = 0;
    decl->n_allocated_entries = 0;
    decl->entries = NULL;

    clang_visitChildren(cursor, fill_enum_members, decl);

    if (decl_ptr)
        decl_ptr->enum_decl = decl;
}

static void register_typedef(const char *name,
                             CXToken *tokens, unsigned n_tokens,
                             TypedefDeclaration *decl, CXCursor cursor)
{
    unsigned n;

    if (n_typedefs == n_allocated_typedefs) {
        unsigned num = n_allocated_typedefs + 16;
        void *mem = realloc(typedefs, sizeof(*typedefs) * num);
        if (!mem) {
            fprintf(stderr, "Ran out of memory while declaring typedef %s\n",
                    name);
            exit(1);
        }
        n_allocated_typedefs = num;
        typedefs = (TypedefDeclaration *) mem;
    }

    n = n_typedefs++;
    typedefs[n].name = strdup(name);
    if (decl->struct_decl) {
        typedefs[n].struct_decl = decl->struct_decl;
        typedefs[n].proxy = NULL;
        typedefs[n].enum_decl = NULL;
    } else if (decl->enum_decl) {
        typedefs[n].enum_decl = decl->enum_decl;
        typedefs[n].struct_decl = NULL;
        typedefs[n].proxy = NULL;
    } else {
        typedefs[n].enum_decl = NULL;
        typedefs[n].struct_decl = NULL;
        typedefs[n].proxy = concat_name(tokens, 1, n_tokens - 3);
    }
    memcpy(&typedefs[n].cursor, &cursor, sizeof(cursor));
}

static unsigned get_token_offset(CXToken token)
{
    CXSourceLocation l = clang_getTokenLocation(TU, token);
    CXFile file;
    unsigned line, col, off;

    clang_getSpellingLocation(l, &file, &line, &col, &off);

    return off;
}

static StructDeclaration *find_struct_decl_by_name(const char *name)
{
    unsigned n;

    for (n = 0; n < n_structs; n++) {
        if (!strcmp(name, structs[n].name))
            return &structs[n];
    }

    return NULL;
}

static TypedefDeclaration *find_typedef_decl_by_name(const char *name)
{
    unsigned n;

    for (n = 0; n < n_typedefs; n++) {
        if (!strcmp(name, typedefs[n].name))
            return &typedefs[n];
    }

    return NULL;
}

// FIXME this function has some duplicate functionality compared to
// fill_struct_members() further up.
static StructDeclaration *find_struct_decl(const char *var, CXToken *tokens,
                                           unsigned n_tokens)
{
    /*
     * In the list of tokens that make up a sequence like:
     * 'static const struct str_type name = { val }',
     * A) find the token that contains 'var', get the type (token before that)
     * B) check the tokens before that one to see if type is a struct
     * C) if not, check if the type is a typedef and go back to (B)
     * D) if type is a struct, return that type's StructDeclaration;
     *    if type is not a struct and not a typedef, return NULL.
     */
    unsigned n, var_tok_idx;

    for (n = 0; n < n_tokens; n++) {
        CXString spelling = clang_getTokenSpelling(TU, tokens[n]);
        int res = strcmp(clang_getCString(spelling), var);
        clang_disposeString(spelling);
        if (!res)
            break;
    }
    if (n == n_tokens)
        return NULL;

    // is it a struct?
    var_tok_idx = n;
    if (var_tok_idx > 1) {
        CXString spelling;
        int res;

        spelling = clang_getTokenSpelling(TU, tokens[var_tok_idx - 2]);
        res = strcmp(clang_getCString(spelling), "struct");
        clang_disposeString(spelling);

        if (!res) {
            StructDeclaration *str_decl;

            spelling = clang_getTokenSpelling(TU, tokens[var_tok_idx - 1]);
            str_decl = find_struct_decl_by_name(clang_getCString(spelling));
            clang_disposeString(spelling);

            return str_decl;
        }
    }

    // is it a typedef?
    if (var_tok_idx > 0) {
        CXString spelling;
        TypedefDeclaration *td_decl;

        spelling = clang_getTokenSpelling(TU, tokens[var_tok_idx - 1]);
        td_decl = find_typedef_decl_by_name(clang_getCString(spelling));
        clang_disposeString(spelling);

        // FIXME if struct_decl is not set but proxy is, then it may be
        // a struct typedef declared in advance, whereas the struct itself
        // was declared separately. In that case, we should find the struct
        // declaration delayed, e.g. here/now.
        if (td_decl && td_decl->struct_decl)
            return td_decl->struct_decl;
    }

    return NULL;
}

static StructDeclaration *find_encompassing_struct_decl(unsigned start,
                                                        unsigned end,
                                                        StructArrayList **ptr)
{
    /*
     * In previously registered arrays/structs, find one with a start-end
     * that fully contains the given start/end function arguments. If found,
     * return that array/struct's type. If not found, return NULL.
     */
    unsigned n;

    *ptr = NULL;
    for (n = 0; n < n_struct_array_lists; n++) {
        if (start >= struct_array_lists[n].value_offset.start &&
            end   <= struct_array_lists[n].value_offset.end) {
            if (struct_array_lists[n].type == TYPE_ARRAY) {
                *ptr = &struct_array_lists[n];
                return struct_array_lists[n].str_decl;
            } else if (struct_array_lists[n].type == TYPE_STRUCT) {
                // FIXME return type of that member
                return NULL;
            } else {
                return NULL;
            }
        }
    }

    return NULL;
}

static unsigned find_member_index_in_struct(StructDeclaration *str_decl,
                                            const char *member)
{
    unsigned n;

    for (n = 0; n < str_decl->n_entries; n++) {
        if (!strcmp(str_decl->entries[n].name, member))
            return n;
    }

    return -1;
}

static StructDeclaration *find_struct_decl_for_type_name(const char *name)
{
    if (!strncmp(name, "struct ", 7)) {
        return find_struct_decl_by_name(name + 7);
    } else {
        TypedefDeclaration *decl = find_typedef_decl_by_name(name);
        return decl ? decl->struct_decl : NULL;
    }
}

/*
 * Structure to keep track of compound literals that we eventually want
 * to replace with something else.
 */
enum CLType {
    TYPE_UNKNOWN = 0,
    TYPE_OMIT_CAST,     // AVRational x = (AVRational) { y, z }
                        // -> AVRational x = { y, z }
    TYPE_TEMP_ASSIGN,   // AVRational x; [..] x = (AVRational) { y, z }
                        // -> [..] { AVRational tmp = { y, z }; x = tmp; }
                        // can also be used for return
};

typedef struct {
    enum CLType type;
    struct {
        unsigned start, end; // to get the values
    } value_token, cast_token;
    union {
        struct {
            unsigned assign_start;
        } t_a; // for TYPE_TEMP_ASSIGN
    } data;
    StructDeclaration *str_decl; // struct type
} CompoundLiteralList;
static CompoundLiteralList *comp_literal_lists = NULL;
static unsigned n_comp_literal_lists = 0;
static unsigned n_allocated_comp_literal_lists = 0;

/*
 * Helper struct for traversing the tree. This allows us to keep state
 * beyond the current and parent node.
 */
typedef struct CursorRecursion CursorRecursion;
struct CursorRecursion {
    enum CXCursorKind kind;
    CursorRecursion *parent;
    CXToken *tokens;
    unsigned n_tokens;
    union {
        void *opaque;
        StructArrayList *l; // InitListExpr and UnexposedExpr
        // after an InitListExpr
        StructDeclaration *str_decl; // VarDecl
        TypedefDeclaration *td_decl; // TypedefDecl
        CompoundLiteralList *cl_list; // CompoundLiteralExpr
    } data;
};

static void analyze_compound_literal_lineage(CompoundLiteralList *l,
                                             CursorRecursion *rec)
{
    CursorRecursion *p = rec;

#define DEBUG 0
    dprintf("CL lineage: ");
    do {
        dprintf("%d, ", p->kind);
    } while ((p = p->parent));
    dprintf("\n");
#define DEBUG 0

    if (rec->kind == CXCursor_VarDecl) {
        l->type = TYPE_OMIT_CAST;
    } else if (rec->kind == CXCursor_BinaryOperator ||
               rec->kind == CXCursor_ReturnStmt) {
        l->type = TYPE_TEMP_ASSIGN;
        l->data.t_a.assign_start = get_token_offset(rec->tokens[0]);
    }
}

static enum CXChildVisitResult callback(CXCursor cursor, CXCursor parent,
                                        CXClientData client_data)
{
    enum CXChildVisitResult res = CXChildVisit_Recurse;
    CXString str;
    CXSourceRange range;
    CXToken *tokens = 0;
    unsigned n_tokens = 0;
    CXSourceLocation pos;
    CXFile file;
    unsigned line, col, off, i;
    CXString filename;
    CursorRecursion rec;

    range = clang_getCursorExtent(cursor);
    pos   = clang_getCursorLocation(cursor);
    str   = clang_getCursorSpelling(cursor);
    clang_tokenize(TU, range, &tokens, &n_tokens);
    clang_getSpellingLocation(pos, &file, &line, &col, &off);
    filename = clang_getFileName(file);

    rec.kind = cursor.kind;
    rec.data.opaque = NULL;
    rec.parent = (CursorRecursion *) client_data;
    rec.tokens = tokens;
    rec.n_tokens = n_tokens;

#define DEBUG 0
    dprintf("DERP: %d [%d] %s @ %d:%d in %s\n", cursor.kind, parent.kind,
            clang_getCString(str), line, col,
            clang_getCString(filename));
    for (i = 0; i < n_tokens; i++)
    {
        CXString spelling = clang_getTokenSpelling(TU, tokens[i]);
        CXSourceLocation l = clang_getTokenLocation(TU, tokens[i]);
        clang_getSpellingLocation(l, &file, &line, &col, &off);
        dprintf("token = '%s' @ %d:%d\n", clang_getCString(spelling), line, col);
        clang_disposeString(spelling);
    }
#define DEBUG 0

    switch (cursor.kind) {
    case CXCursor_TypedefDecl: {
        TypedefDeclaration decl;
        memset(&decl, 0, sizeof(decl));
        rec.data.td_decl = &decl;
        clang_visitChildren(cursor, callback, &rec);
        register_typedef(clang_getCString(str), tokens, n_tokens,
                         &decl, cursor);
        break;
    }
    case CXCursor_StructDecl:
        register_struct(clang_getCString(str), cursor,
                        parent.kind == CXCursor_TypedefDecl ?
                            rec.parent->data.td_decl : NULL);
        break;
    case CXCursor_EnumDecl:
        register_enum(clang_getCString(str), cursor,
                      parent.kind == CXCursor_TypedefDecl ?
                            rec.parent->data.td_decl : NULL);
        break;
    case CXCursor_VarDecl:
        // e.g. static const struct <type> name { val }
        //      ^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^
        rec.data.str_decl = find_struct_decl(clang_getCString(str),
                                             tokens, n_tokens);
        clang_visitChildren(cursor, callback, &rec);
        break;
    case CXCursor_CompoundLiteralExpr: {
        CompoundLiteralList *l;

        if (n_comp_literal_lists == n_allocated_comp_literal_lists) {
            unsigned num = n_allocated_comp_literal_lists + 16;
            void *mem = realloc(comp_literal_lists,
                                sizeof(*comp_literal_lists) * num);
            if (!mem) {
                fprintf(stderr, "Failed to allocate memory for complitlist\n");
                exit(1);
            }
            comp_literal_lists = (CompoundLiteralList *) mem;
            n_allocated_comp_literal_lists = num;
        }
        l = &comp_literal_lists[n_comp_literal_lists++];
        memset(l, 0, sizeof(*l));
        rec.data.cl_list = l;
        l->cast_token.start = get_token_offset(tokens[0]);
        clang_visitChildren(cursor, callback, &rec);
        analyze_compound_literal_lineage(l, rec.parent->parent);
        break;
    }
    case CXCursor_TypeRef:
        if (parent.kind == CXCursor_CompoundLiteralExpr) {
            CompoundLiteralList *l = rec.parent->data.cl_list;

            // (type) { val }
            //  ^^^^
            l->cast_token.end = get_token_offset(tokens[n_tokens - 1]);
            l->str_decl = find_struct_decl_for_type_name(clang_getCString(str));
        }
        clang_visitChildren(cursor, callback, &rec);
        break;
    case CXCursor_InitListExpr:
        if (parent.kind == CXCursor_CompoundLiteralExpr) {
            CompoundLiteralList *l = rec.parent->data.cl_list;

            // (type) { val }
            //        ^^^^^^^
            l->value_token.start = get_token_offset(tokens[0]);
            l->value_token.end   = get_token_offset(tokens[n_tokens - 2]);
            clang_visitChildren(cursor, callback, &rec);
        } else {
            // another { val } or { .member = val } or { [index] = val }
            StructArrayList *l;

            if (n_struct_array_lists == n_allocated_struct_array_lists) {
                unsigned num = n_allocated_struct_array_lists + 16;
                void *mem = realloc(struct_array_lists,
                                    sizeof(*struct_array_lists) * num);
                if (!mem) {
                    fprintf(stderr, "Failed to allocate memory for str/arr\n");
                    exit(1);
                }
                struct_array_lists = (StructArrayList *) mem;
                n_allocated_struct_array_lists = num;
            }
            l = &struct_array_lists[n_struct_array_lists++];
            l->type = TYPE_IRRELEVANT;
            l->n_entries = l->n_allocated_entries = 0;
            l->entries = NULL;
            l->value_offset.start = get_token_offset(tokens[0]);
            l->value_offset.end   = get_token_offset(tokens[n_tokens - 2]);
            if (parent.kind == CXCursor_VarDecl) {
                l->str_decl = rec.parent->data.str_decl;
                l->level = 0;
            } else {
                StructArrayList *parent;
                l->str_decl = find_encompassing_struct_decl(l->value_offset.start,
                                                            l->value_offset.end,
                                                            &parent);
                l->level = parent ? parent->level + 1 : 0;

                // FIXME if needed, if the parent is an InitListExpr also,
                // here we could increment the parent l->n_entries to keep
                // track of the number (and in case of struct: type) of each
                // of the children nodes.
                //
                // E.g. { var, { var2, var3 }, var4 }
                //      ^^^^^^^^^^^^^^^^^^^^^^^^^^^^^ <- parent
                //             ^^^^^^^^^^^^^^         <- cursor
            }

            rec.data.l = l;
            clang_visitChildren(cursor, callback, &rec);
        }
        break;
    case CXCursor_UnexposedExpr:
        if (parent.kind == CXCursor_InitListExpr) {
            CXString spelling = clang_getTokenSpelling(TU, tokens[0]);
            const char *istr = clang_getCString(spelling);
            StructArrayList *l = rec.parent->data.l;

            if (!strcmp(istr, "[") || !strcmp(istr, ".")) {
                StructArrayItem *sai;
                enum StructArrayType exp_type = istr[0] == '.' ?
                                                TYPE_STRUCT : TYPE_ARRAY;
                // [index] = val   or   .member = val
                // ^^^^^^^^^^^^^        ^^^^^^^^^^^^^
                if (l->type == TYPE_IRRELEVANT) {
                    l->type = exp_type;
                } else if (l->type != exp_type) {
                    fprintf(stderr, "Mixed struct/array!\n");
                    exit(1);
                }

                if (l->n_entries == l->n_allocated_entries) {
                    unsigned num = l->n_allocated_entries + 16;
                    void *mem = realloc(l->entries, sizeof(*l->entries) * num);
                    if (!mem) {
                        fprintf(stderr, "Failed to allocate str/arr entry mem\n");
                        exit(1);
                    }
                    l->entries = (StructArrayItem *) mem;
                    l->n_allocated_entries = num;
                }

                sai = &l->entries[l->n_entries];
                sai->index = 0;
                sai->expression_offset.start = get_token_offset(tokens[0]);
                sai->expression_offset.end   = get_token_offset(tokens[n_tokens - 2]);
                sai->value_offset.start = get_token_offset(tokens[3 + (istr[0] == '[')]);
                sai->value_offset.end   = get_token_offset(tokens[n_tokens - 2]);
                rec.data.l = l;
                clang_visitChildren(cursor, callback, &rec);
                l->n_entries++;
            } else {
                clang_visitChildren(cursor, callback, &rec);
            }
            clang_disposeString(spelling);
        } else {
            clang_visitChildren(cursor, callback, &rec);
        }
        break;
    case CXCursor_MemberRef:
        if (parent.kind == CXCursor_UnexposedExpr) {
            // designated initializer (struct)
            // .member = val
            //  ^^^^^^
            StructArrayList *l = rec.parent->data.l;
            StructArrayItem *sai = &l->entries[l->n_entries];
            const char *member = clang_getCString(str);

            assert(sai);
            assert(l->type == TYPE_STRUCT);
            sai->index = find_member_index_in_struct(l->str_decl, member);
        }
        break;
    case CXCursor_IntegerLiteral:
    case CXCursor_DeclRefExpr:
    case CXCursor_BinaryOperator:
        if (parent.kind == CXCursor_UnexposedExpr && rec.parent->data.opaque) {
            CXString spelling = clang_getTokenSpelling(TU, tokens[n_tokens - 1]);
            if (!strcmp(clang_getCString(spelling), "]")) {
                // [index] = { val }
                //  ^^^^^
                int cache[3] = { 0 };
                StructArrayList *l = (StructArrayList *) rec.parent->data.l;
                StructArrayItem *sai = &l->entries[l->n_entries];

                fill_enum_value(cursor, parent, cache);
                assert(cache[0] == 1);
                assert(sai);
                assert(l->type == TYPE_ARRAY);
                sai->index = cache[1];
            }
            clang_disposeString(spelling);
        } else if (cursor.kind != CXCursor_BinaryOperator)
            break;
    default:
        clang_visitChildren(cursor, callback, &rec);
        break;
    }

    clang_disposeString(str);
    clang_disposeTokens(TU, tokens, n_tokens);
    clang_disposeString(filename);

    return CXChildVisit_Continue;
}

static void get_token_position(CXToken token, unsigned *lnum,
                               unsigned *pos, unsigned *off)
{
    CXFile file;
    CXSourceLocation l = clang_getTokenLocation(TU, token);
    clang_getSpellingLocation(l, &file, lnum, pos, off);

    // clang starts counting at 1 for some reason
    (*lnum)--;
    (*pos)--;
}

static void indent_for_token(CXToken token, unsigned *lnum,
                             unsigned *pos, unsigned *off)
{
    unsigned l, p;
    get_token_position(token, &l, &p, off);
    for (; *lnum < l; (*lnum)++, *pos = 0)
        printf("\n");
    for (; *pos < p; (*pos)++)
        printf(" ");
}

static void print_token(CXToken token, unsigned *lnum,
                        unsigned *pos)
{
    CXString s = clang_getTokenSpelling(TU, token);
    const char *str = clang_getCString(s);
    printf("%s", str);
    (*pos) += strlen(str);
    clang_disposeString(s);
}

static unsigned find_token_for_offset(CXToken *tokens, unsigned n_tokens,
                                      unsigned n, unsigned off)
{
    for (; n < n_tokens; n++) {
        unsigned l, p, o;
        get_token_position(tokens[n], &l, &p, &o);
        if (o == off)
            return n;
    }

    abort();
}

static unsigned find_value_index(StructArrayList *l, unsigned i)
{
    unsigned n;

    if (l->type == TYPE_IRRELEVANT) {
        return i;
    } else {
        for (n = 0; n < l->n_entries; n++) {
            if (l->entries[n].index == i)
                return n;
        }
    }

    return -1;
}

static unsigned find_index_for_level(unsigned level, unsigned index,
                                     unsigned start)
{
    unsigned n, cnt = 0;

    for (n = start; n < n_struct_array_lists; n++) {
        if (struct_array_lists[n].level < level) {
            return n_struct_array_lists;
        } else if (struct_array_lists[n].level == level) {
            if (cnt++ == index)
                return n;
        }
    }

    return n_struct_array_lists;
}

static void replace_struct_array(unsigned *_saidx, unsigned *lnum,
                                 unsigned *cpos, unsigned *_n,
                                 CXToken *tokens, unsigned n_tokens)
{
    unsigned saidx = *_saidx, off, i, n = *_n, j;

    // we assume here the indenting for the first opening token,
    // i.e. the '{', is already taken care of
    print_token(tokens[n++], lnum, cpos);

    for (j = 0, i = 0; i < struct_array_lists[saidx].n_entries; j++) {
        unsigned expr_off_s = struct_array_lists[saidx].entries[i].expression_offset.start;
        unsigned expr_off_e = struct_array_lists[saidx].entries[i].expression_offset.end;
        unsigned val_idx = find_value_index(&struct_array_lists[saidx], j);
        // FIXME this doesn't work if i == 0
        // the proper solution for that is to indent before, then print the
        // value + inter-value tokens or this placeholder + comma
        if (val_idx == -1) {
            printf(",");
            if (saidx < n_struct_array_lists - 1 &&
                struct_array_lists[saidx + 1].level > struct_array_lists[saidx].level) {
                printf("{}");
            } else {
                printf("0");
            }
            continue; // gap
        }
        unsigned val_off_s = struct_array_lists[saidx].entries[val_idx].value_offset.start;
        unsigned val_off_e = struct_array_lists[saidx].entries[val_idx].value_offset.end;
        unsigned indent_token_end = find_token_for_offset(tokens, n_tokens, *_n, expr_off_s);
        unsigned next_indent_token_start = find_token_for_offset(tokens, n_tokens, *_n,
                                                                 expr_off_e);
        unsigned val_token_start = find_token_for_offset(tokens, n_tokens, *_n, val_off_s);
        unsigned val_token_end = find_token_for_offset(tokens, n_tokens, *_n, val_off_e);
        unsigned saidx2 = find_index_for_level(struct_array_lists[saidx].level + 1,
                                               val_idx, saidx + 1);

        // indent as if we were in order
        for (; n <= indent_token_end; n++) {
            indent_for_token(tokens[n], lnum, cpos, &off);
            if (n != indent_token_end)
                print_token(tokens[n], lnum, cpos);
        }

        // adjust position
        get_token_position(tokens[val_token_start], lnum, cpos, &off);

        // print values out of order
        for (n = val_token_start; n <= val_token_end; n++) {
            if (saidx2 < n_struct_array_lists &&
                off == struct_array_lists[saidx2].value_offset.start) {
                replace_struct_array(&saidx2, lnum, cpos, &n,
                                     tokens, n_tokens);
            } else {
                print_token(tokens[n], lnum, cpos);
            }
            if (n != val_token_end)
                indent_for_token(tokens[n + 1], lnum, cpos, &off);
        }

        // adjust token index and position back
        n = next_indent_token_start;
        get_token_position(tokens[n], lnum, cpos, &off);
        CXString spelling = clang_getTokenSpelling(TU, tokens[n]);
        (*cpos) += strlen(clang_getCString(spelling));
        clang_disposeString(spelling);
        n++;
        i++;
    }

    // update *saidx
    *_saidx = find_index_for_level(struct_array_lists[saidx].level, 1, saidx);

    // print '}' closing token
    n = find_token_for_offset(tokens, n_tokens, *_n,
                              struct_array_lists[saidx].value_offset.end);
    indent_for_token(tokens[n], lnum, cpos, &off);
    print_token(tokens[n], lnum, cpos);
    *_n = n;
}

static void print_tokens(CXToken *tokens, unsigned n_tokens)
{
    unsigned cpos = 0, lnum = 0, n, saidx = 0, off;

    for (n = 0; n < n_tokens; n++) {
        indent_for_token(tokens[n], &lnum, &cpos, &off);
        if (saidx < n_struct_array_lists &&
            off == struct_array_lists[saidx].value_offset.start) {
            if (struct_array_lists[saidx].type == TYPE_IRRELEVANT ||
                struct_array_lists[saidx].n_entries == 0) {
                saidx++;
                print_token(tokens[n], &lnum, &cpos);
            } else {
                replace_struct_array(&saidx, &lnum, &cpos, &n,
                                     tokens, n_tokens);
            }
        } else {
            print_token(tokens[n], &lnum, &cpos);
        }
    }

    // each file ends with a newline
    printf("\n");
}

static void cleanup(void)
{
    unsigned n, m;

#define DEBUG 0
    dprintf("N compound literals: %d\n", n_comp_literal_lists);
    for (n = 0; n < n_comp_literal_lists; n++) {
        dprintf("[%d]: type=%d, struct=%p (%s), variable range=%u-%u\n",
                n, comp_literal_lists[n].type,
                comp_literal_lists[n].str_decl,
                comp_literal_lists[n].str_decl ?
                    comp_literal_lists[n].str_decl->name : "<none>",
                comp_literal_lists[n].value_token.start,
                comp_literal_lists[n].value_token.end);
    }
    free(comp_literal_lists);

    dprintf("N array/struct variables: %d\n", n_struct_array_lists);
    for (n = 0; n < n_struct_array_lists; n++) {
        dprintf("[%d]: type=%d, struct=%p (%s), level=%d, n_entries=%d, range=%u-%u\n",
                n, struct_array_lists[n].type,
                struct_array_lists[n].str_decl,
                struct_array_lists[n].str_decl ?
                    struct_array_lists[n].str_decl->name : "<none>",
                struct_array_lists[n].level,
                struct_array_lists[n].n_entries,
                struct_array_lists[n].value_offset.start,
                struct_array_lists[n].value_offset.end);
        for (m = 0; m < struct_array_lists[n].n_entries; m++) {
            dprintf(" [%d]: idx=%d, range=%u-%u\n",
                    m, struct_array_lists[n].entries[m].index,
                    struct_array_lists[n].entries[m].value_offset.start,
                    struct_array_lists[n].entries[m].value_offset.end);
        }
        free(struct_array_lists[n].entries);
    }
    free(struct_array_lists);

    dprintf("N typedef entries: %d\n", n_typedefs);
    for (n = 0; n < n_typedefs; n++) {
        if (typedefs[n].struct_decl) {
            if (typedefs[n].struct_decl->name[0]) {
                dprintf("[%d]: %s (struct %s = %p)\n",
                        n, typedefs[n].name,
                        typedefs[n].struct_decl->name,
                        typedefs[n].struct_decl);
            } else {
                dprintf("[%d]: %s (<anonymous> struct = %p)\n",
                        n, typedefs[n].name,
                        typedefs[n].struct_decl);
            }
        } else if (typedefs[n].enum_decl) {
            if (typedefs[n].enum_decl->name[0]) {
                dprintf("[%d]: %s (enum %s = %p)\n",
                        n, typedefs[n].name,
                        typedefs[n].enum_decl->name,
                        typedefs[n].enum_decl);
            } else {
                dprintf("[%d]: %s (<anonymous> enum = %p)\n",
                        n, typedefs[n].name,
                        typedefs[n].enum_decl);
            }
        } else {
            dprintf("[%d]: %s (%s)\n",
                    n, typedefs[n].name, typedefs[n].proxy);
            free(typedefs[n].proxy);
        }
        free(typedefs[n].name);
    }
    free(typedefs);

    // free memory
    dprintf("N struct entries: %d\n", n_structs);
    for (n = 0; n < n_structs; n++) {
        if (structs[n].name[0]) {
            dprintf("[%d]: %s (%p)\n", n, structs[n].name, &structs[n]);
        } else {
            dprintf("[%d]: <anonymous> (%p)\n", n, &structs[n]);
        }
        for (m = 0; m < structs[n].n_entries; m++) {
            dprintf(" [%d]: %s (%s/%d/%d)\n",
                    m, structs[n].entries[m].name,
                    structs[n].entries[m].type,
                    structs[n].entries[m].n_ptrs,
                    structs[n].entries[m].array_size);
            free(structs[n].entries[m].type);
            free(structs[n].entries[m].name);
        }
        free(structs[n].entries);
        free(structs[n].name);
    }
    free(structs);

    dprintf("N enum entries: %d\n", n_enums);
    for (n = 0; n < n_enums; n++) {
        if (enums[n].name[0]) {
            dprintf("[%d]: %s (%p)\n", n, enums[n].name, &enums[n]);
        } else {
            dprintf("[%d]: <anonymous> (%p)\n", n, &enums[n]);
        }
        for (m = 0; m < enums[n].n_entries; m++) {
            dprintf(" [%d]: %s = %d\n", m,
                    enums[n].entries[m].name,
                    enums[n].entries[m].value);
            free(enums[n].entries[m].name);
        }
        free(enums[n].entries);
        free(enums[n].name);
    }
    free(enums);
#define DEBUG 0
}

int main(int argc, char *argv[])
{
    CXIndex index;
    unsigned n_tokens;
    CXToken *tokens;
    CXSourceRange range;
    CXCursor cursor;
    CursorRecursion rec;

    index  = clang_createIndex(1, 1);
    TU     = clang_createTranslationUnitFromSourceFile(index, argv[1], 0,
                                                       NULL, 0, NULL);
    cursor = clang_getTranslationUnitCursor(TU);
    range  = clang_getCursorExtent(cursor);
    clang_tokenize(TU, range, &tokens, &n_tokens);

    rec.tokens = tokens;
    rec.n_tokens = n_tokens;
    rec.kind = CXCursor_TranslationUnit;
    rec.parent = NULL;
    rec.data.opaque = NULL;
    clang_visitChildren(cursor, callback, &rec);
    print_tokens(tokens, n_tokens);
    clang_disposeTokens(TU, tokens, n_tokens);

    clang_disposeTranslationUnit(TU);
    clang_disposeIndex(index);

    cleanup();

    return 0;
}
