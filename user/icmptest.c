#include "kernel/types.h"
#include "user/user.h"

// 手动构造小端 IP 值 10.0.2.2 -> 0x0202000A
static inline uint32 make_ip(uint8 a, uint8 b, uint8 c, uint8 d) {
    return (uint32)a | ((uint32)b << 8) | ((uint32)c << 16) | ((uint32)d << 24);
}

int main() {
    int fd = socket();
    if (fd < 0) {
        printf("socket failed\n");
        exit(1);
    }

    if (bind(fd, 2001) < 0) {
        printf("bind failed\n");
        exit(1);
    }

    char buf[] = "test";
    // 小端 IP 地址 10.0.2.2
    uint32 raddr = make_ip(10, 0, 2, 2);   // 结果为 0x0202000A
    if (sendto(fd, buf, 4, &raddr, 33333) < 0) {
        printf("sendto failed\n");
        exit(1);
    }

    printf("Sent UDP to 10.0.2.2:33333, waiting for ICMP error...\n");

    char recv_buf[100];
    uint32 from_ip;
    uint16 from_port;
    int n = recvfrom(fd, recv_buf, sizeof(recv_buf), &from_ip, &from_port);
    if (n < 0) {
        printf("recvfrom returned ICMP error: %d (code)\n", -n);
    } else {
        printf("recvfrom returned %d bytes (unexpected)\n", n);
    }

    exit(0);
}
