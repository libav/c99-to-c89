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

/* FIXME we're not taking pointers or array sizes into account here,
 * in large part because Libav doesn't use those in combination with
 * typedefs. */
typedef struct {
    char *proxy;
    char *name;
    StructDeclaration *decl;
    CXCursor cursor;
} TypedefDeclaration;
static TypedefDeclaration *typedefs = NULL;
static unsigned n_typedefs = 0;
static unsigned n_allocated_typedefs = 0;

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

        clang_disposeString(cstr);
    }

    return CXChildVisit_Continue;
}

static void register_struct(const char *str, CXCursor cursor,
                            StructDeclaration **decl_ptr)
{
    unsigned n;
    StructDeclaration *decl;

    for (n = 0; n < n_structs; n++) {
        if (!strcmp(structs[n].name, str) &&
            memcmp(&cursor, &structs[n].cursor, sizeof(cursor))) {
            /* already exists */
            if (decl_ptr)
                *decl_ptr = &structs[n];
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
        *decl_ptr = decl;
}

static void register_typedef(const char *name,
                             CXToken *tokens, unsigned n_tokens,
                             StructDeclaration *decl, CXCursor cursor)
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
    if (decl) {
        typedefs[n].decl = decl;
    } else {
        typedefs[n].proxy = concat_name(tokens, 1, n_tokens - 3);
    }
    memcpy(&typedefs[n].cursor, &cursor, sizeof(cursor));
}

static enum CXChildVisitResult callback(CXCursor cursor, CXCursor parent,
                                        CXClientData client_data)
{
    enum CXChildVisitResult res = CXChildVisit_Recurse;
    CXString str;
    CXSourceRange range;
    CXToken *tokens = 0;
    unsigned int n_tokens = 0;
    CXSourceLocation pos;
    CXFile file;
    unsigned int line, col, off;
    CXString filename;

    range = clang_getCursorExtent(cursor);
    pos   = clang_getCursorLocation(cursor);
    str   = clang_getCursorSpelling(cursor);
    clang_tokenize(TU, range, &tokens, &n_tokens);
    clang_getSpellingLocation(pos, &file, &line, &col, &off);
    filename = clang_getFileName(file);

    switch (cursor.kind) {
    case CXCursor_TypedefDecl: {
        StructDeclaration *decl = NULL;
        clang_visitChildren(cursor, callback, &decl);
        register_typedef(clang_getCString(str), tokens, n_tokens, decl, cursor);
        break;
    }
    case CXCursor_StructDecl:
        register_struct(clang_getCString(str), cursor,
                        (StructDeclaration **) client_data);
        break;
    case CXCursor_CompoundLiteralExpr:
        dprintf("Compound literal: %s\n", clang_getCString(str));
        for (unsigned int i = 0; i < n_tokens; i++)
        {
            CXString spelling = clang_getTokenSpelling(TU, tokens[i]);
            CXSourceLocation l = clang_getTokenLocation(TU, tokens[i]);
            clang_getSpellingLocation(l, &file, &line, &col, &off);
            dprintf("token = '%s' @ %d:%d\n", clang_getCString(spelling), line, col);
            clang_disposeString(spelling);
        }
        clang_visitChildren(cursor, callback, 0);
        break;
    case CXCursor_TypeRef:
        if (parent.kind == CXCursor_CompoundLiteralExpr) {
            //
        }
        clang_visitChildren(cursor, callback, 0);
        break;
    case CXCursor_InitListExpr:
        if (parent.kind == CXCursor_CompoundLiteralExpr) {
            //
        }
        clang_visitChildren(cursor, callback, 0);
        break;
    default:
#define DEBUG 0
        dprintf("DERP: %d [%d] %s @ %d:%d in %s\n", cursor.kind, parent.kind,
                clang_getCString(str), line, col,
                clang_getCString(filename));
        for (unsigned int i = 0; i < n_tokens; i++)
        {
            CXString spelling = clang_getTokenSpelling(TU, tokens[i]);
            CXSourceLocation l = clang_getTokenLocation(TU, tokens[i]);
            clang_getSpellingLocation(l, &file, &line, &col, &off);
            dprintf("token = '%s' @ %d:%d\n", clang_getCString(spelling), line, col);
            clang_disposeString(spelling);
        }
        clang_visitChildren(cursor, callback, 0);
        break;
#define DEBUG 0
    }

    clang_disposeString(str);
    clang_disposeTokens(TU, tokens, n_tokens);
    clang_disposeString(filename);

    return CXChildVisit_Continue;
}

static void print_tokens(CXToken *tokens, unsigned n_tokens)
{
    unsigned cpos = 0, lnum = 0, n;

    for (n = 0; n < n_tokens; n++) {
        CXToken *tok = &tokens[n];
        CXString spelling = clang_getTokenSpelling(TU, tokens[n]);
        CXSourceLocation l = clang_getTokenLocation(TU, tokens[n]);
        CXFile file;
        unsigned line, col, off;
        const char *cstr;

        // get position of token
        clang_getSpellingLocation(l, &file, &line, &col, &off);
        col--; line--; // somehow counting starts at 1 in clang
        assert(line >= lnum);

        // add newlines where necessary
        for (; lnum < line; lnum++) {
            printf("\n");
            cpos = 0;
        }

        // add indenting where necessary
        for (; cpos < col; cpos++) {
            printf(" ");
        }

        // print token
        cstr = clang_getCString(spelling);
        printf("%s", cstr);
        cpos += strlen(cstr);

        clang_disposeString(spelling);
    }

    // each file ends with a newline
    printf("\n");
}

static void cleanup(void)
{
    unsigned n, m;

    dprintf("N typedef entries: %d\n", n_typedefs);
    for (n = 0; n < n_typedefs; n++) {
        if (typedefs[n].decl) {
            if (typedefs[n].decl->name[0]) {
                dprintf("[%d]: %s (struct %s = %p)\n",
                        n, typedefs[n].name,
                        typedefs[n].decl->name,
                        typedefs[n].decl);
            } else {
                dprintf("[%d]: %s (<anonymous> struct = %p)\n",
                        n, typedefs[n].name,
                        typedefs[n].decl);
            }
        } else {
            dprintf("[%d]: %s (%s = %p)\n",
                    n, typedefs[n].name, typedefs[n].proxy, typedefs[n].decl);
            free(typedefs[n].name);
        }
        free(typedefs[n].proxy);
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
}

int main(int argc, char *argv[])
{
    CXIndex index;
    unsigned i, n_tokens;
    CXToken *tokens;
    CXSourceRange range;
    CXCursor cursor;

    index  = clang_createIndex(1, 1);
    TU     = clang_createTranslationUnitFromSourceFile(index, argv[1], 0,
                                                       NULL, 0, NULL);
    cursor = clang_getTranslationUnitCursor(TU);
    range  = clang_getCursorExtent(cursor);
    clang_tokenize(TU, range, &tokens, &n_tokens);
    for (i = 0; i < n_tokens; i++)
    {
#define DEBUG 0
        CXString spelling = clang_getTokenSpelling(TU, tokens[i]);
        CXSourceLocation l = clang_getTokenLocation(TU, tokens[i]);
        CXFile file;
        unsigned line, col, off;

        clang_getSpellingLocation(l, &file, &line, &col, &off);
        dprintf("token = '%s' @ %d:%d\n", clang_getCString(spelling), line, col);
        clang_disposeString(spelling);
#define DEBUG 0
    }

    clang_visitChildren(cursor, callback, 0);
    print_tokens(tokens, n_tokens);
    clang_disposeTokens(TU, tokens, n_tokens);

    clang_disposeTranslationUnit(TU);
    clang_disposeIndex(index);

    cleanup();

    return 0;
}
