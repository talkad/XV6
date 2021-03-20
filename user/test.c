#include "kernel/types.h"
#include "user/user.h"
#include "kernel/fcntl.h"

struct perf {
  uint64 ctime;                // Process creation time
  uint64 ttime;                // Process termination time
  uint64 stime;                // The total time the process spent in the SLEEPING mode
  uint64 retime;               // The total time the process spent in the RUNNABLE mode
  uint64 rutime;               // The total time the process spent in the RUNNING mode
  uint64 bursttime;            // Approximate estimated burst time
};



int main(int argc, char** argv){
    int i;
    struct perf* pr = malloc(sizeof(*pr));

    int pid = fork();
    if(pid!=0){
        int status;
        wait_stat(&status, pr);
        fprintf(2, "process status:\n creation = %d\n termination = %d\n sleeping = %d\n runable = %d\n running = %d\n bursttime = %d\n",
            pr->ctime, pr->ttime, pr->stime, pr->retime, pr->rutime, pr->bursttime);
    }
    else{
        for(i=0; i<=1000; i++)
            fprintf(2, "PANOS\n");
    }

    free(pr);
    exit(0);
}