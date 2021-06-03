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

// Make a direct-map page table for the kernel.
pagetable_t
kvmmake(void)
{
  pagetable_t kpgtbl;

  kpgtbl = (pagetable_t) kalloc();
  memset(kpgtbl, 0, PGSIZE);

  // uart registers
  kvmmap(kpgtbl, UART0, UART0, PGSIZE, PTE_R | PTE_W);

  // virtio mmio disk interface
  kvmmap(kpgtbl, VIRTIO0, VIRTIO0, PGSIZE, PTE_R | PTE_W);

  // PLIC
  kvmmap(kpgtbl, PLIC, PLIC, 0x400000, PTE_R | PTE_W);

  // map kernel text executable and read-only.
  kvmmap(kpgtbl, KERNBASE, KERNBASE, (uint64)etext-KERNBASE, PTE_R | PTE_X);

  // map kernel data and the physical RAM we'll make use of.
  kvmmap(kpgtbl, (uint64)etext, (uint64)etext, PHYSTOP-(uint64)etext, PTE_R | PTE_W);

  // map the trampoline for trap entry/exit to
  // the highest virtual address in the kernel.
  kvmmap(kpgtbl, TRAMPOLINE, (uint64)trampoline, PGSIZE, PTE_R | PTE_X);

  // map kernel stacks
  proc_mapstacks(kpgtbl);
  
  return kpgtbl;
}

// Initialize the one kernel_pagetable
void
kvminit(void)
{
  kernel_pagetable = kvmmake();
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

// add a mapping to the kernel page table.
// only used when booting.
// does not flush TLB or enable paging.
void
kvmmap(pagetable_t kpgtbl, uint64 va, uint64 pa, uint64 sz, int perm)
{
  if(mappages(kpgtbl, va, sz, pa, perm) != 0)
    panic("kvmmap");
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

int
mappage(pagetable_t pagetable, uint64 va, uint64 pa, int perm)
{
  uint64 a;
  pte_t *pte;

  a = PGROUNDDOWN(va);
  
    if((pte = walk(pagetable, a, 1)) == 0)
      return -1;
    if(*pte & PTE_V)
      panic("remap");
    *pte = PA2PTE(pa) | perm | PTE_V;
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
    if(((*pte & PTE_V) == 0) && (*pte & PTE_PG) == 0)
      panic("uvmunmap: not mapped");
    if(PTE_FLAGS(*pte) == PTE_V)
      panic("uvmunmap: not a leaf");
    if(do_free && (*pte & PTE_V)){
      uint64 pa = PTE2PA(*pte);
      kfree((void*)pa);
    }

    #ifndef NONE
    struct proc *p = myproc();

    if(p->pid > 2){
      for(int i = 0; i < MAX_TOTAL_PAGES; i++){
        if(p->pages[i].va == va){
          p->pages[i].used = 0;
          p->pages[i].va = -1;
          p->pages[i].offset = -1;
          p->pages[i].onRAM ? p->primaryMemCounter-- : p->secondaryMemCounter--;
        }
      }
    }
    #endif

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

int 
free_swap_idx(){
  int index = 0;
  struct pageStat *pg;
  struct pageStat *pages = myproc()->pages;
  for(pg = pages; pg < (pages + MAX_TOTAL_PAGES); ++pg){
    if(!pg->used){
      return index;
    }
    ++index;  
  }
  return -1; // never happens
}

int
va_index(struct proc *p, uint64 va, int onRam){
  int index = -1;

  for(int i = 0; i < MAX_TOTAL_PAGES; ++i){
    if(p->pages[i].used && ((p->pages[i].onRAM == onRam) && (p->pages[i].va == va))){
      index = i;
      break;
   }
  }   

  return index;
}

void remove_pageStat(struct proc *p, int index){
  p->pages[index].used = 0;
  p->pages[index].va = -1;
  p->pages[index].offset = -1;
  p->pages[index].scfifo_time = 0;
  p->pages[index].counter = 0;
}

void add_ram_pageStat(struct proc *p, int index, uint64 va){
  p->pages[index].used = 1;
  p->pages[index].offset = -1;
  p->pages[index].va = va;
  p->pages[index].onRAM = 1;

  #ifdef SCFIFO
  p->pages[index].scfifo_time = nextTime(p);
  #endif

  #ifdef LAPA
  p->pages[index].counter = 0xFFFFFFFF;
  #endif

  #ifdef NFUA
  p->pages[index].counter = 0;
  #endif

}

void add_disk_pageStat(struct proc *p, int index, uint64 va, int offset){
  p->pages[index].used = 1;
  p->pages[index].offset = offset;
  p->pages[index].va = va;
  p->pages[index].onRAM = 0;
}

int
twoWaySwap(struct proc *p, uint64 swapOutVA, uint64 swapInVA, int swapDirection){
  int ramIndex = -1, diskIndex = -1, diskOffset = -1;
  pte_t *pte = walk(p->pagetable, swapOutVA, 0);
  void *pa = kalloc();
  char *buffer = kalloc();

  if(!pa || !buffer)
    panic("no more memory bye bye");

  if((*pte & PTE_U)){
    ramIndex = va_index(p, swapOutVA, 1);
    diskIndex = (swapDirection == TWOWAYSWAP) ? va_index(p, swapInVA, 0) : free_swap_idx();
    remove_pageStat(p, ramIndex);
    *pte &= ~PTE_V;
  }

  if(ramIndex == -1)
    panic("twoWaySwap - ramIndex - should not happen");

  if(diskIndex == -1)
    panic("twoWaySwap - diskIndex - should not happen");

  if(swapDirection == TWOWAYSWAP){
    add_ram_pageStat(p, ramIndex, swapInVA);

    diskOffset = p->pages[diskIndex].offset;

    readFromSwapFile(p, buffer, diskOffset, PGSIZE);
    
    if(mappage(p->pagetable, swapInVA, (uint64)pa, PTE_W| PTE_X | PTE_R | PTE_U) == -1){
      kfree(pa);
      panic("mappage failed bye bye");
    }

  memmove(pa, buffer, PGSIZE);
  
  remove_pageStat(p, diskIndex);

  pte = walk(p->pagetable, swapInVA, 0);
  *pte &= ~PTE_PG;
  *pte |= PTE_V;
  }

  pte = walk(p->pagetable, swapOutVA, 0);

  writeToSwapFile(p, (void*)(PTE2PA(*pte)), ((diskOffset == -1) ? p->filesz : diskOffset), PGSIZE);

  add_disk_pageStat(p, diskIndex, swapOutVA, ((diskOffset == -1) ? p->filesz : diskOffset));

  kfree((void*)(PTE2PA(*pte)));
  *pte |= PTE_PG;
  *pte &= ~PTE_V;

  kfree((void*)buffer);

  if(swapDirection == TODISKSWAP){
    p->filesz += PGSIZE;
    add_ram_pageStat(p, ramIndex, swapInVA);
    p->secondaryMemCounter++;
  }

  return ramIndex;
}

void update_counter_aging(struct proc *p){
  struct pageStat *ps;
  pte_t *pte;
  for(ps = p->pages; ps < &p->pages[MAX_TOTAL_PAGES]; ++ps){
    if(ps->used && ps->onRAM){
      pte = walk(p->pagetable, ps->va, 0);
      if((*pte & PTE_U)){
          ps->counter >>= 1;
          if((*pte & PTE_A)){
            ps->counter |= (1 << 31);
            *pte &= ~PTE_A;
          }
      }
    }
  }
}

int
nfua_paging(uint64 va, int swapDirection){
  int swapIndex = -1, counter_index = 0, index = 0;
  uint min_counter = __INT_MAX__;
  struct proc *p = myproc();
  struct pageStat *ps;

  for(ps = p->pages; ps < &p->pages[MAX_TOTAL_PAGES]; ++ps){
    if((ps->used && ps->onRAM) && index >=USERPAGESSTART){
      if(ps->counter < min_counter){
        min_counter = ps->counter;
        swapIndex = counter_index;
      }
    }
    ++counter_index;
    ++index;
  }

  return twoWaySwap(p, p->pages[swapIndex].va, va, swapDirection);
}

int
lapa_paging(uint64 va, int swapDirection){
  uint min_counter = __INT_MAX__;
  int min_ones_counter = __INT_MAX__, ones_counter = 0, swapIndex = -1, counterIndex = 0;
  struct proc *p = myproc();
  struct pageStat *ps;

  for(ps = p->pages; ps < &p->pages[MAX_TOTAL_PAGES]; ++ps){
    if((ps->used && ps->onRAM)){
      for(int i = 0; i < 32; ++i){
        if((ps->counter & (1 << i)))
          ++ones_counter;
      }

      if(ones_counter < min_ones_counter){
        min_ones_counter = ones_counter;
        min_counter = ps->counter;
        swapIndex = counterIndex;
      }
      else if(ones_counter == min_ones_counter){
        if(ps->counter < min_counter){
          min_counter = ps->counter;
          swapIndex = counterIndex;
        }
      }     
    }
    ones_counter = 0;
    ++counterIndex;
  }

return twoWaySwap(p,p->pages[swapIndex].va, va, swapDirection);
}

int
scfifo_paging(uint64 va, int swapDirection){
  struct proc *p = myproc();
  pte_t *pte;
  uint time = __INT_MAX__;
  int swapIndex = -1;

  for(;;){
    for(int i = 0; i< MAX_TOTAL_PAGES; ++i){
      if(p->pages[i].used && p->pages[i].onRAM){
        pte = walk(p->pagetable, p->pages[i].va, 0);
        if((*pte & PTE_U)){
          if(p->pages[i].scfifo_time < time){
            time = p->pages[i].scfifo_time;
            swapIndex = i;
          }
        }
      }
    }

    pte = walk(p->pagetable, p->pages[swapIndex].va, 0);
    
    if((*pte & PTE_A)){
      *pte &= ~PTE_A;
      p->pages[swapIndex].scfifo_time = nextTime(p);
      time = __INT_MAX__;
      swapIndex = -1;
    }
    else
      break;
  }

  return twoWaySwap(p, p->pages[swapIndex].va, va, swapDirection);
}

int
replace_page(uint64 va, int swapDirection){
  int ramIndex = -1;

  #ifdef SCFIFO
  ramIndex = scfifo_paging(va, swapDirection);
  #endif
  
  #ifdef LAPA
  ramIndex = lapa_paging(va, swapDirection);
  #endif

  #ifdef NFUA
  ramIndex = nfua_paging(va, swapDirection);
  #endif

  return ramIndex;
}

// Allocate PTEs and physical memory to grow process from oldsz to
// newsz, which need not be page aligned.  Returns new size or 0 on error.
uint64
uvmalloc(pagetable_t pagetable, uint64 oldsz, uint64 newsz)
{
  char *mem;
  uint64 a;
  struct proc *p = myproc();
  
  if(newsz < oldsz)
    return oldsz;
  if(PGROUNDUP(newsz-oldsz)/PGSIZE > MAX_TOTAL_PAGES - p->primaryMemCounter - p->secondaryMemCounter && p->pid > 2){
    printf("file too big!\n");
    exit(-1);
  }
  
  oldsz = PGROUNDUP(oldsz);
  for(a = oldsz; a < newsz; a += PGSIZE){

    #ifndef NONE
    if(p->pid > 2){
      if(p->primaryMemCounter < MAX_PSYC_PAGES){
        int freeIndex = free_swap_idx();
        add_ram_pageStat(p, freeIndex, a);
        p->primaryMemCounter++;
      }
      else{
        if(p->secondaryMemCounter == MAX_PSYC_PAGES)
          panic("memory full both on ram and disk");
        replace_page(a, TODISKSWAP);
      } 
    }
    #endif

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
    if(((*pte & PTE_V) == 0) && (*pte & PTE_PG) == 0)
      panic("uvmcopy: page not present neither on memory nor disk");


  #ifndef NONE
  struct proc *p = myproc();
  pte_t *new_pte;

  if(p->pid > 2){
    if((*pte & PTE_PG)){
      if(!(new_pte = walk(new, i, 0)))
        return -1;
    *new_pte &= ~PTE_V;
    *new_pte |= PTE_PG;
    continue;
    }
  }
  #endif

    if((*pte & PTE_V) != 0){
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
  uint64 n, va0, pa0;

  while(len > 0){
    va0 = PGROUNDDOWN(srcva);
    pa0 = walkaddr(pagetable, va0);
    if(pa0 == 0)
      return -1;
    n = PGSIZE - (srcva - va0);
    if(n > len)
      n = len;
    memmove(dst, (void *)(pa0 + (srcva - va0)), n);

    len -= n;
    dst += n;
    srcva = va0 + PGSIZE;
  }
  return 0;
}

// Copy a null-terminated string from user to kernel.
// Copy bytes to dst from virtual address srcva in a given page table,
// until a '\0', or max.
// Return 0 on success, -1 on error.
int
copyinstr(pagetable_t pagetable, char *dst, uint64 srcva, uint64 max)
{
  uint64 n, va0, pa0;
  int got_null = 0;

  while(got_null == 0 && max > 0){
    va0 = PGROUNDDOWN(srcva);
    pa0 = walkaddr(pagetable, va0);
    if(pa0 == 0)
      return -1;
    n = PGSIZE - (srcva - va0);
    if(n > max)
      n = max;

    char *p = (char *) (pa0 + (srcva - va0));
    while(n > 0){
      if(*p == '\0'){
        *dst = '\0';
        got_null = 1;
        break;
      } else {
        *dst = *p;
      }
      --n;
      --max;
      p++;
      dst++;
    }

    srcva = va0 + PGSIZE;
  }
  if(got_null){
    return 0;
  } else {
    return -1;
  }
}


