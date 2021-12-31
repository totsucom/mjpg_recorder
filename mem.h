#ifndef __MYMEM_H
#define __MYMEM_H
#include <stdio.h>
#include <stdbool.h>

#define MYMEM_LOCKABLE

#ifdef MYMEM_LOCKABLE
#include <pthread.h>
#endif

typedef struct {
    void *ptr;
    int allocated_size;
    int active_size;
#ifdef MYMEM_LOCKABLE
    pthread_mutex_t mutex;
    bool locked;
#endif
} MYMEMORY;

typedef enum {
    ALLOC_NEW,
    REALLOC_JUST_SIZE,
    REALLOC_GROW_ONLY
} ALLOCATEMODE;

#ifdef MYMEM_LOCKABLE
bool lock_memory(MYMEMORY *pMem);
int trylock_memory(MYMEMORY *pMem);
bool unlock_memory(MYMEMORY *pMem);
#endif

void init_memory_struct(MYMEMORY *pMem);
bool alloc_memory(MYMEMORY *pMem, int size, ALLOCATEMODE allocmode, bool fillzero);
void free_memory(MYMEMORY *pMem);
void clear_memory(MYMEMORY *pMem, bool fillzero);
int available_memory_size(MYMEMORY *pMem);
int load_fp_to_memory(MYMEMORY *pMem, FILE *fp, int size, bool append);
bool save_memory_to_fp(MYMEMORY *pMem, FILE *fp);
FILE *memory_to_readable_fp(MYMEMORY *pMem);
FILE *memory_to_writable_fp(MYMEMORY *pMem);

#endif