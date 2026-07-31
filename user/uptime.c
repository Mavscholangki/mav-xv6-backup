#include "kernel/types.h"
#include "user/user.h"

int main(void) {
    int ticks = uptime();
    printf("%d\n", ticks);
    exit(0);
}