#include <stdlib.h>
#include <string.h>
#include <stddef.h>
#include <errno.h>
#include "mem.h"

//構造体を初期化する
void init_memory_struct(MYMEMORY *pMem) {
    if (pMem == NULL) return;
    memset(pMem, 0, sizeof(MYMEMORY));
}

//ロック
#ifdef MYMEM_LOCKABLE
bool lock_memory(MYMEMORY *pMem) {
    if (pMem == NULL) return false;
    if (pMem->locked) return true;
    if (pthread_mutex_lock(&pMem->mutex) != 0)
        return false;
    pMem->locked = true;
    return true;
}
#endif

//ロック（トライ版）
#ifdef MYMEM_LOCKABLE
int trylock_memory(MYMEMORY *pMem) {
    if (pMem == NULL) return -1;
    if (pMem->locked) return true;
    if (pthread_mutex_trylock(&pMem->mutex) == 0) {
        pMem->locked = true;
        return 1;   //OK(locked)
    }
    if (errno == EBUSY)
        return 0;   //NG(busy)
    return -1;      //NG(error)
}
#endif

//アンロック
#ifdef MYMEM_LOCKABLE
bool unlock_memory(MYMEMORY *pMem) {
    if (pMem == NULL) return false;
    if (!pMem->locked) return true;
    if (pthread_mutex_unlock(&pMem->mutex) != 0)
        return false;
    pMem->locked = false;
    return true;
}
#endif

//メモリを確保、再確保も可能
//空の構造体にreallocした場合は新規で確保される
//reallocでsaize=0の場合はfreeを実行する
//再確保で失敗した場合は旧メモリも解放される
bool alloc_memory(MYMEMORY *pMem, int size, ALLOCATEMODE allocmode, bool fillzero) {
    if (pMem == NULL) return false;
    void *p;
    if (allocmode != ALLOC_NEW && pMem->ptr != NULL) {
        if (size <= 0) {
            free_memory(pMem);
            return true;
        }
        if (allocmode == REALLOC_JUST_SIZE ||
            (allocmode == REALLOC_GROW_ONLY && pMem->allocated_size < size)) {
            p = realloc(pMem->ptr, size);
            if (p == NULL) {
                free_memory(pMem);
                return false;
            }
            pMem->ptr = p;
            pMem->allocated_size = size;
        }
        if (fillzero) memset(pMem->ptr, 0, pMem->allocated_size);
    } else {
        memset(pMem, 0, sizeof(MYMEMORY));
#ifdef MYMEM_LOCKABLE
        if (pthread_mutex_init(&pMem->mutex, NULL) != 0)
            return false;
#endif
        if (size > 0) {
            void *p = malloc(size);
            if (p == NULL) {
#ifdef MYMEM_LOCKABLE
                pthread_mutex_destroy(&pMem->mutex);
#endif
                return false;
            }
            pMem->ptr = p;
            pMem->allocated_size = size;
            if (fillzero) memset(p, 0, size);
        }
    }
    return true;
}

void free_memory(MYMEMORY *pMem) {
    if (pMem == NULL) return;
#ifdef MYMEM_LOCKABLE
    unlock_memory(pMem);
#endif
    if (pMem->ptr != NULL) free(pMem->ptr);
#ifdef MYMEM_LOCKABLE
    pthread_mutex_destroy(&pMem->mutex);
#endif
    memset(pMem, 0, sizeof(MYMEMORY));
}

void clear_memory(MYMEMORY *pMem, bool fillzero) {
    if (pMem == NULL) return;
    pMem->active_size = 0;
    if (fillzero && pMem->ptr != NULL) memset(pMem->ptr, 0, pMem->allocated_size);
}

int available_memory_size(MYMEMORY *pMem) {
    if (pMem == NULL || pMem->ptr == NULL) return 0;
    return pMem->allocated_size - pMem->active_size;
}

//読んだサイズを返す
int load_fp_to_memory(MYMEMORY *pMem, FILE *fp, int size, bool append) {
    if (pMem == NULL || pMem->ptr == NULL || fp == NULL || size <= 0) return 0;
    int available = pMem->allocated_size;
    char *p = (char *)pMem->ptr;
    if (append) {
        available -= pMem->active_size;
        p += pMem->active_size;
    }
    int readable_size = (available < size) ? available : size;
    if (readable_size <= 0) return 0;

    int read_size = fread(p, 1, readable_size, fp);
    pMem->active_size += read_size;
    return read_size;
}

bool save_memory_to_fp(MYMEMORY *pMem, FILE *fp) {
    if (pMem == NULL || pMem->ptr == NULL || fp == NULL || pMem->active_size <= 0) return false;
    int size = fwrite(pMem->ptr, 1, pMem->active_size, fp);
    return (size == pMem->active_size);
}

//ファイルポインタはfclose()でクローズできる
//active_sizeの示す有効なバッファサイズを渡していることに注意
FILE *memory_to_readable_fp(MYMEMORY *pMem) {
    if (pMem == NULL || pMem->ptr == NULL) return NULL;
    return fmemopen(pMem->ptr, pMem->active_size, "rb");
}

//ファイルポインタはfclose()でクローズできる
//バッファ全体を渡していることに注意
//active_size=0が設定され、必要に応じてユーザーがactive_sizeを設定すること
FILE *memory_to_writable_fp(MYMEMORY *pMem) {
    if (pMem == NULL) return NULL;
    pMem->active_size = 0;
    if (pMem->ptr == NULL) return NULL;
    return fmemopen(pMem->ptr, pMem->allocated_size, "wb");
}
