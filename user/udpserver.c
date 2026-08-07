#include "kernel/types.h"
#include "user/user.h"

// static inline uint32 my_ntohl(uint32 x) {
//     return ((x & 0xff000000) >> 24) |
//            ((x & 0x00ff0000) >> 8) |
//            ((x & 0x0000ff00) << 8) |
//            ((x & 0x000000ff) << 24);
// }

int main() {
    int fd = socket();
    if (fd < 0) {
        printf("socket() failed\n");
        exit(1);
    }

    if (bind(fd, 2000) < 0) {
        printf("bind() failed\n");
        exit(1);
    }

    printf("UDP echo server listening on port 2000\n");

    char buf[2048];
    while (1) {
        uint32 raddr;
        uint16 rport;
        int n = recvfrom(fd, buf, sizeof(buf), &raddr, &rport);
        if (n < 0) {
            printf("recvfrom error\n");
            break;
        }
        printf("Received %d bytes from %x:%d\n", n, raddr, rport);

        // 回显
        //uint32 host_raddr = my_ntohl(raddr);   // 转换为小端
        printf("Sending %d bytes back to %x:%d\n", n, raddr, rport);
        if (sendto(fd, buf, n, &raddr, rport) < 0) {  // 传地址
            printf("sendto error\n");
        } else {
            printf("sendto success\n");
        }
    }
    exit(0);
}