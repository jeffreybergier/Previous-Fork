/*
 * Network File System Program
 * 
 * Created by Simon Schubiger
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
#include <inttypes.h>
#ifndef _WIN32
#include <sys/statvfs.h>
#endif

#include "vfs.h"
#include "rpc.h"
#include "nfs.h"


#ifndef _WIN32

#if !HAVE_STRUCT_STAT_ST_ATIMESPEC
#define st_atimespec st_atim
#endif

#if !HAVE_STRUCT_STAT_ST_MTIMESPEC
#define st_mtimespec st_mtim
#endif

#endif

struct timeval_t {
    uint32_t sec;
    uint32_t usec;
};

struct sattr_t {
    uint32_t mode;
    uint32_t uid;
    uint32_t gid;
    uint32_t size;
    struct timeval_t atime;
    struct timeval_t mtime;
    
    uint32_t rdev; /* FIXME: used for CREATE but does not belong here */
};

struct fattr_t {
    uint32_t type;
    uint32_t mode;
    uint32_t nlink;
    uint32_t uid;
    uint32_t gid;
    uint32_t size;
    uint32_t blocksize;
    uint32_t rdev;
    uint32_t blocks;
    uint32_t fsid;
    uint32_t fileid;
    struct timeval_t atime;
    struct timeval_t mtime;
    struct timeval_t ctime;
};

enum {
    NFS_OK             = 0,
    NFSERR_PERM        = 1,
    NFSERR_NOENT       = 2,
    NFSERR_IO          = 5,
    NFSERR_NXIO        = 6,
    NFSERR_ACCES       = 13,
    NFSERR_EXIST       = 17,
    NFSERR_NODEV       = 19,
    NFSERR_NOTDIR      = 20,
    NFSERR_ISDIR       = 21,
    NFSERR_FBIG        = 27,
    NFSERR_NOSPC       = 28,
    NFSERR_ROFS        = 30,
    NFSERR_NAMETOOLONG = 63,
    NFSERR_NOTEMPTY    = 66,
    NFSERR_DQUOT       = 69,
    NFSERR_STALE       = 70,
    NFSERR_WFLUSH      = 99
};

static int nfs_err(int error) {
    switch (error) {
        case 0:      return NFS_OK;
        case ENOENT: return NFSERR_NOENT;
        case EACCES: return NFSERR_ACCES;
        case EINVAL: return NFSERR_IO;
        default:
            return NFSERR_IO;
    }
}

enum NFTYPE {
    NFNON  = 0, 
    NFREG  = 1,
    NFDIR  = 2,
    NFBLK  = 3,
    NFCHR  = 4,
    NFLNK  = 5, 
    NFSOCK = 6,
    NFFIFO = 7, 
    NFBAD  = 8
};

static int valid32(uint32_t statval) { return statval != 0xFFFFFFFF; }
static int valid16(uint32_t statval) { return (statval & 0x0000FFFF) != 0x0000FFFF; }

#define NFS_FIFO_DEV 0xFFFFFFFF

static const int BLOCK_SIZE = 4096;

static void setUserID(uint32_t uid, uint32_t gid) {
    vfs_set_default_uid_gid(uid, gid);
}

static int getPath(struct xdr_t* m_in, char* path, uint64_t* fhandle) {
    const char* cpath;
    uint64_t data[4];
    int result;
    
    if (m_in->size < FHSIZE) return -1;
    xdr_read_data(m_in, (void*)data, FHSIZE);
    
    if (fhandle) *fhandle = data[0];
    
    result = vfs_get_canonical_patch(data[0], &cpath);
    
    strncpy(path, cpath, RPC_MAXPATHLEN);
    
    return result;
}

static int getFullPath(struct xdr_t* m_in, char* result) {
    char path[RPC_MAXPATHLEN];
    int status;
    
    status = getPath(m_in, result, NULL);
    if (status <= 0) return status;
    
    if (xdr_read_string(m_in, path) < 0) return -1;
    if (result[strlen(result)-1] != '/') strncat(result, "/", RPC_MAXPATHLEN);
    strncat(result, path, RPC_MAXPATHLEN);
    return 1;
}

static int checkFile(struct xdr_t* m_out, const char* path) {
    if (strlen(path) == 0) {
        xdr_write_long(m_out, NFSERR_STALE);
        return 0;
    }
    
#ifndef _WIN32
    /* links always pass (will be resolved on the client side via readlink) */
    struct stat fstat;
    if (vfs_stat(path, &fstat) == 0 && (fstat.st_mode & S_IFMT) == S_IFLNK)
        return 1;
#endif
    
    if (vfs_access(path, F_OK)) {
        xdr_write_long(m_out, NFSERR_NOENT);
        return 0;
    }
    
    return 1;
}

static int write_fattr(struct xdr_t* m_out, const char* path) {
    struct stat fstat;
    uint32_t type = NFNON;

    if (vfs_stat(path, &fstat) != 0) {
        return 0;
    }
    
    if     (S_ISREG (fstat.st_mode)) type = NFREG;
    else if(S_ISDIR (fstat.st_mode)) type = NFDIR;
    else if(S_ISBLK (fstat.st_mode)) type = NFBLK;
    else if(S_ISCHR (fstat.st_mode)) type = NFCHR;
#ifndef _WIN32
    else if(S_ISLNK (fstat.st_mode)) type = NFLNK;
    else if(S_ISSOCK(fstat.st_mode)) type = NFSOCK;
    else if(S_ISFIFO(fstat.st_mode)) {
        type = NFCHR;
        fstat.st_rdev = NFS_FIFO_DEV;
        fstat.st_mode = (fstat.st_mode & ~S_IFMT) | S_IFCHR;
    }
    
    xdr_write_long(m_out, type);
    xdr_write_long(m_out, fstat.st_mode & 0xFFFF);
    xdr_write_long(m_out, fstat.st_nlink);
    xdr_write_long(m_out, fstat.st_uid);
    xdr_write_long(m_out, fstat.st_gid);
    xdr_write_long(m_out, (uint32_t)(fstat.st_size));
    xdr_write_long(m_out, fstat.st_blksize);
    xdr_write_long(m_out, fstat.st_rdev);
    xdr_write_long(m_out, (uint32_t)(fstat.st_blocks));
    xdr_write_long(m_out, fstat.st_dev); /* fsid */
    xdr_write_long(m_out, vfs_file_id(fstat.st_ino));
    xdr_write_long(m_out, (uint32_t)(fstat.st_atimespec.tv_sec));
    xdr_write_long(m_out, (uint32_t)(fstat.st_atimespec.tv_nsec / 1000));
    xdr_write_long(m_out, (uint32_t)(fstat.st_mtimespec.tv_sec));
    xdr_write_long(m_out, (uint32_t)(fstat.st_mtimespec.tv_nsec / 1000));
    xdr_write_long(m_out, (uint32_t)(fstat.st_mtimespec.tv_sec)); /* ctime ignored, we use mtime instead */
    xdr_write_long(m_out, (uint32_t)(fstat.st_mtimespec.tv_nsec / 1000));
#else
    if (type == NFDIR && fstat.st_size == 0) {
        fstat.st_size = BLOCK_SIZE;
    }
    xdr_write_long(m_out, type);
    xdr_write_long(m_out, (uint32_t)(fstat.st_mode & 0xFFFF));
    xdr_write_long(m_out, (uint32_t)(fstat.st_nlink));
    xdr_write_long(m_out, (uint32_t)(fstat.st_uid));
    xdr_write_long(m_out, (uint32_t)(fstat.st_gid));
    xdr_write_long(m_out, (uint32_t)(fstat.st_size));
    xdr_write_long(m_out, (uint32_t)(BLOCK_SIZE));
    xdr_write_long(m_out, (uint32_t)(fstat.st_rdev));
    xdr_write_long(m_out, (uint32_t)((fstat.st_size + BLOCK_SIZE - 1) / BLOCK_SIZE));
    xdr_write_long(m_out, (uint32_t)(fstat.st_dev)); /* fsid */
    xdr_write_long(m_out, vfs_file_id(vfs_get_filehandle(path)));
    xdr_write_long(m_out, (uint32_t)(fstat.st_atime));
    xdr_write_long(m_out, (uint32_t)(0));
    xdr_write_long(m_out, (uint32_t)(fstat.st_mtime));
    xdr_write_long(m_out, (uint32_t)(0));
    xdr_write_long(m_out, (uint32_t)(fstat.st_mtime)); /* ctime ignored, we use mtime instead */
    xdr_write_long(m_out, (uint32_t)(0));
#endif
    return 1;
}

#define FATTR_INVALID ~0

static int read_sattr(struct xdr_t* m_in, struct sattr_t* sattr) {
    if (m_in->size < 8 * 4) {
        return -1;
    }
    sattr->mode       = xdr_read_long(m_in);
    sattr->uid        = xdr_read_long(m_in);
    sattr->gid        = xdr_read_long(m_in);
    sattr->size       = xdr_read_long(m_in);
    sattr->atime.sec  = xdr_read_long(m_in);
    sattr->atime.usec = xdr_read_long(m_in);
    sattr->mtime.sec  = xdr_read_long(m_in);
    sattr->mtime.usec = xdr_read_long(m_in);
    sattr->rdev       = FATTR_INVALID;
    return 0;
}

static struct stat from_sattr(struct sattr_t* sattr) {
    struct stat fstat;
    
    fstat.st_mode              = sattr->mode;
    fstat.st_uid               = sattr->uid;
    fstat.st_gid               = sattr->gid;
    fstat.st_size              = sattr->size;
#ifdef _WIN32
    fstat.st_atime             = sattr->atime.sec;
    fstat.st_mtime             = sattr->mtime.sec;
#else
    fstat.st_atimespec.tv_sec  = sattr->atime.sec;
    fstat.st_atimespec.tv_nsec = sattr->atime.usec * 1000;
    fstat.st_mtimespec.tv_sec  = sattr->mtime.sec;
    fstat.st_mtimespec.tv_nsec = sattr->mtime.usec * 1000;
#endif
    fstat.st_rdev              = sattr->rdev;
    
    return fstat;
}

static void write_handle(struct xdr_t* m_out, uint64_t handle) {
    uint64_t data[4] = {handle,0,0,0};
    xdr_write_data(m_out, (void*)data, FHSIZE);
}


static uint32_t nfs_blocks(const struct statvfs* fsstat, uint32_t fsblocks) {
    uint64_t result = fsblocks;
    /* take minimum as block size, looks like every filesystem uses these fields somewhat different */
    result *= (uint64_t)min(fsstat->f_frsize, fsstat->f_bsize);
    result /= BLOCK_SIZE;
    if(result >= 0x7FFFFFFF) result = 0x7FFFFFFF; /* fix size for signed 32bit */
    return (uint32_t)result;
}


static int proc_getattr(struct rpc_t* rpc) {
    char path[RPC_MAXPATHLEN];
    
    struct xdr_t* m_in  = rpc->m_in;
    struct xdr_t* m_out = rpc->m_out;

    if (getPath(m_in, path, NULL) < 0) return RPC_GARBAGE_ARGS;
    
    rpc_log(rpc, "GETATTR %s", path);
    
    if (!(checkFile(m_out, path)))
        return RPC_SUCCESS;
    
    xdr_write_long(m_out, NFS_OK);
    write_fattr(m_out, path);
    return RPC_SUCCESS;
}

static int proc_setattr(struct rpc_t* rpc) {
    char path[RPC_MAXPATHLEN];
    struct sattr_t sattr;
    struct xdr_t* m_in  = rpc->m_in;
    struct xdr_t* m_out = rpc->m_out;
    
    if (getPath(m_in, path, NULL) < 0) return RPC_GARBAGE_ARGS;
    
    if (read_sattr(m_in, &sattr) < 0) return RPC_GARBAGE_ARGS;
    
    rpc_log(rpc, "SETATTR %s", path);
    
    if (!(checkFile(m_out, path)))
        return RPC_SUCCESS;
    
    vfs_set_attrs(path, from_sattr(&sattr));
    
    xdr_write_long(m_out, NFS_OK);
    write_fattr(m_out, path);
    return RPC_SUCCESS;
}

static int proc_root(struct rpc_t* rpc) {
    rpc_log(rpc, "ROOT");
    return RPC_PROC_UNAVAIL;
}

static int proc_lookup(struct rpc_t* rpc) {
    char path[RPC_MAXPATHLEN];
    uint64_t fhandle;
    
    struct xdr_t* m_in  = rpc->m_in;
    struct xdr_t* m_out = rpc->m_out;

    if (getFullPath(m_in, path) < 0) return RPC_GARBAGE_ARGS;
    
    if (!(checkFile(m_out, path)))
        return RPC_SUCCESS;
    
    fhandle = vfs_get_filehandle(path);
    if (fhandle) {
        xdr_write_long(m_out, NFS_OK);
        rpc_log(rpc, "LOOKUP %s=%" PRIu64, path, fhandle);
        write_handle(m_out, fhandle);
        write_fattr(m_out, path);
    } else {
        xdr_write_long(m_out, NFSERR_NOENT);
        rpc_log(rpc, "LOOKUP %s not found", path);
    }
    return RPC_SUCCESS;
}

static int proc_readlink(struct rpc_t* rpc) {
    int err;
    char path[RPC_MAXPATHLEN];
    char result[RPC_MAXPATHLEN];
    
    struct xdr_t* m_in  = rpc->m_in;
    struct xdr_t* m_out = rpc->m_out;
    
    if (getPath(m_in, path, NULL) < 0) return RPC_GARBAGE_ARGS;
    
    rpc_log(rpc, "READLINK %s", path);
    
    if (!(checkFile(m_out, path)))
        return RPC_SUCCESS;
    
    err = vfs_readlink(path, result);
    if (err) {
        xdr_write_long(m_out, nfs_err(err));
    } else {
        xdr_write_long(m_out, NFS_OK);
        xdr_write_string(m_out, RPC_MAXPATHLEN, result);
    }
    
    return RPC_SUCCESS;
}

static int proc_read(struct rpc_t* rpc) {
    char path[RPC_MAXPATHLEN];
    uint8_t* data;
    int len;
    
    uint32_t offset;
    uint32_t count;
    uint32_t totalcount;
    
    struct xdr_t* m_in  = rpc->m_in;
    struct xdr_t* m_out = rpc->m_out;
    
    if (getPath(m_in, path, NULL) < 0) return RPC_GARBAGE_ARGS;
        
    if (m_in->size < 3 * 4) return RPC_GARBAGE_ARGS;
    offset     = xdr_read_long(m_in);
    count      = xdr_read_long(m_in);
    totalcount = xdr_read_long(m_in); /* unused */
    
    rpc_log(rpc, "READ %s", path);
    
    if (!(checkFile(m_out, path)))
        return RPC_SUCCESS;
    
    data = (uint8_t*)malloc(count);
    
    len = vfs_read(path, offset, data, count);
    if (len >= 0) {
        count = len;
        xdr_write_long(m_out, NFS_OK);
    } else {
        count = 0;
        xdr_write_long(m_out, nfs_err(errno));
    }
    write_fattr(m_out, path);
    xdr_write_long(m_out, count);
    xdr_write_data(m_out, (void*)data, count);
    
    free(data);
    
    return RPC_SUCCESS;
}

static int proc_writecache(struct rpc_t* rpc) {
    rpc_log(rpc, "WRITECACHE");
    return RPC_PROC_UNAVAIL;
}

static int proc_write(struct rpc_t* rpc) {
    char path[RPC_MAXPATHLEN];
    uint8_t* data;
    int len;
    int status;
    
    uint32_t beginoffset;
    uint32_t offset;
    uint32_t totalcount;
    
    struct xdr_t* m_in  = rpc->m_in;
    struct xdr_t* m_out = rpc->m_out;
    
    if (getPath(m_in, path, NULL) < 0) return RPC_GARBAGE_ARGS;
        
    if (m_in->size < 4 * 4) return RPC_GARBAGE_ARGS;
    beginoffset = xdr_read_long(m_in);
    offset      = xdr_read_long(m_in);
    totalcount  = xdr_read_long(m_in);
    
    len = xdr_read_long(m_in);
    if (m_in->size < len) return RPC_GARBAGE_ARGS;
    
    rpc_log(rpc, "WRITE %s", path);
    
    if (!(checkFile(m_out, path)))
        return RPC_SUCCESS;
    
    data = (uint8_t*)malloc(len);
    xdr_read_data(m_in, data, len);
    
    status = vfs_write(path, offset, data, len);
    if (status > 0) {
        xdr_write_long(m_out, NFS_OK);
    } else if (status == 0) {
        xdr_write_long(m_out, NFSERR_ISDIR);
    } else {
        xdr_write_long(m_out, nfs_err(errno));
    }
    
    write_fattr(m_out, path);
    
    free(data);
    
    return RPC_SUCCESS;
}

static int proc_create(struct rpc_t* rpc) {
    char path[RPC_MAXPATHLEN];
    struct sattr_t sattr;
    int status;
    
    struct xdr_t* m_in  = rpc->m_in;
    struct xdr_t* m_out = rpc->m_out;
    
    status = getFullPath(m_in, path);
    if (status < 0) return RPC_GARBAGE_ARGS;
    if (read_sattr(m_in, &sattr) < 0) return RPC_GARBAGE_ARGS;
    
    rpc_log(rpc, "CREATE %s", path);
    
    if (status == 0) return RPC_SUCCESS;
        
    if (!(valid16(sattr.uid))) sattr.uid = vfs_get_uid(path);
    if (!(valid16(sattr.gid))) sattr.gid = vfs_get_gid(path);
    
    /* size field is used to set device numbers for special devices over NFS */
    if (S_ISCHR(sattr.mode)) {
        if (sattr.size == NFS_FIFO_DEV) {
            sattr.mode = (sattr.mode & ~S_IFMT) | S_IFIFO;
        } else {
            sattr.rdev = sattr.size;
        }
        sattr.size = 0;
    } else if (S_ISBLK(sattr.mode)) {
        sattr.rdev = sattr.size;
        sattr.size = 0;
    }
    
    if (vfs_access(path, F_OK) == 0) {
        if(!(valid32(sattr.size)) || sattr.size) {
            vfs_set_attrs(path, from_sattr(&sattr));
            xdr_write_long(m_out, NFS_OK);
            write_handle(m_out, vfs_get_filehandle(path));
            write_fattr(m_out, path);
            
            return RPC_SUCCESS;
        }
    }
    /* file does not exist or must be truncated (fstat.size == 0) */
    
    status = vfs_create(path);
    if (status > 0) {
        vfs_set_attrs(path, from_sattr(&sattr));
        xdr_write_long(m_out, NFS_OK);
        write_handle(m_out, vfs_get_filehandle(path));
        write_fattr(m_out, path);
    } else {
        nfs_err(errno);
    }
    return RPC_SUCCESS;
}

static int proc_remove(struct rpc_t* rpc) {
    char path[RPC_MAXPATHLEN];
    int err;
    
    struct xdr_t* m_in  = rpc->m_in;
    struct xdr_t* m_out = rpc->m_out;
    
    if (getFullPath(m_in, path) < 0) return RPC_GARBAGE_ARGS;
    
    rpc_log(rpc, "REMOVE %s", path);
    
    if (!(checkFile(m_out, path)))
        return RPC_SUCCESS;
    
    err = nfs_err(vfs_remove(path));
    xdr_write_long(m_out, err);
    
    return RPC_SUCCESS;
}

static int proc_rename(struct rpc_t* rpc) {
    char pathFrom[RPC_MAXPATHLEN];
    char pathTo[RPC_MAXPATHLEN];
    int err;
    
    struct xdr_t* m_in  = rpc->m_in;
    struct xdr_t* m_out = rpc->m_out;
    
    if (getFullPath(m_in, pathFrom) < 0) return RPC_GARBAGE_ARGS;
    if (getFullPath(m_in, pathTo) < 0) return RPC_GARBAGE_ARGS;
    
    rpc_log(rpc, "RENAME %s->%s", pathFrom, pathTo);
    
    if (!(checkFile(m_out, pathFrom)))
        return RPC_SUCCESS;
        
    err = nfs_err(vfs_rename(pathFrom, pathTo));
    xdr_write_long(m_out, err);
    
    return RPC_SUCCESS;
}

static int proc_link(struct rpc_t* rpc) {
    char pathFrom[RPC_MAXPATHLEN];
    char pathTo[RPC_MAXPATHLEN];
    int err;
    
    struct xdr_t* m_in  = rpc->m_in;
    struct xdr_t* m_out = rpc->m_out;
    
    if (getPath(m_in, pathFrom, NULL) < 0) return RPC_GARBAGE_ARGS;
    if (getFullPath(m_in, pathTo) < 0) return RPC_GARBAGE_ARGS;
    
    rpc_log(rpc, "LINK %s->%s", pathFrom, pathTo);
    
    xdr_write_long(m_out, nfs_err(vfs_link(pathFrom, pathTo)));
    
    return RPC_SUCCESS;
}

static int proc_symlink(struct rpc_t* rpc) {
    char pathFrom[RPC_MAXPATHLEN];
    char pathTo[RPC_MAXPATHLEN];
    int err;
    struct sattr_t sattr;
    
    struct xdr_t* m_in  = rpc->m_in;
    struct xdr_t* m_out = rpc->m_out;
    
    if (getFullPath(m_in, pathTo) < 0) return RPC_GARBAGE_ARGS;
    if (xdr_read_string(m_in, pathFrom) < 0) return RPC_GARBAGE_ARGS;
        
    if (read_sattr(m_in, &sattr) < 0) return RPC_GARBAGE_ARGS;
    
    rpc_log(rpc, "SYMLINK %s->%s", pathFrom, pathTo);
    
    err = vfs_symlink(pathFrom, pathTo);
    if(!(err)) vfs_set_attrs(pathTo, from_sattr(&sattr));
    xdr_write_long(m_out, nfs_err(err));
    
    return RPC_SUCCESS;
}

static int proc_mkdir(struct rpc_t* rpc) {
    char path[RPC_MAXPATHLEN];
    int status;
    int err;
    struct sattr_t sattr;

    struct xdr_t* m_in  = rpc->m_in;
    struct xdr_t* m_out = rpc->m_out;

    status = getFullPath(m_in, path);
    if (status < 0) return RPC_GARBAGE_ARGS;
    if (status == 0) return RPC_SUCCESS;
    
    if (read_sattr(m_in, &sattr) < 0) return RPC_GARBAGE_ARGS;
    
    rpc_log(rpc, "MKDIR");
    
    err = vfs_mkdir(path);
    if (err) {
        xdr_write_long(m_out, nfs_err(err));
    } else {
        vfs_set_attrs(path, from_sattr(&sattr));
        xdr_write_long(m_out, NFS_OK);
        write_handle(m_out, vfs_get_filehandle(path));
        write_fattr(m_out, path);
    }
    
    return RPC_SUCCESS;
}

static int proc_rmdir(struct rpc_t* rpc) {
    char path[RPC_MAXPATHLEN];
    int err;
    
    struct xdr_t* m_in  = rpc->m_in;
    struct xdr_t* m_out = rpc->m_out;
    
    if (getFullPath(m_in, path) < 0) return RPC_GARBAGE_ARGS;
    
    rpc_log(rpc, "RMDIR");
    
    if (!(checkFile(m_out, path)))
        return RPC_SUCCESS;
    
    err = nfs_err(vfs_rmdir(path));
    xdr_write_long(m_out, err);
    
    return RPC_SUCCESS;
}

static int proc_readdir(struct rpc_t* rpc) {
    char path[RPC_MAXPATHLEN];
    char name[RPC_MAXNAMELEN];
    
    struct dirent* fileinfo;
    DIR* handle;
    uint32_t cookie;
    uint32_t count;
    uint32_t eof;
    uint64_t fhandle;
    
    struct xdr_t* m_in  = rpc->m_in;
    struct xdr_t* m_out = rpc->m_out;
    
    if (getPath(m_in, path, &fhandle) < 0) return RPC_GARBAGE_ARGS;
    
    if (m_in->size < 2 * 4) return RPC_GARBAGE_ARGS;
    cookie = xdr_read_long(m_in);
    count  = xdr_read_long(m_in);
    
    rpc_log(rpc, "READDIR %s", path);
    
    if (!(checkFile(m_out, path)))
        return RPC_SUCCESS;
        
    eof     = 1;
    handle  = vfs_opendir(path);
    if (handle) {
        xdr_write_long(m_out, NFS_OK);
        int skip = cookie;
        for (fileinfo = readdir(handle); fileinfo; fileinfo = readdir(handle)) {
#if HAVE_STRUCT_DIRENT_D_NAMELEN
            size_t namelen = fileinfo->d_namlen;
#else
            size_t namelen = strlen(fileinfo->d_name);
#endif
            if(--skip >= 0) continue;
            
#ifndef _WIN32
            xdr_write_long(m_out, 1); /* value follows */
            xdr_write_long(m_out, vfs_file_id(fileinfo->d_ino));
#endif
            strncpy(name, fileinfo->d_name, namelen);
            name[namelen] = '\0';
            rpc_log(rpc, "%d %s %s", cookie, path, name);
#ifdef _WIN32
            char pth[RPC_MAXPATHLEN];
            strncpy(pth, path, RPC_MAXPATHLEN);
            if (pth[strlen(pth)-1] != '/') strncat(pth, "/", RPC_MAXPATHLEN);
            strncat(pth, name, RPC_MAXPATHLEN);
            const uint64_t fileno = vfs_get_filehandle(pth);
            xdr_write_long(m_out, 1); /* value follows */
            xdr_write_long(m_out, vfs_file_id(fileno));
#endif
            xdr_write_string(m_out, RPC_MAXNAMELEN, name);
            xdr_write_long(m_out, cookie+1);
            cookie++;
            if (m_out->size >= count - 128) { /* 128: give some space for XDR data */
                eof = 0;
                break;
            }
        }
        closedir(handle);
        xdr_write_long(m_out, 0);  /* no value follows */
        xdr_write_long(m_out, eof);
    } else {
        xdr_write_long(m_out, nfs_err(errno));
    }
    
    return RPC_SUCCESS;
}

static int proc_statfs(struct rpc_t* rpc) {
    int err;
    char path[RPC_MAXPATHLEN];
    struct statvfs fsstat;
    
    struct xdr_t* m_in  = rpc->m_in;
    struct xdr_t* m_out = rpc->m_out;

    if (getPath(m_in, path, NULL) < 0) return RPC_GARBAGE_ARGS;
    
    rpc_log(rpc, "STATFS");
    
    if(!(checkFile(m_out, path)))
        return RPC_SUCCESS;
    
    err = vfs_statfs(path, &fsstat);
    if (err) {
        xdr_write_long(m_out, nfs_err(err));
    } else {
        xdr_write_long(m_out, NFS_OK);
        xdr_write_long(m_out, BLOCK_SIZE*2); /* transfer size */
        xdr_write_long(m_out, BLOCK_SIZE);   /* block size */
        xdr_write_long(m_out, nfs_blocks(&fsstat, fsstat.f_blocks)); /* total blocks */
        xdr_write_long(m_out, nfs_blocks(&fsstat, fsstat.f_bfree));  /* free blocks */
        xdr_write_long(m_out, nfs_blocks(&fsstat, fsstat.f_bavail)); /* available blocks */
    }
    
    return RPC_SUCCESS;
}


int nfs_prog(struct rpc_t* rpc) {
    switch (rpc->proc) {
        case NFSPROC_NULL:
            return proc_null(rpc);
            
        case NFSPROC_GETATTR:
            return proc_getattr(rpc);
            
        case NFSPROC_SETATTR:
            return proc_setattr(rpc);
            
        case NFSPROC_ROOT:
            return proc_root(rpc);
            
        case NFSPROC_LOOKUP:
            return proc_lookup(rpc);
            
        case NFSPROC_READLINK:
            return proc_readlink(rpc);
            
        case NFSPROC_READ:
            return proc_read(rpc);
            
        case NFSPROC_WRITECACHE:
            return proc_writecache(rpc);
            
        case NFSPROC_WRITE:
            return proc_write(rpc);
            
        case NFSPROC_CREATE:
            return proc_create(rpc);
            
        case NFSPROC_REMOVE:
            return proc_remove(rpc);
            
        case NFSPROC_RENAME:
            return proc_rename(rpc);
            
        case NFSPROC_LINK:
            return proc_link(rpc);
            
        case NFSPROC_SYMLINK:
            return proc_symlink(rpc);
            
        case NFSPROC_MKDIR:
            return proc_mkdir(rpc);
            
        case NFSPROC_RMDIR:
            return proc_rmdir(rpc);
            
        case NFSPROC_READDIR:
            return proc_readdir(rpc);
            
        case NFSPROC_STATFS:
            return proc_statfs(rpc);

        default:
            break;
    }
    rpc_log(rpc, "Process unavailable");
    return RPC_PROC_UNAVAIL;
}
