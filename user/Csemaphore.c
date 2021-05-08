#include "Csemaphore.h"

void csem_down(struct counting_semaphore *sem){
    bsem_down(sem->binSemB);
    bsem_down(sem->binSemA);
    sem->value--;

    if(sem->value > 0)
        bsem_up(sem->binSemB);

    bsem_up(sem->binSemA);    
}

void csem_up(struct counting_semaphore *sem){
    bsem_down(sem-binSemA);
    sem->value++;

    if(sem->value == 1)
        bsem_up(sem->binSemB);

    bsem_up(sem->binSemA);    
}

int csem_alloc(struct counting_semaphore *sem, int initial_value){
    if(sem){
        int firstsem = bsem_alloc();
        int secondsem = bsem_alloc();
        if((firstsem == -1) || (secondsem == -1)){
            bsem_free(firstsem);
            bsem_free(secondsem);
            return -1;
        }

        sem->value = initial_value
        sem->binSemA = firstsem;
        sem->binSemB = secondsem;

        if(initial_value == 0)
            bsem_down(sem->binSemB);

        return 0;
    }
    else
        return -1;
}

void csem_free(struct counting_semaphore *sem){
    bsem_free(sem->binSemA);
    bsem_free(sem->binSemB);
    free(sem);
}