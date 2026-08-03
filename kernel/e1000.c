#include "types.h"
#include "param.h"
#include "memlayout.h"
#include "riscv.h"
#include "spinlock.h"
#include "proc.h"
#include "defs.h"
#include "e1000_dev.h"
#include "net.h"

static struct spinlock tx_lock;
static struct spinlock rx_lock;

#define TX_RING_SIZE 16
static struct tx_desc tx_ring[TX_RING_SIZE] __attribute__((aligned(16)));
static struct mbuf *tx_mbufs[TX_RING_SIZE];

#define RX_RING_SIZE 16
static struct rx_desc rx_ring[RX_RING_SIZE] __attribute__((aligned(16)));
static struct mbuf *rx_mbufs[RX_RING_SIZE];

// remember where the e1000's registers live.
static volatile uint32 *regs;

struct spinlock e1000_lock;

// TX software queue for challenge 1
#define TX_SOFTQ_SIZE 32
static struct mbuf *tx_softq[TX_SOFTQ_SIZE];
static int tx_softq_head;
static int tx_softq_tail;
static struct spinlock tx_softq_lock;

static void tx_refill(void);

// called by pci_init().
// xregs is the memory address at which the
// e1000's registers are mapped.
void
e1000_init(uint32 *xregs)
{
  int i;

  initlock(&tx_lock, "e1000_tx");
  initlock(&rx_lock, "e1000_rx");
  memset(tx_mbufs, 0, sizeof(tx_mbufs));

  initlock(&e1000_lock, "e1000");
  initlock(&tx_softq_lock, "e1000_tx_softq");
  tx_softq_head = tx_softq_tail = 0;

  regs = xregs;

  // Reset the device
  regs[E1000_IMS] = 0; // disable interrupts
  regs[E1000_CTL] |= E1000_CTL_RST;
  regs[E1000_IMS] = 0; // redisable interrupts
  __sync_synchronize();

  // [E1000 14.5] Transmit initialization
  memset(tx_ring, 0, sizeof(tx_ring));
  for (i = 0; i < TX_RING_SIZE; i++) {
    tx_ring[i].status = E1000_TXD_STAT_DD;
    tx_mbufs[i] = 0;
  }
  regs[E1000_TDBAL] = (uint64) tx_ring;
  if(sizeof(tx_ring) % 128 != 0)
    panic("e1000");
  regs[E1000_TDLEN] = sizeof(tx_ring);
  regs[E1000_TDH] = regs[E1000_TDT] = 0;
  
  // [E1000 14.4] Receive initialization
  memset(rx_ring, 0, sizeof(rx_ring));
  for (i = 0; i < RX_RING_SIZE; i++) {
    rx_mbufs[i] = mbufalloc(0);
    if (!rx_mbufs[i])
      panic("e1000");
    rx_ring[i].addr = (uint64) rx_mbufs[i]->head;
  }
  regs[E1000_RDBAL] = (uint64) rx_ring;
  if(sizeof(rx_ring) % 128 != 0)
    panic("e1000");
  regs[E1000_RDH] = 0;
  regs[E1000_RDT] = RX_RING_SIZE - 1;
  regs[E1000_RDLEN] = sizeof(rx_ring);

  // filter by qemu's MAC address, 52:54:00:12:34:56
  regs[E1000_RA] = 0x12005452;
  regs[E1000_RA+1] = 0x5634 | (1<<31);
  // multicast table
  for (int i = 0; i < 4096/32; i++)
    regs[E1000_MTA + i] = 0;

  // transmitter control bits.
  regs[E1000_TCTL] = E1000_TCTL_EN |  // enable
    E1000_TCTL_PSP |                  // pad short packets
    (0x10 << E1000_TCTL_CT_SHIFT) |   // collision stuff
    (0x40 << E1000_TCTL_COLD_SHIFT);
  regs[E1000_TIPG] = 10 | (8<<10) | (6<<20); // inter-pkt gap

  // receiver control bits.
  regs[E1000_RCTL] = E1000_RCTL_EN | // enable receiver
    E1000_RCTL_BAM |                 // enable broadcast
    E1000_RCTL_SZ_2048 |             // 2048-byte rx buffers
    E1000_RCTL_SECRC;                // strip CRC
  
  // ask e1000 for receive interrupts.
  regs[E1000_RDTR] = 0; // interrupt after every received packet (no timer)
  regs[E1000_RADV] = 0; // interrupt after every packet (no timer)
  regs[E1000_IMS] = (1 << 0) | (1 << 7); // RXDW -- Receiver Descriptor Write Back
  // Enable transmit completion interrupt for challenge 1
  regs[E1000_IMS] |= (1 << 1); // TXDW
}

static void
tx_refill(void)
{
  // 必须持有 tx_lock
  while (tx_softq_head != tx_softq_tail) {
    int idx = regs[E1000_TDT] & (TX_RING_SIZE - 1);
    if (!(tx_ring[idx].status & E1000_TXD_STAT_DD)) {
      // 硬件环满，无法继续填充
      break;
    }
    // 从软件队列取出一个 mbuf
    struct mbuf *m = tx_softq[tx_softq_head];
    tx_softq_head = (tx_softq_head + 1) % TX_SOFTQ_SIZE;
    // 如果该槽位有旧 mbuf 残留
    if (tx_mbufs[idx]) {
      mbuffree(tx_mbufs[idx]);
      tx_mbufs[idx] = 0;
    }
    // 填充描述符
    tx_ring[idx].addr = (uint64)m->head;
    tx_ring[idx].length = m->len;
    tx_ring[idx].cmd = E1000_TXD_CMD_EOP | E1000_TXD_CMD_RS;
    tx_ring[idx].status = 0;
    tx_mbufs[idx] = m;
    // 更新 TDT
    regs[E1000_TDT] = (idx + 1) % TX_RING_SIZE;
  }
}

int
e1000_transmit(struct mbuf *m)
{
  acquire(&tx_lock);

  // Enqueue the mbuf into software queue if not full.
  int next_tail = (tx_softq_tail + 1) % TX_SOFTQ_SIZE;
  if (next_tail == tx_softq_head) {
    // Software queue full, drop the packet.
    //printf("e1000_transmit: soft queue full, dropping\n");
    release(&tx_lock);
    return -1;
  }
  tx_softq[tx_softq_tail] = m;
  tx_softq_tail = next_tail;

  // Try to push as many queued mbufs as possible to the hardware ring.
  tx_refill();

  release(&tx_lock);
  return 0;
}

void
e1000_recv(void)
{
  acquire(&rx_lock);

  uint32 rdt_val = regs[E1000_RDT];
  //uint32 rdh_val = regs[E1000_RDH];
  int idx = (rdt_val + 1) % RX_RING_SIZE;
  //printf("e1000_recv: RDT=%d RDH=%d idx=%d status=%x\n", rdt_val, rdh_val, idx, rx_ring[idx].status);

  while (rx_ring[idx].status & E1000_RXD_STAT_DD) {
    //printf("e1000_recv: processing idx=%d len=%d\n", idx, rx_ring[idx].length);
    struct mbuf *m = rx_mbufs[idx];
    if (!m) {
      panic("e1000_recv: no mbuf");
    }

    // 设置接收到的数据长度
    m->len = rx_ring[idx].length;

    // 将数据包交给网络协议栈（net_rx 会负责最终释放 mbuf）
    net_rx(m);

    // 分配新的 mbuf 替换已用掉的
    struct mbuf *new_m = mbufalloc(0);
    if (!new_m) {
      // 分配失败，无法继续接收，内核 panic（或者尝试复用旧 mbuf，但这里简单处理）
      panic("e1000_recv: mbufalloc failed");
    }
    rx_mbufs[idx] = new_m;
    rx_ring[idx].addr = (uint64)new_m->head;
    rx_ring[idx].status = 0;   // 清除状态，使硬件可以再次使用

    // 更新 RDT 为当前已处理的描述符索引
    regs[E1000_RDT] = idx;
    // 验证写入是否成功
    uint32 check = regs[E1000_RDT];
    if (check != idx) {
      //printf("e1000_recv: RDT write failed, wrote %d got %d\n", idx, check);
      regs[E1000_RDT] = idx; // 重试一次
    }

    // 移动到下一个描述符
    idx = (idx + 1) % RX_RING_SIZE;
  }

  release(&rx_lock);
}

void
e1000_intr(void)
{
  uint32 icr = regs[E1000_ICR];
  //printf("e1000_intr: icr=%x\n", icr);
  // tell the e1000 we've seen this interrupt;
  // without this the e1000 won't raise any
  // further interrupts.
  regs[E1000_ICR] = 0xffffffff;

  if (icr & ((1 << 0) | (1 << 7))) {
    e1000_recv();
  }
  if (icr & (1 << 1)) {
    acquire(&tx_lock);
    tx_refill();
    release(&tx_lock);
  }
}
