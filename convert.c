extern "C" {
#include <stdio.h>
#include <Index.h>
#include <string.h>
}

#include <string>
#include <queue>

using namespace std;

queue<string> my_queue;
bool next = false;
bool have_struct = false;

enum CXChildVisitResult callback(CXCursor cursor, CXCursor parent,
CXClientData client_data) {

    CXString str;
    string cppstr;

    if (cursor.kind != CXCursor_FieldDecl && parent.kind == CXCursor_StructDecl)
        return CXChildVisit_Continue;

    if (cursor.kind == CXCursor_TypedefDecl) {
        if (next && !have_struct) {
            str = clang_getCursorSpelling(cursor);
            if (!my_queue.empty())
                printf("\nNAME: %s\n", clang_getCString(str));
            while (!my_queue.empty()) {
                cppstr = my_queue.front();
                printf("FIELD_NAME: %s\n", cppstr.c_str());
                my_queue.pop();
            }
            clang_disposeString(str);
            next = false;
        }
    } else if (cursor.kind == CXCursor_StructDecl) {
        str = clang_getCursorSpelling (cursor);
        if (strlen(clang_getCString(str))) {
            printf("\nNAME: %s\n", clang_getCString(str));
            have_struct = true;
        } else {
            have_struct = false;
        }

        clang_disposeString(str);

        return CXChildVisit_Recurse;
    } else if (cursor.kind == CXCursor_FieldDecl && parent.kind == CXCursor_StructDecl) {
        str = clang_getCursorSpelling (cursor);

        if (have_struct) {
           printf("FIELD_NAME: %s\n", clang_getCString(str));
        } else {
            cppstr.assign(clang_getCString(str));
            my_queue.push(cppstr);
        }

        clang_disposeString(str);

        next = true;

        return CXChildVisit_Continue;
    } else {
//        printf("DERP: %d %d\n", cursor.kind, CXCursor_VarDecl);
    }

    return CXChildVisit_Continue;
}

int main() {
    CXIndex index;
    CXTranslationUnit TU;
    char* args[] = { "-I/home/derekb/dev/conv/c_bin/lib/clang/3.1/include/" };

    index = clang_createIndex(1, 1);
    TU    = clang_createTranslationUnitFromSourceFile(index, "proresdsp.h", 1, args, 0, NULL);

    clang_visitChildren(clang_getTranslationUnitCursor(TU), callback, 0);

    clang_disposeTranslationUnit(TU);
    clang_disposeIndex(index);
    return 0;
}
