#include "param.h"
#include "types.h"
#include "memlayout.h"
#include "elf.h"
#include "riscv.h"
#include "defs.h"
#include "fs.h"
#include "spinlock.h"
#include "proc.h"

/*
 * the kernel's page table.
 */
pagetable_t kernel_pagetable;

extern char etext[];  // kernel.ld sets this to end of kernel code.

extern char trampoline[]; // trampoline.S

/*
 * create a direct-map page table for the kernel.
 */
void
kvminit()
{
  kernel_pagetable = (pagetable_t) kalloc();
  memset(kernel_pagetable, 0, PGSIZE);

  // uart registers
  kvmmap(kernel_pagetable, UART0, UART0, PGSIZE, PTE_R | PTE_W);   // MODIFIED

  // virtio mmio disk interface
  kvmmap(kernel_pagetable, VIRTIO0, VIRTIO0, PGSIZE, PTE_R | PTE_W); // MODIFIED

  // CLINT
  kvmmap(kernel_pagetable, CLINT, CLINT, 0x10000, PTE_R | PTE_W);   // MODIFIED

  // PLIC
  kvmmap(kernel_pagetable, PLIC, PLIC, 0x400000, PTE_R | PTE_W);    // MODIFIED

  // map kernel text executable and read-only.
  kvmmap(kernel_pagetable, KERNBASE, KERNBASE, (uint64)etext-KERNBASE, PTE_R | PTE_X); // MODIFIED

  // map kernel data and the physical RAM we'll make use of.
  kvmmap(kernel_pagetable, (uint64)etext, (uint64)etext, PHYSTOP-(uint64)etext, PTE_R | PTE_W); // MODIFIED

  // map the trampoline for trap entry/exit to
  // the highest virtual address in the kernel.
  kvmmap(kernel_pagetable, TRAMPOLINE, (uint64)trampoline, PGSIZE, PTE_R | PTE_X); // MODIFIED
}

// create a new kernel page table with the same mappings as kernel_pagetable
pagetable_t kvmmake(void) {
    pagetable_t kpgtbl = (pagetable_t) kalloc();
    if (kpgtbl == 0) panic("kvmmake: kalloc");
    memset(kpgtbl, 0, PGSIZE);

    // 原有映射（设备、内核文本/数据、trampoline）
    kvmmap(kpgtbl, UART0, UART0, PGSIZE, PTE_R | PTE_W);
    kvmmap(kpgtbl, VIRTIO0, VIRTIO0, PGSIZE, PTE_R | PTE_W);
    kvmmap(kpgtbl, CLINT, CLINT, 0x10000, PTE_R | PTE_W);
    kvmmap(kpgtbl, PLIC, PLIC, 0x400000, PTE_R | PTE_W);
    kvmmap(kpgtbl, KERNBASE, KERNBASE, (uint64)etext - KERNBASE, PTE_R | PTE_X);
    kvmmap(kpgtbl, (uint64)etext, (uint64)etext, PHYSTOP - (uint64)etext, PTE_R | PTE_W);
    kvmmap(kpgtbl, TRAMPOLINE, (uint64)trampoline, PGSIZE, PTE_R | PTE_X);

    // 映射所有 CPU 的全局内核栈
    extern uint64 global_kstack_pa[NCPU];
    for (int i = 0; i < NCPU; i++) {
        uint64 va = KSTACK(i);
        uint64 pa = global_kstack_pa[i];
        kvmmap(kpgtbl, va, pa, PGSIZE, PTE_R | PTE_W);
    }

    return kpgtbl;
}

// Switch h/w page table register to the kernel's page table,
// and enable paging.
void
kvminithart()
{
  w_satp(MAKE_SATP(kernel_pagetable));
  sfence_vma();
}

// Return the address of the PTE in page table pagetable
// that corresponds to virtual address va.  If alloc!=0,
// create any required page-table pages.
//
// The risc-v Sv39 scheme has three levels of page-table
// pages. A page-table page contains 512 64-bit PTEs.
// A 64-bit virtual address is split into five fields:
//   39..63 -- must be zero.
//   30..38 -- 9 bits of level-2 index.
//   21..29 -- 9 bits of level-1 index.
//   12..20 -- 9 bits of level-0 index.
//    0..11 -- 12 bits of byte offset within the page.
pte_t *
walk(pagetable_t pagetable, uint64 va, int alloc)
{
  if(va >= MAXVA)
    panic("walk");

  for(int level = 2; level > 0; level--) {
    pte_t *pte = &pagetable[PX(level, va)];
    if(*pte & PTE_V) {
      pagetable = (pagetable_t)PTE2PA(*pte);
    } else {
      if(!alloc || (pagetable = (pde_t*)kalloc()) == 0)
        return 0;
      memset(pagetable, 0, PGSIZE);
      *pte = PA2PTE(pagetable) | PTE_V;
    }
  }
  return &pagetable[PX(0, va)];
}

// 返回 L1 页表项的指针（不进入 L0），用于大页映射。
// 如果 alloc==1 且 L2 页表项不存在，则分配 L1 页表。
// 如果 L1 页表项无效，则直接返回其指针，由调用者设置大页叶子。
static pte_t *
walk_to_l1(pagetable_t pagetable, uint64 va, int alloc)
{
  if (va >= MAXVA)
    panic("walk_to_l1");

  // Level 2
  pte_t *pte = &pagetable[PX(2, va)];
  if (!(*pte & PTE_V)) {
    if (!alloc)
      return 0;
    if ((pagetable = (pde_t*)kalloc()) == 0)
      return 0;
    memset(pagetable, 0, PGSIZE);
    *pte = PA2PTE(pagetable) | PTE_V;
  } else {
    pagetable = (pagetable_t)PTE2PA(*pte);
  }

  // Level 1
  // 直接返回 L1 的 PTE 指针，即使它无效（调用者会设置它为大页叶子）
  return &pagetable[PX(1, va)];
}

// Look up a virtual address, return the physical address,
// or 0 if not mapped.
// Can only be used to look up user pages.
uint64
walkaddr(pagetable_t pagetable, uint64 va)
{
  pte_t *pte;
  uint64 pa;

  if(va >= MAXVA)
    return 0;

  pte = walk(pagetable, va, 0);
  if(pte == 0)
    return 0;
  if((*pte & PTE_V) == 0)
    return 0;
  if((*pte & PTE_U) == 0)
    return 0;
  pa = PTE2PA(*pte);
  return pa;
}

int mappages_huge(pagetable_t pagetable, uint64 va, uint64 size, uint64 pa, int perm);
// add a mapping to the given page table.
// only used when booting and for per-process kernel page tables.
// does not flush TLB or enable paging.
void
kvmmap(pagetable_t pagetable, uint64 va, uint64 pa, uint64 sz, int perm)
{
  // 检查是否满足大页条件（仅当所有地址和大小都 2MiB 对齐）
  if ((va % HUGE_PGSIZE == 0) && (pa % HUGE_PGSIZE == 0) && (sz % HUGE_PGSIZE == 0) && sz > 0) {
    if (mappages_huge(pagetable, va, sz, pa, perm) != 0)
      panic("kvmmap: huge");
  } else {
    if (mappages(pagetable, va, sz, pa, perm) != 0)
      panic("kvmmap");
  }
}

// Copy user page table mappings from upgtbl to kernel page table kpgtbl
// for virtual addresses [start, end).  start and end should be page-aligned.
void
u2kvmcopy(pagetable_t kpgtbl, pagetable_t upgtbl, uint64 start, uint64 end)
{
  pte_t *pte;
  uint64 pa, va;
  uint flags;
  for (va = PGROUNDUP(start); va < end; va += PGSIZE) {
    if (va >= CLINT) break;   // 不映射高于或等于 CLINT 的地址
    if ((pte = walk(upgtbl, va, 0)) == 0)
      continue;
    if ((*pte & PTE_V) == 0)
      continue;
    if ((*pte & (PTE_R | PTE_W | PTE_X)) == 0)
      continue;
    pa = PTE2PA(*pte);
    flags = PTE_FLAGS(*pte) & ~PTE_U;
    if (mappages(kpgtbl, va, PGSIZE, pa, flags) != 0)
      panic("u2kvmcopy: mappages");
  }
}

// Remove user mappings from kernel page table for [start, end)
void
kvmclear_user(pagetable_t kpgtbl, uint64 start, uint64 end)
{
  pte_t *pte;
  start = PGROUNDUP(start);
  end = PGROUNDUP(end);
  for (uint64 va = start; va < end; va += PGSIZE) {
    if ((pte = walk(kpgtbl, va, 0)) == 0)
      continue;  // not mapped, skip
    if (*pte & PTE_V) {
      *pte = 0;   // just clear, don't free physical page
    }
  }
}

// translate a kernel virtual address to
// a physical address. only needed for
// addresses on the stack.
// assumes va is page aligned.
// translate a kernel virtual address to a physical address.
// uses the current process's kernel page table if in process context,
// otherwise uses the global kernel page table.
// assumes va is page aligned.
uint64
kvmpa(uint64 va)
{
  uint64 off = va % PGSIZE;
  pte_t *pte;
  uint64 pa;
  pagetable_t pagetable = kernel_pagetable;
  struct proc *p = myproc();
  if (p && p->kpgtbl) {
    pagetable = p->kpgtbl;
  }
  pte = walk(pagetable, va, 0);
  if(pte == 0)
    panic("kvmpa");
  if((*pte & PTE_V) == 0)
    panic("kvmpa");
  pa = PTE2PA(*pte);
  return pa+off;
}

// Create PTEs for virtual addresses starting at va that refer to
// physical addresses starting at pa. va and size might not
// be page-aligned. Returns 0 on success, -1 if walk() couldn't
// allocate a needed page-table page.
int
mappages(pagetable_t pagetable, uint64 va, uint64 size, uint64 pa, int perm)
{
  uint64 a, last;
  pte_t *pte;

  a = PGROUNDDOWN(va);
  last = PGROUNDDOWN(va + size - 1);
  for(;;){
    if((pte = walk(pagetable, a, 1)) == 0)
      return -1;
    if(*pte & PTE_V)
      panic("remap");
    *pte = PA2PTE(pa) | perm | PTE_V;
    if(a == last)
      break;
    a += PGSIZE;
    pa += PGSIZE;
  }
  return 0;
}

// 映射大页（2 MiB 对齐），仅用于内核页表。
// va, pa 必须 2 MiB 对齐，size 必须 2 MiB 的整数倍。
// 成功返回 0，失败返回 -1。
int
mappages_huge(pagetable_t pagetable, uint64 va, uint64 size, uint64 pa, int perm)
{
  if (va % HUGE_PGSIZE != 0 || pa % HUGE_PGSIZE != 0 || size % HUGE_PGSIZE != 0)
    panic("mappages_huge: unaligned");

  for (uint64 a = va; a < va + size; a += HUGE_PGSIZE) {
    pte_t *pte = walk_to_l1(pagetable, a, 1);
    if (pte == 0)
      return -1;
    if (*pte & PTE_V)
      panic("mappages_huge: remap");
    *pte = PA2PTE(pa) | perm | PTE_V;
    pa += HUGE_PGSIZE;
  }
  return 0;
}

// Remove npages of mappings starting from va. va must be
// page-aligned. The mappings must exist.
// Optionally free the physical memory.
void
uvmunmap(pagetable_t pagetable, uint64 va, uint64 npages, int do_free)
{
  uint64 a;
  pte_t *pte;

  if((va % PGSIZE) != 0)
    panic("uvmunmap: not aligned");

  for(a = va; a < va + npages*PGSIZE; a += PGSIZE){
    if((pte = walk(pagetable, a, 0)) == 0)
      panic("uvmunmap: walk");
    if((*pte & PTE_V) == 0)
      panic("uvmunmap: not mapped");
    if(PTE_FLAGS(*pte) == PTE_V)
      panic("uvmunmap: not a leaf");
    if(do_free){
      uint64 pa = PTE2PA(*pte);
      kfree((void*)pa);
    }
    *pte = 0;
  }
}

// create an empty user page table.
// returns 0 if out of memory.
pagetable_t
uvmcreate()
{
  pagetable_t pagetable;
  pagetable = (pagetable_t) kalloc();
  if(pagetable == 0)
    return 0;
  memset(pagetable, 0, PGSIZE);
  return pagetable;
}

// Load the user initcode into address 0 of pagetable,
// for the very first process.
// sz must be less than a page.
void
uvminit(pagetable_t pagetable, uchar *src, uint sz)
{
  char *mem;

  if(sz >= PGSIZE)
    panic("inituvm: more than a page");
  mem = kalloc();
  memset(mem, 0, PGSIZE);
  mappages(pagetable, 0, PGSIZE, (uint64)mem, PTE_W|PTE_R|PTE_X|PTE_U);
  memmove(mem, src, sz);
}

// Allocate PTEs and physical memory to grow process from oldsz to
// newsz, which need not be page aligned.  Returns new size or 0 on error.
uint64
uvmalloc(pagetable_t pagetable, uint64 oldsz, uint64 newsz)
{
  char *mem;
  uint64 a;

  if(newsz < oldsz)
    return oldsz;

  oldsz = PGROUNDUP(oldsz);
  for(a = oldsz; a < newsz; a += PGSIZE){
    mem = kalloc();
    if(mem == 0){
      uvmdealloc(pagetable, a, oldsz);
      return 0;
    }
    memset(mem, 0, PGSIZE);
    if(mappages(pagetable, a, PGSIZE, (uint64)mem, PTE_W|PTE_X|PTE_R|PTE_U) != 0){
      kfree(mem);
      uvmdealloc(pagetable, a, oldsz);
      return 0;
    }
  }
  return newsz;
}

// Deallocate user pages to bring the process size from oldsz to
// newsz.  oldsz and newsz need not be page-aligned, nor does newsz
// need to be less than oldsz.  oldsz can be larger than the actual
// process size.  Returns the new process size.
uint64
uvmdealloc(pagetable_t pagetable, uint64 oldsz, uint64 newsz)
{
  if(newsz >= oldsz)
    return oldsz;

  if(PGROUNDUP(newsz) < PGROUNDUP(oldsz)){
    int npages = (PGROUNDUP(oldsz) - PGROUNDUP(newsz)) / PGSIZE;
    uvmunmap(pagetable, PGROUNDUP(newsz), npages, 1);
  }

  return newsz;
}

// Recursively free page-table pages.
// All leaf mappings must already have been removed.
void
freewalk(pagetable_t pagetable)
{
  // there are 2^9 = 512 PTEs in a page table.
  for(int i = 0; i < 512; i++){
    pte_t pte = pagetable[i];
    if((pte & PTE_V) && (pte & (PTE_R|PTE_W|PTE_X)) == 0){
      // this PTE points to a lower-level page table.
      uint64 child = PTE2PA(pte);
      freewalk((pagetable_t)child);
      pagetable[i] = 0;
    } else if(pte & PTE_V){
      panic("freewalk: leaf");
    }
  }
  kfree((void*)pagetable);
}

// Free user memory pages,
// then free page-table pages.
void
uvmfree(pagetable_t pagetable, uint64 sz)
{
  if(sz > 0)
    uvmunmap(pagetable, 0, PGROUNDUP(sz)/PGSIZE, 1);
  freewalk(pagetable);
}

// Given a parent process's page table, copy
// its memory into a child's page table.
// Copies both the page table and the
// physical memory.
// returns 0 on success, -1 on failure.
// frees any allocated pages on failure.
int
uvmcopy(pagetable_t old, pagetable_t new, uint64 sz)
{
  pte_t *pte;
  uint64 pa, i;
  uint flags;
  char *mem;

  for(i = 0; i < sz; i += PGSIZE){
    if((pte = walk(old, i, 0)) == 0)
      panic("uvmcopy: pte should exist");
    if((*pte & PTE_V) == 0)
      panic("uvmcopy: page not present");
    pa = PTE2PA(*pte);
    flags = PTE_FLAGS(*pte);
    if((mem = kalloc()) == 0)
      goto err;
    memmove(mem, (char*)pa, PGSIZE);
    if(mappages(new, i, PGSIZE, (uint64)mem, flags) != 0){
      kfree(mem);
      goto err;
    }
  }
  return 0;

 err:
  uvmunmap(new, 0, i / PGSIZE, 1);
  return -1;
}

// mark a PTE invalid for user access.
// used by exec for the user stack guard page.
void
uvmclear(pagetable_t pagetable, uint64 va)
{
  pte_t *pte;
  
  pte = walk(pagetable, va, 0);
  if(pte == 0)
    panic("uvmclear");
  *pte &= ~PTE_U;
}

// Copy from kernel to user.
// Copy len bytes from src to virtual address dstva in a given page table.
// Return 0 on success, -1 on error.
int
copyout(pagetable_t pagetable, uint64 dstva, char *src, uint64 len)
{
  uint64 n, va0, pa0;

  while(len > 0){
    va0 = PGROUNDDOWN(dstva);
    pa0 = walkaddr(pagetable, va0);
    if(pa0 == 0)
      return -1;
    n = PGSIZE - (dstva - va0);
    if(n > len)
      n = len;
    memmove((void *)(pa0 + (dstva - va0)), src, n);

    len -= n;
    src += n;
    dstva = va0 + PGSIZE;
  }
  return 0;
}

// Copy from user to kernel.
// Copy len bytes to dst from virtual address srcva in a given page table.
// Return 0 on success, -1 on error.
int
copyin(pagetable_t pagetable, char *dst, uint64 srcva, uint64 len)
{
  return copyin_new(pagetable, dst, srcva, len);
}

// Copy a null-terminated string from user to kernel.
// Copy bytes to dst from virtual address srcva in a given page table,
// until a '\0', or max.
// Return 0 on success, -1 on error.
int
copyinstr(pagetable_t pagetable, char *dst, uint64 srcva, uint64 max)
{
  return copyinstr_new(pagetable, dst, srcva, max);
}

// 递归打印页表，depth 表示当前层级（0=顶层，1=中间，2=底层）
static void vmprint_level(pagetable_t pagetable, int depth) {
  for (int i = 0; i < 512; i++) {
    pte_t pte = pagetable[i];
    if (!(pte & PTE_V))
      continue;

    // 缩进，格式：每个层级用 ".." 表示，层级间一个空格
    for (int j = 0; j <= depth; j++) {
      printf("..");
      if (j < depth)
        printf(" ");
    }
    // 打印索引，然后直接打印 pte 和 pa，后面必须带换行
    printf("%d: pte %p pa %p\n", i, pte, PTE2PA(pte));

    // 如果是内部页表，递归
    if ((pte & (PTE_R | PTE_W | PTE_X)) == 0) {
      uint64 child_pa = PTE2PA(pte);
      vmprint_level((pagetable_t)child_pa, depth + 1);
    }
  }
}

void vmprint(pagetable_t pagetable) {
  printf("page table %p\n", pagetable);
  vmprint_level(pagetable, 0);
}

// free a kernel page table, but do NOT free leaf physical pages
void
free_kernel_pagetable(pagetable_t pagetable)
{
  for(int i = 0; i < 512; i++){
    pte_t pte = pagetable[i];
    if(!(pte & PTE_V))
      continue;
    // internal page table
    if((pte & (PTE_R | PTE_W | PTE_X)) == 0){
      uint64 child = PTE2PA(pte);
      free_kernel_pagetable((pagetable_t)child);
      pagetable[i] = 0;
    } else {
      // leaf: just clear PTE, do not free physical memory
      pagetable[i] = 0;
    }
  }
  kfree((void*)pagetable);
}