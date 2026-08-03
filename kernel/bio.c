// Buffer cache.
//
// The buffer cache is a linked list of buf structures holding
// cached copies of disk block contents.  Caching disk blocks
// in memory reduces the number of disk reads and also provides
// a synchronization point for disk blocks used by multiple processes.
//
// Interface:
// * To get a buffer for a particular disk block, call bread.
// * After changing buffer data, call bwrite to write it to disk.
// * When done with the buffer, call brelse.
// * Do not use the buffer after calling brelse.
// * Only one process at a time can use a buffer,
//     so do not keep them longer than necessary.

#include "types.h"
#include "param.h"
#include "spinlock.h"
#include "sleeplock.h"
#include "riscv.h"
#include "defs.h"
#include "fs.h"
#include "buf.h"

struct buf buf[NBUF];   // 所有缓冲区

#define NBUCKET 13
struct bucket {
  struct spinlock lock;
  struct buf head;   // 链表头
} buckets[NBUCKET];

struct spinlock evict_lock;
extern uint ticks;

void
binit(void)
{
  struct buf *b;

  initlock(&evict_lock, "bcache.evict");

  for (int i = 0; i < NBUCKET; i++) {
    initlock(&buckets[i].lock, "bcache.bucket");
    buckets[i].head.next = &buckets[i].head;
    buckets[i].head.prev = &buckets[i].head;
  }

  // 将缓冲区分配到各个桶
  for (b = buf; b < buf + NBUF; b++) {
    int bucket = (b - buf) % NBUCKET;
    b->next = buckets[bucket].head.next;
    b->prev = &buckets[bucket].head;
    buckets[bucket].head.next->prev = b;
    buckets[bucket].head.next = b;
    b->timestamp = 0;
    b->refcnt = 0;
    b->dev = -1;
    b->blockno = -1;
    initsleeplock(&b->lock, "buffer");
  }
}

static struct buf*
bget(uint dev, uint blockno)
{
  struct buf *b;
  int bucket = blockno % NBUCKET;

  // 第一次查找：持桶锁，安全遍历
  acquire(&buckets[bucket].lock);
  for (b = buckets[bucket].head.next; b != &buckets[bucket].head; b = b->next) {
    if (b->dev == dev && b->blockno == blockno) {
      b->refcnt++;
      b->timestamp = ticks;
      release(&buckets[bucket].lock);
      acquiresleep(&b->lock);
      return b;
    }
  }
  release(&buckets[bucket].lock);

  // 未命中，需要替换（以下路径持锁）
  acquire(&evict_lock);

  // 第二次查找（持桶锁，防止其他进程已插入）
  acquire(&buckets[bucket].lock);
  for (b = buckets[bucket].head.next; b != &buckets[bucket].head; b = b->next) {
    if (b->dev == dev && b->blockno == blockno) {
      b->refcnt++;
      b->timestamp = ticks;
      release(&buckets[bucket].lock);
      release(&evict_lock);
      acquiresleep(&b->lock);
      return b;
    }
  }
  release(&buckets[bucket].lock);

  // --- 替换逻辑开始 ---
  struct buf *victim = 0;
  uint oldest;
  struct bucket *victim_bucket = 0;
  struct bucket *target_bucket = &buckets[bucket];

  while (1) {
    // 1. 扫描所有桶，选择 refcnt==0 且 timestamp 最小的
    victim = 0;
    oldest = 0xffffffff;
    for (int i = 0; i < NBUCKET; i++) {
      acquire(&buckets[i].lock);
      for (b = buckets[i].head.next; b != &buckets[i].head; b = b->next) {
        if (b->refcnt == 0 && b->timestamp < oldest) {
          oldest = b->timestamp;
          victim = b;
          victim_bucket = &buckets[i];
        }
      }
      release(&buckets[i].lock);
    }
    if (!victim)
      panic("bget: no buffers");

    // 2. 按地址顺序加锁 victim_bucket 和 target_bucket
    if (victim_bucket == target_bucket) {
      acquire(&victim_bucket->lock);
    } else if (victim_bucket < target_bucket) {
      acquire(&victim_bucket->lock);
      acquire(&target_bucket->lock);
    } else {
      acquire(&target_bucket->lock);
      acquire(&victim_bucket->lock);
    }

    // 3. 原子地将 victim->refcnt 从 0 改为 -1（表示正在淘汰）
    if (__sync_val_compare_and_swap(&victim->refcnt, 0, -1) == 0) {
      // 成功置为 -1，跳出循环
      break;
    } else {
      // 失败，释放锁并重试
      if (victim_bucket == target_bucket) {
        release(&victim_bucket->lock);
      } else if (victim_bucket < target_bucket) {
        release(&target_bucket->lock);
        release(&victim_bucket->lock);
      } else {
        release(&victim_bucket->lock);
        release(&target_bucket->lock);
      }
      // 继续循环，重新选择受害者
    }
  }

  // 现在 victim 可用且 refcnt 已被置为 -1，已持有必要的锁
  // 从 victim_bucket 中移除 victim
  victim->prev->next = victim->next;
  victim->next->prev = victim->prev;

  // 插入到 target_bucket 头部
  victim->next = target_bucket->head.next;
  victim->prev = &target_bucket->head;
  target_bucket->head.next->prev = victim;
  target_bucket->head.next = victim;

  // 更新块信息
  victim->dev = dev;
  victim->blockno = blockno;
  victim->timestamp = ticks;
  victim->valid = 0;
  // 将 refcnt 设为 1（表示新块已被引用）
  __sync_lock_test_and_set(&victim->refcnt, 1);

  // 释放锁（逆序）
  if (victim_bucket == target_bucket) {
    release(&victim_bucket->lock);
  } else if (victim_bucket < target_bucket) {
    release(&target_bucket->lock);
    release(&victim_bucket->lock);
  } else {
    release(&victim_bucket->lock);
    release(&target_bucket->lock);
  }

  release(&evict_lock);

  acquiresleep(&victim->lock);
  return victim;
}

// Return a locked buf with the contents of the indicated block.
struct buf*
bread(uint dev, uint blockno)
{
  struct buf *b;

  b = bget(dev, blockno);
  if(!b->valid) {
    virtio_disk_rw(b, 0);
    b->valid = 1;
  }
  return b;
}

// Write b's contents to disk.  Must be locked.
void
bwrite(struct buf *b)
{
  if(!holdingsleep(&b->lock))
    panic("bwrite");
  virtio_disk_rw(b, 1);
}

// Release a locked buffer.
// Now uses atomic operations and no bucket lock.
void
brelse(struct buf *b)
{
  if(!holdingsleep(&b->lock))
    panic("brelse");

  int new = __sync_fetch_and_sub(&b->refcnt, 1);
  if (new == 0) {  // 原值 1，减后变为 0
    b->timestamp = ticks;   // 非原子，可接受
  }

  releasesleep(&b->lock);
}

// Increment reference count without locking.
void
bpin(struct buf *b) {
  __sync_fetch_and_add(&b->refcnt, 1);
}

// Decrement reference count without locking.
void
bunpin(struct buf *b) {
  __sync_fetch_and_sub(&b->refcnt, 1);
}
