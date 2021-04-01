#include "types.h"
#include "riscv.h"
#include "defs.h"
#include "date.h"
#include "param.h"
#include "memlayout.h"
#include "spinlock.h"
#include "proc.h"
#include "syscall.h"

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
  int fork_res = fork();
  if((myproc()->mask | 1 << SYS_fork) == myproc()->mask)
    printf("%d: syscall fork NULL -> %d", myproc()->pid, fork_res);
  return fork_res;
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
  int addr;
  int n;

  if(argint(0, &n) < 0)
    return -1;
  addr = myproc()->sz;
  
  if(growproc(n) < 0){
      if((myproc()->mask | 1 << SYS_sbrk) == myproc()->mask)
        printf("%d: syscall sbrk %lu -> %d", myproc()->pid, n, -1);
    return -1;
  }

  if((myproc()->mask | 1 << SYS_sbrk) == myproc()->mask)
    printf("%d: syscall sbrk %lu -> %d", myproc()->pid, n, 0);

  return addr;
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

  int kill_res = kill(pid);

  if((myproc()->mask | 1 << SYS_kill) == myproc()->mask)
    printf("%d: syscall kill %d -> %d", myproc()->pid, pid, kill_res);

  return kill_res;
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

uint64
sys_trace(void)
{
  int mask;
  int pid;

  if(argaddr(0, &mask) < 0)
    return -1;

  if(argaddr(1, &pid) < 0)
    return -1;

  return trace(mask, pid);  
}

// return the process wait status
uint64
sys_wait_stat(void)
{ 
  uint64 stat_addr;
  uint64 perf_addr;

  if(argaddr(0, &stat_addr) < 0)
    return -1;

  if(argaddr(1, &perf_addr) < 0)
    return -1;

  return wait_stat(stat_addr, perf_addr);
}

// set priority to cuurent proccess
uint64
sys_set_priority(void)
{
  int priority;

  if(argint(0, &priority) < 0)
    return -1;
  return set_priority(priority);
}
