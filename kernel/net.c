//
// networking protocol support (IP, UDP, ARP, etc.).
//

#include "types.h"
#include "param.h"
#include "memlayout.h"
#include "riscv.h"
#include "spinlock.h"
#include "proc.h"
#include "net.h"
#include "defs.h"

#define ARP_CACHE_SIZE 16
#define ARP_TIMEOUT    300      // 30 秒 (ticks)
#define PENDING_TIMEOUT 100     // 挂起包最长等待 10 秒

static uint32 gateway_ip = MAKE_IP_ADDR(10, 0, 2, 2);

// ARP 缓存条目
struct arp_entry {
  uint32 ip;
  uint8  mac[6];
  uint   last_seen;   // 最后更新时间（ticks）
  int    valid;
};

// 挂起包队列节点
struct pending_pkt {
  struct mbuf *m;
  uint32 dst_ip;
  uint   enqueue_time;  // 入队时的 ticks
  struct pending_pkt *next;
};

// 全局变量
static struct arp_entry arp_cache[ARP_CACHE_SIZE];
static struct spinlock arp_lock;
static struct pending_pkt *pending_head;
static struct spinlock pending_lock;
static int net_initialized = 0;

static uint32 local_ip = MAKE_IP_ADDR(10, 0, 2, 15); // qemu's idea of the guest IP
static uint8 local_mac[ETHADDR_LEN] = { 0x52, 0x54, 0x00, 0x12, 0x34, 0x56 };
static uint8 broadcast_mac[ETHADDR_LEN] = { 0xFF, 0XFF, 0XFF, 0XFF, 0XFF, 0XFF };

static void
net_init(void)
{
  if (net_initialized)
    return;
  initlock(&arp_lock, "arp");
  initlock(&pending_lock, "pending");
  net_initialized = 1;
}

static uint8*
arp_lookup(uint32 ip)
{
  acquire(&arp_lock);
  for (int i = 0; i < ARP_CACHE_SIZE; i++) {
    if (arp_cache[i].valid && arp_cache[i].ip == ip) {
      if (ticks - arp_cache[i].last_seen < ARP_TIMEOUT) {
        release(&arp_lock);
        return arp_cache[i].mac;
      } else {
        // 超时，标记失效
        arp_cache[i].valid = 0;
      }
    }
  }
  release(&arp_lock);
  //printf("arp_lookup HIT: ip=%x\n", ip);
  return 0;
}

static void
arp_update(uint32 ip, uint8 *mac)
{
  acquire(&arp_lock);
  int empty = -1;
  for (int i = 0; i < ARP_CACHE_SIZE; i++) {
    if (arp_cache[i].valid && arp_cache[i].ip == ip) {
      memmove(arp_cache[i].mac, mac, 6);
      arp_cache[i].last_seen = ticks;
      release(&arp_lock);
      return;
    }
    if (empty == -1 && !arp_cache[i].valid)
      empty = i;
  }
  // 未找到，使用空槽；若无空槽则覆盖第一个（简单策略）
  if (empty == -1) empty = 0;
  arp_cache[empty].ip = ip;
  memmove(arp_cache[empty].mac, mac, 6);
  arp_cache[empty].last_seen = ticks;
  arp_cache[empty].valid = 1;
  release(&arp_lock);
}

static void
pending_add(struct mbuf *m, uint32 dip)
{
  struct pending_pkt *p = kalloc();
  if (!p) {
    // 内存不足，丢弃包
    mbuffree(m);
    return;
  }
  p->m = m;
  p->dst_ip = dip;
  p->enqueue_time = ticks;
  p->next = 0;
  acquire(&pending_lock);
  p->next = pending_head;
  pending_head = p;
  //printf("pending_add: added packet for ip=%x\n", dip);
  release(&pending_lock);
}

static void
pending_cleanup(void)
{
  struct pending_pkt *head = 0;
  acquire(&pending_lock);
  struct pending_pkt **pp = &pending_head;
  while (*pp) {
    if (ticks - (*pp)->enqueue_time > PENDING_TIMEOUT) {
      struct pending_pkt *p = *pp;
      *pp = p->next;
      p->next = head;
      head = p;
    } else {
      pp = &(*pp)->next;
    }
  }
  release(&pending_lock);

  // 释放超时的包
  while (head) {
    struct pending_pkt *p = head;
    head = p->next;
    mbuffree(p->m);
    kfree(p);
  }
}

// Strips data from the start of the buffer and returns a pointer to it.
// Returns 0 if less than the full requested length is available.
char *
mbufpull(struct mbuf *m, unsigned int len)
{
  char *tmp = m->head;
  if (m->len < len)
    return 0;
  m->len -= len;
  m->head += len;
  return tmp;
}

// Prepends data to the beginning of the buffer and returns a pointer to it.
char *
mbufpush(struct mbuf *m, unsigned int len)
{
  m->head -= len;
  if (m->head < m->buf)
    panic("mbufpush");
  m->len += len;
  return m->head;
}

// Appends data to the end of the buffer and returns a pointer to it.
char *
mbufput(struct mbuf *m, unsigned int len)
{
  char *tmp = m->head + m->len;
  m->len += len;
  if (m->len > MBUF_SIZE)
    panic("mbufput");
  return tmp;
}

// Strips data from the end of the buffer and returns a pointer to it.
// Returns 0 if less than the full requested length is available.
char *
mbuftrim(struct mbuf *m, unsigned int len)
{
  if (len > m->len)
    return 0;
  m->len -= len;
  return m->head + m->len;
}

// Allocates a packet buffer.
struct mbuf *
mbufalloc(unsigned int headroom)
{
  struct mbuf *m;
 
  if (headroom > MBUF_SIZE)
    return 0;
  m = kalloc();
  if (m == 0)
    return 0;
  m->next = 0;
  m->head = (char *)m->buf + headroom;
  m->len = 0;
  memset(m->buf, 0, sizeof(m->buf));
  return m;
}

// Frees a packet buffer.
void
mbuffree(struct mbuf *m)
{
  kfree(m);
}

// Pushes an mbuf to the end of the queue.
void
mbufq_pushtail(struct mbufq *q, struct mbuf *m)
{
  m->next = 0;
  if (!q->head){
    q->head = q->tail = m;
    return;
  }
  q->tail->next = m;
  q->tail = m;
}

// Pops an mbuf from the start of the queue.
struct mbuf *
mbufq_pophead(struct mbufq *q)
{
  struct mbuf *head = q->head;
  if (!head)
    return 0;
  q->head = head->next;
  return head;
}

// Returns one (nonzero) if the queue is empty.
int
mbufq_empty(struct mbufq *q)
{
  return q->head == 0;
}

// Intializes a queue of mbufs.
void
mbufq_init(struct mbufq *q)
{
  q->head = 0;
}

// This code is lifted from FreeBSD's ping.c, and is copyright by the Regents
// of the University of California.
static unsigned short
in_cksum(const unsigned char *addr, int len)
{
  int nleft = len;
  const unsigned short *w = (const unsigned short *)addr;
  unsigned int sum = 0;
  unsigned short answer = 0;

  /*
   * Our algorithm is simple, using a 32 bit accumulator (sum), we add
   * sequential 16 bit words to it, and at the end, fold back all the
   * carry bits from the top 16 bits into the lower 16 bits.
   */
  while (nleft > 1)  {
    sum += *w++;
    nleft -= 2;
  }

  /* mop up an odd byte, if necessary */
  if (nleft == 1) {
    *(unsigned char *)(&answer) = *(const unsigned char *)w;
    sum += answer;
  }

  /* add back carry outs from top 16 bits to low 16 bits */
  sum = (sum & 0xffff) + (sum >> 16);
  sum += (sum >> 16);
  /* guaranteed now that the lower 16 bits of sum are correct */

  answer = ~sum; /* truncate to 16 bits */
  return answer;
}

// sends an ethernet packet
void
net_tx_eth(struct mbuf *m, uint16 ethtype, uint8 *dmac)
{
  struct eth *ethhdr;
  ethhdr = mbufpushhdr(m, *ethhdr);
  memmove(ethhdr->shost, local_mac, ETHADDR_LEN);
  if (dmac)
    memmove(ethhdr->dhost, dmac, ETHADDR_LEN);
  else
    memmove(ethhdr->dhost, broadcast_mac, ETHADDR_LEN);
  ethhdr->type = htons(ethtype);
  int ret = e1000_transmit(m);
  if (ret) {
      //printf("net_tx_eth: e1000_transmit failed, ret=%d\n", ret);
      mbuffree(m);
  } else {
      //printf("net_tx_eth: e1000_transmit success\n");
  }
}

static void
pending_flush(uint32 ip, uint8 *mac)
{
  struct pending_pkt *head = 0;
  // 从全局队列中摘除匹配 ip 的节点，构建临时链表
  acquire(&pending_lock);
  struct pending_pkt **pp = &pending_head;
  //printf("pending_flush: flushing packets for ip=%x count=...\n", ip);
  while (*pp) {
    if ((*pp)->dst_ip == ip) {
      struct pending_pkt *p = *pp;
      *pp = p->next;
      p->next = head;
      head = p;
    } else {
      pp = &(*pp)->next;
    }
  }
  release(&pending_lock);

  // 发送临时链表中的每个包
  while (head) {
    struct pending_pkt *p = head;
    head = p->next;
    // 发送，net_tx_eth 会推入以太网头并使用指定的 mac
    net_tx_eth(p->m, ETHTYPE_IP, mac);
    //printf("pending_flush: sent one packet\n");
    kfree(p);
  }
}

// sends an ARP packet
static int
net_tx_arp(uint16 op, uint8 dmac[ETHADDR_LEN], uint32 dip)
{
  struct mbuf *m;
  struct arp *arphdr;

  m = mbufalloc(MBUF_DEFAULT_HEADROOM);
  if (!m)
    return -1;

  // generic part of ARP header
  arphdr = mbufputhdr(m, *arphdr);
  arphdr->hrd = htons(ARP_HRD_ETHER);
  arphdr->pro = htons(ETHTYPE_IP);
  arphdr->hln = ETHADDR_LEN;
  arphdr->pln = sizeof(uint32);
  arphdr->op = htons(op);

  // ethernet + IP part of ARP header
  memmove(arphdr->sha, local_mac, ETHADDR_LEN);
  arphdr->sip = htonl(local_ip);
  memmove(arphdr->tha, dmac, ETHADDR_LEN);
  arphdr->tip = htonl(dip);

  // header is ready, send the packet
  net_tx_eth(m, ETHTYPE_ARP, 0);
  return 0;
}

// sends an IP packet
static void
net_tx_ip(struct mbuf *m, uint8 proto, uint32 dip)
{
  struct ip *iphdr;

  // push the IP header
  iphdr = mbufpushhdr(m, *iphdr);
  memset(iphdr, 0, sizeof(*iphdr));
  iphdr->ip_vhl = (4 << 4) | (20 >> 2);
  iphdr->ip_p = proto;
  iphdr->ip_src = htonl(local_ip);
  iphdr->ip_dst = htonl(dip);
  iphdr->ip_len = htons(m->len);
  iphdr->ip_ttl = 100;
  iphdr->ip_sum = in_cksum((unsigned char *)iphdr, sizeof(*iphdr));

  // 判断目标 IP 是否在本地子网 (10.0.2.0/24)
  #define LOCAL_NETWORK 0x0a000200
  #define LOCAL_NETMASK 0xffffff00
  uint32 nexthop;
  if ((dip & LOCAL_NETMASK) == LOCAL_NETWORK) {
    nexthop = dip;
  } else {
    nexthop = gateway_ip; // 默认网关 10.0.2.2
  }

  // 尝试查找 ARP 缓存（使用下一跳 IP）
  uint8 *dmac = arp_lookup(nexthop);
  //printf("net_tx_ip: dmac=%p\n", dmac);
  if (dmac) {
    //printf("net_tx_ip: send directly\n");
    net_tx_eth(m, ETHTYPE_IP, dmac);
  } else {
    //printf("net_tx_ip: send ARP request and pend\n");
    net_tx_arp(ARP_OP_REQUEST, broadcast_mac, nexthop);
    pending_add(m, nexthop); // 存储下一跳 IP，用于匹配 ARP 响应
  }
}

// sends a UDP packet
void
net_tx_udp(struct mbuf *m, uint32 dip,
           uint16 sport, uint16 dport)
{
  net_init();
  struct udp *udphdr;

  // put the UDP header
  udphdr = mbufpushhdr(m, *udphdr);
  udphdr->sport = htons(sport);
  udphdr->dport = htons(dport);
  udphdr->ulen = htons(m->len);
  udphdr->sum = 0; // zero means no checksum is provided

  // now on to the IP layer
  net_tx_ip(m, IPPROTO_UDP, dip);
}

// receives an ARP packet
static void
net_rx_arp(struct mbuf *m)
{
  struct arp *arphdr;
  uint8 smac[ETHADDR_LEN];
  uint32 sip, tip;

  arphdr = mbufpullhdr(m, *arphdr);
  if (!arphdr)
    goto done;

  // validate the ARP header
  if (ntohs(arphdr->hrd) != ARP_HRD_ETHER ||
      ntohs(arphdr->pro) != ETHTYPE_IP ||
      arphdr->hln != ETHADDR_LEN ||
      arphdr->pln != sizeof(uint32)) {
    goto done;
  }

  // 先提取发送者信息
  memmove(smac, arphdr->sha, ETHADDR_LEN);
  sip = ntohl(arphdr->sip);
  // 更新 ARP 缓存（无论请求还是回复，都更新）
  arp_update(sip, smac);
  // 收到任何 ARP 包，立即尝试发送等待该 IP 的包
  pending_flush(sip, smac);
  //printf("ARP reply from %x, flushing\n", sip);

  uint16 op = ntohs(arphdr->op);
  if (op == ARP_OP_REPLY) {
    tip = ntohl(arphdr->tip);
    //printf("net_rx_arp: ARP reply from %x tip=%x local_ip=%x\n", sip, tip, local_ip);
  } else if (op == ARP_OP_REQUEST) {
    tip = ntohl(arphdr->tip);
    if (tip == local_ip) {
      //printf("net_rx_arp: ARP request from %x for %x\n", sip, tip);
      net_tx_arp(ARP_OP_REPLY, smac, sip);
    }
  }

done:
  mbuffree(m);
}

// receives a UDP packet
static void
net_rx_udp(struct mbuf *m, uint16 len, struct ip *iphdr)
{
  struct udp *udphdr;
  uint32 sip;
  uint16 sport, dport;


  udphdr = mbufpullhdr(m, *udphdr);
  if (!udphdr)
    goto fail;

  // TODO: validate UDP checksum

  // validate lengths reported in headers
  if (ntohs(udphdr->ulen) != len)
    goto fail;
  len -= sizeof(*udphdr);
  if (len > m->len)
    goto fail;
  // minimum packet size could be larger than the payload
  mbuftrim(m, m->len - len);

  // parse the necessary fields
  sip = ntohl(iphdr->ip_src);
  sport = ntohs(udphdr->sport);
  dport = ntohs(udphdr->dport);
  sockrecvudp(m, sip, dport, sport);
  return;

fail:
  mbuffree(m);
}

// receives an IP packet
static void
net_rx_ip(struct mbuf *m)
{
  struct ip *iphdr;
  uint16 len;

  iphdr = mbufpullhdr(m, *iphdr);
  if (!iphdr)
	  goto fail;

  // check IP version and header len
  if (iphdr->ip_vhl != ((4 << 4) | (20 >> 2)))
    goto fail;
  // validate IP checksum
  if (in_cksum((unsigned char *)iphdr, sizeof(*iphdr)))
    goto fail;
  // can't support fragmented IP packets
  if (htons(iphdr->ip_off) != 0)
    goto fail;
  // is the packet addressed to us?
  if (htonl(iphdr->ip_dst) != local_ip)
    goto fail;
  // can only support UDP
  if (iphdr->ip_p != IPPROTO_UDP)
    goto fail;

  len = ntohs(iphdr->ip_len) - sizeof(*iphdr);
  net_rx_udp(m, len, iphdr);
  return;

fail:
  mbuffree(m);
}

// called by e1000 driver's interrupt handler to deliver a packet to the
// networking stack
void net_rx(struct mbuf *m)
{
  net_init();
  pending_cleanup();
  struct eth *ethhdr;
  uint16 type;

  ethhdr = mbufpullhdr(m, *ethhdr);
  if (!ethhdr) {
    mbuffree(m);
    return;
  }

  type = ntohs(ethhdr->type);
  if (type == ETHTYPE_IP)
    net_rx_ip(m);
  else if (type == ETHTYPE_ARP)
    net_rx_arp(m);
  else
    mbuffree(m);
}
