#include "kernel/types.h"
#include "user/user.h"

int main(void)
{
    int p1[2], p2[2];  // p1: 父->子, p2: 子->父
    char buf[1];

    // 创建两个管道
    pipe(p1);
    pipe(p2);

    int pid = fork();

    if (pid == 0) {
        // 子进程
        // 关闭不需要的写端/读端
        close(p1[1]);  // 子进程不向 p1 写
        close(p2[0]);  // 子进程不从 p2 读

        // 从父进程读取一个字节
        read(p1[0], buf, 1);
        printf("%d: received ping\n", getpid());

        // 向父进程回传一个字节
        write(p2[1], buf, 1);

        // 关闭剩余文件描述符
        close(p1[0]);
        close(p2[1]);
        exit(0);
    }
    else {
        // 父进程
        // 关闭不需要的读端/写端
        close(p1[0]);  // 父进程不从 p1 读
        close(p2[1]);  // 父进程不向 p2 写

        // 向子进程发送一个字节
        write(p1[1], "A", 1);

        // 从子进程读取回传的字节
        read(p2[0], buf, 1);
        printf("%d: received pong\n", getpid());

        // 等待子进程结束（防止僵尸进程）
        wait(0);

        // 关闭剩余文件描述符
        close(p1[1]);
        close(p2[0]);
        exit(0);
    }
}