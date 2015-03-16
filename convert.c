/*
 * C99-to-MSVC-compatible-C89 syntax converter
 * Copyright (c) 2012 Ronald S. Bultje <rsbultje@gmail.com>
 * Copyright (c) 2012 Derek Buitenhuis <derek.buitenhuis@gmail.com>
 * Copyright (c) 2012 Martin Storsjo <martin@martin.st>
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * You may obtain a copy of the License at
 *
 * http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 */

#include <assert.h>
#include <stdio.h>
#include <clang-c/Index.h>
#include <string.h>
#include <stdlib.h>
#include <inttypes.h>

#ifdef _MSC_VER
#define strtoll _strtoi64
#endif

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
    unsigned struct_decl_idx;
    char *name;
    unsigned n_ptrs; // 0 if not a pointer
    unsigned array_depth; // 0 if no array
    CXCursor cursor;
} StructMember;

typedef struct {
    StructMember *entries;
    unsigned n_entries;
    unsigned n_allocated_entries;
    char *name;
    CXCursor cursor;
    int is_union;
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
    unsigned struct_decl_idx;
    unsigned enum_decl_idx;
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
    unsigned struct_decl_idx;
    unsigned array_depth;
    StructArrayItem *entries;
    unsigned level;
    unsigned n_entries;
    unsigned n_allocated_entries;
    struct {
        unsigned start, end;
    } value_offset;
    int convert_to_assignment;
    char *name;
} StructArrayList;
static StructArrayList *struct_array_lists = NULL;
static unsigned n_struct_array_lists = 0;
static unsigned n_allocated_struct_array_lists = 0;

typedef struct {
    int end;
    int n_scopes;
} EndScope;
static EndScope *end_scopes = NULL;
static unsigned n_end_scopes = 0;
static unsigned n_allocated_end_scopes = 0;

static FILE *out;

static CXTranslationUnit TU;

#define DEBUG 0
#define dprintf(...) \
    if (DEBUG) \
        printf(__VA_ARGS__)

static unsigned find_token_index(CXToken *tokens, unsigned n_tokens,
                                 const char *str)
{
    unsigned n;

    for (n = n_tokens - 1; n != (unsigned) -1; n--) {
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
        clang_disposeString(tstr);
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
        clang_disposeString(tstr);
    }

    return str;
}

static void register_struct(const char *str, CXCursor cursor,
                            TypedefDeclaration *decl_ptr, int is_union);
static void register_enum(const char *str, CXCursor cursor,
                          TypedefDeclaration *decl_ptr);
static unsigned find_struct_decl_idx_for_type_name(const char *name);

static enum CXChildVisitResult find_anon_struct(CXCursor cursor,
                                                CXCursor parent,
                                                CXClientData client_data)
{
    CXString cstr = clang_getCursorSpelling(cursor);
    const char *str = clang_getCString(cstr);

    switch (cursor.kind) {
    case CXCursor_StructDecl:
        register_struct(str, cursor, client_data, 0);
        break;
    case CXCursor_UnionDecl:
        register_struct(str, cursor, client_data, 1);
        break;
    case CXCursor_EnumDecl:
        register_enum(str, cursor, client_data);
        break;
    case CXCursor_TypeRef: {
        TypedefDeclaration *td = client_data;
        td->struct_decl_idx = find_struct_decl_idx_for_type_name(str);
        break;
    }
    default:
        break;
    }

    clang_disposeString(cstr);

    return CXChildVisit_Continue;
}

static enum CXChildVisitResult fill_struct_members(CXCursor cursor,
                                                   CXCursor parent,
                                                   CXClientData client_data)
{
    unsigned decl_idx = (unsigned) client_data;
    StructDeclaration *decl = &structs[decl_idx];
    CXString cstr = clang_getCursorSpelling(cursor);
    const char *str = clang_getCString(cstr);

    switch (cursor.kind) {
    case CXCursor_FieldDecl: {
        unsigned n = decl->n_entries, idx, m;
        CXToken *tokens = 0;
        unsigned int n_tokens = 0;
        CXSourceRange range = clang_getCursorExtent(cursor);
        TypedefDeclaration td;

        // padding bitfields
        if (!strcmp(str, "")) {
            clang_disposeString(cstr);
            return CXChildVisit_Continue;
        }

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
        decl->entries[n].array_depth = 0;
        for (m = idx + 1; m < n_tokens; m++) {
            CXString tstr = clang_getTokenSpelling(TU, tokens[m]);
            const char *cstr = clang_getCString(tstr);
            int res = strcmp(cstr, ";") && strcmp(cstr, ",");
            if (!strcmp(cstr, "["))
                decl->entries[n].array_depth++;
            clang_disposeString(tstr);
            if (!res)
                break;
        }

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

        memset(&td, 0, sizeof(td));
        td.struct_decl_idx = (unsigned) -1;
        clang_visitChildren(cursor, find_anon_struct, &td);
        decl->entries[n].struct_decl_idx = td.struct_decl_idx;

        // FIXME it's not hard to find the struct name (either because
        // tokens[idx-2-n_ptrs] == 'struct', or because tokens[idx-1-n_ptrs]
        // is a typedef for the struct name), and then we can use
        // find_struct_decl() to find the StructDeclaration belonging to
        // that type.

        clang_disposeTokens(TU, tokens, n_tokens);
        break;
    }
    case CXCursor_StructDecl:
        register_struct(str, cursor, NULL, 0);
        break;
    case CXCursor_UnionDecl:
        register_struct(str, cursor, NULL, 1);
        break;
    case CXCursor_EnumDecl:
        register_enum(str, cursor, NULL);
        break;
    default:
        break;
    }

    clang_disposeString(cstr);

    return CXChildVisit_Continue;
}

static void register_struct(const char *str, CXCursor cursor,
                            TypedefDeclaration *decl_ptr, int is_union)
{
    unsigned n;
    StructDeclaration *decl;

    for (n = 0; n < n_structs; n++) {
        if ((str[0] != 0 && !strcmp(structs[n].name, str)) ||
            !memcmp(&cursor, &structs[n].cursor, sizeof(cursor))) {
            /* already exists */
            if (decl_ptr)
                decl_ptr->struct_decl_idx = n;
            if (structs[n].n_entries == 0) {
                // Fill in structs that were defined (empty) earlier, i.e.
                // 'struct AVFilterPad;', followed by the full declaration
                // 'struct AVFilterPad { ... };'
                clang_visitChildren(cursor, fill_struct_members, (void *) n);
            }
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

    if (decl_ptr)
        decl_ptr->struct_decl_idx = n_structs;
    decl = &structs[n_structs++];
    decl->name = strdup(str);
    decl->cursor = cursor;
    decl->n_entries = 0;
    decl->n_allocated_entries = 0;
    decl->entries = NULL;
    decl->is_union = is_union;

    clang_visitChildren(cursor, fill_struct_members, (void *) (n_structs - 1));
}

static int arithmetic_expression(int val1, const char *expr, int val2)
{
    assert(expr[1] == 0 || expr[2] == 0);

    if (expr[1] == 0) {
        switch (expr[0]) {
        case '^': return val1 ^ val2;
        case '|': return val1 | val2;
        case '&': return val1 & val2;
        case '+': return val1 + val2;
        case '-': return val1 - val2;
        case '*': return val1 * val2;
        case '/': return val1 / val2;
        case '%': return val1 % val2;
        default:
            fprintf(stderr, "Arithmetic expression '%c' not handled\n",
                    expr[0]);
            exit(1);
        }
    } else {
#define TWOCHARCODE(a, b) ((a << 8) | b)
#define TWOCHARTAG(expr) (TWOCHARCODE(expr[0], expr[1]))
        switch (TWOCHARTAG(expr)) {
        case TWOCHARCODE('<', '='): return val1 <= val2;
        case TWOCHARCODE('>', '='): return val1 >= val2;
        case TWOCHARCODE('!', '='): return val1 != val2;
        case TWOCHARCODE('=', '='): return val1 == val2;
        case TWOCHARCODE('<', '<'): return val1 << val2;
        case TWOCHARCODE('>', '>'): return val1 >> val2;
        default:
            fprintf(stderr, "Arithmetic expression '%s' not handled\n",
                    expr);
            exit(1);
        }
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

typedef struct FillEnumMemberCache {
    int n[3];
    char *op;
} FillEnumMemberCache;

static enum CXChildVisitResult fill_enum_value(CXCursor cursor,
                                               CXCursor parent,
                                               CXClientData client_data)
{
    FillEnumMemberCache *cache = (FillEnumMemberCache *) client_data;
    CXToken *tokens = 0;
    unsigned int n_tokens = 0;
    CXSourceRange range = clang_getCursorExtent(cursor);

    clang_tokenize(TU, range, &tokens, &n_tokens);
    if (parent.kind == CXCursor_BinaryOperator && cache->n[0] == 0) {
        CXString str = clang_getTokenSpelling(TU, tokens[n_tokens - 1]);
        cache->op = strdup(clang_getCString(str));
        clang_disposeString(str);
    }

    switch (cursor.kind) {
    case CXCursor_UnaryOperator: {
        CXString tsp = clang_getTokenSpelling(TU, tokens[0]);
        const char *str = clang_getCString(tsp);
        clang_visitChildren(cursor, fill_enum_value, client_data);
        assert(str[1] == 0 && (str[0] == '+' || str[0] == '-' || str[0] == '~'));
        assert(cache->n[0] == 1);
        if (str[0] == '-') {
            cache->n[1] = -cache->n[1];
        } else if (str[0] == '~') {
            cache->n[1] = ~cache->n[1];
        }
        clang_disposeString(tsp);
        break;
    }
    case CXCursor_BinaryOperator: {
        FillEnumMemberCache cache2;

        memset(&cache2, 0, sizeof(cache2));
        assert(n_tokens >= 4);
        clang_visitChildren(cursor, fill_enum_value, &cache2);
        assert(cache2.n[0] == 2);
        assert(cache2.op != NULL);
        cache->n[++cache->n[0]] = arithmetic_expression(cache2.n[1],
                                                        cache2.op,
                                                        cache2.n[2]);
        free(cache2.op);
        break;
    }
    case CXCursor_IntegerLiteral: {
        CXString tsp;
        const char *str;
        char *end;

        assert(n_tokens == 2);
        tsp = clang_getTokenSpelling(TU, tokens[0]);
        str = clang_getCString(tsp);
        cache->n[++cache->n[0]] = strtol(str, &end, 0);
        assert(end - str == strlen(str) ||
               (end - str == strlen(str) - 1 && // str may have a suffix like 'U' that strtol doesn't consume
                (*end == 'U' || *end == 'u')));
        clang_disposeString(tsp);
        break;
    }
    case CXCursor_DeclRefExpr: {
        CXString tsp;

        assert(n_tokens == 2);
        tsp = clang_getTokenSpelling(TU, tokens[0]);
        cache->n[++cache->n[0]] = find_enum_value(clang_getCString(tsp));
        clang_disposeString(tsp);
        break;
    }
    case CXCursor_CharacterLiteral: {
        CXString spelling;
        const char *str;

        assert(n_tokens == 2);
        spelling = clang_getTokenSpelling(TU, tokens[0]);
        str = clang_getCString(spelling);
        assert(strlen(str) == 3 && str[0] == '\'' && str[2] == '\'');
        cache->n[++cache->n[0]] = str[1];
        clang_disposeString(spelling);
        break;
    }
    case CXCursor_ParenExpr:
        clang_visitChildren(cursor, fill_enum_value, client_data);
        break;
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
        FillEnumMemberCache cache;

        memset(&cache, 0, sizeof(cache));
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
        clang_visitChildren(cursor, fill_enum_value, &cache);
        assert(cache.n[0] <= 1);
        if (cache.n[0] == 1) {
            decl->entries[n].value = cache.n[1];
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
        if ((str[0] != 0 && !strcmp(enums[n].name, str)) ||
            !memcmp(&cursor, &enums[n].cursor, sizeof(cursor))) {
            /* already exists */
            if (decl_ptr)
                decl_ptr->enum_decl_idx = n;
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

    if (decl_ptr)
        decl_ptr->enum_decl_idx = n_enums;
    decl = &enums[n_enums++];
    decl->name = strdup(str);
    decl->cursor = cursor;
    decl->n_entries = 0;
    decl->n_allocated_entries = 0;
    decl->entries = NULL;

    clang_visitChildren(cursor, fill_enum_members, decl);
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
    if (decl->struct_decl_idx != (unsigned) -1) {
        typedefs[n].struct_decl_idx = decl->struct_decl_idx;
        typedefs[n].proxy = NULL;
        typedefs[n].enum_decl_idx = (unsigned) -1;
    } else if (decl->enum_decl_idx != (unsigned) -1) {
        typedefs[n].enum_decl_idx = decl->enum_decl_idx;
        typedefs[n].struct_decl_idx = (unsigned) -1;
        typedefs[n].proxy = NULL;
    } else {
        typedefs[n].enum_decl_idx = (unsigned) -1;
        typedefs[n].struct_decl_idx = (unsigned) -1;
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

static unsigned find_struct_decl_idx_by_name(const char *name)
{
    unsigned n;

    for (n = 0; n < n_structs; n++) {
        if (!strcmp(name, structs[n].name))
            return n;
    }

    return (unsigned) -1;
}

static void resolve_proxy(TypedefDeclaration *decl)
{
    if (decl->struct_decl_idx != (unsigned) -1 ||
        decl->enum_decl_idx != (unsigned) -1)
        return;

    decl->struct_decl_idx = find_struct_decl_idx_for_type_name(decl->proxy);
    // we could theoretically also resolve the enum, but we wouldn't use
    // that information, so let's just not
}

static TypedefDeclaration *find_typedef_decl_by_name(const char *name)
{
    unsigned n;

    for (n = 0; n < n_typedefs; n++) {
        if (!strcmp(name, typedefs[n].name)) {
            resolve_proxy(&typedefs[n]);
            return &typedefs[n];
        }
    }

    return NULL;
}

// FIXME this function has some duplicate functionality compared to
// fill_struct_members() further up.
static unsigned find_struct_decl_idx(const char *var, CXToken *tokens,
                                     unsigned n_tokens, unsigned *depth)
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

    *depth = 0;
    for (n = 0; n < n_tokens; n++) {
        CXString spelling = clang_getTokenSpelling(TU, tokens[n]);
        int res = strcmp(clang_getCString(spelling), var);
        clang_disposeString(spelling);
        if (!res)
            break;
    }
    if (n == n_tokens)
        return (unsigned) -1;

    var_tok_idx = n;
    for (n = var_tok_idx + 1; n < n_tokens; n++) {
        CXString spelling = clang_getTokenSpelling(TU, tokens[n]);
        int res = strcmp(clang_getCString(spelling), "=");
        if (!strcmp(clang_getCString(spelling), "["))
            (*depth)++;
        clang_disposeString(spelling);
        if (!res)
            break;
    }

    // is it a struct?
    if (var_tok_idx > 1) {
        CXString spelling;
        int res;

        spelling = clang_getTokenSpelling(TU, tokens[var_tok_idx - 2]);
        res = strcmp(clang_getCString(spelling), "struct");
        clang_disposeString(spelling);

        if (!res) {
            unsigned str_decl;

            spelling = clang_getTokenSpelling(TU, tokens[var_tok_idx - 1]);
            str_decl = find_struct_decl_idx_by_name(clang_getCString(spelling));
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

        if (td_decl && td_decl->struct_decl_idx != (unsigned) -1)
            return td_decl->struct_decl_idx;
    }

    return (unsigned) -1;
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

static unsigned find_struct_decl_idx_for_type_name(const char *name)
{
    if (!strncmp(name, "const ", 6))
        name += 6;

    if (!strncmp(name, "struct ", 7)) {
        return find_struct_decl_idx_by_name(name + 7);
    } else if (!strncmp(name, "union ", 6)) {
        return find_struct_decl_idx_by_name(name + 6);
    } else {
        TypedefDeclaration *decl = find_typedef_decl_by_name(name);
        return decl ? decl->struct_decl_idx : (unsigned) -1;
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
    TYPE_CONST_DECL,    // anything with a const that can be statically
                        // declared, e.g. x = ((const int[]){ y, z })[0] ->
                        // static const int tmp[] = { y, z } [..] x = tmp[0]
    TYPE_NEW_CONTEXT,   // func(); int x; [..] -> func(); { int x; [..] }
    TYPE_LOOP_CONTEXT,  // for(int i = 0; ... -> { int i = 0; for (; ... }
};

typedef struct {
    enum CLType type;
    struct {
        unsigned start, end; // to get the values
    } value_token, cast_token, context;
    unsigned cast_token_array_start;
    unsigned struct_decl_idx; // struct type
    union {
        struct {
            char *tmp_var_name; // temporary variable name for the constant
                                // data, assigned in the first stage (var
                                // declaration), and used in the second stage
                                // (replacement of the CL with the var ref)
        } t_c_d;
    } data;
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
    unsigned child_cntr;
    unsigned allow_var_decls;
    CXToken *tokens;
    unsigned n_tokens;
    union {
        void *opaque;
        unsigned sal_idx;            // InitListExpr and UnexposedExpr
                                     // after an InitListExpr
        struct {
            unsigned struct_decl_idx;
            unsigned array_depth;
        } var_decl_data;             // VarDecl
        TypedefDeclaration *td_decl; // TypedefDecl
        unsigned cl_idx;             // CompoundLiteralExpr
    } data;
    int is_function;
    int end_scopes;
};

static unsigned find_encompassing_struct_decl(unsigned start, unsigned end,
                                              StructArrayList **ptr,
                                              CursorRecursion *rec,
                                              unsigned *depth)
{
    /*
     * In previously registered arrays/structs, find one with a start-end
     * that fully contains the given start/end function arguments. If found,
     * return that array/struct's type. If not found, return NULL.
     */
    unsigned n;

    *depth = 0;
    *ptr = NULL;
    for (n = n_struct_array_lists - 1; n != (unsigned) -1; n--) {
        if (start >= struct_array_lists[n].value_offset.start &&
            end   <= struct_array_lists[n].value_offset.end &&
            !(start == struct_array_lists[n].value_offset.start &&
              end   == struct_array_lists[n].value_offset.end)) {
            if (struct_array_lists[n].type == TYPE_ARRAY) {
                /* { <- parent
                 *   [..] = { .. }, <- us
                 * } */
                assert((rec->parent->kind == CXCursor_UnexposedExpr &&
                        rec->parent->parent->kind == CXCursor_InitListExpr) ||
                       rec->parent->kind == CXCursor_InitListExpr);

                *ptr = &struct_array_lists[n];
                assert(struct_array_lists[n].array_depth > 0);
                *depth = struct_array_lists[n].array_depth - 1;

                return struct_array_lists[n].struct_decl_idx;
            } else if (struct_array_lists[n].type == TYPE_STRUCT) {
                /* { <- parent
                 *   .member = { .. }, <- us
                 * } */
                unsigned m;
                StructArrayList *l = *ptr = &struct_array_lists[n];

                assert((rec->parent->kind == CXCursor_UnexposedExpr &&
                        rec->parent->parent->kind == CXCursor_InitListExpr) ||
                       rec->parent->kind == CXCursor_InitListExpr);
                assert(l->array_depth == 0);
                for (m = 0; m <= l->n_entries; m++) {
                    if (start >= l->entries[m].expression_offset.start &&
                        end   <= l->entries[m].expression_offset.end) {
                        unsigned s_idx = l->struct_decl_idx;
                        unsigned m_idx = l->entries[m].index;
                        *depth = structs[s_idx].entries[m_idx].array_depth;
                        return structs[s_idx].entries[m_idx].struct_decl_idx;
                    }
                }

                // Can this ever trigger?
                return (unsigned) -1;
            } else if (rec->parent->kind == CXCursor_InitListExpr) {
                /* { <- parent
                 *   { .. }, <- us (so now the question is: array or struct?)
                 * } */
                StructArrayList *l = *ptr = &struct_array_lists[n];
                unsigned s_idx = l->struct_decl_idx;
                unsigned m_idx = rec->parent->child_cntr - 1;

                assert(rec->parent->kind == CXCursor_InitListExpr);

                if (l->array_depth > 0) {
                    *depth = l->array_depth - 1;
                    return l->struct_decl_idx;
                } else if (s_idx != (unsigned) -1) {
                    assert(m_idx < structs[s_idx].n_entries);
                    *depth = structs[s_idx].entries[m_idx].array_depth;
                    return structs[s_idx].entries[m_idx].struct_decl_idx;
                } else {
                    return (unsigned) -1;
                }
            } else {
                // Can this ever trigger?
                return (unsigned) -1;
            }
        }
    }

    return (unsigned) -1;
}

static int is_const(CompoundLiteralList *l, CursorRecursion *rec)
{
    unsigned n;

    for (n = 0; n < rec->n_tokens - 1; n++) {
        unsigned off = get_token_offset(rec->tokens[n]);

        if (off > l->cast_token.start && off < l->cast_token.end) {
            CXString spelling = clang_getTokenSpelling(TU, rec->tokens[n]);
            int res = strcmp(clang_getCString(spelling), "const");
            clang_disposeString(spelling);
            if (!res)
                return 1;
        } else if (off >= l->cast_token.end)
            break;
    }

    return 0;
}

static CursorRecursion *find_function_or_top(CursorRecursion *rec)
{
    CursorRecursion *p;

    for (p = rec; p->parent->kind != CXCursor_FunctionDecl &&
         p->parent->kind != CXCursor_TranslationUnit; p = p->parent) ;

    return p;
}

static CursorRecursion *find_var_decl_context(CursorRecursion *rec)
{
    CursorRecursion *p;

    /* Find a recursion level in which we can declare a new context,
     * i.e. a "{" or a "do {", within which we can declare new variables
     * in a c89-compatible way. At the end of the returned recursion
     * level's token range, we'll add a "}" or a "} while (0);". */
    for (p = rec; p != NULL; p = p->parent) {
        switch (p->kind) {
        case CXCursor_VarDecl:
        case CXCursor_ReturnStmt:
        case CXCursor_CompoundStmt:
        case CXCursor_IfStmt:
        case CXCursor_SwitchStmt:
            return p;
        case CXCursor_CallExpr:
        case CXCursor_CompoundAssignOperator:
        case CXCursor_BinaryOperator:
            // FIXME: do/while/for
            if ((p->parent->kind == CXCursor_IfStmt && p->parent->child_cntr > 1) ||
                (p->parent->kind == CXCursor_CaseStmt && p->parent->child_cntr > 1) ||
                p->parent->kind == CXCursor_CompoundStmt ||
                p->parent->kind == CXCursor_DefaultStmt) {
                return p;
            }
            break;
        default:
            break;
        }
    }

    return NULL;
}

static void analyze_compound_literal_lineage(CompoundLiteralList *l,
                                             CursorRecursion *rec)
{
    CursorRecursion *p = rec, *p2;

#define DEBUG 0
    dprintf("CL lineage: ");
    do {
        dprintf("%d[%d], ", p->kind, p->child_cntr);
    } while ((p = p->parent));
    dprintf("\n");
#define DEBUG 0

    p = rec->parent->parent;
    p2 = find_function_or_top(rec);
    if (p2->parent->kind != CXCursor_FunctionDecl) {
        l->context.start = get_token_offset(p2->tokens[0]);
        l->type = TYPE_CONST_DECL;
        return;
    }
    if (p->kind == CXCursor_VarDecl) {
        l->type = TYPE_OMIT_CAST;
        l->context.start = l->cast_token.start;
    } else if ((p = find_var_decl_context(p))) {
        l->type = TYPE_TEMP_ASSIGN;
        l->context.start = get_token_offset(p->tokens[0]);
        if (p->kind == CXCursor_VarDecl) {
            /* if the parent is a VarDecl, the context.end should be the end
             * of the whole context in which that variable exists, not just
             * the end of the context of this particular statement. */
            p = p->parent;
            assert(p->kind == CXCursor_DeclStmt);
            p = p->parent;
        }
        l->context.end = get_token_offset(p->tokens[p->n_tokens - 1]);
    }
}

static void analyze_decl_context(CompoundLiteralList *l,
                                 CursorRecursion *rec)
{
    CursorRecursion *p = rec->parent;

    // FIXME if parent.kind == CXCursor_CompoundStmt, simply go from here until
    // the end of that compound context.
    // in other cases (e.g. declaration inside a for/while), find the complete
    // context (e.g. before the while/for) and declare new context around that
    // whole thing
    if (p->kind == CXCursor_CompoundStmt) {
        l->type = TYPE_NEW_CONTEXT;
        l->context.start = get_token_offset(rec->tokens[0]);
        l->cast_token.start = get_token_offset(rec->tokens[0]);
        l->context.end = get_token_offset(p->tokens[p->n_tokens - 1]);
    } else if (p->kind == CXCursor_ForStmt && rec->parent->child_cntr == 1) {
        l->type = TYPE_LOOP_CONTEXT;
        l->context.start = get_token_offset(p->tokens[0]);
        l->context.end = get_token_offset(p->tokens[p->n_tokens - 1]);
        l->cast_token.start = get_token_offset(rec->tokens[0]);
        l->cast_token.end = get_token_offset(rec->tokens[rec->n_tokens - 2]);
    }
}

static void get_comp_literal_type_info(StructArrayList *sal,
                                       CompoundLiteralList *cl,
                                       CXToken *tokens, unsigned n_tokens,
                                       unsigned start, unsigned end)
{
    // FIXME also see find_struct_decl_idx()
    unsigned type_tok_idx = (unsigned) -1, array_tok_idx = (unsigned) -1,
             end_tok_idx = (unsigned) -1, n;
    char *type;

    for (n = 0; n < n_tokens; n++) {
        unsigned off = get_token_offset(tokens[n]);
        if (off == cl->cast_token.start) {
            type_tok_idx = n + 1;
        } else if (off == cl->cast_token.end) {
            end_tok_idx = n;
        }
        if (off == cl->cast_token_array_start) {
            array_tok_idx = n;
        }
    }
    assert(array_tok_idx != (unsigned) -1 &&
           end_tok_idx != (unsigned) -1 &&
           type_tok_idx != (unsigned) -1);

    sal->array_depth = 0;
    for (n = array_tok_idx; n < end_tok_idx; n++) {
        CXString spelling = clang_getTokenSpelling(TU, tokens[n]);
        int res = strcmp(clang_getCString(spelling), "[");
        clang_disposeString(spelling);
        if (!res)
            sal->array_depth++;
    }
    type = concat_name(tokens, type_tok_idx, array_tok_idx - 1);
    sal->struct_decl_idx = find_struct_decl_idx_for_type_name(type);
    free(type);

    sal->level = 0;
    for (n = n_struct_array_lists - 1; n != (unsigned) -1; n--) {
        if (start >= struct_array_lists[n].value_offset.start &&
            end   <= struct_array_lists[n].value_offset.end &&
            !(start == struct_array_lists[n].value_offset.start &&
              end   == struct_array_lists[n].value_offset.end)) {
                sal->level = struct_array_lists[n].level + 1;
                return;
        }
    }
}

static unsigned get_n_tokens(CXToken *tokens, unsigned n_tokens)
{
    /* clang will set n_tokens to the number including the start of the
     * next statement, regardless of whether that is part of the next
     * statement or not. We actually care, since in some cases (if it's
     * a ";"), we want to close contexts after it, whereas in other
     * cases (if it's the start of the next statement, e.g. "static void
     * function1(..) { .. } static void function2(..) { }", we want to
     * close context before the last token (which in the first case is
     * ";", but in the second case is "static"). */
    int res;
    if (n_tokens > 0) {
        CXString spelling = clang_getTokenSpelling(TU, tokens[n_tokens - 1]);
        res = strcmp(clang_getCString(spelling), ";");
        clang_disposeString(spelling);
    } else {
        res = 1;
    }

    return n_tokens - !!res;
}

static char *find_variable_name(CursorRecursion *rec)
{
    unsigned n;
    // typename varname = { ...
    // typename can be a typedef or "union something"
    for (n = 1; n < rec->n_tokens; n++) {
        CXString spelling = clang_getTokenSpelling(TU, rec->tokens[n]);
        if (!strcmp(clang_getCString(spelling), "=")) {
            char *name;
            clang_disposeString(spelling);
            spelling = clang_getTokenSpelling(TU, rec->tokens[n - 1]);
            name = strdup(clang_getCString(spelling));
            clang_disposeString(spelling);
            return name;
        }
        clang_disposeString(spelling);
    }
    fprintf(stderr, "Unable to find variable name in assignment\n");
    abort();
}

static int index_is_unique(StructArrayList *l, int idx) {
  unsigned n;

  for (n = 0; n < l->n_entries; n++) {
    if (l->entries[n].index == idx)
      return 0;
  }

  return 1;
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
    CursorRecursion rec, *rec_ptr;
    int is_union, is_in_function = 0;

    range = clang_getCursorExtent(cursor);
    pos   = clang_getCursorLocation(cursor);
    str   = clang_getCursorSpelling(cursor);
    clang_tokenize(TU, range, &tokens, &n_tokens);
    clang_getSpellingLocation(pos, &file, &line, &col, &off);
    filename = clang_getFileName(file);

    memset(&rec, 0, sizeof(rec));
    rec.kind = cursor.kind;
    rec.allow_var_decls = 0;
    rec.parent = (CursorRecursion *) client_data;
    rec.parent->child_cntr++;
    rec.tokens = tokens;
    rec.n_tokens = get_n_tokens(tokens, n_tokens);
    if (cursor.kind == CXCursor_FunctionDecl)
        rec.is_function = 1;
    if (parent.kind == CXCursor_CompoundStmt)
        rec.parent->allow_var_decls &= cursor.kind == CXCursor_DeclStmt;

    rec_ptr = (CursorRecursion *) client_data;
    while (rec_ptr) {
        if (rec_ptr->is_function) {
            is_in_function = 1;
            break;
        }
        rec_ptr = rec_ptr->parent;
    }

#define DEBUG 0
    dprintf("DERP: %d [%d:%d] %s @ %d:%d in %s\n", cursor.kind, parent.kind,
            rec.parent->child_cntr, clang_getCString(str), line, col,
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
        decl.struct_decl_idx = (unsigned) -1;
        decl.enum_decl_idx = (unsigned) -1;
        rec.data.td_decl = &decl;
        clang_visitChildren(cursor, callback, &rec);
        register_typedef(clang_getCString(str), tokens, n_tokens,
                         &decl, cursor);
        break;
    }
    case CXCursor_StructDecl:
    case CXCursor_UnionDecl:
        is_union = cursor.kind == CXCursor_UnionDecl;
        if (parent.kind == CXCursor_TypedefDecl) {
            register_struct(clang_getCString(str), cursor,
                            rec.parent->data.td_decl, is_union);
        } else if (parent.kind == CXCursor_VarDecl) {
            TypedefDeclaration td;
            memset(&td, 0, sizeof(td));
            td.struct_decl_idx = (unsigned) -1;
            register_struct(clang_getCString(str), cursor, &td, is_union);
            rec.parent->data.var_decl_data.struct_decl_idx = td.struct_decl_idx;
        } else {
            register_struct(clang_getCString(str), cursor, NULL, is_union);
        }
        break;
    case CXCursor_EnumDecl:
        register_enum(clang_getCString(str), cursor,
                      parent.kind == CXCursor_TypedefDecl ?
                            rec.parent->data.td_decl : NULL);
        break;
    case CXCursor_TypeRef: {
        if (parent.kind == CXCursor_VarDecl &&
            rec.parent->data.var_decl_data.struct_decl_idx == (unsigned) -1) {
            const char *cstr = clang_getCString(str);
            unsigned idx = find_struct_decl_idx_for_type_name(cstr);
            rec.parent->data.var_decl_data.struct_decl_idx = idx;
        }
        break;
    }
    case CXCursor_DeclStmt:
        if (parent.kind != CXCursor_CompoundStmt ||
            !rec.parent->allow_var_decls) {
            // e.g. void function() { int x; function(); int y; ... }
            //                                           ^^^^^^
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
            clang_visitChildren(cursor, callback, &rec);
            analyze_decl_context(l, &rec);
        } else {
            clang_visitChildren(cursor, callback, &rec);
        }
        break;
    case CXCursor_VarDecl: {
        // e.g. static const struct <type> name { val }
        //      ^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^
        unsigned idx = find_struct_decl_idx(clang_getCString(str),
                                            tokens, n_tokens,
                                            &rec.data.var_decl_data.array_depth);
        rec.data.var_decl_data.struct_decl_idx = idx;
        clang_visitChildren(cursor, callback, &rec);
        break;
    }
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
        rec.data.cl_idx = n_comp_literal_lists - 1;
        l->cast_token.start = get_token_offset(tokens[0]);
        l->struct_decl_idx = (unsigned) -1;
        clang_visitChildren(cursor, callback, &rec);
        analyze_compound_literal_lineage(l, &rec);
        break;
    }
    case CXCursor_InitListExpr:
        if (parent.kind == CXCursor_CompoundLiteralExpr) {
            CompoundLiteralList *l = &comp_literal_lists[rec.parent->data.cl_idx];

            // (type) { val }
            //        ^^^^^^^
            l->value_token.start = get_token_offset(tokens[0]);
            l->value_token.end   = get_token_offset(tokens[n_tokens - 2]);
            if (!l->cast_token.end) {
                for (i = 0; i < rec.parent->n_tokens - 1; i++) {
                    CXString spelling = clang_getTokenSpelling(TU,
                                                        rec.parent->tokens[i]);
                    unsigned off = get_token_offset(rec.parent->tokens[i]);
                    int res = strcmp(clang_getCString(spelling), "[");
                    clang_disposeString(spelling);
                    if (!res)
                        l->cast_token_array_start = off;
                    if (off == l->value_token.start)
                        break;
                    else
                        l->cast_token.end = off;
                }
                if (!l->cast_token_array_start)
                    l->cast_token_array_start = l->cast_token.end;
            }
        }
        {
            // another { val } or { .member = val } or { [index] = val }
            StructArrayList *l;
            unsigned parent_idx = (unsigned) -1;

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
            l->name = NULL;
            l->convert_to_assignment = 0;
            l->value_offset.start = get_token_offset(tokens[0]);
            l->value_offset.end   = get_token_offset(tokens[n_tokens - 2]);
            if (rec.parent->kind == CXCursor_VarDecl) {
                l->struct_decl_idx = rec.parent->data.var_decl_data.struct_decl_idx;
                l->array_depth     = rec.parent->data.var_decl_data.array_depth;
                l->level = 0;
            } else if (rec.parent->kind == CXCursor_CompoundLiteralExpr) {
                CompoundLiteralList *cl = &comp_literal_lists[rec.parent->data.cl_idx];
                get_comp_literal_type_info(l, cl,
                                           rec.parent->tokens,
                                           rec.parent->n_tokens,
                                           l->value_offset.start,
                                           l->value_offset.end);
            } else {
                StructArrayList *parent;
                unsigned depth;
                unsigned idx = find_encompassing_struct_decl(l->value_offset.start,
                                                             l->value_offset.end,
                                                             &parent, &rec,
                                                             &depth);
                l->level = parent ? parent->level + 1 : 0;
                l->struct_decl_idx = idx;
                l->array_depth = depth;

                // If the parent is an InitListExpr also, we increment the
                // parent l->n_entries to keep track of the number (and thus
                // in case of a struct: the type) of each child node.
                //
                // E.g. { var, { var2, var3 }, var4 }
                //      ^^^^^^^^^^^^^^^^^^^^^^^^^^^^^ <- parent
                //             ^^^^^^^^^^^^^^         <- cursor
                if (rec.parent->kind == CXCursor_InitListExpr && parent) {
                    unsigned s = l->value_offset.start;
                    unsigned e = l->value_offset.end;
                    StructArrayItem *sai;

                    if (parent->n_entries == parent->n_allocated_entries) {
                        unsigned num = parent->n_allocated_entries + 16;
                        void *mem = realloc(parent->entries,
                                            sizeof(*parent->entries) * num);
                        if (!mem) {
                          fprintf(stderr, "Failed to allocate str/arr entry mem\n");
                          exit(1);
                        }
                        parent->entries = (StructArrayItem *) mem;
                        parent->n_allocated_entries = num;
                    }

                    sai = &parent->entries[parent->n_entries];
                    sai->value_offset.start = s;
                    sai->value_offset.end   = e;
                    sai->expression_offset.start = s;
                    sai->expression_offset.end   = e;
                    sai->index = parent->n_entries > 0 ?
                                 parent->entries[parent->n_entries - 1].index + 1 :
                                 rec.parent->child_cntr - 1;
                    parent_idx = parent - struct_array_lists;
                }
            }

            rec.data.sal_idx = n_struct_array_lists - 1;
            clang_visitChildren(cursor, callback, &rec);
            if (rec.parent->kind == CXCursor_InitListExpr &&
                parent_idx != (unsigned) -1) {
                struct_array_lists[parent_idx].n_entries++;
            }
            l = &struct_array_lists[rec.data.sal_idx];
            if (l->convert_to_assignment &&
                rec.parent->kind == CXCursor_VarDecl) {
                l->value_offset.start -= 2; // Swallow the assignment character
                l->value_offset.end   += 1; // Swallow the final semicolon
                free(l->name);
                l->name = find_variable_name(rec.parent);
                rec_ptr = (CursorRecursion *) client_data;
                while (rec_ptr->kind != CXCursor_CompoundStmt)
                    rec_ptr = rec_ptr->parent;
                if (rec_ptr->kind != CXCursor_CompoundStmt) {
                    fprintf(stderr, "Unable to find enclosing compound statement\n");
                    exit(1);
                }
                rec_ptr->end_scopes++;
            } else
                l->convert_to_assignment = 0;
        }
        break;
    case CXCursor_UnexposedExpr:
        if (parent.kind == CXCursor_InitListExpr) {
            CXString spelling = clang_getTokenSpelling(TU, tokens[0]);
            CXString spelling2 = clang_getTokenSpelling(TU, tokens[1]);
            const char *istr = clang_getCString(spelling);
            const char *istr2 = clang_getCString(spelling2);
            StructArrayList *l = &struct_array_lists[rec.parent->data.sal_idx];
            StructArrayItem *sai;

            if (!strcmp(istr, "[") || !strcmp(istr, ".") || !strcmp(istr2, ":")) {
                enum StructArrayType exp_type = (istr[0] == '.' || istr2[0] == ':') ?
                                                TYPE_STRUCT : TYPE_ARRAY;
                // [index] = val   or   .member = val   or   member: val
                // ^^^^^^^^^^^^^        ^^^^^^^^^^^^^        ^^^^^^^^^^^
                if (l->type == TYPE_IRRELEVANT) {
                    l->type = exp_type;
                } else if (l->type != exp_type) {
                    fprintf(stderr, "Mixed struct/array!\n");
                    exit(1);
                }
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
            sai->index = l->n_entries ? l->entries[l->n_entries - 1].index + 1 : 0;
            sai->expression_offset.start = get_token_offset(tokens[0]);
            sai->expression_offset.end   = get_token_offset(tokens[n_tokens - 2]);
            if (!strcmp(istr, ".")) {
                sai->value_offset.start = get_token_offset(tokens[3]);
            } else if (!strcmp(istr2, ":")) {
                sai->value_offset.start = get_token_offset(tokens[2]);
            } else if (!strcmp(istr, "[")) {
                unsigned n;
                for (n = 2; n < n_tokens - 2; n++) {
                    CXString spelling = clang_getTokenSpelling(TU, tokens[n]);
                    int res = strcmp(clang_getCString(spelling), "]");
                    clang_disposeString(spelling);
                    if (!res)
                        break;
                }
                assert(n < n_tokens - 2);
                sai->value_offset.start = get_token_offset(tokens[n + 2]);
            } else {
                sai->value_offset.start = get_token_offset(tokens[0]);
            }
            sai->value_offset.end   = get_token_offset(tokens[n_tokens - 2]);
            rec.data.sal_idx = rec.parent->data.sal_idx;
            clang_visitChildren(cursor, callback, &rec);
            assert(index_is_unique(&struct_array_lists[rec.parent->data.sal_idx],
                                   sai->index));
            struct_array_lists[rec.parent->data.sal_idx].n_entries++;
            clang_disposeString(spelling);
            clang_disposeString(spelling2);
        } else {
            clang_visitChildren(cursor, callback, &rec);
        }
        break;
    case CXCursor_MemberRef:
        if (parent.kind == CXCursor_UnexposedExpr &&
            rec.parent->parent->kind == CXCursor_InitListExpr) {
            // designated initializer (struct)
            // .member = val
            //  ^^^^^^
            StructArrayList *l = &struct_array_lists[rec.parent->data.sal_idx];
            StructArrayItem *sai = &l->entries[l->n_entries];
            const char *member = clang_getCString(str);

            assert(sai);
            assert(l->type == TYPE_STRUCT);
            assert(l->struct_decl_idx != (unsigned) -1);
            sai->index = find_member_index_in_struct(&structs[l->struct_decl_idx],
                                                     member);
            if (structs[l->struct_decl_idx].is_union && is_in_function)
                l->convert_to_assignment = 1;
        }
        break;
    case CXCursor_CompoundStmt:
        rec.allow_var_decls = 1;
        clang_visitChildren(cursor, callback, &rec);
        if (rec.end_scopes) {
            EndScope *e;
            if (n_end_scopes == n_allocated_end_scopes) {
                unsigned num = n_allocated_end_scopes + 16;
                void *mem = realloc(end_scopes,
                                    sizeof(*end_scopes) * num);
                if (!mem) {
                    fprintf(stderr, "Failed to allocate memory for str/arr\n");
                    exit(1);
                }
                end_scopes = (EndScope *) mem;
                n_allocated_end_scopes = num;
            }
            e = &end_scopes[n_end_scopes++];
            e->end = get_token_offset(tokens[n_tokens - 2]);
            e->n_scopes = rec.end_scopes;
        }
        break;
    case CXCursor_IntegerLiteral:
    case CXCursor_DeclRefExpr:
    case CXCursor_BinaryOperator:
        if (parent.kind == CXCursor_UnexposedExpr &&
            rec.parent->parent->kind == CXCursor_InitListExpr) {
            CXString spelling = clang_getTokenSpelling(TU, tokens[n_tokens - 1]);
            if (!strcmp(clang_getCString(spelling), "]")) {
                // [index] = { val }
                //  ^^^^^
                FillEnumMemberCache cache;
                StructArrayList *l = &struct_array_lists[rec.parent->data.sal_idx];
                StructArrayItem *sai = &l->entries[l->n_entries];

                memset(&cache, 0, sizeof(cache));
                fill_enum_value(cursor, parent, &cache);
                assert(cache.n[0] == 1);
                assert(sai);
                assert(l->type == TYPE_ARRAY);
                sai->index = cache.n[1];
            }
            clang_disposeString(spelling);
        } else if (cursor.kind != CXCursor_BinaryOperator)
            break;
    default:
        clang_visitChildren(cursor, callback, &rec);
        break;
    }

    // default list filler for scalar (non-list) value types
    if (rec.parent->kind == CXCursor_InitListExpr &&
        cursor.kind != CXCursor_InitListExpr &&
        cursor.kind != CXCursor_UnexposedExpr) {
        unsigned s = get_token_offset(tokens[0]);
        StructArrayItem *sai;
        StructArrayList *parent = &struct_array_lists[rec.parent->data.sal_idx];

        if (parent != NULL) {
            if (parent->n_entries == parent->n_allocated_entries) {
                unsigned num = parent->n_allocated_entries + 16;
                void *mem = realloc(parent->entries,
                                    sizeof(*parent->entries) * num);
                if (!mem) {
                    fprintf(stderr, "Failed to allocate str/arr entry mem\n");
                    exit(1);
                }
                parent->entries = (StructArrayItem *) mem;
                parent->n_allocated_entries = num;
            }

            sai = &parent->entries[parent->n_entries];
            sai->value_offset.start = s;
            sai->value_offset.end   = s;
            sai->expression_offset.start = s;
            sai->expression_offset.end   = s;
            sai->index = parent->n_entries > 0 ?
                         parent->entries[parent->n_entries - 1].index + 1 :
                         rec.parent->child_cntr - 1;
            assert(index_is_unique(parent, sai->index));
            parent->n_entries++;
        }
    }

    clang_disposeString(str);
    clang_disposeTokens(TU, tokens, n_tokens);
    clang_disposeString(filename);

    return CXChildVisit_Continue;
}

static double eval_expr(CXToken *tokens, unsigned *n, unsigned last);

static double eval_prim(CXToken *tokens, unsigned *n, unsigned last)
{
    CXString s;
    const char *str;
    if (*n > last) {
        fprintf(stderr, "Unable to parse an expression primary, no more tokens\n");
        exit(1);
    }
    s = clang_getTokenSpelling(TU, tokens[*n]);
    str = clang_getCString(s);
    if (!strcmp(str, "-")) {
        (*n)++;
        clang_disposeString(s);
        return -eval_prim(tokens, n, last);
    } else if (!strcmp(str, "(")) {
        double d;
        (*n)++;
        clang_disposeString(s);
        if (*n + 1 <= last) {
            CXString s2;
            const char *str2;
            s = clang_getTokenSpelling(TU, tokens[*n]);
            str = clang_getCString(s);
            s2 = clang_getTokenSpelling(TU, tokens[*n + 1]);
            str2 = clang_getCString(s2);
            // This should ideally recognize all built-in types
            // and also check the type name against all typedefs
            // (it also doesn't support two word typnames such as structs.
            // This is enough for handling double casts in DBL_MAX in
            // certain glibc versions though.
            if (!strcmp(str2, ")") && !strcmp(str, "double")) {
                clang_disposeString(s);
                clang_disposeString(s2);
                (*n) += 2;
                return eval_prim(tokens, n, last);
            }
            clang_disposeString(s);
            clang_disposeString(s2);
        }
        d = eval_expr(tokens, n, last);
        if (*n > last) {
            fprintf(stderr, "No right parenthesis found\n");
            exit(1);
        }
        s = clang_getTokenSpelling(TU, tokens[*n]);
        str = clang_getCString(s);
        if (!strcmp(str, ")")) {
            clang_disposeString(s);
            (*n)++;
        } else {
            fprintf(stderr, "No right parenthesis found\n");
            exit(1);
        }
        return d;
    } else {
        char *end;
        double d;
        if (str[0] == '0' && (str[1] == 'x' || str[1] == 'X')) {
            d = strtoll(str, &end, 16);
        } else {
            d = strtod(str, &end);
        }
        // Handle a possible f suffix for float constants
        if (end != str && (*end == 'f' || *end == 'F'))
            end++;
        // Handle a possible l suffix for int constants
        while (end != str && (*end == 'l' || *end == 'L'))
            end++;
        if (*end != '\0') {
            fprintf(stderr, "Unable to parse %s as expression primary\n", str);
            exit(1);
        }
        (*n)++;
        clang_disposeString(s);
        return d;
    }
}

static double eval_term(CXToken *tokens, unsigned *n, unsigned last)
{
    double left = eval_prim(tokens, n, last);
    while (*n <= last) {
        CXString s = clang_getTokenSpelling(TU, tokens[*n]);
        const char *str = clang_getCString(s);
        if (!strcmp(str, "*")) {
            (*n)++;
            left *= eval_prim(tokens, n, last);
        } else if (!strcmp(str, "/")) {
            (*n)++;
            left /= eval_prim(tokens, n, last);
        } else {
            clang_disposeString(s);
            return left;
        }
        clang_disposeString(s);
    }
    return left;
}

static double eval_expr(CXToken *tokens, unsigned *n, unsigned last)
{
    double left = eval_term(tokens, n, last);
    while (*n <= last) {
        CXString s = clang_getTokenSpelling(TU, tokens[*n]);
        const char *str = clang_getCString(s);
        if (!strcmp(str, "-")) {
            (*n)++;
            left -= eval_term(tokens, n, last);
        } else if (!strcmp(str, "+")) {
            (*n)++;
            left += eval_term(tokens, n, last);
        } else {
            clang_disposeString(s);
            return left;
        }
        clang_disposeString(s);
    }
    return left;
}

static double eval_tokens(CXToken *tokens, unsigned first, unsigned last)
{
    unsigned n = first;
    double d = eval_expr(tokens, &n, last);
    if (n <= last) {
        fprintf(stderr, "Unable to parse tokens as expression\n");
        exit(1);
    }
    return d;
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
        fprintf(out, "\n");
    for (; *pos < p; (*pos)++)
        fprintf(out, " ");
}

static void print_literal_text(const char *str, unsigned *lnum,
                               unsigned *pos)
{
    fprintf(out, "%s", str);
    (*pos) += strlen(str);
}

static void print_token(CXToken token, unsigned *lnum,
                        unsigned *pos)
{
    CXString s = clang_getTokenSpelling(TU, token);
    print_literal_text(clang_getCString(s), lnum, pos);
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
            return n;
        } else if (struct_array_lists[n].level == level) {
            if (cnt++ == index)
                return n;
        }
    }

    return n_struct_array_lists;
}

static void reorder_compound_literal_list(unsigned n)
{
    if (!n_comp_literal_lists)
        return;

    // FIXME probably slow - quicksort?
    for (; n < n_comp_literal_lists - 1; n++) {
        unsigned l, lowest = n;

        // find the lowest
        for (l = n + 1; l < n_comp_literal_lists; l++) {
            if (comp_literal_lists[l].context.start <
                comp_literal_lists[lowest].context.start) {
                lowest = l;
            }
        }

        // move it in place
        if (lowest != n) {
            CompoundLiteralList bak = comp_literal_lists[lowest];
            memmove(&comp_literal_lists[n + 1], &comp_literal_lists[n],
                    sizeof(comp_literal_lists[0]) * (lowest - n));
            comp_literal_lists[n] = bak;
        }
    }
}

static void print_token_wrapper(CXToken *tokens, unsigned n_tokens,
                                unsigned *n, unsigned *lnum, unsigned *cpos,
                                unsigned *saidx, unsigned *clidx, unsigned *esidx,
                                unsigned off);

static void declare_variable(CompoundLiteralList *l, unsigned cur_tok_off,
                             unsigned *clidx, unsigned *_saidx, unsigned *esidx,
                             CXToken *tokens, unsigned n_tokens,
                             const char *var_name, unsigned *lnum,
                             unsigned *cpos)
{
    unsigned idx1, idx2, off, n, saidx = *_saidx;

    /* type information, e.g. 'int' or 'struct AVRational' */
    idx1 = find_token_for_offset(tokens, n_tokens, cur_tok_off,
                                 l->cast_token.start);
    idx2 = find_token_for_offset(tokens, n_tokens, cur_tok_off,
                                 l->cast_token_array_start);
    get_token_position(tokens[idx1 + 1], lnum, cpos, &off);
    for (n = idx1 + 1; n <= idx2 - 1; n++) {
        indent_for_token(tokens[n], lnum, cpos, &off);
        print_token(tokens[n], lnum, cpos);
    }

    /* variable name and array tokens, e.g. 'tmp[]' */
    print_literal_text(" ", lnum, cpos);
    print_literal_text(var_name, lnum, cpos);
    idx1 = find_token_for_offset(tokens, n_tokens, cur_tok_off,
                                 l->cast_token.end);
    for (n = idx2; n <= idx1 - 1; n++) {
        indent_for_token(tokens[n], lnum, cpos, &off);
        print_token(tokens[n], lnum, cpos);
    }
    print_literal_text(" = ", lnum, cpos);

    /* value */
    idx1 = find_token_for_offset(tokens, n_tokens, cur_tok_off,
                                 l->value_token.start);
    idx2 = find_token_for_offset(tokens, n_tokens, cur_tok_off,
                                 l->value_token.end);
    get_token_position(tokens[idx1], lnum, cpos, &off);
    while (saidx < n_struct_array_lists &&
           struct_array_lists[saidx].value_offset.start < off)
        saidx++;
    for (n = idx1; n <= idx2; n++) {
        indent_for_token(tokens[n], lnum, cpos, &off);
        print_token_wrapper(tokens, n_tokens, &n, lnum, cpos,
                            &saidx, clidx, esidx, off);
    }
}

static void replace_comp_literal(CompoundLiteralList *l,
                                 unsigned *clidx, unsigned *saidx, unsigned *esidx,
                                 unsigned *lnum, unsigned *cpos, unsigned *_n,
                                 CXToken *tokens, unsigned n_tokens)
{
    static unsigned unique_cntr = 0;

    if (l->type == TYPE_OMIT_CAST) {
        unsigned off;

        *_n = find_token_for_offset(tokens, n_tokens, *_n,
                                    l->cast_token.end);
        get_token_position(tokens[*_n + 1], lnum, cpos, &off);
        (*clidx)++;
    } else if (l->type == TYPE_TEMP_ASSIGN) {
        if (l->context.start < l->cast_token.start) {
            unsigned off;
            char tmp[256];

            // open a new context, so we can declare a new variable
            print_literal_text("{ ", lnum, cpos);
            snprintf(tmp, sizeof(tmp), "tmp__%u", unique_cntr++);
            l->data.t_c_d.tmp_var_name = strdup(tmp);
            declare_variable(l, *_n, clidx, saidx, esidx,
                             tokens, n_tokens, tmp, lnum, cpos);
            print_literal_text("; ", lnum, cpos);

            // re-insert in list now for replacement of the variable
            // reference (instead of the actual CL)
            l->context.start = l->cast_token.start;
            reorder_compound_literal_list(l - comp_literal_lists);
            get_token_position(tokens[*_n], lnum, cpos, &off);
            (*_n)--;
        } else if (l->context.start == l->cast_token.start) {
            // FIXME duplicate of code in TYPE_CONST_DECL
            unsigned off;
            char *tmp_var_name = l->data.t_c_d.tmp_var_name;

            // replace original CL with a reference to the
            // newly declared static const variable
            print_literal_text(tmp_var_name, lnum, cpos);
            l->data.t_c_d.tmp_var_name = NULL;
            free(tmp_var_name);
            *_n = find_token_for_offset(tokens, n_tokens, *_n,
                                        l->value_token.end);
            get_token_position(tokens[*_n + 1], lnum, cpos, &off);
            l->context.start = l->context.end;
            reorder_compound_literal_list(l - comp_literal_lists);
        } else {
            print_token(tokens[*_n], lnum, cpos);

            {
                // Bugfix. Consider preprocessor directives, don't insert closing }
                // at the end of the line if it doesn't end with ";" or "}".
                unsigned tok_lnum = *lnum;
                unsigned tok_pos = *cpos;
                unsigned off;
                get_token_position(tokens[*_n + 1], &tok_lnum, &tok_pos, &off);
                if (tok_lnum > *lnum)
                {
                    // Get previous token spelling.
                    CXString s = clang_getTokenSpelling(TU, tokens[*_n]);
                    const char * spelling = clang_getCString(s);
                    if (strcmp(spelling, ";") && strcmp(spelling, "}"))
                    {
                        print_literal_text("\n", lnum, cpos);
                        (*lnum)++;
                        *cpos = 0;
                    }
                    clang_disposeString(s);
                }
            }

            // multiple contexts may want to close here - close all at once
            do {
                print_literal_text(" }", lnum, cpos);
                (*clidx)++;
            } while (*clidx < n_comp_literal_lists &&
                     comp_literal_lists[*clidx].context.start == l->context.start);
        }
    } else if (l->type == TYPE_CONST_DECL) {
        if (l->context.start < l->cast_token.start) {
            unsigned off;
            char tmp[256];

            // declare static const variable
            print_literal_text("static ", lnum, cpos);
            snprintf(tmp, sizeof(tmp), "tmp__%u", unique_cntr++);
            l->data.t_c_d.tmp_var_name = strdup(tmp);
            declare_variable(l, *_n, clidx, saidx, esidx,
                             tokens, n_tokens, tmp, lnum, cpos);
            print_literal_text(";", lnum, cpos);

            // re-insert in list now for replacement of the variable
            // reference (instead of the actual CL)
            l->context.start = l->cast_token.start;
            reorder_compound_literal_list(l - comp_literal_lists);
            (*_n)--;
            get_token_position(tokens[*_n], lnum, cpos, &off);
        } else {
            // FIXME duplicate of code in TYPE_TEMP_ASSIGN
            unsigned off;
            char *tmp_var_name = l->data.t_c_d.tmp_var_name;

            // replace original CL with a reference to the
            // newly declared static const variable
            print_literal_text(tmp_var_name, lnum, cpos);
            l->data.t_c_d.tmp_var_name = NULL;
            free(tmp_var_name);
            *_n = find_token_for_offset(tokens, n_tokens, *_n,
                                        l->value_token.end);
            get_token_position(tokens[*_n + 1], lnum, cpos, &off);
            (*clidx)++;
        }
    } else if (l->type == TYPE_NEW_CONTEXT) {
        if (l->context.start == l->cast_token.start) {
            unsigned off;

            print_literal_text("{ ", lnum, cpos);

            // FIXME it may be easier to replicate the variable declaration
            // and initialization here, and then to actually empty out the
            // original location where the variable initialization/declaration
            // happened
            l->context.start = l->context.end;
            l->type = TYPE_TEMP_ASSIGN;
            reorder_compound_literal_list(l - comp_literal_lists);
            get_token_position(tokens[*_n], lnum, cpos, &off);
            (*_n)--;
        }
    } else if (l->type == TYPE_LOOP_CONTEXT) {
        if (l->context.start < l->cast_token.start) {
            unsigned off, idx1, idx2, n;

            // add variable declaration/init, add terminating ';'
            print_literal_text("{ ", lnum, cpos);
            l->context.start = l->cast_token.start;
            idx1 = find_token_for_offset(tokens, n_tokens, *_n,
                                         l->cast_token.start);
            idx2 = find_token_for_offset(tokens, n_tokens, *_n,
                                         l->cast_token.end);
            get_token_position(tokens[idx1], lnum, cpos, &off);
            for (n = idx1; n <= idx2; n++) {
                indent_for_token(tokens[n], lnum, cpos, &off);
                print_token(tokens[n], lnum, cpos);
            }
            print_literal_text("; ", lnum, cpos);
            get_token_position(tokens[*_n], lnum, cpos, &off);
            (*_n)--;
        } else if (l->context.start == l->cast_token.start) {
            unsigned off;

            // remove variable declaration/init, remove ',' if present
            l->context.start = l->context.end;
            l->type = TYPE_TEMP_ASSIGN;
            (*_n)--;
            do {
                (*_n)++;
                get_token_position(tokens[*_n], lnum, cpos, &off);
            } while (off < l->cast_token.end);
            reorder_compound_literal_list(l - comp_literal_lists);
        }
    }
}

static void replace_struct_array(unsigned *_saidx, unsigned *_clidx, unsigned *esidx,
                                 unsigned *lnum, unsigned *cpos, unsigned *_n,
                                 CXToken *tokens, unsigned n_tokens)
{
    unsigned saidx = *_saidx, off, i, n = *_n, j;
    StructArrayList *sal = &struct_array_lists[saidx];
    StructDeclaration *decl = sal->struct_decl_idx != (unsigned) -1 ?
                              &structs[sal->struct_decl_idx] : NULL;
    int is_union = decl ? decl->is_union : 0;

    if (sal->convert_to_assignment) {
        CXString spelling;
        print_literal_text(";", lnum, cpos);
        for (i = 0; i < sal->n_entries; i++) {
            StructArrayItem *sai = &sal->entries[i];
            unsigned token_start = find_token_for_offset(tokens, n_tokens, *_n, sai->value_offset.start);
            unsigned token_end   = find_token_for_offset(tokens, n_tokens, *_n, sai->value_offset.end);
            unsigned saidx2 = 0;

            print_literal_text(sal->name, lnum, cpos);
            print_literal_text(".", lnum, cpos);
            print_literal_text(structs[sal->struct_decl_idx].entries[sai->index].name, lnum, cpos);
            print_literal_text("=", lnum, cpos);
            get_token_position(tokens[token_start], lnum, cpos, &off);
            for (n = token_start; n <= token_end; n++)
                print_token_wrapper(tokens, n_tokens, &n, lnum, cpos,
                                    &saidx2, _clidx, esidx, off);
            print_literal_text(";", lnum, cpos);
        }
        n = find_token_for_offset(tokens, n_tokens, *_n,
                                  struct_array_lists[saidx].value_offset.end);
        *_n = n;
        print_literal_text("{", lnum, cpos);

        // adjust token index and position back
        get_token_position(tokens[n], lnum, cpos, &off);
        spelling = clang_getTokenSpelling(TU, tokens[n]);
        (*cpos) += strlen(clang_getCString(spelling));
        clang_disposeString(spelling);
        return;
    }

    // we assume here the indenting for the first opening token,
    // i.e. the '{', is already taken care of
    print_token(tokens[n++], lnum, cpos);
    indent_for_token(tokens[n], lnum, cpos, &off);

    for (i = 0; i < struct_array_lists[saidx].n_entries; i++)
      assert(struct_array_lists[saidx].entries[i].index != (unsigned) -1);

    for (j = 0, i = 0; i < struct_array_lists[saidx].n_entries; j++) {
        unsigned expr_off_s, expr_off_e, val_idx, val_off_s, val_off_e, saidx2,
                 indent_token_end, next_indent_token_start, val_token_start,
                 val_token_end;
        int print_normal = 1;
        CXString spelling;
        StructMember *member = decl ? &decl->entries[j] : NULL;

        val_idx = find_value_index(&struct_array_lists[saidx], j);

        assert(struct_array_lists[saidx].array_depth > 0 ||
               j < structs[struct_array_lists[saidx].struct_decl_idx].n_entries);
        if (val_idx == (unsigned) -1) {
            unsigned depth = struct_array_lists[saidx].array_depth;
            unsigned idx = struct_array_lists[saidx].struct_decl_idx;
            if (is_union) // Don't print the filler zeros for unions
                continue;
            if (depth > 1) {
                print_literal_text("{ 0 }", lnum, cpos);
            } else if (depth == 1) {
                if (idx != (unsigned) -1) {
                    print_literal_text("{ 0 }", lnum, cpos);
                } else {
                    print_literal_text("0", lnum, cpos);
                }
            } else if ((structs[idx].entries[j].struct_decl_idx != (unsigned) -1 &&
                        structs[idx].entries[j].n_ptrs == 0) ||
                       structs[idx].entries[j].array_depth) {
                print_literal_text("{ 0 }", lnum, cpos);
            } else {
                print_literal_text("0", lnum, cpos);
            }
            print_literal_text(", ", lnum, cpos);
            continue; // gap
        }

        expr_off_e = struct_array_lists[saidx].entries[i].expression_offset.end;
        next_indent_token_start = find_token_for_offset(tokens, n_tokens, *_n,
                                                        expr_off_e);
        val_off_s = struct_array_lists[saidx].entries[val_idx].value_offset.start;
        val_token_start = find_token_for_offset(tokens, n_tokens, *_n, val_off_s);
        val_off_e = struct_array_lists[saidx].entries[val_idx].value_offset.end;
        val_token_end = find_token_for_offset(tokens, n_tokens, *_n, val_off_e);
        saidx2 = find_index_for_level(struct_array_lists[saidx].level + 1,
                                      val_idx, saidx + 1);
        if (saidx2 < n_struct_array_lists &&
            struct_array_lists[saidx2].level < struct_array_lists[saidx].level)
            saidx2 = n_struct_array_lists;

        // adjust position
        get_token_position(tokens[val_token_start], lnum, cpos, &off);

        if (is_union && j != 0) {
            StructMember *first_member = &decl->entries[0];
            if ((!strcmp(first_member->type, "double") ||
                 !strcmp(first_member->type, "float")) && !first_member->n_ptrs) {
                fprintf(stderr, "Can't convert type %s to %s for union\n",
                        member->type, first_member->type);
                exit(1);
            }
            if (first_member->n_ptrs)
                print_literal_text("(void*) ", lnum, cpos);
            if (member->n_ptrs)
                print_literal_text("(intptr_t) ", lnum, cpos);

            if ((!strcmp(member->type, "double") ||
                 !strcmp(member->type, "float")) && !member->n_ptrs) {
                // Convert a literal floating pointer number (not a pointer to
                // one of them) to its binary representation
                union {
                    uint64_t i;
                    double f;
                } if64;
                char buf[20];
                if64.f = eval_tokens(tokens, val_token_start, val_token_end);
                if (!strcmp(member->type, "float")) {
                    union {
                        uint32_t i;
                        float f;
                    } if32;
                    if32.f = if64.f;
                    if64.i = if32.i;
                }
                snprintf(buf, sizeof(buf), "%#"PRIx64, if64.i);
                print_literal_text(buf, lnum, cpos);
                print_normal = 0;
            }
        }
        // print values out of order
        for (n = val_token_start; n <= val_token_end && print_normal; n++) {
            print_token_wrapper(tokens, n_tokens, &n, lnum, cpos,
                                &saidx2, _clidx, esidx, off);
            if (n != val_token_end)
                indent_for_token(tokens[n + 1], lnum, cpos, &off);
        }

        // adjust token index and position back
        n = next_indent_token_start;
        get_token_position(tokens[n], lnum, cpos, &off);
        spelling = clang_getTokenSpelling(TU, tokens[n]);
        (*cpos) += strlen(clang_getCString(spelling));
        clang_disposeString(spelling);
        n++;

        if (++i < struct_array_lists[saidx].n_entries) {
            expr_off_s = struct_array_lists[saidx].entries[i].expression_offset.start;
            indent_token_end = find_token_for_offset(tokens, n_tokens, *_n, expr_off_s);
        } else {
            indent_token_end = find_token_for_offset(tokens, n_tokens, *_n,
                                                     struct_array_lists[saidx].value_offset.end);
        }

        if (is_union) // Unions should be initialized by only one element
            break;

        if (n < indent_token_end)
            indent_for_token(tokens[n], lnum, cpos, &off);
        for (; n < indent_token_end; n++) {
            print_token(tokens[n], lnum, cpos);
            indent_for_token(tokens[n + 1], lnum, cpos, &off);
        }
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

static void print_token_wrapper(CXToken *tokens, unsigned n_tokens,
                                unsigned *n, unsigned *lnum, unsigned *cpos,
                                unsigned *saidx, unsigned *clidx, unsigned *esidx,
                                unsigned off)
{
    *saidx = 0;
    while (*saidx < n_struct_array_lists &&
           struct_array_lists[*saidx].value_offset.start < off)
        (*saidx)++;
    *clidx = 0;
    while (*clidx < n_comp_literal_lists &&
           (comp_literal_lists[*clidx].type == TYPE_UNKNOWN ||
            comp_literal_lists[*clidx].context.start < off))
        (*clidx)++;

    if (*saidx < n_struct_array_lists &&
        off == struct_array_lists[*saidx].value_offset.start) {
        if (struct_array_lists[*saidx].type == TYPE_IRRELEVANT ||
            struct_array_lists[*saidx].n_entries == 0) {
            (*saidx)++;
            print_token(tokens[*n], lnum, cpos);
        } else {
            replace_struct_array(saidx, clidx, esidx, lnum, cpos, n,
                                 tokens, n_tokens);
        }
    } else if (*clidx < n_comp_literal_lists &&
               off == comp_literal_lists[*clidx].context.start) {
        if (comp_literal_lists[*clidx].type == TYPE_UNKNOWN) {
            print_token(tokens[*n], lnum, cpos);
        } else {
            replace_comp_literal(&comp_literal_lists[*clidx],
                                 clidx, saidx, esidx, lnum, cpos, n,
                                 tokens, n_tokens);
        }
        while (*clidx < n_comp_literal_lists &&
               comp_literal_lists[*clidx].type == TYPE_UNKNOWN)
            (*clidx)++;
    } else {
        print_token(tokens[*n], lnum, cpos);
    }

    while (*esidx < n_end_scopes && off >= end_scopes[*esidx].end - 1) {
        int i;
        for (i = 0; i < end_scopes[*esidx].n_scopes; i++)
            print_literal_text("}", lnum, cpos);
        (*cpos) -= end_scopes[*esidx].n_scopes;
        (*esidx)++;
    }
}

static void print_tokens(CXToken *tokens, unsigned n_tokens)
{
    unsigned cpos = 0, lnum = 0, n, saidx = 0, clidx = 0, esidx = 0, off;

    reorder_compound_literal_list(0);

    for (n = 0; n < n_tokens; n++) {
        indent_for_token(tokens[n], &lnum, &cpos, &off);
        print_token_wrapper(tokens, n_tokens, &n,
                            &lnum, &cpos, &saidx, &clidx, &esidx, off);
    }

    // each file ends with a newline
    fprintf(out, "\n");
}

static void cleanup(void)
{
    unsigned n, m;

#define DEBUG 0
    dprintf("N compound literals: %d\n", n_comp_literal_lists);
    for (n = 0; n < n_comp_literal_lists; n++) {
        dprintf("[%d]: type=%d, struct=%d (%s), variable range=%u-%u\n",
                n, comp_literal_lists[n].type,
                comp_literal_lists[n].struct_decl_idx,
                comp_literal_lists[n].struct_decl_idx != (unsigned) -1 ?
                    structs[comp_literal_lists[n].struct_decl_idx].name : "<none>",
                comp_literal_lists[n].value_token.start,
                comp_literal_lists[n].value_token.end);
    }
    free(comp_literal_lists);

    dprintf("N array/struct variables: %d\n", n_struct_array_lists);
    for (n = 0; n < n_struct_array_lists; n++) {
        dprintf("[%d]: type=%d, struct=%d (%s), level=%d, n_entries=%d, range=%u-%u, depth=%u\n",
                n, struct_array_lists[n].type,
                struct_array_lists[n].struct_decl_idx,
                struct_array_lists[n].struct_decl_idx != (unsigned) -1 ?
                    (structs[struct_array_lists[n].struct_decl_idx].name[0] ?
                     structs[struct_array_lists[n].struct_decl_idx].name :
                     "<anonymous>") : "<none>",
                struct_array_lists[n].level,
                struct_array_lists[n].n_entries,
                struct_array_lists[n].value_offset.start,
                struct_array_lists[n].value_offset.end,
                struct_array_lists[n].array_depth);
        for (m = 0; m < struct_array_lists[n].n_entries; m++) {
            dprintf(" [%d]: idx=%d, range=%u-%u\n",
                    m, struct_array_lists[n].entries[m].index,
                    struct_array_lists[n].entries[m].value_offset.start,
                    struct_array_lists[n].entries[m].value_offset.end);
        }
        free(struct_array_lists[n].entries);
        free(struct_array_lists[n].name);
    }
    free(struct_array_lists);

    dprintf("N extra scope ends: %d\n", n_end_scopes);
    for (n = 0; n < n_end_scopes; n++) {
        dprintf("[%d]: end=%u n_scopes=%u\n",
                n, end_scopes[n].end, end_scopes[n].n_scopes);
    }
    free(end_scopes);

    dprintf("N typedef entries: %d\n", n_typedefs);
    for (n = 0; n < n_typedefs; n++) {
        if (typedefs[n].struct_decl_idx != (unsigned) -1) {
            if (structs[typedefs[n].struct_decl_idx].name[0]) {
                dprintf("[%d]: %s (struct %s = %d)\n",
                        n, typedefs[n].name,
                        structs[typedefs[n].struct_decl_idx].name,
                        typedefs[n].struct_decl_idx);
            } else {
                dprintf("[%d]: %s (<anonymous> struct = %d)\n",
                        n, typedefs[n].name,
                        typedefs[n].struct_decl_idx);
            }
        } else if (typedefs[n].enum_decl_idx != (unsigned) -1) {
            if (enums[typedefs[n].enum_decl_idx].name[0]) {
                dprintf("[%d]: %s (enum %s = %d)\n",
                        n, typedefs[n].name,
                        enums[typedefs[n].enum_decl_idx].name,
                        typedefs[n].enum_decl_idx);
            } else {
                dprintf("[%d]: %s (<anonymous> enum = %d)\n",
                        n, typedefs[n].name,
                        typedefs[n].enum_decl_idx);
            }
        } else {
            dprintf("[%d]: %s (%s)\n",
                    n, typedefs[n].name, typedefs[n].proxy);
        }
        if (typedefs[n].proxy)
            free(typedefs[n].proxy);
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
            dprintf(" [%d]: %s (%s/%d/%d/%u)\n",
                    m, structs[n].entries[m].name,
                    structs[n].entries[m].type,
                    structs[n].entries[m].n_ptrs,
                    structs[n].entries[m].array_depth,
                    structs[n].entries[m].struct_decl_idx);
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

int convert(const char *infile, const char *outfile, int ms_compat)
{
    CXIndex index;
    unsigned n_tokens;
    CXToken *tokens;
    CXSourceRange range;
    CXCursor cursor;
    CursorRecursion rec;
    const char *ms_argv[] = { "-fms-extensions", "-target", "i386-pc-win32", NULL };
    const char **argv = NULL;
    int argc = 0;
    if (ms_compat) {
        argv = ms_argv;
        argc = 3;
    }

    out    = fopen(outfile, "w");
    if (!out) {
        fprintf(stderr, "Unable to open output file %s\n", outfile);
        return 1;
    }
    index  = clang_createIndex(1, 1);
    TU     = clang_createTranslationUnitFromSourceFile(index, infile, argc,
                                                       argv, 0, NULL);
    cursor = clang_getTranslationUnitCursor(TU);
    range  = clang_getCursorExtent(cursor);
    clang_tokenize(TU, range, &tokens, &n_tokens);

    memset(&rec, 0, sizeof(rec));
    rec.tokens = tokens;
    rec.n_tokens = n_tokens;
    rec.kind = CXCursor_TranslationUnit;
    clang_visitChildren(cursor, callback, &rec);
    print_tokens(tokens, n_tokens);
    clang_disposeTokens(TU, tokens, n_tokens);

    clang_disposeTranslationUnit(TU);
    clang_disposeIndex(index);

    cleanup();
    fclose(out);

    return 0;
}

int main(int argc, char *argv[])
{
    int arg = 1;
    int ms_compat = 0;
    while (arg < argc) {
        if (!strcmp(argv[arg], "-ms")) {
            ms_compat = 1;
        } else {
            break;
        }
        arg++;
    }
    if (argc < arg + 2) {
        fprintf(stderr, "%s [-ms] <in> <out>\n", argv[0]);
        return 1;
    }
    return convert(argv[arg], argv[arg + 1], ms_compat);
}
