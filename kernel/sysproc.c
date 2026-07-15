#include "types.h"
#include "riscv.h"
#include "defs.h"
#include "date.h"
#include "param.h"
#include "memlayout.h"
#include "spinlock.h"
#include "proc.h"

uint64
sys_exit(void)
{
  int n;
  if(argint(0, &n) < 0)
    return -1;
  exit(n);
  return 0;  // not reached
}

uint64
sys_getpid(void)
{
  return myproc()->pid;
}

uint64
sys_fork(void)
{
  return fork();
}

uint64
sys_wait(void)
{
  uint64 p;
  if(argaddr(0, &p) < 0)
    return -1;
  return wait(p);
}

uint64
sys_sbrk(void)
{
  int n;
  uint64 oldsz = myproc()->sz;
  
  if(argint(0, &n) < 0)
    return -1;
  
  uint64 newsz = oldsz + n;
  
  // 检查溢出或超出最大地址
  if(newsz > MAXVA || (n > 0 && newsz < oldsz) || (n < 0 && newsz > oldsz))
    return -1;
  
  if(n < 0) {
    // 缩小内存：释放从新大小到旧大小之间已映射的页
    // 释放范围：[PGROUNDUP(newsz), PGROUNDUP(oldsz))
    uint64 start = PGROUNDUP(newsz);
    uint64 end = PGROUNDUP(oldsz);
    if(start < end) {
      uvmunmap(myproc()->pagetable, start, (end - start) / PGSIZE, 1);
    }
  }
  
  myproc()->sz = newsz;
  return oldsz;
}

uint64
sys_sleep(void)
{
  int n;
  uint ticks0;

  if(argint(0, &n) < 0)
    return -1;
  acquire(&tickslock);
  ticks0 = ticks;
  while(ticks - ticks0 < n){
    if(myproc()->killed){
      release(&tickslock);
      return -1;
    }
    sleep(&ticks, &tickslock);
  }
  release(&tickslock);
  return 0;
}

uint64
sys_kill(void)
{
  int pid;

  if(argint(0, &pid) < 0)
    return -1;
  return kill(pid);
}

// return how many clock tick interrupts have occurred
// since start.
uint64
sys_uptime(void)
{
  uint xticks;

  acquire(&tickslock);
  xticks = ticks;
  release(&tickslock);
  return xticks;
}
