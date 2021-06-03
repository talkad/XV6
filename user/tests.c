#include "kernel/types.h"
#include "kernel/stat.h"
#include "user/user.h"

#define PGSIZE 4096

void
test_alloc_and_use_ramonly(void){
    printf("test_alloc_and_use_ramonly\n");
    char* a = sbrk(10*PGSIZE);
    for(int i = 0; i < 10*PGSIZE; ++i)
        a[i] = 'A';
    sbrk(-10*PGSIZE);
    printf("TEST PASSED\n\n");
}

void
test_alloc_over_totalpages(void){
    printf("test_alloc_over_totalpages (should point out file exceeds)\n");
    int cpid = fork();
    if(!cpid)
        malloc(32*PGSIZE);
    wait(0);
    printf("TEST PASSED\n\n");     
}

void
test_alloc_and_use_with_disk(void){
    printf("test_alloc_and_use_with_disk\n");
    char* a = malloc(18*PGSIZE);
    for(int i = 0; i < 18*PGSIZE; ++i)
        a[i] = 'C';      
    free(a);
    printf("TEST PASSED\n\n");
}

void
test_alloc_and_use_ramonly_with_fork(void){
    printf("test_alloc_and_use_ramonly_with_fork\n");
    char* a = sbrk(7*PGSIZE);
    int cpid = fork();
    if(!cpid){
        for(int i = 0; i < 7*PGSIZE; ++i)
            a[i] = 'D';  
        sbrk(-7*PGSIZE);   
        exit(0);
    }
    wait(0);      
    sbrk(-7*PGSIZE);
    printf("TEST PASSED\n\n");
}

void
test_alloc_and_use_with_disk_and_fork(void){
    printf("test_alloc_and_use_with_disk_and_fork\n");
    char* a = malloc(20*PGSIZE);
    int cpid = fork();
    for(int i = 0; i < 20*PGSIZE; ++i)
        a[i] = 'D'; 
        
    if(!cpid){
        for(int i = 0; i < 20*PGSIZE; ++i)
            a[i] = 'd';      
        free(a);    
        exit(0);
    }
    wait(0);      
    free(a);
    printf("TEST PASSED\n\n");
}

int
main(int argc, char *argv[])
{
    // test_alloc_and_use_ramonly();
    // test_alloc_and_use_with_disk();
    // test_alloc_over_totalpages();
    // test_alloc_and_use_ramonly_with_fork();
    test_alloc_and_use_with_disk_and_fork();
  exit(0);
}