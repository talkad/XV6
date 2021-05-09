#include "kernel/param.h"
#include "kernel/types.h"
#include "kernel/stat.h"
#include "user/user.h"
#include "kernel/fs.h"
#include "kernel/fcntl.h"
#include "kernel/syscall.h"
#include "kernel/memlayout.h"
#include "kernel/riscv.h"
#include "kernel/syscall.h"

#include "kernel/spinlock.h"  // NEW INCLUDE FOR ASS2
// #include "Csemaphore.h"   // NEW INCLUDE FOR ASS 2
#include "kernel/proc.h"         // NEW INCLUDE FOR ASS 2, has all the signal definitions and sigaction definition.  Alternatively, copy the relevant things into user.h and include only it, and then no need to include spinlock.h .


// test if child is killed (status = -1)
void
killstatus(char *s)
{
  int xst;
  
  for(int i = 0; i < 100; i++){
    int pid1 = fork();
    if(pid1 < 0){
      printf("%s: fork failed\n", s);
      exit(1);
    }
    if(pid1 == 0){
      while(1) {
        getpid();
      }
      exit(0);
    }
    sleep(1);
    kill(pid1,SIGKILL);
    wait(&xst);
    if(xst != -1) {
       printf("%s: status should be -1\n", s);
       exit(1);
    }
  }
  exit(0);
}

void
old_sigaction_test(){
  struct sigaction action = {(void*)SIGSTOP, 0x8000};
  struct sigaction oldaction;

  sigaction(20, &action, &oldaction);

  if(oldaction.sa_handler != (void*)SIG_DFL)
    exit(1);

  sigaction(20, &action, &oldaction);

  if(oldaction.sa_handler != (void*)SIGSTOP)
    exit(1);

  exit(0);
}

// test sigaction
void
sigaction_test(){
  int status;
  int pid = fork();
  if(pid){
    sleep(3);
    kill(pid, 15);
    sleep(10);
    kill(pid, SIGCONT);
    printf("father is going to wait\n");
    wait(&status);
    exit(0);
  }
  else{
    struct sigaction action = {(void*)SIGSTOP, 0};
    sigaction(15, &action, 0);

    uint oldmask = sigprocmask(0x8000);

    if(oldmask != 0){
      printf("sigprocmask failed");
      exit(1);
    }

    sleep(20);
    printf("child signal is still blocked \n");
    oldmask = sigprocmask(0);
    if(oldmask != 0x8000){
      printf("sigprocmask failed");
      exit(1);
    }

    exit(0);
  }
}

// test sigaction
void
THE_TEST_THAT_NEVER_ENDS(){
 int status;
  int pid = fork();
  if(pid){
             printf("hellllllllllll %d", pid);

    sleep(15);
    kill(pid, SIGSTOP);

    wait(&status);

    printf("noooooooooooooooooooooooooooooo \n");

    exit(1);
  }
  else{
    int i = 0;

    sleep(10);
    while(1){
      printf("%d\n", i);
      i++;
    }

    exit(0);
  }
}

// test sigaction
void
THE_TEST_THAT_NEVER_ENDS_ADVANCED(){
  int status;
  int pid = fork();
  if(pid){
    sleep(5);
    printf("killed \n");
    kill(pid, 31);
    wait(&status);

    printf("noooooooooooooooooooooooooooooo \n");

    exit(0);
  }
  else{
    struct sigaction action = {(void*)SIGSTOP, 0};
    sigaction(31, &action, 0);

    sleep(10);
    while(1)
      printf("a");

    exit(0);
  }
}

// change mask of a proccess
void
sigprocmaskTest(char *s)
{
  uint mask = 42;
  uint mask1 = sigprocmask(mask);

  int pid1 = fork();
  if(pid1 == 0){
    if(sigprocmask(0) != 42){
      printf("the proc mask value should be like the father");
      exit(1);
    }
    exit(0);
  }

  if(mask1 != 0 || sigprocmask(mask) != 42){
    printf("proc mask didn't change");
    exit(1);
  }
  
  exit(0);
}

int wait_sig = 0;

void test_handler(int signum){
    printf("aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa test handler\n");
    wait_sig = 1;
    printf("Received sigtest\n");
}

void signal_test(char *s){
  printf("asaaaaaaaaaaaaaaaaaaaaaaaaaa\n");
    int pid;
    int testsig;
    testsig=15;
    struct sigaction act = {test_handler, (uint)(1 << 29)};
    struct sigaction old;

    printf("cccccccccccccccccccc\n");

    sigprocmask(0);
    sigaction(testsig, &act, &old);
    printf("ddddddddddd\n");
    if((pid = fork()) == 0){
      printf("bbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbb");
        while(!wait_sig)
          printf("%d", wait_sig);
            // sleep(1);
        exit(0);
    }
    kill(pid, testsig);
    wait(&pid);
    printf("Finished testing signals\n");
}

// int num = 0;

void test_handler2(int signum){
    printf("oooooooooooooooooooooooooooooooooooooooooooooooooooooooooooooooooooooooo it's working\n");
    // num = 1;
}

void user_handler_test(char *s){
    struct sigaction act = {test_handler2, 0x8000};

    sigaction(31, &act, 0);
    kill(getpid(), 31);
}


void test_handler3(int signum){
    printf("111111111111111111111111111111111 it's working\n");
    // num = 1;
}

void test_handler4(int signum){
    printf("222222222222222222222222222222222 it's working\n");
    // num = 1;
}

void user_handler_test2(char *s){
    struct sigaction act3 = {test_handler3, 0x8000};
    struct sigaction act4 = {test_handler4, 0x8000};

    sigaction(30, &act3, 0);
    kill(getpid(), 30);

    sigaction(31, &act4, 0);
    kill(getpid(), 31);
}

//
// use sbrk() to count how many free physical memory pages there are.
// touches the pages to force allocation.
// because out of memory with lazy allocation results in the process
// taking a fault and being killed, fork and report back.
//
int
countfree()
{
  int fds[2];

  if(pipe(fds) < 0){
    printf("pipe() failed in countfree()\n");
    exit(1);
  }
  
  int pid = fork();

  if(pid < 0){
    printf("fork failed in countfree()\n");
    exit(1);
  }

  if(pid == 0){
    close(fds[0]);
    
    while(1){
      uint64 a = (uint64) sbrk(4096);
      if(a == 0xffffffffffffffff){
        break;
      }

      // modify the memory to make sure it's really allocated.
      *(char *)(a + 4096 - 1) = 1;

      // report back one more page.
      if(write(fds[1], "x", 1) != 1){
        printf("write() failed in countfree()\n");
        exit(1);
      }
    }

    exit(0);
  }

  close(fds[1]);

  int n = 0;
  while(1){
    char c;
    int cc = read(fds[0], &c, 1);
    if(cc < 0){
      printf("read() failed in countfree()\n");
      exit(1);
    }
    if(cc == 0)
      break;
    n += 1;
  }

  close(fds[0]);
  wait((int*)0);
  
  return n;
}

// run each test in its own process. run returns 1 if child's exit()
// indicates success.
int
run(void f(char *), char *s) {
  int pid;
  int xstatus;

  printf("test %s: ", s);
  if((pid = fork()) < 0) {
    printf("runtest: fork error\n");
    exit(1);
  }
  if(pid == 0) {
    f(s);
    exit(0);
  } else {
    wait(&xstatus);
    if(xstatus != 0) 
      printf("FAILED\n");
    else
      printf("OK\n");
    return xstatus == 0;
  }
}

int
main(int argc, char *argv[])
{
  int continuous = 0;
  char *justone = 0;

  if(argc == 2 && strcmp(argv[1], "-c") == 0){
    continuous = 1;
  } else if(argc == 2 && strcmp(argv[1], "-C") == 0){
    continuous = 2;
  } else if(argc == 2 && argv[1][0] != '-'){
    justone = argv[1];
  } else if(argc > 1){
    printf("Usage: signaltests [-c] [testname]\n");
    exit(1);
  }
  
  struct test {
    void (*f)(char *);
    char *s;
  } tests[] = {
          {killstatus, "killstatus"},
          {old_sigaction_test, "old_sigaction"},
    {sigaction_test, "sigaction_test"},
          {sigprocmaskTest, "sigprocmaskTest"},
    {signal_test, "signal_test"},

          // {THE_TEST_THAT_NEVER_ENDS, "THE_TEST_THAT_NEVER_ENDS"},
          // {THE_TEST_THAT_NEVER_ENDS_ADVANCED, "THE_TEST_THAT_NEVER_ENDS_ADVANCED"},
    {user_handler_test2, "user_handler_test"},
    { 0, 0},
  };

  if(continuous){
    printf("continuous signaltests starting\n");
    while(1){
      int fail = 0;
      int free0 = countfree();
      for (struct test *t = tests; t->s != 0; t++) {
        if(!run(t->f, t->s)){
          fail = 1;
          break;
        }
      }
      if(fail){
        printf("SOME TESTS FAILED\n");
        if(continuous != 2)
          exit(1);
      }
      int free1 = countfree();
      if(free1 < free0){
        printf("FAILED -- lost %d free pages\n", free0 - free1);
        if(continuous != 2)
          exit(1);
      }
    }
  }

  printf("signaltests starting\n");
  int free0 = countfree();
  int free1 = 0;
  int fail = 0;
  for (struct test *t = tests; t->s != 0; t++) {
    if((justone == 0) || strcmp(t->s, justone) == 0) {
      if(!run(t->f, t->s))
        fail = 1;
    }
  }

  if(fail){
    printf("SOME TESTS FAILED\n");
    exit(1);
  } else if((free1 = countfree()) < free0){
    printf("FAILED -- lost some free pages %d (out of %d)\n", free1, free0);
    exit(1);
  } else {
    printf("ALL TESTS PASSED\n");
    exit(0);
  }
}
