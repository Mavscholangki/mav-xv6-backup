#include "types.h"
#include "param.h"
#include "memlayout.h"
#include "riscv.h"
#include "spinlock.h"
#include "proc.h"
#include "defs.h"

struct spinlock tickslock;
uint ticks;

extern char trampoline[], uservec[], userret[];

// in kernelvec.S, calls kerneltrap().
void kernelvec();

extern int devintr();

void
trapinit(void)
{
  initlock(&tickslock, "time");
}

// set up to take exceptions and traps while in the kernel.
void
trapinithart(void)
{
  w_stvec((uint64)kernelvec);
}

//
// handle an interrupt, exception, or system call from user space.
// called from trampoline.S
//
void
usertrap(void)
{
  int which_dev = 0;

  if((r_sstatus() & SSTATUS_SPP) != 0)
    panic("usertrap: not from user mode");

  // send interrupts and exceptions to kerneltrap(),
  // since we're now in the kernel.
  w_stvec((uint64)kernelvec);

  struct proc *p = myproc();
  
  // save user program counter.
  p->trapframe->epc = r_sepc();
  
  if(r_scause() == 8){
    // system call

    if(p->killed)
      exit(-1);

    // sepc points to the ecall instruction,
    // but we want to return to the next instruction.
    p->trapframe->epc += 4;

    // an interrupt will change sstatus &c registers,
    // so don't enable until done with those registers.
    intr_on();

    syscall();
  } else if(r_scause() == 13 || r_scause() == 15) {
    uint64 va = r_stval();
    uint64 va_aligned = PGROUNDDOWN(va);
    uint64 stack_bottom = PGROUNDDOWN(p->trapframe->sp);

    if(va >= p->sz) {
      p->killed = 1;
    } else if(va >= stack_bottom - PGSIZE && va < stack_bottom + PGSIZE) {
      // 保护页或栈页，杀死进程
      p->killed = 1;
    } else {
      // 堆区，尝试惰性分配
      pte_t *pte = walk(p->pagetable, va_aligned, 0);
      if(pte && (*pte & PTE_V)) {
        // 已经映射，无需操作
      } else {
        char *mem = kalloc();
        if(mem == 0) {
          p->killed = 1;
        } else {
          memset(mem, 0, PGSIZE);
          if(mappages(p->pagetable, va_aligned, PGSIZE, (uint64)mem, PTE_W|PTE_R|PTE_X|PTE_U) != 0){
            kfree(mem);
            p->killed = 1;
          }
        }
      }
    }
  } else if((which_dev = devintr()) != 0){
    // ok
  } else {
    printf("usertrap(): unexpected scause %p pid=%d\n", r_scause(), p->pid);
    printf("            sepc=%p stval=%p\n", r_sepc(), r_stval());
    p->killed = 1;
  }

  if(p->killed)
    exit(-1);

  // give up the CPU if this is a timer interrupt.
  if(which_dev == 2)
    yield();

  usertrapret();
}

//
// return to user space
//
void
usertrapret(void)
{
  struct proc *p = myproc();

  // we're about to switch the destination of traps from
  // kerneltrap() to usertrap(), so turn off interrupts until
  // we're back in user space, where usertrap() is correct.
  intr_off();

  // send syscalls, interrupts, and exceptions to trampoline.S
  w_stvec(TRAMPOLINE + (uservec - trampoline));

  // set up trapframe values that uservec will need when
  // the process next re-enters the kernel.
  p->trapframe->kernel_satp = r_satp();         // kernel page table
  p->trapframe->kernel_sp = p->kstack + PGSIZE; // process's kernel stack
  p->trapframe->kernel_trap = (uint64)usertrap;
  p->trapframe->kernel_hartid = r_tp();         // hartid for cpuid()

  // set up the registers that trampoline.S's sret will use
  // to get to user space.
  
  // set S Previous Privilege mode to User.
  unsigned long x = r_sstatus();
  x &= ~SSTATUS_SPP; // clear SPP to 0 for user mode
  x |= SSTATUS_SPIE; // enable interrupts in user mode
  w_sstatus(x);

  // set S Exception Program Counter to the saved user pc.
  w_sepc(p->trapframe->epc);

  // tell trampoline.S the user page table to switch to.
  uint64 satp = MAKE_SATP(p->pagetable);

  // jump to trampoline.S at the top of memory, which 
  // switches to the user page table, restores user registers,
  // and switches to user mode with sret.
  uint64 fn = TRAMPOLINE + (userret - trampoline);
  ((void (*)(uint64,uint64))fn)(TRAPFRAME, satp);
}

// interrupts and exceptions from kernel code go here via kernelvec,
// on whatever the current kernel stack is.
void 
kerneltrap()
{
  int which_dev = 0;
  uint64 sepc = r_sepc();
  uint64 sstatus = r_sstatus();
  uint64 scause = r_scause();
  
  if((sstatus & SSTATUS_SPP) == 0)
    panic("kerneltrap: not from supervisor mode");
  if(intr_get() != 0)
    panic("kerneltrap: interrupts enabled");

  // 处理内核态访问用户地址时发生的缺页（用于 lazy allocation + simple copyin）
  if (scause == 13 || scause == 15) {
    struct proc *p = myproc();
    uint64 va = r_stval();
    if (p != 0 && va < p->sz && va < CLINT) {
      // 排除栈区域（栈已映射，不应缺页）
      uint64 stack_bottom = PGROUNDDOWN(p->trapframe->sp);
      if (!(va >= stack_bottom - PGSIZE && va < stack_bottom + PGSIZE)) {
        uint64 va_aligned = PGROUNDDOWN(va);
        // 检查用户页表是否已有映射
        pte_t *pte = walk(p->pagetable, va_aligned, 0);
        if (pte != 0 && (*pte & PTE_V)) {
          // 用户页表已映射，但内核页表可能未映射
          // 检查内核页表是否已映射，若未映射则建立映射
          pte_t *kpte = walk(p->kpgtbl, va_aligned, 0);
          if (kpte == 0 || (*kpte & PTE_V) == 0) {
            uint64 pa = PTE2PA(*pte);
            uint flags = PTE_FLAGS(*pte) & ~PTE_U; // 内核页表不需要 U 位
            if (mappages(p->kpgtbl, va_aligned, PGSIZE, pa, flags) != 0)
              panic("kerneltrap: mappages kernel for existing user page failed");
          }
          // 映射建立完成，返回重新执行
          return;
        } else {
          // 用户页表未映射，进行惰性分配
          char *mem = kalloc();
          if (mem == 0) {
            panic("kerneltrap: lazy alloc failed");
          }
          memset(mem, 0, PGSIZE);
          // 映射到用户页表
          if (mappages(p->pagetable, va_aligned, PGSIZE, (uint64)mem, PTE_W|PTE_R|PTE_U) != 0) {
            kfree(mem);
            panic("kerneltrap: mappages user failed");
          }
          // 映射到内核页表
          if (mappages(p->kpgtbl, va_aligned, PGSIZE, (uint64)mem, PTE_W|PTE_R) != 0) {
            // 回退：清除用户映射，释放物理页
            uvmunmap(p->pagetable, va_aligned, 1, 1);
            panic("kerneltrap: mappages kernel failed");
          }
          // 分配完成，返回重新执行
          return;
        }
      }
    }
    // 非法地址或不合法情况，继续下面的处理（通常会 panic）
  }

  if((which_dev = devintr()) == 0){
    printf("scause %p\n", scause);
    printf("sepc=%p stval=%p\n", r_sepc(), r_stval());
    panic("kerneltrap");
  }

  // give up the CPU if this is a timer interrupt.
  if(which_dev == 2 && myproc() != 0 && myproc()->state == RUNNING)
    yield();

  // the yield() may have caused some traps to occur,
  // so restore trap registers for use by kernelvec.S's sepc instruction.
  w_sepc(sepc);
  w_sstatus(sstatus);
}

void
clockintr()
{
  acquire(&tickslock);
  ticks++;
  wakeup(&ticks);
  release(&tickslock);
}

// check if it's an external interrupt or software interrupt,
// and handle it.
// returns 2 if timer interrupt,
// 1 if other device,
// 0 if not recognized.
int
devintr()
{
  uint64 scause = r_scause();

  if((scause & 0x8000000000000000L) &&
     (scause & 0xff) == 9){
    // this is a supervisor external interrupt, via PLIC.

    // irq indicates which device interrupted.
    int irq = plic_claim();

    if(irq == UART0_IRQ){
      uartintr();
    } else if(irq == VIRTIO0_IRQ){
      virtio_disk_intr();
    } else if(irq){
      printf("unexpected interrupt irq=%d\n", irq);
    }

    // the PLIC allows each device to raise at most one
    // interrupt at a time; tell the PLIC the device is
    // now allowed to interrupt again.
    if(irq)
      plic_complete(irq);

    return 1;
  } else if(scause == 0x8000000000000001L){
    // software interrupt from a machine-mode timer interrupt,
    // forwarded by timervec in kernelvec.S.

    if(cpuid() == 0){
      clockintr();
    }
    
    // acknowledge the software interrupt by clearing
    // the SSIP bit in sip.
    w_sip(r_sip() & ~2);

    return 2;
  } else {
    return 0;
  }
}

