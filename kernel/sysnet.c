//
// network system calls.
//

#include "types.h"
#include "param.h"
#include "memlayout.h"
#include "riscv.h"
#include "spinlock.h"
#include "proc.h"
#include "defs.h"
#include "fs.h"
#include "sleeplock.h"
#include "file.h"
#include "net.h"

struct sock {
  struct sock *next; // the next socket in the list
  uint32 raddr;      // the remote IPv4 address
  uint16 lport;      // the local UDP port number
  uint16 rport;      // the remote UDP port number
  struct spinlock lock; // protects the rxq
  struct mbufq rxq;  // a queue of packets waiting to be received
  uint32 last_raddr;   // 最后收到的远程 IP
  uint16 last_rport;   // 最后收到的远程端口
};

#define SOCK_HASH_SIZE 17
static struct spinlock locks[SOCK_HASH_SIZE];
static struct sock *sock_hash[SOCK_HASH_SIZE];

static int
sock_hash_idx(uint16 lport)
{
    return lport % SOCK_HASH_SIZE;
}

void
sockinit(void)
{
  for (int i = 0; i < SOCK_HASH_SIZE; i++) {
    initlock(&locks[i], "sockhash");
  }
  memset(sock_hash, 0, sizeof(sock_hash));
}

int
sockalloc(struct file **f, uint32 raddr, uint16 lport, uint16 rport)
{
  struct sock *si, *pos;

  si = 0;
  *f = 0;
  if ((*f = filealloc()) == 0)
    goto bad;
  if ((si = (struct sock*)kalloc()) == 0)
    goto bad;

  // initialize objects
  si->raddr = raddr;
  si->lport = lport;
  si->rport = rport;
  initlock(&si->lock, "sock");
  mbufq_init(&si->rxq);
  (*f)->type = FD_SOCK;
  (*f)->readable = 1;
  (*f)->writable = 1;
  (*f)->sock = si;

    // add to hash table
  int idx = sock_hash_idx(lport);
  acquire(&locks[idx]);
  // Check for duplicate within the same bucket.
  for (pos = sock_hash[idx]; pos; pos = pos->next) {
    if (pos->raddr == raddr &&
        pos->lport == lport &&
        pos->rport == rport) {
      release(&locks[idx]);
      goto bad;
    }
  }
  si->next = sock_hash[idx];
  sock_hash[idx] = si;
  release(&locks[idx]);
  return 0;

bad:
  if (si)
    kfree((char*)si);
  if (*f)
    fileclose(*f);
  return -1;
}

void
sockclose(struct sock *si)
{
  struct sock **pos;
  struct mbuf *m;

  // remove from hash table
  int idx = sock_hash_idx(si->lport);
  acquire(&locks[idx]);
  pos = &sock_hash[idx];
  while (*pos) {
    if (*pos == si) {
      *pos = si->next;
      break;
    }
    pos = &(*pos)->next;
  }
  release(&locks[idx]);

  // free any pending mbufs
  while (!mbufq_empty(&si->rxq)) {
    m = mbufq_pophead(&si->rxq);
    mbuffree(m);
  }

  kfree((char*)si);
}

int
sockread(struct sock *si, uint64 addr, int n)
{
  struct proc *pr = myproc();
  struct mbuf *m;
  int len;

  acquire(&si->lock);
  while (mbufq_empty(&si->rxq) && !pr->killed) {
    sleep(&si->rxq, &si->lock);
  }
  if (pr->killed) {
    release(&si->lock);
    return -1;
  }
  m = mbufq_pophead(&si->rxq);
  release(&si->lock);

  len = m->len;
  if (len > n)
    len = n;
  if (copyout(pr->pagetable, addr, m->head, len) == -1) {
    mbuffree(m);
    return -1;
  }
  mbuffree(m);
  return len;
}

int
sockwrite(struct sock *si, uint64 addr, int n)
{
  struct proc *pr = myproc();
  struct mbuf *m;

  m = mbufalloc(MBUF_DEFAULT_HEADROOM);
  if (!m)
    return -1;

  if (copyin(pr->pagetable, mbufput(m, n), addr, n) == -1) {
    mbuffree(m);
    return -1;
  }
  net_tx_udp(m, si->raddr, si->lport, si->rport);
  return n;
}

// called by protocol handler layer to deliver UDP packets
void
sockrecvudp(struct mbuf *m, uint32 raddr, uint16 lport, uint16 rport)
{
  //printf("sockrecvudp: raddr=%x lport=%d rport=%d\n", raddr, lport, rport);
  struct sock *si;
  int idx = sock_hash_idx(lport);

  acquire(&locks[idx]);
  for (si = sock_hash[idx]; si; si = si->next) {
    //printf("checking si: lport=%d raddr=%x rport=%d\n", si->lport, si->raddr, si->rport);
    // 匹配本地端口，远程地址和端口允许通配（0 表示任意）
    if (si->lport == lport &&
        (si->raddr == 0 || si->raddr == raddr) &&
        (si->rport == 0 || si->rport == rport)) {
      acquire(&si->lock);
      si->last_raddr = raddr;   // 保存远程地址
      si->last_rport = rport;   // 保存远程端口
      mbufq_pushtail(&si->rxq, m);
      wakeup(&si->rxq);
      release(&si->lock);
      release(&locks[idx]);
      return;
    }
  }
  release(&locks[idx]);
  mbuffree(m);
}

uint64 sys_socket(void) {
    struct file *f;
    struct sock *si;
    int fd;

    if ((si = kalloc()) == 0)
        return -1;
    initlock(&si->lock, "sock");
    mbufq_init(&si->rxq);
    si->raddr = 0;      // 未绑定远程地址
    si->rport = 0;      // 未绑定远程端口
    si->lport = 0;      // 未绑定本地端口
    si->next = 0;
    si->last_raddr = 0;
    si->last_rport = 0;

    if ((f = filealloc()) == 0) {
        kfree(si);
        return -1;
    }
    if ((fd = fdalloc(f)) < 0) {
        fileclose(f);
        kfree(si);
        return -1;
    }
    f->type = FD_SOCK;
    f->readable = 1;
    f->writable = 1;
    f->sock = si;
    return fd;
}

uint64 sys_bind(void) {
    int fd;
    uint16 lport;
    struct file *f;
    struct sock *si;
    int idx;

    if (argfd(0, &fd, &f) < 0)
        return -1;
    if (f->type != FD_SOCK)
        return -1;
    si = f->sock;
    if (argint(1, (int*)&lport) < 0)
        return -1;

    if (si->lport != 0)
        return -1;  // 已绑定，不允许重复绑定

    // 检查该端口是否已被占用
    idx = sock_hash_idx(lport);
    acquire(&locks[idx]);
    struct sock *pos;
    for (pos = sock_hash[idx]; pos; pos = pos->next) {
        if (pos->lport == lport) {
            release(&locks[idx]);
            return -1;
        }
    }
    si->lport = lport;
    si->next = sock_hash[idx];
    sock_hash[idx] = si;
    release(&locks[idx]);
    return 0;
}

uint64 sys_sendto(void) {
    int fd;
    uint64 addr;
    int len;
    uint32 raddr;
    uint16 rport;
    struct file *f;
    struct sock *si;
    struct mbuf *m;
    struct proc *p = myproc();

    if (argfd(0, &fd, &f) < 0) return -1;
    if (argaddr(1, &addr) < 0) return -1;

    // 读取 len（用 argaddr 避免符号问题）
    uint64 len64;
    if (argaddr(2, &len64) < 0) return -1;
    len = (int)len64;

    // 读取 raddr 指针
    uint64 raddr_ptr;
    if (argaddr(3, &raddr_ptr) < 0) return -1;

    // 读取 rport
    if (argint(4, (int*)&rport) < 0) return -1;

    if (f->type != FD_SOCK || len < 0 || len > MBUF_SIZE)
        return -1;
    si = f->sock;
    if (si->lport == 0)
        return -1;

    m = mbufalloc(MBUF_DEFAULT_HEADROOM);
    if (!m) return -1;

    // 从用户空间复制 raddr 值
    if (copyin(p->pagetable, (char*)&raddr, raddr_ptr, sizeof(uint32)) < 0) {
        printf("sys_sendto: copyin raddr failed\n");
        mbuffree(m);
        return -1;
    }

    // 复制数据
    if (copyin(p->pagetable, mbufput(m, len), addr, len) == -1) {
        printf("sys_sendto: copyin data failed\n");
        mbuffree(m);
        return -1;
    }

    printf("sys_sendto: raddr=%x len=%d\n", raddr, len);
    net_tx_udp(m, raddr, si->lport, rport);
    return len;
}

uint64 sys_recvfrom(void) {
    int fd;
    uint64 addr, raddr_ptr;
    uint64 rport_ptr;
    int len;
    struct file *f;
    struct sock *si;
    struct mbuf *m;
    struct proc *p = myproc();

    if (argfd(0, &fd, &f) < 0 ||
        argaddr(1, &addr) < 0 ||
        argint(2, &len) < 0 ||
        argaddr(3, &raddr_ptr) < 0 ||
        argaddr(4, &rport_ptr) < 0)
        return -1;

    if (f->type != FD_SOCK || len < 0)
        return -1;
    si = f->sock;

    // 等待数据到达
    acquire(&si->lock);
    while (mbufq_empty(&si->rxq) && !p->killed) {
        sleep(&si->rxq, &si->lock);
    }
    if (p->killed) {
        release(&si->lock);
        return -1;
    }
    m = mbufq_pophead(&si->rxq);
    release(&si->lock);

    int read_len = m->len;
    if (read_len > len)
        read_len = len;

    // 复制数据到用户空间
    if (copyout(p->pagetable, addr, m->head, read_len) < 0) {
        mbuffree(m);
        return -1;
    }
    // 复制发送方 IP 和端口到用户空间
    if (copyout(p->pagetable, raddr_ptr, (char*)&si->last_raddr, sizeof(uint32)) < 0 ||
        copyout(p->pagetable, rport_ptr, (char*)&si->last_rport, sizeof(uint16)) < 0) {
        mbuffree(m);
        return -1;
    }
    mbuffree(m);
    return read_len;
}
