#include "kernel/types.h"
#include "kernel/param.h"
#include "user/user.h"

int main(int argc, char *argv[]) {
    if (argc < 2) {
        fprintf(2, "Usage: xargs <command> [args...]\n");
        exit(1);
    }

    char *exec_argv[MAXARG];
    int exec_argc = 0;
    for (int i = 1; i < argc; i++) {
        exec_argv[exec_argc] = argv[i];
        exec_argc++;
    }

    char line[512];
    char c;
    int idx;

    while (1) {
        idx = 0;
        // 读取一行
        int ret;
        while ((ret = read(0, &c, 1)) > 0) {
            if (c == '\n') break;

            if (idx < sizeof(line) - 1) {
                line[idx] = c;
                idx++;
            }
        }
        // 如果 read 返回 0（EOF）且没有读取任何字符，则结束
        if (ret == 0 && idx == 0) {
            break;
        }
        line[idx] = '\0';

        exec_argv[exec_argc] = line;
        exec_argv[exec_argc + 1] = 0;

        int pid = fork();
        if (pid == 0) {
            exec(exec_argv[0], exec_argv);
            fprintf(2, "xargs: exec %s failed\n", exec_argv[0]);
            exit(1);
        }
        else {
            wait(0);
        }
    }
    exit(0);
}