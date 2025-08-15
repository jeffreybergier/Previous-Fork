/*
 *  im.c (former DiskImage.cpp)
 *  Previous
 *
 *  Created by Simon Schubiger on 03.03.19.
 *
 *  Rewritten in C by Andreas Grabher.
 */

#include <stdlib.h>
#include <string.h>
#include <inttypes.h>
#include <errno.h>

#include "rs.h"
#include "im.h"
#include "part.h"


static bool label_valid(const char* v) {
    return strncmp(v, "dlV3", 4) == 0 || strncmp(v, "dlV2", 4) == 0 || strncmp(v, "NeXT", 4) == 0;
}

struct im_t* diskimage_init(const char* path) {
    struct im_t* im = (struct im_t*)malloc(sizeof(struct im_t));
    
    im->imf        = fopen(path, "rb");
    im->error      = NULL;
    im->parts      = NULL;
    im->diskOffset = 0;
    im->blockSize  = BLOCKSZ;
    im->rawOptical = false;
    im->sectorSize = 0;
    im->path       = path;
    if (im->imf == NULL) {
        im->error = strerror(errno);
        return im;
    }
    memset(&im->dl, 0, sizeof(im->dl));
    
    if (diskimage_read(im, 0, sizeof(im->dl), &im->dl)) {
        im->error = "Reading disk label failed";
        return im;
    }
    if (label_valid(im->dl.dl_version) == false) {
        im->diskOffset = 46;
        
        if (diskimage_read(im, 0, sizeof(im->dl), &im->dl)) {
            im->error = "Reading disk label failed";
            return im;
        }
        if (label_valid(im->dl.dl_version) == false) {
            im->diskOffset = MO_BLOCK0;
            im->blockSize  = MO_BLOCKSZ;
            im->rawOptical = true;
            
            memset(im->bbt, 0, sizeof(im->bbt));
            memset(im->bm, 0, sizeof(im->bm));
            im->bbt_size = 0;
            im->spa      = 1;
            
            diskimage_read(im, 0, sizeof(im->dl), &im->dl);
            if (label_valid(im->dl.dl_version) == false) {
                printf("No valid disk label found.\n");
                memset(&im->dl, 0, sizeof(im->dl));
                im->diskOffset = 0;
                im->blockSize  = BLOCKSZ;
                im->rawOptical = false;
                im->sectorSize = 0x400;
                printf("Partition data of type 4.3BSD with %"PRIu64" Byte sectors is assumed.\n\n", im->sectorSize);
                partition_init(0, im, NULL);
                return im;
            } else {
                int bad = 0;
                
                printf("Magneto-optical disk detected\n");
                
                if (strncmp(im->dl.dl_version, "NeXT", 4)) {
                    im->spa = 1;
                } else {
                    im->spa = ntohl(im->dl.dl_dt.d_nsectors) >> 1;
                }
                if (im->spa < 1) {
                    printf("Bad number of sectors per alternate\n");
                    im->spa = 1;
                }
                
                im->apag = ntohs(im->dl.dl_dt.d_ag_alts) / im->spa;
                if (im->apag < 1) {
                    printf("Bad number of alternates per alternate group\n");
                    im->apag = 1;
                }
                
                if (strncmp(im->dl.dl_version, "dlV3", 4)) {
                    im->bbt_off  = 558;
                    im->bbt_size = 1670;
                } else {
                    im->bbt_off  = 4*BLOCKSZ;
                    im->bbt_size = 3*BLOCKSZ;
                }
                if (diskimage_read(im, im->bbt_off, im->bbt_size * sizeof(uint32_t), im->bbt)) {
                    im->error = "Reading bad block table failed";
                    return im;
                }
                
                im->bm_off  = 16*BLOCKSZ;
                im->bm_size = 16*BLOCKSZ;
                if (diskimage_read(im, im->bm_off, im->bm_size * sizeof(uint32_t), im->bm)) {
                    im->error = "Reading bitmap failed";
                    return im;
                }
                
                for (int i = 0; i < im->bbt_size; i++) {
                    if (im->bbt[i] == 0xFFFFFFFF) {
                        im->bbt_size = i;
                        break;
                    }
                    if (im->bbt[i] > 0) {
                        bad++;
                    }
                }
                if (bad > 0) {
                    bad *= im->spa;
                    printf("Disk has %d bad blocks\n", bad);
                    printf("Label: %.4s\n", im->dl.dl_version);
                    printf("%d entries in bad block table\n", im->bbt_size);
                    printf("%d alternate groups of %d sectors each\n", ntohs(im->dl.dl_dt.d_ngroups), ntohs(im->dl.dl_dt.d_ag_size));
                    printf("%d sectors per alternate group at offset %d\n", ntohs(im->dl.dl_dt.d_ag_alts), ntohs(im->dl.dl_dt.d_ag_off));
                    printf("%d alternates per alternate group with %d sectors per alternate\n", im->apag, im->spa);
                }
                printf("\n");
            }
        } else {
            printf("DiskCopyII image detected. Skipping header.\n\n");
        }
    }
    im->sectorSize = ntohl(im->dl.dl_dt.d_secsize);
    if (im->sectorSize < 0x400 || im->sectorSize > 0x2000 || (im->rawOptical && im->sectorSize != 0x400)) {
        printf("Sector size: %"PRIu64"\n", im->sectorSize);
        im->error = "Unsupported sector size";
        return im;
    }
    
    /* Add partitions */
    for (int p = 0; p < NPART; p++) {
        if (ntohs(im->dl.dl_dt.d_partitions[p].p_bsize) == 0 || ntohs(im->dl.dl_dt.d_partitions[p].p_bsize) == 0xffff)
            continue;
        partition_init(p, im, &im->dl.dl_dt.d_partitions[p]);
    }
    return im;
}

void diskimage_uninit(struct im_t* im) {
    if (im->imf) fclose(im->imf);
    partition_uninit(im->parts);
    free(im);
}

int diskimage_read(struct im_t* im, int64_t offset, int64_t size, void* data) {
    int64_t block     = offset / BLOCKSZ;
    int64_t blockOff  = offset % BLOCKSZ;
    int result        = 0;
    size_t bytesRead  = 0;
    uint8_t* dataPtr  = (uint8_t*)data;
    uint8_t* buffer   = (uint8_t*)malloc(im->blockSize);
    while (size > 0) {
        int64_t rdSize = size < (BLOCKSZ - blockOff) ? size : (BLOCKSZ - blockOff);
        fseeko(im->imf, block * im->blockSize + im->diskOffset, SEEK_SET);
        bytesRead = fread(buffer, 1, im->blockSize, im->imf);
        if (im->rawOptical) {
            int bmIndex = (int)(block / im->spa);
            int bmShift = (bmIndex & 0xF) << 1;
            int bmValue = (ntohl(im->bm[bmIndex>>4]) >> bmShift) & 3;
            switch (bmValue) {
                case BM_UNTESTED:
                case BM_WRITTEN:
                    if (rs_decode(buffer) >= 0)
                        break;
                    if (bmValue != BM_UNTESTED)
                        printf("Warning: block %"PRId64" not decodable\n", block);
                case BM_BAD:
                {
                    bool mappedBlock = false;
                    for (int bbtIndex = 0; bbtIndex < im->bbt_size; bbtIndex++) {
                        if (ntohl(im->bbt[bbtIndex]) == 0)
                            continue;
                        if (ntohl(im->bbt[bbtIndex]) == block) {
                            int64_t reserve = bbtIndex / im->apag;
                            if (reserve < ntohs(im->dl.dl_dt.d_ngroups)) {
                                reserve *= ntohs(im->dl.dl_dt.d_ag_size);
                                reserve += ntohs(im->dl.dl_dt.d_ag_off) + (bbtIndex % im->apag) * im->spa;
                            } else {
                                reserve = ntohs(im->dl.dl_dt.d_ngroups) * ntohs(im->dl.dl_dt.d_ag_size);
                                reserve += (bbtIndex - ntohs(im->dl.dl_dt.d_ngroups) * im->apag) * im->spa;
                            }
                            reserve += block % im->spa;
                            reserve += ntohs(im->dl.dl_dt.d_front);
                            
                            printf("Mapping bad block %"PRId64" to %"PRId64"\n", block, reserve);
                            if (diskimage_read(im, reserve * BLOCKSZ, BLOCKSZ, buffer) == ERR_NO) {
                                mappedBlock = true;
                            }
                            break;
                        }
                    }
                    if (mappedBlock)
                        break;
                    if (bmValue != BM_UNTESTED)
                        printf("Unable to re-map bad block %"PRId64"\n", block);
                }
                case BM_ERASED:
                    memset(buffer, 0, im->blockSize);
                    break;
                    
                default:
                    break;
            }
        }
        if (bytesRead != im->blockSize) {
            result = ferror(im->imf) ? ERR_FAIL : ERR_EOF;
            const char* errstr = (result == ERR_EOF) ? "End of file" : "Read error";
            printf("Can't read %"PRId64" bytes at offset %"PRId64" (%s).\n", size, offset, errstr);
            break;
        }
        memcpy(dataPtr, buffer + blockOff, rdSize);
        blockOff = 0;
        size    -= rdSize;
        dataPtr += rdSize;
        block++;
    }
    free(buffer);
    
    return result;
}

bool diskimage_valid(struct im_t* im) {
    return !(im->error && strlen(im->error));
}

void diskimage_print(struct im_t* im) {
    struct part_t* part = im->parts;
    if (im->dl.dl_version[0]) {
        uint64_t size = im->sectorSize;
        size *= ntohl(im->dl.dl_dt.d_ntracks);
        size *= ntohl(im->dl.dl_dt.d_nsectors);
        size *= ntohl(im->dl.dl_dt.d_ncylinders);
        size >>= 20;
        printf("Disk '%.*s' '%.*s' %"PRIu64" MBytes\n", MAXDNMLEN, im->dl.dl_dt.d_name, MAXTYPLEN, im->dl.dl_dt.d_type, size);
        printf("  Version:     '%.*s'\n", 4, im->dl.dl_version);
        printf("  Label:       '%.*s'\n", MAXLBLLEN, im->dl.dl_label);
        printf("  Sector size: %"PRIu64" Bytes\n\n", im->sectorSize);
    }
    while (part) {
        partition_print(part);
        part = part->next;
    }
}
