#include "types.h"
#include "param.h"
#include "memlayout.h"
#include "riscv.h"
#include "spinlock.h"
#include "proc.h"
#include "defs.h"

struct cpu cpus[NCPU];

struct proc proc[NPROC];

struct proc *initproc;

int nextpid = 1;
struct spinlock pid_lock;

int nexttid = 1;
struct spinlock tid_lock;

static int semaphore_array[MAX_BSEM] = {[0 ... (MAX_BSEM - 1)] = UNLOCKED};

extern void forkret(void);
static void freeproc(struct proc *p);

extern char trampoline[]; // trampoline.S

// helps ensure that wakeups of wait()ing
// parents are not lost. helps obey the
// memory model when using p->parent.
// must be acquired before any p->lock.
struct spinlock wait_lock;

// Allocate a page for each process's kernel stack.
// Map it high in memory, followed by an invalid
// guard page.
void
proc_mapstacks(pagetable_t kpgtbl) {
  struct proc *p;
  
  for(p = proc; p < &proc[NPROC]; p++) {
    char *pa = kalloc();
    if(pa == 0)
      panic("kalloc");
    uint64 va = KSTACK((int) (p - proc));
    kvmmap(kpgtbl, va, (uint64)pa, PGSIZE, PTE_R | PTE_W);
  }
}

// initialize the proc table at boot time.
void
procinit(void)
{
  struct proc *p;
  struct thread *t;
  int i;

  initlock(&pid_lock, "nextpid");
  initlock(&tid_lock, "nexttid");
  initlock(&wait_lock, "wait_lock");
  for(p = proc; p < &proc[NPROC]; p++) {
    i = 0;

      initlock(&p->lock, "proc");
      for(t = p->threads; t < &p->threads[NTHREAD]; t++){
        initlock(&t->lock, "thread");
        t->state = UNUSED;
        t->kstack = 0;
        t->idx = i++;
      }
    p->threads[0].kstack = KSTACK((int) (p - proc));
    
  }
}

// Must be called with interrupts disabled,
// to prevent race with process being moved
// to a different CPU.
int
cpuid()
{
  int id = r_tp();
  return id;
}

// Return this CPU's cpu struct.
// Interrupts must be disabled.
struct cpu*
mycpu(void) {
  int id = cpuid();
  struct cpu *c = &cpus[id];
  return c;
}

// Return the current struct proc *, or zero if none.
struct proc*
myproc(void) {
  struct proc *p = mythread()->parent;
  return p;
}

struct thread*
mythread(void) {
  push_off();
  struct cpu *c = mycpu();
  struct thread *t = c->thread;
  pop_off();
  return t;
}

int
allocpid() {
  int pid;
  
  acquire(&pid_lock);
  pid = nextpid;
  nextpid = nextpid + 1;
  release(&pid_lock);

  return pid;
}

int
alloctid() {
  int tid;
  
  acquire(&tid_lock);
  tid = nexttid;
  nexttid = nexttid + 1;
  release(&tid_lock);

  return tid;
}

// Look in the process table for an UNUSED proc.
// If found, initialize state required to run in the kernel,
// and return with p->lock held.
// If there are no free procs, or a memory allocation fails, return 0.

static struct thread* allocthread(struct proc*);

static struct proc*
allocproc(void)
{
  struct proc *p;
  int i = 0;

  for(p = proc; p < &proc[NPROC]; p++) {
    acquire(&p->lock);
    if(p->state == UNUSED) {
      goto found;
    } else {
      release(&p->lock);
    }
  }
  return 0;

found:
  p->pid = allocpid();
  p->state = USED;

  struct thread *maint = p->threads;
  acquire(&maint->lock);
  maint->state = USED;
  maint->tid = alloctid();
  maint->parent = p;
  memset(&maint->context, 0, sizeof(maint->context));
  maint->context.ra = (uint64)forkret;
  maint->context.sp = maint->kstack + PGSIZE;

  // Allocate a trapframe page.
  if((p->framehead = (struct trapframe *)kalloc()) == 0){
    freeproc(p);
    release(&maint->lock);
    release(&p->lock);
    return 0;
  }

  // Allocate a backup trapframe page.
  if((p->backupframehead = (struct trapframe *)kalloc()) == 0){
    freeproc(p);
    release(&maint->lock);
    release(&p->lock);
    return 0;
  }

  maint->trapframe = p->framehead;
  maint->trap_backup = p->backupframehead;

  release(&maint->lock);

  // An empty user page table.
  p->pagetable = proc_pagetable(p);
  if(p->pagetable == 0){
    freeproc(p);
    release(&maint->lock);
    release(&p->lock);
    return 0;
  }

  p->sig_mask = 0;
  p->pending_sig = 0;

  // Set up the SIG_DFL for all signals
  for(i = 0; i < 32; i++)
    p->sig_handlers[i] = SIG_DFL;

  return p;
}

// free a proc structure and the data hanging from it,
// including user pages.
// p->lock must be held.

static void freethread(struct thread*);

static void
freeproc(struct proc *p)
{
  struct thread *t;
  int xstate;

  for(t = p->threads; t < &p->threads[NTHREAD]; t++){
    acquire(&t->lock);

    if(t->state == ZOMBIE){
      freethread(t);
      release(&t->lock);
    }
    else if(t->state != UNUSED){
      t->killed = 1;

      if(t->state == SLEEPING){
        t->state = RUNNABLE;
      }

      release(&t->lock);
      kthread_join(t->tid, &xstate);

      acquire(&t->lock);
      freethread(t);
      release(&t->lock);
    }
    else{
      release(&t->lock);
    }
  }

  if(p->pagetable)
    proc_freepagetable(p->pagetable, p->sz);

  if(p->framehead)
    kfree(p->framehead); 
  p->framehead = 0;
  if(p->backupframehead)
    kfree(p->backupframehead); 
  p->backupframehead = 0;     
  p->pagetable = 0;
  p->sz = 0;
  p->pid = 0;
  p->parent = 0;
  p->name[0] = 0;
  p->killed = 0;
  p->freezed = 0;
  p->sighandler_flag = 0;
  p->xstate = 0;
  p->state = UNUSED;
}

// Create a user page table for a given process,
// with no user memory, but with trampoline pages.
pagetable_t
proc_pagetable(struct proc *p){
  pagetable_t pagetable;

  // An empty page table.
  pagetable = uvmcreate();
  if(pagetable == 0)
    return 0;

  // map the trampoline code (for system call return)
  // at the highest user virtual address.
  // only the supervisor uses it, on the way
  // to/from user space, so not PTE_U.
  if(mappages(pagetable, TRAMPOLINE, PGSIZE,
              (uint64)trampoline, PTE_R | PTE_X) < 0){
    uvmfree(pagetable, 0);
    return 0;
  }

  // map the trapframe just below TRAMPOLINE, for trampoline.S.
  if(mappages(pagetable, TRAPFRAME, PGSIZE,
              (uint64)(p->framehead), PTE_R | PTE_W) < 0){
    uvmunmap(pagetable, TRAMPOLINE, 1, 0);
    uvmfree(pagetable, 0);
    return 0;
  }

  return pagetable;
}

// Free a process's page table, and free the
// physical memory it refers to.
void
proc_freepagetable(pagetable_t pagetable, uint64 sz)
{
  uvmunmap(pagetable, TRAMPOLINE, 1, 0);
  uvmunmap(pagetable, TRAPFRAME, 1, 0);
  uvmfree(pagetable, sz);
}

// a user program that calls exec("/init")
// od -t xC initcode
uchar initcode[] = {
  0x17, 0x05, 0x00, 0x00, 0x13, 0x05, 0x45, 0x02,
  0x97, 0x05, 0x00, 0x00, 0x93, 0x85, 0x35, 0x02,
  0x93, 0x08, 0x70, 0x00, 0x73, 0x00, 0x00, 0x00,
  0x93, 0x08, 0x20, 0x00, 0x73, 0x00, 0x00, 0x00,
  0xef, 0xf0, 0x9f, 0xff, 0x2f, 0x69, 0x6e, 0x69,
  0x74, 0x00, 0x00, 0x24, 0x00, 0x00, 0x00, 0x00,
  0x00, 0x00, 0x00, 0x00
};

// Set up first user process.
void
userinit(void)
{
  struct proc *p;

  p = allocproc();
  initproc = p;
  
  // allocate one user page and copy init's instructions
  // and data into it.
  uvminit(p->pagetable, initcode, sizeof(initcode));
  p->sz = PGSIZE;

  // prepare for the very first "return" from kernel to user.
  p->threads[0].trapframe->epc = 0;      // user program counter
  p->threads[0].trapframe->sp = PGSIZE;  // user stack pointer

  safestrcpy(p->name, "initcode", sizeof(p->name));
  p->cwd = namei("/");

  p->state = RUNNABLE;
  p->threads[0].state = RUNNABLE;

  release(&p->lock);
}

// Grow or shrink user memory by n bytes.
// Return 0 on success, -1 on failure.
int
growproc(int n)
{
  uint sz;
  struct proc *p = myproc();

  acquire(&p->lock);

  sz = p->sz;
  if(n > 0){
    if((sz = uvmalloc(p->pagetable, sz, sz + n)) == 0) {
      release(&p->lock);
      return -1;
    }
  } else if(n < 0){
    sz = uvmdealloc(p->pagetable, sz, sz + n);
  }
  p->sz = sz;

  release(&p->lock);

  return 0;
}

// Create a new process, copying the parent.
// Sets up child kernel stack to return as if from fork() system call.
int
fork(void)
{
  int i, pid;
  struct proc *np;
  struct proc *p = myproc();
  struct thread *t = mythread();

  // Allocate process.
  if((np = allocproc()) == 0){
    return -1;
  }

  acquire(&p->lock);
  acquire(&t->lock);
  acquire(&np->threads[0].lock);

  // Copy user memory from parent to child.
  if(uvmcopy(p->pagetable, np->pagetable, p->sz) < 0){
    freeproc(np);
    release(&np->threads[0].lock);
    release(&np->lock);
    release(&t->lock);
    release(&p->lock);
    return -1;
  }

  np->sz = p->sz;

  // copy saved user registers.
  *(np->threads[0].trapframe) = *(t->trapframe);

  // Cause fork to return 0 in the child.
  np->threads[0].trapframe->a0 = 0;

  // increment reference counts on open file descriptors.
  for(i = 0; i < NOFILE; i++)
    if(p->ofile[i])
      np->ofile[i] = filedup(p->ofile[i]);
  np->cwd = idup(p->cwd);

  safestrcpy(np->name, p->name, sizeof(p->name));

  pid = np->pid;

  release(&np->lock);

  release(&np->threads[0].lock);
  release(&t->lock);
  release(&p->lock);

  acquire(&wait_lock);
  np->parent = p;
  release(&wait_lock);

  acquire(&p->lock);
  acquire(&np->lock);

  // inherit sigMask and the handlers of the parent proccess
  np->sig_mask = p->sig_mask;
  
  for(i = 0; i < 32; i++){
    np->sigactions[i].sa_handler = p->sigactions[i].sa_handler;
    np->sigactions[i].sigmask = p->sigactions[i].sigmask;

    if((uint64)p->sig_handlers[i] < 32)
      np->sig_handlers[i] = p->sig_handlers[i];
    else
      np->sig_handlers[i] = & np->sigactions[i];
  }

  release(&np->lock);
  release(&p->lock);

  acquire(&np->lock);
  acquire(&np->threads[0].lock);
  np->state = RUNNABLE;
  np->threads[0].state = RUNNABLE;
  release(&np->threads[0].lock);
  release(&np->lock);


  return pid;
}

// Pass p's abandoned children to init.
// Caller must hold wait_lock.
void
reparent(struct proc *p)
{
  struct proc *pp;

  for(pp = proc; pp < &proc[NPROC]; pp++){
    if(pp->parent == p){
      pp->parent = initproc;
      wakeup(initproc);
    }
  }
}

// Exit the current process.  Does not return.
// An exited process remains in the zombie state
// until its parent calls wait().
void
exit(int status)
{
  struct proc *p = myproc();
  struct thread *t = mythread();
t->line = __LINE__;
  if(p == initproc)
    panic("init exiting");

  // Close all open files.
  for(int fd = 0; fd < NOFILE; fd++){
    if(p->ofile[fd]){
      struct file *f = p->ofile[fd];
      fileclose(f);
      p->ofile[fd] = 0;
    }
  }
t->line = __LINE__;
  begin_op();
  iput(p->cwd);
  end_op();
  p->cwd = 0;
t->line = __LINE__;
  acquire(&wait_lock);
t->line = __LINE__;
  // Give any children to init.
  reparent(p);
t->line = __LINE__;
  // Parent might be sleeping in wait().

  wakeup(p->parent);
  wakeup(p);
  wakeup(t);

  t->line = __LINE__;
  acquire(&p->lock);
  t->line = __LINE__;
  acquire(&t->lock);
  t->line = __LINE__;

  p->xstate = status;
  p->state = ZOMBIE;

  t->xstate = status;
  t->state = ZOMBIE;

  t->line = __LINE__;
  release(&p->lock);
  t->line = __LINE__;
  release(&wait_lock);
  t->line = __LINE__;

  // printf("pid %d tries to exit\n", p->pid);
t->line = __LINE__;
  struct thread *itrthread;
  for(itrthread = p->threads; itrthread < &p->threads[NTHREAD]; itrthread++){
   
    if(itrthread != t){
      t->line = __LINE__;
      acquire(&itrthread->lock);
      if(itrthread->state != UNUSED){
        itrthread->killed = 1;

        if(itrthread->state == SLEEPING){
          itrthread->state = RUNNABLE;
        }
      }

      release(&itrthread->lock);
    }
    
  }

  // Jump into the scheduler, never to return.


  sched();
  panic("zombie exit");
}

// Wait for a child process to exit and return its pid.
// Return -1 if this process has no children.
int
wait(uint64 addr)
{
  struct proc *np;
  int havekids, pid;
  struct proc *p = myproc();

  acquire(&wait_lock);

  for(;;){
    // Scan through table looking for exited children.
    havekids = 0;
    for(np = proc; np < &proc[NPROC]; np++){
      if(np->parent == p){
        // make sure the child isn't still in exit() or swtch().
        acquire(&np->lock);

        havekids = 1;
        if(np->state == ZOMBIE){
          // Found one.
          pid = np->pid;
          if(addr != 0 && copyout(p->pagetable, addr, (char *)&np->xstate,
                                  sizeof(np->xstate)) < 0) {
            release(&np->lock);
            release(&wait_lock);
            return -1;
          }
          freeproc(np);
          release(&np->lock);
          release(&wait_lock);

          return pid;
        }
        release(&np->lock);
      }
    }

    // No point waiting if we don't have any children.
    if(!havekids || p->killed){
      release(&wait_lock);
      return -1;
    }
    
    // Wait for a child to exit.
    sleep(p, &wait_lock);  //DOC: wait-sleep
  }
}

// Per-CPU process scheduler.
// Each CPU calls scheduler() after setting itself up.
// Scheduler never returns.  It loops, doing:
//  - choose a process to run.
//  - swtch to start running that process.
//  - eventually that process transfers control
//    via swtch back to the scheduler.
void
scheduler(void)
{
  struct proc *p;
  struct thread *t;
  struct cpu *c = mycpu();
  
  // c->proc = 0;
  c->thread = 0;
  for(;;){
    // Avoid deadlock by ensuring that devices can interrupt.
    intr_on();

    for(p = proc; p < &proc[NPROC]; p++) {
      for(t = p->threads; t< &p->threads[NTHREAD]; t++){
        t->line = __LINE__;
        acquire(&t->lock);
        t->line = __LINE__;
        if(t->state == RUNNABLE){

          t->state = RUNNING;
          c->thread = t;
          t->line = __LINE__;

          swtch(&c->context, &t->context);
          t->line = __LINE__;

          c->thread = 0;
        }
        release(&t->lock);
      }
    }
  }
}

// Switch to scheduler.  Must hold only p->lock
// and have changed proc->state. Saves and restores
// intena because intena is a property of this
// kernel thread, not this CPU. It should
// be proc->intena and proc->noff, but that would
// break in the few places where a lock is held but
// there's no process.
void
sched(void)
{

  int intena;
  struct thread *t = mythread();
t->line = __LINE__;
  if(!holding(&t->lock))
    panic("sched p->lock");
  if(mycpu()->noff != 1)
    panic("sched locks");
  if(t->state == RUNNING)
    panic("sched running");
  if(intr_get())
    panic("sched interruptible");

t->line = __LINE__;

  intena = mycpu()->intena;
  swtch(&t->context, &mycpu()->context);
  t->line = __LINE__;
t->line = __LINE__;
  mycpu()->intena = intena;
}

// Give up the CPU for one scheduling round.
void
yield(void)
{
  struct proc *p = myproc();
  struct thread *t = mythread();

t->line = __LINE__;
  acquire(&p->lock);
  acquire(&t->lock);
t->line = __LINE__;
  t->state = RUNNABLE;
  p->state = RUNNABLE;

  release(&p->lock);
  sched();
  release(&t->lock);
}

// A fork child's very first scheduling by scheduler()
// will swtch to forkret.
void
forkret(void)
{
  static int first = 1;

  // Still holding p->lock from scheduler.
  release(&mythread()->lock);

  if (first) {
    // File system initialization must be run in the context of a
    // regular process (e.g., because it calls sleep), and thus cannot
    // be run from main().
    first = 0;
    fsinit(ROOTDEV);
  }

  usertrapret();
}

// Atomically release lock and sleep on chan.
// Reacquires lock when awakened.
void
sleep(void *chan, struct spinlock *lk)
{
  struct thread *t = mythread();
  
  // Must acquire p->lock in order to
  // change p->state and then call sched.
  // Once we hold p->lock, we can be
  // guaranteed that we won't miss any wakeup
  // (wakeup locks p->lock),
  // so it's okay to release lk.
t->line = __LINE__;
  acquire(&t->lock);  //DOC: sleeplock1
  release(lk);

  // Go to sleep.
  t->chan = chan;
  t->state = SLEEPING;
  t->line = __LINE__;

  sched();
t->line = __LINE__;

  // Tidy up.
  t->chan = 0;

  // Reacquire original lock.
  release(&t->lock);
  acquire(lk);
}

// Wake up all processes sleeping on chan.
// Must be called without any p->lock.
void
wakeup(void *chan)
{
  struct proc *p;
  struct thread *t;
  for(p = proc; p < &proc[NPROC]; p++) {
    for(t = p->threads; t < &p->threads[NTHREAD]; t++){
     
      if(t != mythread()){

         acquire(&t->lock);
        t->line = __LINE__;
        if(t->state == SLEEPING && t->chan == chan) {
          t->state = RUNNABLE;
       }
        release(&t->lock);
     }
    }
  }
}


// Kill the process with the given pid.
// The victim won't exit until it tries to return
// to user space (see usertrap() in trap.c).
int
kill(int pid, int signum)
{
  struct proc *p;
  
  if(signum < 0 || signum >= 32)
    return -1;

  for(p = proc; p < &proc[NPROC]; p++){
    acquire(&p->lock);
    if(p->pid == pid){

      // printf("killed pid %d with %d\n", pid, signum);

      p->pending_sig |= 1<<signum; 

      if(p->state == UNUSED || p->state == ZOMBIE){ // failure - the process does not exists or has terminated execution
        release(&p->lock);
        return -1;
      } 

      release(&p->lock);
      return 0;
    }
    release(&p->lock);
  }
  return -1;
}

// Copy to either a user address, or kernel address,
// depending on usr_dst.
// Returns 0 on success, -1 on error.
int
either_copyout(int user_dst, uint64 dst, void *src, uint64 len)
{
  struct proc *p = myproc();
  if(user_dst){
    return copyout(p->pagetable, dst, src, len);
  } else {
    memmove((char *)dst, src, len);
    return 0;
  }
}

// Copy from either a user address, or kernel address,
// depending on usr_src.
// Returns 0 on success, -1 on error.
int
either_copyin(void *dst, int user_src, uint64 src, uint64 len)
{
  struct proc *p = myproc();
  if(user_src){
    return copyin(p->pagetable, dst, src, len);
  } else {
    memmove(dst, (char*)src, len);
    return 0;
  }
}

// Print a process listing to console.  For debugging.
// Runs when user types ^P on console.
// No lock to avoid wedging a stuck machine further.
void
procdump(void)
{
  static char *states[] = {
  [UNUSED]    "unused",
  [SLEEPING]  "sleep ",
  [RUNNABLE]  "runble",
  [RUNNING]   "run   ",
  [ZOMBIE]    "zombie"
  };
  struct proc *p;
  char *state;

  printf("\n");
  for(p = proc; p < &proc[NPROC]; p++){
    if(p->state == UNUSED)
      continue;
    if(p->state >= 0 && p->state < NELEM(states) && states[p->state])
      state = states[p->state];
    else
      state = "???";
    printf("%d %s %s", p->pid, state, p->name);
    printf("\n");
  }
}

uint 
sigprocmask(uint sigmask){
  struct proc *p = myproc();
  uint oldmask;

  acquire(&p->lock);
  oldmask = p->sig_mask;
  p->sig_mask = sigmask;
  release(&p->lock);

  return oldmask;
}

int
sigaction(int signum, const struct sigaction *act, struct sigaction *oldact){
  struct proc *p = myproc();
  struct sigaction newaction;
  struct sigaction local_act;
  
  if(signum < 0 || signum >=32)
    return -1;

  if(signum == SIGKILL || signum == SIGSTOP)
    return -1; // SIGKILL and SIGSTOP cannot be modified (or ignored)

  acquire(&p->lock);

  if(act != 0){

    copyin(p->pagetable, (char*)&newaction, (uint64)act , sizeof(struct sigaction));

    if((newaction.sigmask & ((1 << SIGKILL) + (1 << SIGSTOP))) > 0){
        release(&p->lock);
        return -1; // SIGKILL and SIGSTOP cannot be blocked
    }
  }

  if((uint64)p->sig_handlers[signum] < 32){ // kernel space handelr

    if(oldact != 0 && copyout(p->pagetable, (uint64)&oldact->sa_handler, (char*)&p->sig_handlers[signum], sizeof(void*)) < 0){
        release(&p->lock);
        return -1;
    }

    if(act != 0){ // 3.2

      if((uint64)newaction.sa_handler < 32)
      {
        p->sig_handlers[signum] = newaction.sa_handler;
      }
      else{

        p->sigactions[signum].sa_handler = newaction.sa_handler;
        p->sigactions[signum].sigmask = newaction.sigmask;

        p->sig_handlers[signum] = &p->sigactions[signum];

      }
    }
  }
  else{ // user space handelr

    copyin(p->pagetable, (char*)&local_act, (uint64)p->sig_handlers[signum] , sizeof(struct sigaction));

    if(oldact != 0 && copyout(p->pagetable, (uint64)oldact, (char*)&local_act, sizeof(struct sigaction)) < -1){
      release(&p->lock);
      return -1;
    }

    if(act != 0){

      if((uint64)newaction.sa_handler < 32)
      {
        p->sig_handlers[signum] = newaction.sa_handler;
      }
      else{
        p->sigactions[signum].sa_handler = newaction.sa_handler;
        p->sigactions[signum].sigmask = newaction.sigmask;

        p->sig_handlers[signum] = &p->sigactions[signum];
      }
    }
  }

  release(&p->lock);

  return 0;
}

void 
sigret(void){
  printf("sigret \n");

  struct thread *t = mythread();

  // Restore the process original trapframe
  memmove(t->trapframe, t->trap_backup, sizeof(*t->trapframe));

  // Restore the process original signal mask
  t->parent->sig_mask = t->parent->mask_backup;

  // Turn off the flag indicates a user space signal handling for blocking incoming signals at this time.
  t->parent->sighandler_flag = 0;
}

void 
sigkill(void){
  struct proc *p = myproc();

  printf("sigkill activated and the state is %d\n", p->state);


  p->killed = 1;

  if(p->state == SLEEPING){
     p->state = RUNNABLE;
  }
}

void 
sigstop(void){
  printf("sigstop activated\n");

  struct proc *p = myproc();
  p->freezed = 1;
}

void 
sigcont(void){
  printf("sigcont activated\n");

  struct proc *p = myproc();
  p->freezed = 0;
} 

static void
freethread(struct thread *t)
{
  if(&t->parent->threads[0] == t){
    // if(t->trapframe)
    //  kfree((void*)t->trapframe);
    // if(t->trap_backup)
    //   kfree((void*)t->trap_backup);
  }
  else{
      if(t->kstack){
        kfree((void*)t->kstack);
        t->kstack = 0;
      }
  }

  t->trapframe = 0;
  t->trap_backup = 0;
  t->parent = 0;
  t->chan = 0;
  t->killed = 0;
  t->state = UNUSED;
}


static struct thread*
allocthread(struct proc *p)
{
  struct thread *t;
  
  for(t = p->threads ;t < &p->threads[NTHREAD]; t++) {
    t->line = __LINE__;
    acquire(&t->lock);
    if(t->state == UNUSED) {
      goto found;
    }  
    // else if(t->state == ZOMBIE){
    //   freethread(t);
    //   release(&t->lock);
    // }  
    else {
      release(&t->lock);
    }
  }
      t->line = __LINE__;

  return 0;
  
found:
    t->line = __LINE__;

  t->tid = alloctid();
  t->state = EMBRYO;
  t->killed = 0;
  t->parent = p;

  // Allocate a trapframe page.
   t->line = __LINE__;
   t->trapframe = (struct trapframe*)((uint64)(p->framehead) + (uint64)((t->idx)*sizeof(struct trapframe)));
   t->trap_backup = (struct trapframe*)((uint64)(p->backupframehead) + (uint64)((t->idx)*sizeof(struct trapframe)));

  // t->trapframe = t->parent->threads->trapframe + t->idx * sizeof(struct trapframe);
  // t->trap_backup = t->parent->threads->trap_backup + t->idx * sizeof(struct trapframe);

  if((t->kstack = (uint64)kalloc()) == 0){
    freethread(t);
    release(&t->lock);
    return 0;
  }

  // Set up new context to start executing at forkret,
  // which returns to user space.
  memset(&t->context, 0, sizeof(t->context));
  t->context.ra = (uint64)forkret;
  t->context.sp = t->kstack + PGSIZE;

  release(&t->lock);

  return t;
}


int
kthread_create(void(*start_func)(), void *stack){
  struct proc *p = myproc();
  struct thread *t;
  int tid;

  if((t = allocthread(p)) == 0)
    return -1;

t->line = __LINE__;
  acquire(&t->lock);

  *t->trapframe = *mythread()->trapframe;
  t->trapframe->a0 = 0;

  // copyin(p->pagetable, (char*)&t->trapframe->epc, (uint64)start_func, 8);
  // copyin(p->pagetable, (char*)&t->trapframe->sp, (uint64)stack, 8);

  t->trapframe->epc = (uint64)start_func;
  t->trapframe->sp = (uint64)stack;
t->line = __LINE__;
  t->trapframe->sp += MAX_STACK_SIZE - 16;
  t->state = RUNNABLE;
    memset(&t->context, 0, sizeof(t->context));
  t->context.ra = (uint64)forkret;
  t->context.sp = t->kstack + PGSIZE;
t->line = __LINE__;
  tid = t->tid;

  release(&t->lock);

  return tid;
}

int 
kthread_id(void){
  
  return mythread()->tid;
}

void kthread_exit(int status){
  // int tid;
  int terminate = 1;
  struct thread *t = mythread();
  struct proc *threadproc = t->parent;
  struct thread *itrthread;

  acquire(&wait_lock);
  t->line = __LINE__;
  acquire(&t->lock);
  t->state = ZOMBIE;
t->line = __LINE__;
  for(itrthread = threadproc->threads; itrthread < &threadproc->threads[NTHREAD]; itrthread++){
      if(!(itrthread->state == ZOMBIE || itrthread->state == UNUSED)){
        terminate = 0;
        break;
      }
  }
t->line = __LINE__;
  if(terminate){
    threadproc->state = ZOMBIE;
    release(&t->lock);
    release(&wait_lock);
    exit(status);
  }
t->line = __LINE__;

  t->killed = 1;
  t->xstate = status;
  t->state = ZOMBIE;

  wakeup(t);

  release(&wait_lock);
  t->line = __LINE__;
  sched();
  release(&t->lock);
  }

int
kthread_join(int thread_id, int *status){
  struct proc *p = myproc();
  struct thread *t;
  struct thread *foundthread = 0;
  struct thread *mytrd = mythread();

  if(mythread()->tid == thread_id)
    return -1;



  for(;;){
    acquire(&mytrd->lock);

    for(t = p->threads; t < &p->threads[NTHREAD]; t++){
      if(t != mytrd){
        t->line = __LINE__;

        acquire(&t->lock);
        if(t->tid == thread_id){
          foundthread = t;
          if(t->state == ZOMBIE){
            *status = t->xstate;

            release(&t->lock);
            release(&mytrd->lock);
            // freethread(t);
            return 0;
          }
        }
        release(&t->lock);
      }
    }
t->line = __LINE__;
    release(&mytrd->lock);
t->line = __LINE__;
    if(!foundthread)
      return -1;

    acquire(&tid_lock);

    sleep(foundthread, &tid_lock);  
  }
}

int bsem_alloc(){
  int descriptor;
  for(descriptor = 0; descriptor < MAX_BSEM; ++descriptor){
    if(semaphore_array[descriptor] == DEALLOCED){
      semaphore_array[descriptor] = UNLOCKED;
      return descriptor;
    }
  }
  return -1;
}

void bsem_free(int semaphore){
  if(semaphore >= 0 && semaphore < MAX_BSEM)
    semaphore_array[semaphore] = DEALLOCED;
}

void bsem_down(int semaphore){
  if((semaphore < 0 && semaphore >= MAX_BSEM) || (semaphore_array[semaphore] == DEALLOCED))
    return;

  struct thread *t = mythread();

  if(semaphore_array[semaphore] == LOCKED)
    sleep(&semaphore_array[semaphore], &t->lock);
  else
    semaphore_array[semaphore] = 0;    
}

void bsem_up(int semaphore){
  if((semaphore < 0 && semaphore >= MAX_BSEM) || (semaphore_array[semaphore] == DEALLOCED))
    return;

  wakeup(&semaphore_array[semaphore]);

  semaphore_array[semaphore] = UNLOCKED;    
}



