#include "kernel/types.h"
#include "kernel/stat.h"
#include "user/user.h"

/* Possible states of a thread: */
#define FREE        0x0
#define RUNNING     0x1
#define RUNNABLE    0x2
#define BLOCKED     0x3

#define STACK_SIZE  8192
#define MAX_THREAD  4

// 保存被调用者保存的寄存器 (callee-saved)
struct context {
  uint64 ra;   // 返回地址
  uint64 sp;   // 栈指针
  // 以下为 RISC-V 的 callee-saved 寄存器
  uint64 s0;
  uint64 s1;
  uint64 s2;
  uint64 s3;
  uint64 s4;
  uint64 s5;
  uint64 s6;
  uint64 s7;
  uint64 s8;
  uint64 s9;
  uint64 s10;
  uint64 s11;
};

struct thread {
  struct context context;
  char       stack[STACK_SIZE]; /* the thread's stack */
  int        state;             /* FREE, RUNNING, RUNNABLE */

};

// 锁结构
struct lock {
    int locked;                         // 1 if held
    struct thread *waiting[MAX_THREAD]; // 等待该锁的线程队列
    int nwaiting;
};

// 条件变量结构
struct condition {
    struct thread *waiting[MAX_THREAD];
    int nwaiting;
};

// 屏障结构
struct barrier {
    struct lock lock;
    struct condition cond;
    int nthread;        // 总线程数（由初始化时设定）
    int count;          // 当前轮已到达的线程数
    int round;          // 当前轮次编号
};

struct thread all_thread[MAX_THREAD];
struct thread *current_thread;
extern void thread_switch(struct context*, struct context*);
              
void 
thread_init(void)
{
  // main() is thread 0, which will make the first invocation to
  // thread_schedule().  it needs a stack so that the first thread_switch() can
  // save thread 0's state.  thread_schedule() won't run the main thread ever
  // again, because its state is set to RUNNING, and thread_schedule() selects
  // a RUNNABLE thread.
  current_thread = &all_thread[0];
  current_thread->state = RUNNING;
}

void 
thread_schedule(void)
{
  struct thread *t, *next_thread;

  /* Find another runnable thread. */
  next_thread = 0;
  t = current_thread + 1;
  for(int i = 0; i < MAX_THREAD; i++){
    if(t >= all_thread + MAX_THREAD)
      t = all_thread;
    if(t->state == RUNNABLE) {
      next_thread = t;
      break;
    }
    t = t + 1;
  }

  if (next_thread == 0) {
    printf("thread_schedule: no runnable threads\n");
    exit(-1);
  }

  if (current_thread != next_thread) {         /* switch threads?  */
    struct thread *prev = current_thread;
    current_thread = next_thread;
    // 如果 prev == 0，表示这是第一次调度，不需要保存旧上下文
    if (prev == 0) {
      thread_switch(0, &next_thread->context);
    } else {
      thread_switch(&prev->context, &next_thread->context);
    }
  } else
    next_thread = 0;
}

void 
thread_create(void (*func)())
{
  struct thread *t;

  for (t = all_thread; t < all_thread + MAX_THREAD; t++) {
    if (t->state == FREE) break;
  }
  t->state = RUNNABLE;

  // 初始化上下文
  t->context.ra = (uint64) func;          // 返回地址设为线程函数入口
  t->context.sp = (uint64) (t->stack + STACK_SIZE); // 栈顶（栈向下增长）
}

void 
thread_yield(void)
{
  current_thread->state = RUNNABLE;
  thread_schedule();
}

void 
thread_exit(void)
{
  current_thread->state = FREE;
  thread_schedule();  // 切换到下一个线程，不会再回到这里
}

volatile int a_started, b_started, c_started;
volatile int a_n, b_n, c_n;

void 
thread_a(void)
{
  int i;
  printf("thread_a started\n");
  a_started = 1;
  while(b_started == 0 || c_started == 0)
    thread_yield();
  
  for (i = 0; i < 100; i++) {
    printf("thread_a %d\n", i);
    a_n += 1;
    thread_yield();
  }
  printf("thread_a: exit after %d\n", a_n);

  current_thread->state = FREE;
  thread_schedule();
}

void 
thread_b(void)
{
  int i;
  printf("thread_b started\n");
  b_started = 1;
  while(a_started == 0 || c_started == 0)
    thread_yield();
  
  for (i = 0; i < 100; i++) {
    printf("thread_b %d\n", i);
    b_n += 1;
    thread_yield();
  }
  printf("thread_b: exit after %d\n", b_n);

  current_thread->state = FREE;
  thread_schedule();
}

void 
thread_c(void)
{
  int i;
  printf("thread_c started\n");
  c_started = 1;
  while(a_started == 0 || b_started == 0)
    thread_yield();
  
  for (i = 0; i < 100; i++) {
    printf("thread_c %d\n", i);
    c_n += 1;
    thread_yield();
  }
  printf("thread_c: exit after %d\n", c_n);

  current_thread->state = FREE;
  thread_schedule();
}

// 初始化锁
void
lock_init(struct lock *lk)
{
    lk->locked = 0;
    lk->nwaiting = 0;
}

// 获取锁
void
lock_acquire(struct lock *lk)
{
    while (lk->locked) {
        // 锁被占用，将当前线程加入等待队列并阻塞
        if (lk->nwaiting < MAX_THREAD) {
            lk->waiting[lk->nwaiting++] = current_thread;
        }
        current_thread->state = BLOCKED;
        thread_schedule(); // 让出CPU，不会立即返回
        // 当被唤醒后，循环重新检查锁状态
    }
    // 成功获取锁
    lk->locked = 1;
}

// 释放锁
void
lock_release(struct lock *lk)
{
    lk->locked = 0;
    // 唤醒一个等待线程（如果有）
    if (lk->nwaiting > 0) {
        struct thread *t = lk->waiting[0];
        // 将队列中所有线程前移
        for (int i = 0; i < lk->nwaiting - 1; i++)
            lk->waiting[i] = lk->waiting[i+1];
        lk->nwaiting--;
        t->state = RUNNABLE;   // 唤醒
    }
}

// 初始化条件变量
void
cond_init(struct condition *cond)
{
    cond->nwaiting = 0;
}

// 等待条件变量（原子释放锁并阻塞）
void
cond_wait(struct condition *cond, struct lock *lk)
{
    // 1. 将当前线程加入条件变量等待队列，并设置状态为 BLOCKED
    if (cond->nwaiting < MAX_THREAD) {
        cond->waiting[cond->nwaiting++] = current_thread;
    }
    current_thread->state = BLOCKED;

    // 2. 释放锁（但不让出 CPU，因为我们接下来要主动让出）
    lk->locked = 0;
    // 如果有线程在等待这个锁，唤醒第一个（但锁已释放，这些线程会竞争）
    if (lk->nwaiting > 0) {
        struct thread *t = lk->waiting[0];
        for (int i = 0; i < lk->nwaiting - 1; i++)
            lk->waiting[i] = lk->waiting[i+1];
        lk->nwaiting--;
        t->state = RUNNABLE;
    }

    // 3. 让出 CPU（此时当前线程已 BLOCKED，调度器不会选它）
    thread_schedule();

    // 4. 被唤醒后，重新获取锁（可能阻塞）
    lock_acquire(lk);
}

// 唤醒一个等待线程（不释放锁）
void
cond_signal(struct condition *cond)
{
    if (cond->nwaiting > 0) {
        struct thread *t = cond->waiting[0];
        // 队列前移
        for (int i = 0; i < cond->nwaiting - 1; i++)
            cond->waiting[i] = cond->waiting[i+1];
        cond->nwaiting--;
        t->state = RUNNABLE;
    }
}

// 唤醒所有等待线程
void
cond_broadcast(struct condition *cond)
{
    while (cond->nwaiting > 0) {
        struct thread *t = cond->waiting[0];
        for (int i = 0; i < cond->nwaiting - 1; i++)
            cond->waiting[i] = cond->waiting[i+1];
        cond->nwaiting--;
        t->state = RUNNABLE;
    }
}

// 初始化屏障（总线程数由调用者传入）
void
barrier_init(struct barrier *b, int n)
{
    lock_init(&b->lock);
    cond_init(&b->cond);
    b->nthread = n;
    b->count = 0;
    b->round = 0;
}

// 等待屏障
void
barrier_wait(struct barrier *b)
{
    lock_acquire(&b->lock);
    int cur_round = b->round;
    b->count++;
    if (b->count == b->nthread) {
        // 最后一个到达，重置并进入下一轮
        b->count = 0;
        b->round++;
        cond_broadcast(&b->cond);
    } else {
        // 等待，直到轮次变化
        while (b->round == cur_round) {
            cond_wait(&b->cond, &b->lock);
        }
    }
    lock_release(&b->lock);
}

// ==================== 同步原语测试（生产者-消费者） ====================
//
// 测试场景：
//   - 1 个生产者线程，生产 0~99 共 100 个整数
//   - 2 个消费者线程，从共享缓冲区取出整数并累加
//   - 共享缓冲区大小为 1（一个槽位）
//   - 使用锁保护缓冲区和计数，用条件变量实现生产-消费同步

// static struct lock prod_lock;
// static struct condition prod_cond;
// static int shared_buffer;          // 缓冲区（-1 表示空）
// static int produced_count = 0;     // 已生产数量
// static int consumed_count = 0;     // 已消费数量
// static int total_consumed = 0;     // 消费者累加总和
// static int done_producing = 0;     // 生产者是否已完成

// // 生产者线程函数
// static void producer_thread(void) {
//     for (int i = 0; i < 100; i++) {
//         lock_acquire(&prod_lock);
//         // 等待缓冲区为空（buffer == -1）
//         while (shared_buffer != -1) {
//             cond_wait(&prod_cond, &prod_lock); // 释放锁并等待消费者取走数据
//         }
//         // 放入数据
//         shared_buffer = i;
//         produced_count++;
//         // 通知消费者（可能有多个，用广播）
//         cond_broadcast(&prod_cond);
//         lock_release(&prod_lock);
//         // 让出 CPU，让消费者有机会运行
//         thread_yield();
//     }
//     // 生产完成，标记结束并广播唤醒所有等待线程
//     lock_acquire(&prod_lock);
//     done_producing = 1;
//     cond_broadcast(&prod_cond);
//     lock_release(&prod_lock);
//     thread_exit();
// }

// // 由于上面 consumer_thread 有参数，无法直接使用，我们需要重新定义无参版本。
// // 下面提供两个消费者函数，逻辑相同（仅打印信息不同）
// static void consumer_thread1(void) {
//     int local_sum = 0;
//     while (1) {
//         lock_acquire(&prod_lock);
//         while (shared_buffer == -1 && !done_producing) {
//             cond_wait(&prod_cond, &prod_lock);
//         }
//         if (shared_buffer != -1) {
//             int val = shared_buffer;
//             shared_buffer = -1;
//             consumed_count++;
//             local_sum += val;
//             cond_broadcast(&prod_cond);
//             lock_release(&prod_lock);
//             thread_yield();
//         } else if (done_producing && shared_buffer == -1) {
//             lock_release(&prod_lock);
//             break;
//         } else {
//             lock_release(&prod_lock);
//             thread_yield();
//         }
//     }
//     lock_acquire(&prod_lock);
//     total_consumed += local_sum;
//     lock_release(&prod_lock);
//     thread_exit();
// }

// static void consumer_thread2(void) {
//     // 完全相同的逻辑，仅函数名不同
//     int local_sum = 0;
//     while (1) {
//         lock_acquire(&prod_lock);
//         while (shared_buffer == -1 && !done_producing) {
//             cond_wait(&prod_cond, &prod_lock);
//         }
//         if (shared_buffer != -1) {
//             int val = shared_buffer;
//             shared_buffer = -1;
//             consumed_count++;
//             local_sum += val;
//             cond_broadcast(&prod_cond);
//             lock_release(&prod_lock);
//             thread_yield();
//         } else if (done_producing && shared_buffer == -1) {
//             lock_release(&prod_lock);
//             break;
//         } else {
//             lock_release(&prod_lock);
//             thread_yield();
//         }
//     }
//     lock_acquire(&prod_lock);
//     total_consumed += local_sum;
//     lock_release(&prod_lock);
//     thread_exit();
// }

// // 使用无参消费者函数
// void synctest(void) {
//     printf("=== Synchronization Test: Producer-Consumer ===\n");
//     lock_init(&prod_lock);
//     cond_init(&prod_cond);
//     shared_buffer = -1;
//     produced_count = 0;
//     consumed_count = 0;
//     total_consumed = 0;
//     done_producing = 0;

//     thread_create(producer_thread);
//     thread_create(consumer_thread1);
//     thread_create(consumer_thread2);

//     // 主线程变为 RUNNABLE，以便被调度
//     current_thread->state = RUNNABLE;

//     // 循环等待所有工作线程结束（状态为 FREE）
//     while (1) {
//         int all_done = 1;
//         for (int i = 0; i < MAX_THREAD; i++) {
//             struct thread *t = &all_thread[i];
//             // 跳过主线程自身
//             if (t == current_thread) continue;
//             if (t->state != FREE) {
//                 all_done = 0;
//                 break;
//             }
//         }
//         if (all_done) break;
//         thread_yield();   // 让出 CPU，让工作线程运行
//     }

//     // 打印结果
//     printf("Producer produced %d items\n", produced_count);
//     printf("Consumers consumed %d items\n", consumed_count);
//     printf("Total sum of consumed values: %d (expected 4950)\n", total_consumed);
//     if (produced_count == 100 && consumed_count == 100 && total_consumed == 4950) {
//         printf("TEST PASSED!\n");
//     } else {
//         printf("TEST FAILED!\n");
//     }
// }

int 
main(int argc, char *argv[]) 
{
  a_started = b_started = c_started = 0;
  a_n = b_n = c_n = 0;
  thread_init();
  thread_create(thread_a);
  thread_create(thread_b);
  thread_create(thread_c);
  thread_schedule();
  exit(0);
  // // 调用同步测试
  //   thread_init();          // 仍需要初始化 current_thread
  //   synctest();

  //   exit(0);
}
