//
//  FileTableNFSD.cpp
//  Previous
//
//  Created by Simon Schubiger on 04.03.2019
//  Wrapper to C language added by Andreas Grabher
//  Renamed to vfs.cpp
//

#include <stdio.h>
#include <vector>
#include <sys/time.h>

#include "vfs.h"

using namespace std;


FileTableNFSD* nfsd_fts[1] = { NULL }; // to be extended for multiple exports

FileTableNFSD::FileTableNFSD(const HostPath& basePath, const VFSPath& basePathAlias)
: VirtualFS(basePath, basePathAlias)
, mutex(host_mutex_create()) {}

FileTableNFSD::~FileTableNFSD(void) {
    host_mutex_destroy(mutex);
}

bool FileTableNFSD::getCanonicalPath(uint64_t fhandle, std::string& result) {
    NFSDLock lock(mutex);
    map<uint64_t, string>::iterator iter(handle2path.find(fhandle));
    if(iter != handle2path.end()) {
        result = iter->second;
        return true;
    }
    return false;
}

int FileTableNFSD::stat(const VFSPath& absoluteVFSpath, struct stat& fstat) {
    NFSDLock lock(mutex);
    return VirtualFS::stat(absoluteVFSpath, fstat);
}

void FileTableNFSD::move(uint64_t fileHandleFrom, const VFSPath& absoluteVFSpathTo) {
    NFSDLock lock(mutex);
    handle2path.erase(fileHandleFrom);
    handle2path[VirtualFS::getFileHandle(absoluteVFSpathTo)] = absoluteVFSpathTo.canonicalize().string();
}

void FileTableNFSD::remove(uint64_t fileHandle) {
    NFSDLock lock(mutex);
    handle2path.erase(fileHandle);
}

uint64_t FileTableNFSD::getFileHandle(const VFSPath& absoluteVFSpath) {
    NFSDLock lock(mutex);
    uint64_t result(VirtualFS::getFileHandle(absoluteVFSpath));
    handle2path[result] = absoluteVFSpath.canonicalize().string();
    return result;
}

void FileTableNFSD::setFileAttrs(const VFSPath& absoluteVFSpath, const FileAttrs& fstat) {
    NFSDLock lock(mutex);
    VirtualFS::setFileAttrs(absoluteVFSpath, fstat);
}

FileAttrs FileTableNFSD::getFileAttrs(const VFSPath& absoluteVFSpath) {
    NFSDLock lock(mutex);
    return VirtualFS::getFileAttrs(absoluteVFSpath);
}


extern "C" {

void vfs_set_default_uid_gid(uint32_t uid, uint32_t gid) {
    nfsd_fts[0]->setDefaultUID_GID(uid, gid);
}

int vfs_get_canonical_patch(uint64_t handle, const char** path) {
    static string result;
    bool success = nfsd_fts[0]->getCanonicalPath(handle, result);
    
    *path = result.c_str();
    
    return success ? 1 : 0;
}

uint32_t vfs_file_id(uint64_t ino) {
    return nfsd_fts[0]->fileId(ino);
}

uint32_t vfs_get_uid(char* path) {
    return nfsd_fts[0]->vfsGetUID(path, false);
}

uint32_t vfs_get_gid(char* path) {
    return nfsd_fts[0]->vfsGetGID(path, true);
}

int vfs_stat(const char* path, struct stat* fstat) {    
    return nfsd_fts[0]->stat(path, *fstat);
}

int vfs_access(const char* path, int mode) {
    return nfsd_fts[0]->vfsAccess(path, mode);
}

void vfs_set_attrs(const char* path, const struct stat cstat) {
    FileAttrs fstat(cstat);
    FileAttrs newAttrs = nfsd_fts[0]->getFileAttrs(path);
    
    if (FileAttrs::valid16(fstat.mode)) {
        newAttrs.mode &= S_IFMT;
        newAttrs.mode |= fstat.mode & (S_IRWXU | S_IRWXG | S_IRWXO);
        nfsd_fts[0]->vfsChmod(path, newAttrs.mode);
        if (fstat.mode & S_IFMT)
            newAttrs.mode &= ~S_IFMT;
        newAttrs.mode |= fstat.mode;
    }
    if (FileAttrs::valid16(fstat.uid))
        newAttrs.uid = fstat.uid;
    if (FileAttrs::valid16(fstat.gid))
        newAttrs.gid = fstat.gid;
    if (FileAttrs::valid16(fstat.rdev))
        newAttrs.rdev = fstat.rdev;
    
    timeval times[2];
    timeval now;
    gettimeofday(&now, NULL);
    times[0].tv_sec  = FileAttrs::valid32(fstat.atime_sec)  ? fstat.atime_sec  : now.tv_sec;
    times[0].tv_usec = FileAttrs::valid32(fstat.atime_usec) ? fstat.atime_usec : now.tv_usec;
    times[1].tv_sec  = FileAttrs::valid32(fstat.mtime_sec)  ? fstat.mtime_sec  : now.tv_sec;
    times[1].tv_usec = FileAttrs::valid32(fstat.mtime_usec) ? fstat.mtime_usec : now.tv_usec;
    if (FileAttrs::valid32(fstat.atime_sec) || FileAttrs::valid32(fstat.mtime_sec))
        nfsd_fts[0]->vfsUtimes(path, times);
    nfsd_fts[0]->setFileAttrs(path, newAttrs);
}

int vfs_readlink(char* path, char* result) {
    VFSPath vresult;
    
    int err = nfsd_fts[0]->vfsReadlink(path, vresult);
    
    if (!err) {
        strcpy(result, vresult.c_str());
    }
    
    return err;
}

int vfs_read(char* path, size_t offset, uint8_t* data, size_t len) {
    VFSFile file(*nfsd_fts[0], path, "rb");
    if (file.isOpen()) {
        return file.read(offset, data, len);
    } else {
        return -1;
    }
}

int vfs_write(char* path, size_t offset, uint8_t* data, size_t len) {
    FileAttrs attrs = nfsd_fts[0]->getFileAttrs(path);
    if ((attrs.mode & S_IFMT) == S_IFREG) {
        VFSFile file(*nfsd_fts[0], path, "r+b");
        if (file.isOpen()) {
            file.write(offset, data, len);
            return 1;
        } else {
            return -1;
        }
    } else {
        return 0;
    }
}

int vfs_create(char* path) {
    VFSFile file(*nfsd_fts[0], path, "wb");
    if (file.isOpen()) {
        return 1;
    } else {
        return -1;
    }
}

int vfs_remove(char* path) {
    uint64_t fileHandle(nfsd_fts[0]->getFileHandle(path));
    int err = nfsd_fts[0]->vfsRemove(path);
    if (!(err)) nfsd_fts[0]->remove(fileHandle);
    return err;
}

int vfs_rename(char* pathFrom, char* pathTo) {
    uint64_t fileHandleFrom(nfsd_fts[0]->getFileHandle(pathFrom));
    int err = nfsd_fts[0]->vfsRename(pathFrom, pathTo);
    if(!(err)) nfsd_fts[0]->move(fileHandleFrom, pathTo);
    return err;
}

int vfs_link(char* pathFrom, char* pathTo) {
    return nfsd_fts[0]->vfsLink(pathFrom, pathTo, false);
}

int vfs_symlink(char* pathFrom, char* pathTo) {
    return nfsd_fts[0]->vfsLink(pathFrom, pathTo, true);
}

int vfs_mkdir(char* path) {
    return nfsd_fts[0]->vfsMkdir(path, DEFAULT_PERM);
}

int vfs_rmdir(char* path) {
    uint64_t fileHandle(nfsd_fts[0]->getFileHandle(path));
    int err = nfsd_fts[0]->vfsNftw(path, VirtualFS::remove, 3, FTW_DEPTH | FTW_PHYS);
    if(!(err)) nfsd_fts[0]->remove(fileHandle);
    return err;
}

DIR* vfs_opendir(char* path) {
    return nfsd_fts[0]->vfsOpendir(path);
}

int vfs_statfs(char* path, struct statvfs* fsstat) {
    struct statvfs result;
    
    int err = nfsd_fts[0]->vfsStatvfs(path, result);
    
    *fsstat = result;
    
    return err;
}

void vfs_get_basepath_alias(char* path, int len) {
    const char* basepath = nfsd_fts[0]->getBasePathAlias().c_str();
    strlcpy(path, basepath, len);
}

uint64_t vfs_get_filehandle(const char* path) {
    return nfsd_fts[0]->getFileHandle(path);
}

int vfs_read_safe(char* path, size_t offset, uint8_t* data, size_t len) {
    if (nfsd_fts[0]) {
        return vfs_read(path, offset, data, len);
    }
    return -1;
}

int vfs_is_inited(void) {
    if (nfsd_fts[0]) {
        return 1;
    }
    return 0;
}

int vfs_path_changed(char* path) {
    if (nfsd_fts[0]->getBasePath() != HostPath(path)) {
        return 1;
    }
    return 0;
}

void vfs_init(char* path) {
    nfsd_fts[0] = new FileTableNFSD(path, "/");
}

void vfs_uninit(void) {
    delete nfsd_fts[0];
    nfsd_fts[0] = NULL;
}

} // extern "C"
