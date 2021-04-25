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

void sig_handler();
void call_ret();
void kerneltrap();

extern int devintr();

void sig_handler();
void call_ret();
void kerneltrap();

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

  sig_handler();

  // tell trampoline.S the user page table to switch to.
  uint64 satp = MAKE_SATP(p->pagetable);

  // jump to trampoline.S at the top of memory, which 
  // switches to the user page table, restores user registers,
  // and switches to user mode with sret.
  uint64 fn = TRAMPOLINE + (userret - trampoline);
  ((void (*)(uint64,uint64))fn)(TRAPFRAME, satp);
}

void
sig_handler(){
  struct proc *p = myproc();
  int i;

  // handling signals 2.4
  for(i = 0; i < 32; i++){
    if((p->pending_sig & (1<<i)) != 0 && (p->sig_mask & (1<<i)) == 0){
      
      if((uint64)p->sig_handlers[i] == SIG_IGN){
        p->pending_sig &= ~(1 << i);
      }
      else if((uint64)p->sig_handlers[i] == SIG_DFL){
        if(i == SIGSTOP)
          sigstop();
        else if(i == SIGCONT)
          sigcont();
        else
          sigkill();

        p->pending_sig &= ~(1 << i);
      }
      else if((uint64)p->sig_handlers[i] == SIGSTOP){
        sigstop();
        p->pending_sig &= ~(1 << i);
      }
      else if((uint64)p->sig_handlers[i] == SIGCONT){
        sigcont();
        p->pending_sig &= ~(1 << i);
      }
      else if((uint64)p->sig_handlers[i] == SIGKILL){
        printf("ssssssssssssssssssssssssss\n");
        sigkill();
        p->pending_sig &= ~(1 << i);
      }
      else{
      memmove(p->trap_backup, p->trapframe, sizeof(*p->trapframe));

      // copy signal handler to local variable
      // void (*sa_handler) (int);   // todo
      // copyin(p->pagetable, sa_handler, ((char *)((struct sigaction*) p->sig_handlers[i])->sa_handler), sizeof((*sa_handler)));

      // backup and update mask
      p->mask_backup = p->sig_mask;
      p->sig_mask = ((struct sigaction*) p->sig_handlers[i])->sigmask;

      // indicate that this proccess at signal handling
      p->sighandler_flag = 1;

      // trapframe and stack pointer
      p->trapframe->sp -= sizeof(*p->trapframe);
      p->trap_backup->sp = p->trapframe->sp;

      // copy current trapframe to the trapframe back up stack pointer
      copyout(p->pagetable, p->trap_backup->sp, (char *)p->trapframe, sizeof(*p->trapframe));

      // update program counter
      p->trapframe->epc = (uint64)((struct sigaction*) p->sig_handlers[i])->sa_handler;

      // reduce trapframe stack pointer by currenct function length
      p->trapframe->sp -= (&kerneltrap - &call_ret);

      // copy this function to the proccess trapframe stack pointer
      copyout(p->pagetable, p->trap_backup->sp, (char *)&call_ret, &kerneltrap - &call_ret);

      // update registers
      p->trapframe->a0 = i;
      p->trapframe->ra = p->trapframe->sp;

      // intr_on();

      // call_ret();
      }
    }
  }

  if(p->freezed)
    yield();
}

void
call_ret(){
  sigret();
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

