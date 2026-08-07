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
  struct sock *si;
  int idx = sock_hash_idx(lport);

  acquire(&locks[idx]);
  for (si = sock_hash[idx]; si; si = si->next) {
    if (si->raddr == raddr && si->lport == lport && si->rport == rport) {
      acquire(&si->lock);
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
