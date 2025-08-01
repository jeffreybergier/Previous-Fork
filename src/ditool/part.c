/*
 *  part.c (former Partition.cpp)
 *  Previous
 *
 *  Created by Simon Schubiger on 03.03.19.
 *
 *  Rewritten in C by Andreas Grabher.
 */

#include <stdlib.h>
#include <string.h>
#include <inttypes.h>

#include "part.h"
#include "fs.h"
#include "ufs.h"


void partition_init(int partNo, size_t partIdx, struct im_t* im, const struct disk_label* dl, struct disk_partition* partition) {
    struct part_t** parts = &im->parts;
    while (*parts) {
        parts = &(*parts)->next;
    }
    *parts = (struct part_t*)malloc(sizeof(struct part_t));
    (*parts)->partNo = partNo;
    (*parts)->partIdx = partIdx;
    (*parts)->im = im;
    (*parts)->dl = dl;
    (*parts)->part = partition;
    (*parts)->next = NULL;
}

void partition_uninit(struct part_t* parts) {
    struct part_t* next;
    while (parts) {
        next = parts->next;
        free(parts);
        parts = next;
    }
}

int partition_readSectors(struct part_t* part, uint32_t sector, uint32_t count, uint8_t* dst) {
    size_t size;
    int result;
    const struct disktab* dt = &part->im->dl.dl_dt;
    int64_t      usable      = ntohs(dt->d_ag_size) - ntohs(dt->d_ag_alts);
    
    sector += ntohl(part->part->p_base);
    
    int64_t limit = 0;
    int64_t offset = 0;
    do {
        if (usable) {
            offset = sector % usable;
            if (offset >= ntohs(dt->d_ag_off))
                offset += ntohs(dt->d_ag_alts);
            if (offset < ntohs(dt->d_ag_off))
                limit = ntohs(dt->d_ag_off) - offset;
            else
                limit = ntohs(dt->d_ag_size) - offset + ntohs(dt->d_ag_off);
            limit = count > limit ? limit : count;
            offset += sector / usable * ntohs(dt->d_ag_size);
        } else {
            limit  = count;
            offset = sector;
        }
        offset += ntohs(dt->d_front);
        
        offset *= part->im->sectorSize;
        size = limit * part->im->sectorSize;
        
        result = diskimage_read(part->im, offset, size, dst);
        
        sector += limit;
        count  -= limit;
        dst    += size;
    } while (count > 0 && result == ERR_NO);
    
    return result;
}

void partition_print(struct part_t* part) {
    struct ufs_t* ufs;
    uint64_t size = ntohl(part->part->p_size);
    size *= part->im->sectorSize;
    size >>= 20;
    printf("  Partition #%zu: %.*s %"PRIu64" MBytes\n", part->partIdx, MAXFSTLEN - 1, &part->part->p_type[1], size);
    ufs = ufs_init(part);
    if (ufs) {
        ufs_print(ufs);
        ufs_uninit(ufs);
    }
}
