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
#include <sys/wait.h>
#endif

#define CONVERTER "c99conv"

static char* create_cmdline(char **argv)
{
    int i;
    int len = 0, pos = 0;
    char *out;

    for (i = 0; argv[i]; i++)
        len += strlen(argv[i]);

    out = malloc(len + 4 * i + 10);

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

/* On Windows, system() has very frugal length limits */
#ifdef _WIN32
static int exec_argv_out(char **argv, const char *out)
{
    STARTUPINFO si = { 0 };
    PROCESS_INFORMATION pi = { 0 };
    DWORD exit_code;
    char *cmdline = create_cmdline(argv);
    FILE *fp = NULL;
    HANDLE pipe_read, pipe_write = NULL;
    BOOL inherit = FALSE;

    if (out) {
        SECURITY_ATTRIBUTES sa = { 0 };
        fp = fopen(out, "wb");
        if (!fp) {
            perror(out);
            return 1;
        }
        sa.nLength = sizeof(sa);
        sa.bInheritHandle = TRUE;
        CreatePipe(&pipe_read, &pipe_write, &sa, 0);
        si.cb = sizeof(si);
        si.dwFlags = STARTF_USESTDHANDLES;
        si.hStdOutput = pipe_write;
        si.hStdError = GetStdHandle(STD_ERROR_HANDLE);
        inherit = TRUE;
    }
    if (CreateProcess(NULL, cmdline, NULL, NULL, inherit, 0, NULL, NULL, &si, &pi)) {
        free(cmdline);
        if (out) {
            char buf[8192];
            DWORD n;
            CloseHandle(pipe_write);
            while (ReadFile(pipe_read, buf, sizeof(buf), &n, NULL))
                fwrite(buf, 1, n, fp);
            CloseHandle(pipe_read);
            fclose(fp);
        }

        WaitForSingleObject(pi.hProcess, INFINITE);

        if (!GetExitCodeProcess(pi.hProcess, &exit_code))
            exit_code = -1;

        CloseHandle(pi.hProcess);
        CloseHandle(pi.hThread);

        return exit_code;
    } else {
        if (out) {
            fclose(fp);
            CloseHandle(pipe_read);
            CloseHandle(pipe_write);
        }
        free(cmdline);
        return -1;
    }
}
#else
static int exec_argv_out(char **argv, const char *out)
{
    int fds[2];
    pid_t pid;
    int ret = 0;
    FILE *fp;
    if (!out) {
        char *cmdline = create_cmdline(argv);
        ret = system(cmdline);
        free(cmdline);
        return ret;
    }
    fp = fopen(out, "wb");
    if (!fp) {
        perror(out);
        return 1;
    }

    pipe(fds);
    if (!(pid = fork())) {
        close(fds[0]);
        dup2(fds[1], STDOUT_FILENO);
        if (execvp(argv[0], argv)) {
            perror("execvp");
            exit(1);
        }
    }
    close(fds[1]);
    while (1) {
        char buf[8192];
        int n = read(fds[0], buf, sizeof(buf));
        if (n <= 0)
            break;
        fwrite(buf, 1, n, fp);
    }
    close(fds[0]);
    fclose(fp);
    waitpid(pid, &ret, 0);
    return WEXITSTATUS(ret);
}
#endif

int main(int argc, char *argv[])
{
    int i = 1;
    int cpp_argc, cc_argc, pass_argc;
    int exit_code;
    int input_source = 0, input_obj = 0;
    int msvc = 0, keep = 0, noconv = 0, flag_compile = 0;
    char *ptr;
    char temp_file_1[200], temp_file_2[200], fo_buffer[200],
         fi_buffer[200];
    char **cpp_argv, **cc_argv, **pass_argv;
    char *conv_argv[5], *conv_tool;
    const char *source_file = NULL;
    const char *outname = NULL;
    char convert_options[20] = "";

    conv_tool = malloc(strlen(argv[0]) + strlen(CONVERTER) + 1);
    strcpy(conv_tool, argv[0]);

    ptr = strrchr(conv_tool, '\\');
    if (!ptr)
        ptr = strrchr(conv_tool, '/');

    if (!ptr)
        conv_tool[0] = '\0';
    else
        ptr[1] = '\0';
    strcat(conv_tool, CONVERTER);

    for (; i < argc; i++) {
        if (!strcmp(argv[i], "-keep")) {
            keep = 1;
        } else if (!strcmp(argv[i], "-noconv")) {
            noconv = 1;
        } else
            break;
    }

    if (keep && noconv) {
        fprintf(stderr, "Using -keep with -noconv doesn't make any sense!\n "
                        "You cannot keep intermediate files that doesn't exist.\n");
        return 1;
    }

    if (i < argc && !strncmp(argv[i], "cl", 2) && (argv[i][2] == '.' || argv[i][2] == '\0')) {
        msvc = 1;
        strcpy(convert_options, "-ms");
    } else if (i < argc && !strncmp(argv[i], "icl", 3) && (argv[i][3] == '.' || argv[i][3] == '\0'))
        msvc = 1; /* for command line compatibility */

    sprintf(temp_file_1, "preprocessed_%d.c", getpid());
    sprintf(temp_file_2, "converted_%d.c", getpid());

    cpp_argv  = malloc((argc + 2) * sizeof(*cpp_argv));
    cc_argv   = malloc((argc + 3) * sizeof(*cc_argv));
    pass_argv = malloc((argc + 3) * sizeof(*pass_argv));

    cpp_argc = cc_argc = pass_argc = 0;

    for (; i < argc; ) {
        int len           = strlen(argv[i]);
        int ext_inputfile = 0;

        if (len >= 2) {
            const char *ext = &argv[i][len - 2];

            if (!strcmp(ext, ".c") || !strcmp(ext, ".s") || !strcmp(ext, ".S")) {
                ext_inputfile = 1;
                input_source  = 1;
                source_file   = argv[i];
            } else if (!strcmp(ext, ".o") && argv[i][0] != '/' && argv[i][0] != '-') {
                ext_inputfile = 1;
                input_obj     = 1;
            }
        }
        if (!strncmp(argv[i], "-Fo", 3) || !strncmp(argv[i], "-Fi", 3) || !strncmp(argv[i], "-Fe", 3) ||
            !strcmp(argv[i], "-out") || !strcmp(argv[i], "-o") || !strcmp(argv[i], "-FI")) {

            // Copy the output filename only to cc
            if ((!strcmp(argv[i], "-Fo") || !strcmp(argv[i], "-out") || !strcmp(argv[i], "-Fi") ||
                !strcmp(argv[i], "-Fe")) && i + 1 < argc) {

                /* Support the nonstandard syntax -Fo filename or -out filename, to get around
                 * msys file name mangling issues. */
                if (!strcmp(argv[i], "-out"))
                    sprintf(fo_buffer, "-out:%s", argv[i + 1]);
                else
                    sprintf(fo_buffer, "%s%s", argv[i], argv[i + 1]);

                outname = argv[i + 1];

                cc_argv[cc_argc++]     = fo_buffer;
                pass_argv[pass_argc++] = fo_buffer;

                i += 2;
            } else if (!strcmp(argv[i], "-FI") && i + 1 < argc) {
                /* Support the nonstandard syntax -FI filename, to get around
                 * msys file name mangling issues. */
                sprintf(fi_buffer, "%s%s", argv[i], argv[i + 1]);

                cpp_argv[cpp_argc++]   = fi_buffer;
                pass_argv[pass_argc++] = fi_buffer;

                i += 2;
            } else if (!strncmp(argv[i], "-Fo", 3) || !strncmp(argv[i], "-Fi", 3) || !strncmp(argv[i], "-Fe", 3)) {
                cc_argv[cc_argc++]     = argv[i];
                pass_argv[pass_argc++] = argv[i];

                outname = argv[i] + 3;

                i++;
            } else {
                /* -o */
                pass_argv[pass_argc++] = argv[i];
                cc_argv[cc_argc++]     = argv[i++];

                if (i < argc) {
                    outname = argv[i];

                    pass_argv[pass_argc++] = argv[i];
                    cc_argv[cc_argc++]     = argv[i++];
                }
            }

            /* Set easier to understand temp file names. temp_file_2 might
             * have been set in cc_argv already, but we're just updating
             * the same buffer. */
            sprintf(temp_file_1, "%s_preprocessed.c", outname);
            sprintf(temp_file_2, "%s_converted.c", outname);

        } else if (!strcmp(argv[i], "-c")) {
            // Copy the compile flag only to cc, set the preprocess flag for cpp
            pass_argv[pass_argc++] = argv[i];
            cc_argv[cc_argc++]     = argv[i++];

            cpp_argv[cpp_argc++] = "-E";

            if (!noconv)
                flag_compile = 1;
        } else if (ext_inputfile) {
            // Input filename, pass to cpp only, set the temp file input to cc
            pass_argv[pass_argc++] = argv[i];
            cpp_argv[cpp_argc++]   = argv[i++];
            cc_argv[cc_argc++]     = temp_file_2;
        } else if (!strcmp(argv[i], "-MMD") || !strncmp(argv[i], "-D", 2)) {
            // Preprocessor-only parameter
            if (!strcmp(argv[i], "-D")) {
                // Handle -D DEFINE style
                pass_argv[pass_argc++] = argv[i];
                cpp_argv[cpp_argc++]   = argv[i++];
            }
            pass_argv[pass_argc++] = argv[i];
            cpp_argv[cpp_argc++]   = argv[i++];
        } else if (!strcmp(argv[i], "-MF") || !strcmp(argv[i], "-MT")) {
            // Deps generation, pass to cpp only
            pass_argv[pass_argc++] = argv[i];
            cpp_argv[cpp_argc++]   = argv[i++];

            if (i < argc) {
                pass_argv[pass_argc++] = argv[i];
                cpp_argv[cpp_argc++]   = argv[i++];
            }
        } else if (!strncmp(argv[i], "-FI", 3)) {
            // Forced include, pass to cpp only
            pass_argv[pass_argc++] = argv[i];
            cpp_argv[cpp_argc++]   = argv[i++];
        } else {
            // Normal parameter, copy to both cc and cpp
            pass_argv[pass_argc++] = argv[i];
            cc_argv[cc_argc++]     = argv[i];
            cpp_argv[cpp_argc++]   = argv[i];

            i++;
        }
    }

    cpp_argv[cpp_argc++]   = NULL;
    cc_argv[cc_argc++]     = NULL;
    pass_argv[pass_argc++] = NULL;

    if (!flag_compile || !source_file || !outname) {
        /* Doesn't seem like we should be invoked, just call the parameters as such */
        exit_code = exec_argv_out(pass_argv, NULL);

        goto exit;
    }

    exit_code = exec_argv_out(cpp_argv, temp_file_1);
    if (exit_code) {
        if (!keep)
            unlink(temp_file_1);

        goto exit;
    }

    conv_argv[0] = conv_tool;
    conv_argv[1] = convert_options;
    conv_argv[2] = temp_file_1;
    conv_argv[3] = temp_file_2;
    conv_argv[4] = NULL;

    exit_code = exec_argv_out(conv_argv, NULL);
    if (exit_code) {
        if (!keep) {
            unlink(temp_file_1);
            unlink(temp_file_2);
        }

        goto exit;
    }

    if (!keep)
        unlink(temp_file_1);

    exit_code = exec_argv_out(cc_argv, NULL);

    if (!keep)
        unlink(temp_file_2);

exit:
    free(cc_argv);
    free(cpp_argv);
    free(pass_argv);
    free(conv_tool);

    return exit_code ? 1 : 0;
}
