
#include <assert.h>
#include <stdlib.h>
#include <stdio.h>
#include <string.h>
#include <unistd.h>
#include <stdbool.h>
#include <sys/mman.h>
#include <pthread.h>

#include "mmu.h"
#include "pager.h"


void pager_init(int nframes, int nblocks){

}

void pager_create(pid_t pid){

}

void *pager_extend(pid_t pid){

}

void pager_fault(pid_t pid, void *addr){

}

int pager_syslog(pid_t pid, void *addr, size_t len){

}

void pager_destroy(pid_t pid){
    
}