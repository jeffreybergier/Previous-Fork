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

/* Pull in ntohs()/ntohl()/htons()/htonl() declarations... shotgun approach */
#if defined(linux) || defined(__MINGW32__)
    /* netinet/in.h doesn't have proper extern "C" declarations for these... may also apply to other Unices */
    extern "C" uint32_t ntohl(uint32_t);
    extern "C" uint16_t ntohs(uint16_t);
    extern "C" uint32_t htonl(uint32_t);
    extern "C" uint16_t htons(uint16_t);
#else
    #if HAVE_ARPA_INET_H
        #include <arpa/inet.h>
    #endif
    #if HAVE_NETINET_IN_H
        #include <netinet/in.h>
    #endif
    #if HAVE_WINSOCK_H
        #include <winsock.h>
    #endif
#endif


struct im_t* diskimage_init(const char* path) {
    struct im_t* im = (struct im_t*)malloc(sizeof(struct im_t));
    
    im->imf = fopen(path, "rb");
    im->error = NULL;
    im->diskOffset = 0;
    im->blockSize = BLOCKSZ;
    im->rawOptical = false;
    im->sectorSize = 0;
    im->path = path;
    if (im->imf == NULL) {
        im->error = strerror(errno);
        return im;
    }
    
    diskimage_read(im, 0, sizeof(im->dl), &im->dl);
    if (strncmp(im->dl.dl_version, "NeXT", 4) &&
        strncmp(im->dl.dl_version, "dlV2", 4) &&
        strncmp(im->dl.dl_version, "dlV3", 4)) {
        im->diskOffset = MO_BLOCK0;
        im->blockSize  = MO_BLOCKSZ;
        im->rawOptical = true;
        
        memset(im->bbt, 0, sizeof(im->bbt));
        memset(im->bm, 0, sizeof(im->bm));
        im->bbt_size = 0;
        im->spa      = 1;
        
        diskimage_read(im, 0, sizeof(im->dl), &im->dl);
        if (strncmp(im->dl.dl_version, "NeXT", 4) &&
            strncmp(im->dl.dl_version, "dlV2", 4) &&
            strncmp(im->dl.dl_version, "dlV3", 4)) {
            printf("Unknown version: %.4s\n", im->dl.dl_version);
            exit(1);
        }
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
        diskimage_read(im, im->bbt_off, im->bbt_size * sizeof(uint32_t), im->bbt);
        
        im->bm_off  = 16*BLOCKSZ;
        im->bm_size = 16*BLOCKSZ;
        diskimage_read(im, im->bm_off, im->bm_size * sizeof(uint32_t), im->bm);
        
        int bad = 0;
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
            printf("%"PRId64" entries in bad block table\n", im->bbt_size);
            printf("%d alternate groups of %d sectors each\n", ntohs(im->dl.dl_dt.d_ngroups), ntohs(im->dl.dl_dt.d_ag_size));
            printf("%d sectors per alternate group at offset %d\n", ntohs(im->dl.dl_dt.d_ag_alts), ntohs(im->dl.dl_dt.d_ag_off));
            printf("%d alternates per alternate group with %d sectors per alternate\n", im->apag, im->spa);
        }
    }
    im->sectorSize = ntohl(im->dl.dl_dt.d_secsize);
    if (im->sectorSize != 0x400) {
        printf("Unsupported sector size: %"PRIu64"\n", im->sectorSize);
        exit(1);
    }
    
    /* Add partitions */
    im->parts = NULL;
    for (int p = 0, i = 0; p < NPART; p++) {
        if (ntohs(im->dl.dl_dt.d_partitions[p].p_bsize) == 0 || ntohs(im->dl.dl_dt.d_partitions[p].p_bsize) == 0xffff)
            continue;
        partition_init(p, i++, im, &im->dl, &im->dl.dl_dt.d_partitions[p]);
    }
    return im;
}

void diskimage_uninit(struct im_t* im) {
    if (im->imf) fclose(im->imf);
    partition_uninit(im->parts);
    free(im);
}

int diskimage_read(struct im_t* im, int offset, int size, void* data) {
    int64_t block     = offset / BLOCKSZ;
    int64_t blockOff  = offset % BLOCKSZ;
    int result        = 0;
    size_t bytesRead  = 0;
    uint8_t* dataPtr  = (uint8_t*)data;
    uint8_t* buffer   = (uint8_t*)malloc(im->blockSize);
    while (size > 0) {
        int64_t rdSize = (int64_t)size < (BLOCKSZ - blockOff) ? (int64_t)size : (BLOCKSZ - blockOff);
        fseeko(im->imf, block * im->blockSize + im->diskOffset, SEEK_SET);
        bytesRead = fread(buffer, 1, im->blockSize, im->imf);
        if (im->rawOptical) {
            size_t bmIndex = block / im->spa;
            int    bmShift = (bmIndex & 0xF) << 1;
            int    bmValue = (ntohl(im->bm[bmIndex>>4]) >> bmShift) & 3;
            switch (bmValue) {
                case BM_UNTESTED:
                case BM_WRITTEN:
                    if (rs_decode((uint8_t*)buffer) >= 0)
                        break;
                    if (bmValue != BM_UNTESTED)
                        printf("Warning: block %"PRId64" not decodable\n", block);
                case BM_BAD:
                {
                    bool mappedBlock = false;
                    for (size_t bbtIndex = 0; bbtIndex < im->bbt_size; bbtIndex++) {
                        if (ntohl(im->bbt[bbtIndex]) == 0)
                            continue;
                        if (ntohl(im->bbt[bbtIndex]) == block) {
                            uint32_t reserve = bbtIndex / im->apag;
                            if (reserve < ntohs(im->dl.dl_dt.d_ngroups)) {
                                reserve *= ntohs(im->dl.dl_dt.d_ag_size);
                                reserve += ntohs(im->dl.dl_dt.d_ag_off) + (bbtIndex % im->apag) * im->spa;
                            } else {
                                reserve = ntohs(im->dl.dl_dt.d_ngroups) * ntohs(im->dl.dl_dt.d_ag_size);
                                reserve += (bbtIndex - ntohs(im->dl.dl_dt.d_ngroups) * im->apag) * im->spa;
                            }
                            reserve += block % im->spa;
                            reserve += ntohs(im->dl.dl_dt.d_front);
                            
                            printf("Mapping bad block %"PRId64" to %d\n", block, reserve);
                            diskimage_read(im, reserve * BLOCKSZ, BLOCKSZ, buffer);
                            mappedBlock = true;
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
        memcpy(dataPtr, buffer + blockOff, rdSize);
        blockOff = 0;
        size    -= rdSize;
        dataPtr += rdSize;
        block++;
        if (bytesRead != im->blockSize) {
            result = ferror(im->imf) ? ERR_FAIL : ERR_EOF;
            printf("Can't read %d bytes at offset %d\n", size, offset);
            break;
        }
    }
    free(buffer);
    
    return result;
}

bool diskimage_valid(struct im_t* im) {
    return !(im->error && strlen(im->error));
}

void diskimage_print(struct im_t* im) {
    uint64_t size = im->sectorSize;
    struct part_t* part = im->parts;
    size *= ntohl(im->dl.dl_dt.d_ntracks);
    size *= ntohl(im->dl.dl_dt.d_nsectors);
    size *= ntohl(im->dl.dl_dt.d_ncylinders);
    size >>= 20;
    printf("Disk '%.*s' '%.*s' '%.*s' %"PRIu64" MBytes\n", MAXDNMLEN, im->dl.dl_dt.d_name, MAXLBLLEN, im->dl.dl_label, MAXTYPLEN, im->dl.dl_dt.d_type, size);
    printf("  Sector size: %"PRIu64" Bytes\n\n", im->sectorSize);
    while (part) {
        partition_print(part);
        part = part->next;
    }
}
