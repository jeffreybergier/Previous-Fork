//
//  FileTableNFSD.hpp
//  Previous
//
//  Created by Simon Schubiger on 04.03.2019
//  Wrapper to C language added by Andreas Grabher
//  Renamed to vfs.h
//

#ifndef _VFS_H_
#define _VFS_H_

#include <stdint.h>
#include <unistd.h>
#include <dirent.h>
#include <ftw.h>

#ifdef _WIN32
#include <Winsock2.h>
#include <ws2tcpip.h>
#include <stdint.h>
typedef uint32_t fsblkcnt_t;
typedef uint32_t fsfilcnt_t;
struct statvfs
{
    unsigned long int f_bsize;
    unsigned long int f_frsize;
    fsblkcnt_t f_blocks;
    fsblkcnt_t f_bfree;
    fsblkcnt_t f_bavail;
    fsfilcnt_t f_files;
    fsfilcnt_t f_ffree;
    fsfilcnt_t f_favail;
    unsigned long int f_fsid;
    unsigned long int f_flag;
    unsigned long int f_namemax;
};
#else
#include <sys/statvfs.h>
#endif

#define DEFAULT_PERM 0755
#define FATTR_INVALID ~0

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

int valid16(uint32_t statval);
int valid32(uint32_t statval);

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

struct vfs_t {
    char* vfs_base_path;
    char* host_base_path;
    
    uint32_t uid;
    uint32_t gid;
};

struct vfs_t* vfs;


void vfs_path_canonicalize(const char* vfs_path, char* result);

uint32_t vfs_file_id(uint64_t ino);
uint32_t vfs_get_uid(struct vfs_t* vfs, const char* vfs_path, int use_parent);
uint32_t vfs_get_gid(struct vfs_t* vfs, const char* vfs_path, int use_parent);

int vfs_get_fstat(struct vfs_t* vfs, const char* vfs_path, struct stat* fstat);
int vfs_chmod(struct vfs_t* vfs, char* vfs_path, mode_t mode);
int vfs_utimes(struct vfs_t* vfs, char* vfs_path, struct timeval times[2]);
int vfs_stat(struct vfs_t* vfs, const char* vfs_path, struct stat* fstat);

void vfs_set_sattr(struct vfs_t* vfs, const char* vfs_path, struct sattr_t* sattr);
void vfs_get_sattr(struct vfs_t* vfs, const char* vfs_path, struct sattr_t* sattr);
uint64_t vfs_get_fhandle(struct vfs_t* vfs, const char* vfs_path);
int vfs_readlink(struct vfs_t* vfs, const char* vfs_path, char* result);
int vfs_read(struct vfs_t* vfs, const char* path, size_t offset, uint8_t* data, size_t len);
int vfs_write(struct vfs_t* vfs, const char* path, size_t offset, uint8_t* data, size_t len);
int vfs_touch(struct vfs_t* vfs, const char* path);
int vfs_remove(struct vfs_t* vfs, const char* vfs_path);
int vfs_rename(struct vfs_t* vfs, const char* vfs_path_from, const char* vfs_path_to);
int vfs_link(struct vfs_t* vfs, const char* vfs_path_from, const char* vfs_path_to, int soft);
int vfs_mkdir(struct vfs_t* vfs, const char* vfs_path, mode_t mode);
int vfs_rmdir(const char* fpath, const struct stat* fstat, int typeflag, struct FTW* ftwbuf);
int vfs_nftw(struct vfs_t* vfs, const char* vfs_path, int (*fn)(const char *, const struct stat *ptr, int flag, struct FTW *), int depth, int flags);
DIR* vfs_opendir(struct vfs_t* vfs, const char* vfs_path);
int vfs_statfs(struct vfs_t* vfs, const char* vfs_path, struct statvfs* fsstat);

int vfs_access(struct vfs_t* vfs, const char* vfs_path, int mode);

void vfs_set_default_uid_gid(struct vfs_t* vfs, uint32_t uid, uint32_t gid);
void vfs_get_basepath_alias(struct vfs_t* vfs, char* path, int maxlen);

struct vfs_t* vfs_init(const char* host_path, const char* vfs_path_alias);
struct vfs_t* vfs_uninit(struct vfs_t* vfs);

#endif /* _VFS_H_ */
