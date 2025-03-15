/*
 * File Table
 * 
 * Created by Simon Schubiger on 04.03.2019
 * Rewritten in C by Andreas Grabher
 * 
 * Permission is hereby granted, free of charge, to any person obtaining a copy
 * of this software and associated documentation files (the "Software"), to deal
 * in the Software without restriction, including without limitation the rights
 * to use, copy, modify, merge, publish, distribute, sublicense, and/or sell
 * copies of the Software, and to permit persons to whom the Software is
 * furnished to do so, subject to the following conditions:
 *
 * The above copyright notice and this permission notice shall be included in
 * all copies or substantial portions of the Software.
 *
 * THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
 * IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
 * FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL
 * THE AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
 * LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM,
 * OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN
 * THE SOFTWARE.
 */
#include <slirp.h>
#include <stdlib.h>

#include "rpc.h"
#include "filetable.h"


struct ft_t* nfsd_fts[1];

static size_t ft_hash(uint64_t fhandle) {
    return fhandle & HASH_MASK;
}

static void ft_add(struct ft_t* ft, uint64_t fhandle, char* path) {
    struct ft_entry_t** entry;
    size_t index = ft_hash(fhandle);
    
    entry = &ft->table[index];
    
    while (*entry) {
        if ((*entry)->fhandle == fhandle) {
            if (strcmp((*entry)->path, path)) {
                printf("FILE TABLE ENTRY PATH CHANGED: %s->%s\n", (*entry)->path, path);
                free((*entry)->path);
                (*entry)->path = strdup(path);
            }
            return;
        }
        entry = &(*entry)->next;
    }
    *entry = (struct ft_entry_t*)malloc(sizeof(struct ft_entry_t));
    (*entry)->fhandle = fhandle;
    (*entry)->path    = strdup(path);
    (*entry)->next    = NULL;
}

static void ft_erase(struct ft_t* ft, uint64_t fhandle) {
    struct ft_entry_t** entry;
    struct ft_entry_t* next;
    size_t index = ft_hash(fhandle);
    
    entry = &ft->table[index];
    
    while (*entry) {
        if ((*entry)->fhandle == fhandle) {
            free((*entry)->path);
            next = (*entry)->next;
            free(*entry);
            *entry = next;
            return;
        }
        entry = &(*entry)->next;
    }
}

static void ft_delete(struct ft_t* ft, size_t index) {
    struct ft_entry_t** entry;
    struct ft_entry_t* next;
    
    entry = &ft->table[index];
    
    while (*entry) {
        free((*entry)->path);
        next = (*entry)->next;
        free((*entry));
        *entry = next;
    }
}

static char* ft_find(struct ft_t* ft, uint64_t fhandle) {
    struct ft_entry_t* entry;
    size_t index = ft_hash(fhandle);
    
    entry = ft->table[index];
    
    while (entry) {
        if (entry->fhandle == fhandle) {
            return entry->path;
        }
        entry = entry->next;
    }
    return NULL;
}


struct ft_t* ft_init(const char* host_path, const char* base_path_alias) {
    int i;
    struct ft_t* ft = (struct ft_t*)malloc(sizeof(struct ft_t));
    for (i = 0; i < HASH_SIZE; i++) {
        ft->table[i] = NULL;
    }
    ft->mutex = host_mutex_create();
    vfs = ft->vfs = vfs_init(host_path, base_path_alias);
    return ft;
}

struct ft_t* ft_uninit(struct ft_t* ft) {
    int i;
    for (i = 0; i < HASH_SIZE; i++) {
        ft_delete(ft, i);
    }
    host_mutex_destroy(ft->mutex);
    vfs = ft->vfs = vfs_uninit(ft->vfs);
    free(ft);
    return NULL;
}

int ft_is_inited(struct ft_t* ft) {
    if (vfs) {
        return 1;
    }
    return 0;
}

int ft_path_changed(struct ft_t* ft, char* host_path) {
    if (strcmp(vfs->host_base_path, host_path)) {
        return 1;
    }
    return 0;
}

int ft_get_canonical_path(struct ft_t* ft, uint64_t fhandle, char* vfs_path) {
    char* result;
    int retval = 0;
    host_mutex_lock(ft->mutex);
    
    result = ft_find(ft, fhandle);
    if (result == NULL) {
        strlcpy(vfs_path, "", RPC_MAXPATHLEN);
        retval = 0;
    } else {
        strlcpy(vfs_path, result, RPC_MAXPATHLEN);
        retval = 1;
    }
    host_mutex_unlock(ft->mutex);
    return retval;
}

int ft_stat(struct ft_t* ft, const char* vfs_path, struct stat* fstat) {
    int retval = 0;
    host_mutex_lock(ft->mutex);
    
    retval = vfs_get_fstat(vfs, vfs_path, fstat);
    
    host_mutex_unlock(ft->mutex);
    return retval;
}

void ft_move(struct ft_t* ft, uint64_t fhandle_from, char* vfs_path_to) {
    char path[RPC_MAXPATHLEN];
    
    vfs_path_canonicalize(vfs_path_to, path);
    
    host_mutex_lock(ft->mutex);
    
    ft_erase(ft, fhandle_from);
    ft_add(ft, vfs_get_fhandle(vfs, vfs_path_to), path);
    
    host_mutex_unlock(ft->mutex);
}

void ft_remove(struct ft_t* ft, uint64_t fhandle) {
    host_mutex_lock(ft->mutex);
    
    ft_erase(ft, fhandle);
    
    host_mutex_unlock(ft->mutex);
}

uint64_t ft_get_fhandle(struct ft_t* ft, const char* vfs_path) {
    char path[RPC_MAXPATHLEN];
    uint64_t fhandle;

    vfs_path_canonicalize(vfs_path, path);

    host_mutex_lock(ft->mutex);
    
    fhandle = vfs_get_fhandle(ft->vfs, vfs_path);
    ft_add(ft, fhandle, path);
    
    host_mutex_unlock(ft->mutex);
    return fhandle;
}

void ft_set_sattr(struct ft_t* ft, char* vfs_path, struct sattr_t* sattr) {
    host_mutex_lock(ft->mutex);
    
    vfs_set_sattr(ft->vfs, vfs_path, sattr);
    
    host_mutex_unlock(ft->mutex);
}

void ft_get_sattr(struct ft_t* ft, char* vfs_path, struct sattr_t* sattr) {
    host_mutex_lock(ft->mutex);
    
    vfs_get_sattr(ft->vfs, vfs_path, sattr);
    
    host_mutex_unlock(ft->mutex);
}
