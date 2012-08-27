/*
 * C99-to-MSVC-compatible-C89 syntax converter
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

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#ifdef _WIN32
#include <windows.h>
#define getpid GetCurrentProcessId
#else
#include <unistd.h>
#endif

#define CONVERTER "converter"

static char* create_cmdline(char **argv)
{
    int i, len = 0, pos = 0;
    char *out;
    for (i = 0; argv[i]; i++)
        len += strlen(argv[i]);
    out = malloc(len + 4*i + 10);
    for (i = 0; argv[i]; i++) {
        int contains_space = 0, j;
        for (j = 0; argv[i][j] && !contains_space; j++)
            if (argv[i][j] == ' ')
                contains_space = 1;
        if (contains_space)
            out[pos++] = '"';
        strcpy(&out[pos], argv[i]);
        pos += strlen(argv[i]);
        if (contains_space)
            out[pos++] = '"';
        if (argv[i + 1])
            out[pos++] = ' ';
    }
    out[pos] = '\0';
    return out;
}

int main(int argc, char* argv[])
{
    int i, cpp_argc, cc_argc;
    char **cpp_argv, **cc_argv;
    int exit_code;
    int input_source = 0, input_obj = 0, flag_compile = 0;
    int msvc = 0;
    const char *source_file = NULL;
    char *cmdline;
    char temp_file_1[200], temp_file_2[200], arg_buffer[200], fo_buffer[200];
    const char *outname = NULL;
    char *ptr;

    if (argc > 1 && !strncmp(argv[1], "cl", 2))
        msvc = 1;

    sprintf(temp_file_1, "preprocessed_%d.c", getpid());
    sprintf(temp_file_2, "converted_%d.c", getpid());

    cpp_argv = malloc((argc + 2) * sizeof(*cpp_argv));
    cc_argv = malloc((argc + 3) * sizeof(*cc_argv));
    cpp_argc = cc_argc = 0;
    for (i = 1; i < argc; ) {
        int len = strlen(argv[i]);
        int ext_inputfile = 0;
        if (len >= 2) {
            const char *ext = &argv[i][len - 2];
            if (!strcmp(ext, ".c") || !strcmp(ext, ".s") || !strcmp(ext, ".S")) {
                ext_inputfile = 1;
                input_source = 1;
                source_file = argv[i];
            } else if (!strcmp(ext, ".o") && argv[i][0] != '/' && argv[i][0] != '-') {
                ext_inputfile = 1;
                input_obj = 1;
            }
        }
        if (!strncmp(argv[i], "-Fo", 3) || !strcmp(argv[i], "-o")) {
            // Copy the output filename only to cc
            if (!strcmp(argv[i], "-Fo") && i + 1 < argc) {
                /* Support the nonstandard syntax -Fo filename, to get around
                 * msys file name mangling issues. */
                sprintf(fo_buffer, "-Fo%s", argv[i + 1]);
                outname = argv[i + 1];
                cc_argv[cc_argc++] = fo_buffer;
                i += 2;
            } else if (!strncmp(argv[i], "-Fo", 3)) {
                cc_argv[cc_argc++] = argv[i];
                outname = argv[i] + 3;
                i++;
            } else {
                /* -o */
                cc_argv[cc_argc++] = argv[i++];
                if (i < argc) {
                    outname = argv[i];
                    cc_argv[cc_argc++] = argv[i++];
                }
            }
            /* Set easier to understand temp file names. temp_file_2 might
             * have been set in cc_argv already, but we're just updating
             * the same buffer. */
            sprintf(temp_file_1, "%s_preprocessed.c", outname);
            sprintf(temp_file_2, "%s_converted.c", outname);
            if (msvc) {
                sprintf(arg_buffer, "/Fi%s", temp_file_1);
                cpp_argv[cpp_argc++] = arg_buffer;
            } else {
                cpp_argv[cpp_argc++] = "-o";
                cpp_argv[cpp_argc++] = temp_file_1;
            }
        } else if (!strcmp(argv[i], "-c")) {
            // Copy the compile flag only to cc, set the preprocess flag for cpp
            cc_argv[cc_argc++] = argv[i++];
            if (msvc)
                cpp_argv[cpp_argc++] = "/P";
            else
                cpp_argv[cpp_argc++] = "-E";
            flag_compile = 1;
        } else if (ext_inputfile) {
            // Input filename, pass to cpp only, set the temp file input to cc
            cpp_argv[cpp_argc++] = argv[i++];
            cc_argv[cc_argc++] = temp_file_2;
        } else if (!strcmp(argv[i], "-MMD")) {
            // Preprocessor-only parameter
            cpp_argv[cpp_argc++] = argv[i++];
        } else if (!strcmp(argv[i], "-MF") || !strcmp(argv[i], "-MT") ||
                   !strncmp(argv[i], "-FI", 3)) {
            // Deps generation, pass to cpp only
            cpp_argv[cpp_argc++] = argv[i++];
            if (i < argc)
                cpp_argv[cpp_argc++] = argv[i++];
        } else {
            // Normal parameter, copy to both cc and cpp
            cc_argv[cc_argc++]   = argv[i];
            cpp_argv[cpp_argc++] = argv[i];
            i++;
        }
    }

    if (!flag_compile || !source_file || !outname) {
        /* Doesn't seem like we should be invoked, just call the parameters as such */
        cmdline = create_cmdline(&argv[1]);
        exit_code = system(cmdline);
        goto exit;
    }

    cpp_argv[cpp_argc++] = NULL;
    cc_argv[cc_argc++] = NULL;
    cmdline = create_cmdline(cpp_argv);
    exit_code = system(cmdline);
    if (exit_code) {
        unlink(temp_file_1);
        goto exit;
    }
    free(cmdline);
    cmdline = malloc(strlen(argv[0]) + strlen(temp_file_1) + strlen(temp_file_2) + strlen(CONVERTER) + 20);
    strcpy(cmdline, argv[0]);
    ptr = strrchr(argv[0], '\\');
    if (!ptr) {
        ptr = cmdline;
    } else {
        ptr = cmdline + (ptr + 1 - argv[0]);
    }
    sprintf(ptr, "%s %s %s", CONVERTER, temp_file_1, temp_file_2);
    exit_code = system(cmdline);
    if (exit_code) {
        unlink(temp_file_1);
        unlink(temp_file_2);
        goto exit;
    }
    free(cmdline);
    unlink(temp_file_1);
    cmdline = create_cmdline(cc_argv);
    exit_code = system(cmdline);
    unlink(temp_file_2);
exit:
    free(cmdline);
    free(cc_argv);
    free(cpp_argv);
    return exit_code ? 1 : 0;
}
