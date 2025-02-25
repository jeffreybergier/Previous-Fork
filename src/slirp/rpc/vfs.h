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
#include "ctl.h"

#ifdef _WIN32
#include <Winsock2.h>
#include <ws2tcpip.h>
#endif

#ifndef _WIN32
#include <sys/statvfs.h>
#endif

#ifdef __cplusplus

#include "../../ditool/VirtualFS.h"
#include "host.h"

class NFSDLock {
    mutex_t* mutex;
public:
    NFSDLock(mutex_t* mutex) : mutex(mutex) {host_mutex_lock(mutex);}
    ~NFSDLock()                             {host_mutex_unlock(mutex);}
};

class FileTableNFSD : public VirtualFS {
    mutex_t*                        mutex;
    std::map<uint64_t, std::string> handle2path;
public:
    FileTableNFSD(const HostPath& basePath, const VFSPath& basePathAlias);
    virtual ~FileTableNFSD(void);
    
    virtual int         stat            (const VFSPath& absoluteVFSpath, struct stat& stat);
    virtual void        move            (uint64_t fileHandleFrom, const VFSPath& absoluteVFSpathTo);
    virtual void        remove          (uint64_t fileHandle);
    virtual uint64_t    getFileHandle   (const VFSPath& absoluteVFSpath);
    virtual void        setFileAttrs    (const VFSPath& absoluteVFSpath, const FileAttrs& fstat);
    virtual FileAttrs   getFileAttrs    (const VFSPath& absoluteVFSpath);
    
    bool                getCanonicalPath(uint64_t handle, std::string& result);
};


extern "C" {
#endif

void vfs_set_default_uid_gid(uint32_t uid, uint32_t gid);
int vfs_get_canonical_patch(uint64_t handle, const char** path);

uint32_t vfs_file_id(uint64_t ino);
uint32_t vfs_get_uid(char* path);
uint32_t vfs_get_gid(char* path);

int vfs_stat(const char* path, struct stat* fstat);
int vfs_access(const char* path, int mode);
void vfs_set_attrs(const char* path, const struct stat stat);

int vfs_readlink(char* path, char* result);
int vfs_read(char* path, size_t offset, uint8_t* data, size_t len);
int vfs_write(char* path, size_t offset, uint8_t* data, size_t len);
int vfs_create(char* path);
int vfs_remove(char* path);
int vfs_rename(char* pathFrom, char* pathTo);
int vfs_link(char* pathFrom, char* pathTo);
int vfs_symlink(char* pathFrom, char* pathTo);
int vfs_mkdir(char* path);
int vfs_rmdir(char* path);
DIR* vfs_opendir(char* path);
int vfs_statfs(char* path, struct statvfs* fsstat);

void vfs_get_basepath_alias(char* path, int len);
uint64_t vfs_get_filehandle(const char* path);

int vfs_read_safe(char* path, size_t offset, uint8_t* data, size_t len);

int vfs_is_inited(void);
int vfs_path_changed(char* path);
void vfs_init(char* path);
void vfs_uninit(void);

#ifdef __cplusplus
}
#endif

#endif /* _VFS_H_ */
