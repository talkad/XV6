#include "kernel/types.h"
#include "user/user.h"
#include "kernel/fcntl.h"

struct perf {
  int ctime;                // Process creation time
  int ttime;                // Process termination time
  int stime;                // The total time the process spent in the SLEEPING mode
  int retime;               // The total time the process spent in the RUNNABLE mode
  int rutime;               // The total time the process spent in the RUNNING mode
  int average_bursttime;    // Approximate estimated burst time
};


int main(int argc, char** argv){
    int i;
    struct perf *pr = malloc(sizeof(*pr));

    int pid = fork();
    if(pid!=0){
        int status;
        wait_stat(&status, pr);
        fprintf(2, "process status:\n creation = %d\n termination = %d\n sleeping = %d\n runnable = %d\n running = %d \n  burst = %d \n",
            pr->ctime, pr->ttime, pr->stime, pr->retime, pr->rutime, pr->average_bursttime);

        free(pr);
    }
    else{
        sleep(10);
        for(i=0; i<=100; i++)
            fprintf(2, "PANOS\n");

        sleep(10);
    }

    exit(0);
}