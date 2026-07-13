#include "kernel/types.h"
#include "user/user.h"

void sieve(int read_fd) {
    int first;
    // 尝试读取第一个数字
    if (read(read_fd, &first, sizeof(int)) == 0) {
        // 如果读不到，说明上游已关闭，退出
        close(read_fd);
        exit(0);
    }

    printf("prime %d\n", first);

    int new_pipe[2];
    pipe(new_pipe);

    int pid = fork();
    if (pid == 0) {
        // 子进程：过滤后续数字
        close(new_pipe[1]);          // 关闭写端
        sieve(new_pipe[0]);
        // 不会执行到这里，因为 sieve 会 exit
    }
    else {
        // 父进程：继续读取后续数字，过滤掉 first 的倍数
        close(new_pipe[0]);          // 关闭读端
        int n;
        while (read(read_fd, &n, sizeof(int)) > 0) {
            if (n % first != 0) {
                write(new_pipe[1], &n, sizeof(int));
            }
        }
        // 所有数据读完，关闭写端，通知子进程结束
        close(new_pipe[1]);
        close(read_fd);
        // 等待子进程完成
        wait(0);
        exit(0);
    }
}

int main(void) {
    int p[2];
    pipe(p);

    int pid = fork();
    if (pid == 0) {
        // 子进程作为筛法起点
        close(p[1]);          // 关闭写端
        sieve(p[0]);
        exit(0);
    }
    else {
        // 父进程生成数字 2~35
        close(p[0]);          // 关闭读端
        for (int i = 2; i <= 35; i++) {
            write(p[1], &i, sizeof(int));
        }
        close(p[1]);          // 关闭写端，通知子进程数据结束
        wait(0);              // 等待整个管道链结束
        exit(0);
    }
}