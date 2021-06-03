#include "kernel/types.h"
#include "kernel/stat.h"
#include "user/user.h"

#define PGSIZE 4096

int
main(int argc, char *argv[])
{
  char *a = malloc(PGSIZE*25);
  int i = fork();
  if(!i){
    for(int j = 0; j < PGSIZE*25; ++j)
        a[j] = 1;
    for(int j = 0; j < PGSIZE*25; ++j)
        printf("%d",a[j]);
    exit(0);    
  } 
  wait(0);
  exit(0);
}