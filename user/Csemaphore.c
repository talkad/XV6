#include "Csemaphore.h"
#include "kernel/types.h"
#include "kernel/stat.h"
#include "kernel/fcntl.h"
#include "user/user.h"

void csem_down(struct counting_semaphore *sem){
    bsem_down(sem->binSemB);
    bsem_down(sem->binSemA);
    sem->value--;

    if(sem->value > 0)
        bsem_up(sem->binSemB);

    bsem_up(sem->binSemA);    
}

void csem_up(struct counting_semaphore *sem){
    bsem_down(sem->binSemA);
    sem->value++;

    if(sem->value == 1)
        bsem_up(sem->binSemB);

    bsem_up(sem->binSemA);    
}

int csem_alloc(struct counting_semaphore *sem, int initial_value){
    int firstsem = bsem_alloc();
    if(firstsem < 0){
        bsem_free(firstsem);
        return -1;
    }

    int secondsem = bsem_alloc();
    if(secondsem < 0){
        bsem_free(firstsem);
        bsem_free(secondsem);
        return -1;
    }

    sem->value = initial_value;
    sem->binSemA = firstsem;
    sem->binSemB = secondsem;

    if(initial_value == 0)
        bsem_down(sem->binSemB);

    return 0;

}

void csem_free(struct counting_semaphore *sem){
    bsem_free(sem->binSemA);
    bsem_free(sem->binSemB);
}