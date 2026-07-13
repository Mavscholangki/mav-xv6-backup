#include "kernel/types.h"
#include "user/user.h"

int main(int argc, char *argv[])
{
    // 1. 检查参数数量：如果没有传入参数，报错退出
    if (argc < 2) {
        fprintf(2, "Usage: sleep <ticks>\n");
        exit(1);
    }

    // 2. 将字符串参数转换为整数
    int ticks = atoi(argv[1]);

    // 3. 调用 xv6 提供的系统调用 sleep()
    sleep(ticks);

    // 4. 正常退出
    exit(0);
}