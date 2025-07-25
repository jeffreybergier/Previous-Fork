#include <stdlib.h>
#include <string.h>
#include <inttypes.h>
#include <ctype.h>
#include <fcntl.h>
#include <ftw.h>

#include "config.h"
#include "netboot.h"
#include "ufs.h"
#include "vfs.h"

#ifdef _WIN32
#include <shellapi.h>
#else

#if !HAVE_STRUCT_STAT_ST_ATIMESPEC
#define st_atimespec st_atim
#endif

#if !HAVE_STRUCT_STAT_ST_MTIMESPEC
#define st_mtimespec st_mtim
#endif

#endif

/* Helper structs */
struct i2i_t {
    uint32_t inode32;
    uint64_t inode64;
    struct i2i_t* next;
};

static int i2i_add(struct i2i_t** i2i, uint32_t inode32, uint64_t inode64) {
    while (*i2i) {
        if ((*i2i)->inode32 == inode32) {
            if ((*i2i)->inode64 != inode64) {
                printf("i2i error: value exists with different pairing\n");                
            }
            return 1;
        }
        i2i = &(*i2i)->next;
    }
    *i2i = (struct i2i_t*)malloc(sizeof(struct i2i_t));
    (*i2i)->inode32 = inode32;
    (*i2i)->inode64 = inode64;
    (*i2i)->next = NULL;
    return 0;
}

static void i2i_delete(struct i2i_t* i2i) {
    struct i2i_t* next;
    while (i2i) {
        next = i2i->next;
        free(i2i);
        i2i = next;
    }
}

static uint64_t i2i_find(struct i2i_t* i2i, uint32_t inode32) {
    while (i2i) {
        if (i2i->inode32 == inode32) {
            return i2i->inode64;
        }
        i2i = i2i->next;
    }
    return 0;
}


struct i2p_t {
    uint32_t inode32;
    char* path;
    struct i2p_t* next;
};

static int i2p_add(struct i2p_t** i2p, uint32_t inode32, const char* path) {
    if (path == NULL) {
        return -1;
    }
    while (*i2p) {
        if ((*i2p)->inode32 == inode32) {
            if (strcmp((*i2p)->path, path)) {
                printf("i2p error: value exists with different pairing\n");
            }
            return 1;
        }
        i2p = &(*i2p)->next;
    }
    *i2p = (struct i2p_t*)malloc(sizeof(struct i2p_t));
    (*i2p)->inode32 = inode32;
    (*i2p)->path = strdup(path);
    (*i2p)->next = NULL;
    return 0;
}


static void i2p_delete(struct i2p_t* i2p) {
    struct i2p_t* next;
    while (i2p) {
        next = i2p->next;
        free(i2p->path);
        free(i2p);
        i2p = next;
    }
}

static char* i2p_find(struct i2p_t* i2p, uint32_t inode32) {
    while (i2p) {
        if (i2p->inode32 == inode32) {
            return i2p->path;
        }
        i2p = i2p->next;
    }
    return NULL;
}

struct skip_t {
    char* path;
    struct skip_t* next;
};

static int skip_add(struct skip_t** skip, const char* path) {
    if (path == NULL) {
        return -1;
    }
    while (*skip) {
        if (strcmp((*skip)->path, path)) {
            printf("skip error: value exists\n");                
            return -1;
        }
        skip = &(*skip)->next;
    }
    *skip = (struct skip_t*)malloc(sizeof(struct skip_t));
    (*skip)->path = strdup(path);
    (*skip)->next = NULL;
    return 0;
}

static void skip_delete(struct skip_t* skip) {
    struct skip_t* next;
    while (skip) {
        next = skip->next;
        free(skip->path);
        free(skip);
        skip = next;
    }
}

static int skip_find(struct skip_t* skip, const char* path) {
    while (skip) {
        if (strcmp(skip->path, path) == 0) {
            return 1;
        }
        skip = skip->next;
    }
    return 0;
}



static const char* get_option(const char** args, int num_args, const char* opt) {
    int i;
    for (i = 0; i < num_args; i++) {
        if (strcmp(args[i], opt) == 0) {
            if (++i < num_args) {
                return args[i];
            }
            break;
        }
    }
    return NULL;
}

static bool has_option(const char** args, int num_args, const char* opt) {
    int i;
    for (i = 0; i < num_args; i++) {
        if (strcmp(args[i], opt) == 0) {
            return true;
        }
    }
    return false;
}

static void print_help(void) {
    printf("usage : ditool -im <disk_image_file> [options]\n");
    printf("Options:\n");
    printf("  -h          Print this help.\n");
    printf("  -im <file>  Raw disk image file to read from.\n");
    printf("  -lsp        List partitions in disk image.\n");
    printf("  -p          Partition number to work on.\n");
    printf("  -ls         List files in disk image.\n");
    printf("  -lst <type> List files in disk image of type. type=FILE|DIR|SLINK|HLINK|FIFO|CHAR|BLOCK|SOCK\n");
    printf("  -out <path> Copy files from disk image to <path>.\n");
    printf("  -clean      Clean output directory before copying.\n");
    printf("  -netboot    Prepare files in output directory for netboot.\n");
}

static bool ignore_name(const char* name) {
    return strcmp(".", name) == 0 || strcmp("..", name) == 0;
}

static void make_path(const char* path, const char* name, struct path_t* result, struct vfs_t* ft) {
    if (path && strlen(path) > 0) {
        vfscpy(result->vfs, path, sizeof(result->vfs));
    } else {
        vfscpy(result->vfs, "/", sizeof(result->vfs));
    }
    if (name && strlen(name) > 0) {
        if (result->vfs[strlen(result->vfs) - 1] != '/') {
            vfscat(result->vfs, "/", sizeof(result->vfs));
        }
        vfscat(result->vfs, name, sizeof(result->vfs));
    }
    if (strcmp(result->vfs, "/.") == 0) {
        vfscpy(result->vfs, "/", sizeof(result->vfs));
    }
    if (ft) {
        vfs_to_host_path(ft, result);
    }
}

static void copy_attrs(struct stat* fstat, struct icommon* inode, uint32_t rdev) {
    fstat->st_mode              = ntohs(inode->ic_mode);
    fstat->st_uid               = ntohs(inode->ic_uid);
    fstat->st_gid               = ntohs(inode->ic_gid);
    fstat->st_size              = ntohl(inode->ic_size);
#ifdef _WIN32
    fstat->st_atime             = ntohl(inode->ic_atime.tv_sec);
    fstat->st_mtime             = ntohl(inode->ic_mtime.tv_sec);
#else
    fstat->st_atimespec.tv_sec  = ntohl(inode->ic_atime.tv_sec);
    fstat->st_atimespec.tv_nsec = ntohl(inode->ic_atime.tv_usec) * 1000;
    fstat->st_mtimespec.tv_sec  = ntohl(inode->ic_mtime.tv_sec);
    fstat->st_mtimespec.tv_nsec = ntohl(inode->ic_mtime.tv_usec) * 1000;
#endif
    fstat->st_rdev              = rdev;
}

static void set_attrs(struct icommon* inode, uint32_t rdev, struct path_t* dirEntPath, struct vfs_t* ft) {
    struct stat fstat;
    struct sattr_t sattr;
    struct timeval times[2];

    copy_attrs(&fstat, inode, rdev);
    vfs_stat_to_sattr(&fstat, &sattr);
    vfs_set_sattr(ft, dirEntPath, &sattr);
    
    times[0].tv_sec  = fstat.st_atimespec.tv_sec;
    times[0].tv_usec = fstat.st_atimespec.tv_nsec / 1000;
    times[1].tv_sec  = fstat.st_mtimespec.tv_sec;
    times[1].tv_usec = fstat.st_mtimespec.tv_nsec / 1000;
    
    if (vfs_chmod(dirEntPath, fstat.st_mode & ~IFMT))
        printf("Unable to set mode for %s\n", dirEntPath->vfs);
    if (vfs_utimes(dirEntPath, times))
        printf("Unable to set times for %s\n", dirEntPath->vfs);
}

static void set_attrs_recr(struct ufs_t* ufs, struct skip_t** skip, uint32_t ino, const char* path, struct vfs_t* ft) {
    struct dirlist_t* dirlist = ufs_list(ufs, ino);
    struct dirlist_t* entry = dirlist;
    
    while (entry) {
        struct direct* dirEnt = &entry->dir;
        struct path_t dirEntPath;
        struct icommon inode;
        uint32_t rdev = 0;
        
        make_path(path, dirEnt->d_name, &dirEntPath, ft);
        
        entry = entry->next;
        
        if (ignore_name(dirEnt->d_name)) {
            continue;
        }
        if (skip_find(*skip, dirEntPath.vfs)) {
            continue;
        }
        
        if (ufs_readInode(ufs, &inode, ntohl(dirEnt->d_inonum)) == ERR_NO) {
            switch (ntohs(inode.ic_mode) & IFMT) {
                case IFDIR:       /* directory */
                    set_attrs_recr(ufs, skip, ntohl(dirEnt->d_inonum), dirEntPath.vfs, ft);
                    break;
                case IFCHR:       /* character special */
                case IFBLK:       /* block special */
                    rdev = ntohl(inode.ic_db[0]);
                    break;
            }
            set_attrs(&inode, rdev, &dirEntPath, ft);
        }
    }
    
    dirlist_delete(dirlist);
}

static void set_attrs_inode(struct ufs_t* ufs, uint32_t ino, const char* path, struct vfs_t* ft) {
    struct path_t dirEntPath;
    struct icommon inode;
    
    make_path(path, NULL, &dirEntPath, ft);
    
    if (ufs_readInode(ufs, &inode, ino) == ERR_NO) {
        set_attrs(&inode, 0, &dirEntPath, ft);
    }
}

static void verify_attr_recr(struct ufs_t* ufs, struct skip_t** skip, uint32_t ino, const char* path, struct vfs_t* ft) {
    struct dirlist_t* dirlist = ufs_list(ufs, ino);
    struct dirlist_t* entry = dirlist;
    
    while (entry) {
        struct direct* dirEnt = &entry->dir;
        struct path_t dirEntPath;
        struct icommon inode;
        uint32_t rdev = 0;

        make_path(path, dirEnt->d_name, &dirEntPath, ft);
        
        entry = entry->next;
        
        if (skip_find(*skip, dirEntPath.vfs)) {
            continue;
        }
        
        if (ufs_readInode(ufs, &inode, ntohl(dirEnt->d_inonum)) == ERR_NO) {
            struct stat fstat;
            
            switch (ntohs(inode.ic_mode) & IFMT) {
                case IFDIR:       /* directory */
                    if (!(ignore_name(dirEnt->d_name)))
                        verify_attr_recr(ufs, skip, ntohl(dirEnt->d_inonum), dirEntPath.vfs, ft);
                    break;
                case IFCHR:       /* character special */
                case IFBLK:       /* block special */
                    rdev = ntohl(inode.ic_db[0]);
                    break;
            }
            vfs_get_fstat(ft, &dirEntPath, &fstat);
            
            if (fstat.st_mode != ntohs(inode.ic_mode))
                printf("mode mismatch (act/exp) %o != %o %s\n", fstat.st_mode, ntohs(inode.ic_mode), dirEntPath.vfs);
            if (fstat.st_uid != ntohs(inode.ic_uid))
                printf("uid mismatch (act/exp) %d != %d %s\n", fstat.st_uid, ntohs(inode.ic_uid), dirEntPath.vfs);
            if (fstat.st_gid != ntohs(inode.ic_gid))
                printf("gid mismatch (act/exp) %d != %d %s\n", fstat.st_gid, ntohs(inode.ic_gid), dirEntPath.vfs);
            if ((ntohs(inode.ic_mode) & IFMT) != IFDIR) {
                if (fstat.st_size != ntohl(inode.ic_size))
                    printf("size mismatch (act/exp) %"PRId64" != %d %s\n", fstat.st_size, ntohl(inode.ic_size), dirEntPath.vfs);
                /*
                 if(fstat.st_atimespec.tv_sec != fsv(inode.ic_atime.tv_sec))
                 printf("atime_sec mismatch " << dirEntPath << " diff:" << (fstat.st_atimespec.tv_sec - fsv(inode.ic_atime.tv_sec)) << endl;
                 if(fstat.st_atimespec.tv_nsec != fsv(inode.ic_atime.tv_usec) * 1000)
                 printf("atime_nsec mismatch " << dirEntPath << " diff:" << (fstat.st_atimespec.tv_nsec - (fsv(inode.ic_atime.tv_usec) * 1000)) << endl;
                 */
#ifdef _WIN32
                if (fstat.st_mtime != ntohl(inode.ic_mtime.tv_sec))
                    printf("mtime_sec mismatch diff: %ld %s\n", fstat.st_mtime - ntohl(inode.ic_mtime.tv_sec), dirEntPath.vfs);
#else
                if (fstat.st_mtimespec.tv_sec != ntohl(inode.ic_mtime.tv_sec))
                    printf("mtime_sec mismatch diff: %ld %s\n", fstat.st_mtimespec.tv_sec - ntohl(inode.ic_mtime.tv_sec), dirEntPath.vfs);
                if (fstat.st_mtimespec.tv_nsec != ntohl(inode.ic_mtime.tv_usec) * 1000)
                    printf("mtime_nsec mismatch diff: %ld %s\n", fstat.st_mtimespec.tv_nsec - (ntohl(inode.ic_mtime.tv_usec) * 1000), dirEntPath.vfs);
#endif
            }
            if ((uint32_t)fstat.st_rdev != rdev)
                printf("rdev mismatch (act/exp) %d != %d %s\n", fstat.st_rdev, rdev, dirEntPath.vfs);
        }
    }
    
    dirlist_delete(dirlist);
}

static void verify_inodes_recr(struct ufs_t* ufs, struct i2i_t** inode2inode, struct skip_t** skip, uint32_t ino, const char* path, struct vfs_t* ft) {
    struct dirlist_t* dirlist = ufs_list(ufs, ino);
    struct dirlist_t* entry = dirlist;
    
    while (entry) {
        struct direct* dirEnt = &entry->dir;
        struct path_t dirEntPath;
        struct icommon inode;
        struct stat fstat;
        uint64_t ino64;
        
        make_path(path, dirEnt->d_name, &dirEntPath, ft);
        
        entry = entry->next;
        
        if (skip_find(*skip, dirEntPath.vfs)) {
            continue;
        }
        
        vfs_get_fstat(ft, &dirEntPath, &fstat);
        
        ino64 = i2i_find(*inode2inode, dirEnt->d_inonum);
        if (ino64) {
            if (ino64 != fstat.st_ino) {
                printf("inode mismatch (exp/act) %"PRIu64" != %"PRIu64" %s\n", ino64, fstat.st_ino, dirEntPath.vfs);
            }
        } else {
            i2i_add(inode2inode, dirEnt->d_inonum, fstat.st_ino);
        }
        
        if (ufs_readInode(ufs, &inode, ntohl(dirEnt->d_inonum)) == ERR_NO) {
            switch (ntohs(inode.ic_mode) & IFMT) {
                case IFDIR:       /* directory */
                    if (!(ignore_name(dirEnt->d_name)))
                        verify_inodes_recr(ufs, inode2inode, skip, ntohl(dirEnt->d_inonum), dirEntPath.vfs, ft);
                    break;
            }
        }
    }
    
    dirlist_delete(dirlist);
}

static bool do_print(const char* type, const char* listType, bool doPrint, bool force) {
    if (force) {
        force = false;
        return true;
    }
    if (listType) {
        doPrint = strstr(listType, type) != NULL;
    }
    return doPrint;
}

static void process_inodes_recr(struct ufs_t* ufs, struct i2p_t** inode2path, struct skip_t** skip, uint32_t ino, const char* path, struct vfs_t* ft, bool listFiles, const char* listType) {
    struct dirlist_t* dirlist = ufs_list(ufs, ino);
    struct dirlist_t* entry = dirlist;
    
    while (entry) {
        struct direct* dirEnt = &entry->dir;
        struct path_t dirEntPath;
        struct icommon inode;
        
        make_path(path, dirEnt->d_name, &dirEntPath, ft);
        
        entry = entry->next;
        
        if (ufs_readInode(ufs, &inode, ntohl(dirEnt->d_inonum)) == ERR_NO) {
            char* found_path;
            bool doPrint = listFiles;
            bool forcePrint = false;
            
            if (!(ignore_name(dirEnt->d_name)) || strcmp(dirEntPath.vfs, "/") == 0) {
                if (ft && vfs_access(&dirEntPath, F_OK) == 0) {
                    struct stat fstat;
                    
                    if ((ntohs(inode.ic_mode) & IFMT) == IFLNK) {
                        char* link = ufs_readlink(ufs, &inode);
                        if (strcasecmp(link, dirEnt->d_name) == 0) {
                            printf("New file '%s' is link pointing to variant, skipping\n", dirEntPath.vfs);
                            skip_add(skip, dirEntPath.vfs);
                            continue;
                        }
                    }
#ifndef _WIN32
                    vfs_stat(&dirEntPath, &fstat);
                    if (S_ISLNK(fstat.st_mode)) {
                        struct path_t link;
                        vfs_readlink(&dirEntPath, &link);
                        if (strcasecmp(link.vfs, dirEnt->d_name) == 0) {
                            printf("Existing file '%s' is link pointing to variant, removing link\n", dirEntPath.vfs);
                            vfs_remove(&dirEntPath);
                            char tmp[FILENAME_MAX]; 
                            vfscpy(tmp, path, sizeof(tmp)); // string tmp = path;
                            strcat(tmp, "/"); // tmp += "/";
                            vfscat(tmp, link.vfs, sizeof(tmp)); // tmp += link.string();
                            skip_add(skip, tmp); // skip.insert(tmp);
                        }
                    } else
#endif
                        if (strcmp(dirEntPath.vfs, "/")) {
                            printf("WARNING: file '%s' (%s) already exists, skipping.\n", dirEntPath.vfs, dirEntPath.host);
                            skip_add(skip, dirEntPath.vfs);
                            continue;
                        }
                }
                
                found_path = i2p_find(*inode2path, ntohl(dirEnt->d_inonum));
                if (found_path) {
                    struct path_t path_from;
                    make_path(found_path, NULL, &path_from, ft);
                    if ((doPrint = do_print("HLINK", listType, doPrint, forcePrint))) printf("[HLINK] %s <- ", found_path);
                    forcePrint = doPrint;
                    if (ft) vfs_link(&path_from, &dirEntPath, 0);
                } else {
                    i2p_add(inode2path, ntohl(dirEnt->d_inonum), dirEntPath.vfs);
                }            
            }
            
            switch (ntohs(inode.ic_mode) & IFMT) {
                case IFIFO:       /* named pipe (fifo) */
                    if ((doPrint = do_print("FIFO", listType, doPrint, forcePrint))) printf("[FIFO]  ");
                    if (ft) vfs_touch(&dirEntPath);
                    break;
                case IFCHR:       /* character special */
                    if ((doPrint = do_print("CHAR", listType, doPrint, forcePrint))) printf("[CHAR]  ");
                    if (ft) vfs_touch(&dirEntPath);
                    break;
                case IFDIR:       /* directory */
                    if ((doPrint = do_print("DIR", listType, doPrint, forcePrint))) printf("[DIR]   ");
                    if (!(ignore_name(dirEnt->d_name))) {
                        if (doPrint) printf("%s\n", dirEntPath.vfs);
                        doPrint = false;
                        if (ft) vfs_mkdir(&dirEntPath);
                        process_inodes_recr(ufs, inode2path, skip, ntohl(dirEnt->d_inonum), dirEntPath.vfs, ft, listFiles, listType);
                    }
                    break;
                case IFBLK:       /* block special */
                    if ((doPrint = do_print("BLOCK", listType, doPrint, forcePrint))) printf("[BLOCK] ");
                    if (ft) vfs_touch(&dirEntPath);
                    break;
                case IFREG:       /* regular */
                    if ((doPrint = do_print("FILE", listType, doPrint, forcePrint))) printf("[FILE]  ");
                    if (ft && vfs_access(&dirEntPath, F_OK) != 0) {
                        struct file_t* file = file_open(&dirEntPath, "wb"); //VFSFile file(*ft, dirEntPath, "wb");
                        if (file_is_open(file)) {
                            size_t size = ntohl(inode.ic_size);
                            uint8_t* buffer = (uint8_t*)malloc(size); //unique_ptr<uint8_t[]> buffer(new uint8_t[size]);
                            ufs_readFile(ufs, &inode, 0, size, buffer);
                            if (file_write(file, 0, buffer, size) != size) {
                                printf("Error while writing '%s'\n", dirEntPath.vfs);
                                exit(1);
                            }
                            file_close(&dirEntPath, file);
                            free(buffer);
                        }
                    }
                    break;
                case IFLNK: {     /* symbolic link */
                    char* link = ufs_readlink(ufs, &inode);
                    struct path_t path_from;
                    vfscpy(path_from.vfs, link, sizeof(path_from.vfs));
                    if ((doPrint = do_print("SLINK", listType, doPrint, forcePrint))) printf("[SLINK] %s <- ", link);
                    if (ft) vfs_link(&path_from, &dirEntPath, 1);
                    break;
                }
                case IFSOCK:      /* socket */
                    if ((doPrint = do_print("SOCK", listType, doPrint, forcePrint))) printf("[SOCK]  ");
                    if (ft) vfs_touch(&dirEntPath);
                    break;
                default:
                    printf("WARNING: unknown format (%d) '%s'\n", ntohs(inode.ic_mode) & IFMT, dirEntPath.vfs);
                    break;
            }
            
            if (doPrint)
                printf("%s\n", dirEntPath.vfs);
        }
    }
    
    dirlist_delete(dirlist);
}

static void dump_part(struct im_t* im, struct part_t* part, const char* outPath, bool listFiles, const char* listType) {
    struct vfs_t* ft          = NULL;
    struct ufs_t* ufs         = NULL;
    struct i2i_t* inode2inode = NULL;
    struct i2p_t* inode2path  = NULL;
    struct skip_t* skip       = NULL;
    
    ufs = ufs_init(part);
    
    if (ufs && outPath)
        ft = vfs_init(outPath, ufs_mountPoint(ufs));
    
    if (ft) {
        printf("---- copying '%s' partition %zu to '%s'\n", im->path, part->partIdx, ft->base_path.host);
        process_inodes_recr(ufs, &inode2path, &skip, ROOTINO, "", ft, listFiles, listType);
        printf("---- setting file attributes for NFSD\n");
        set_attrs_inode(ufs, ROOTINO, "", ft);
        set_attrs_recr(ufs, &skip, ROOTINO, "", ft);
        printf("---- verifying inode structure\n");
        verify_inodes_recr(ufs, &inode2inode, &skip, ROOTINO, "", ft);
        printf("---- verifying file attributes and sizes\n");
        verify_attr_recr(ufs, &skip, ROOTINO, "", ft);
        ft = vfs_uninit(ft);
    } else {
        printf("---- listing '%s' partition %zu\n", im->path, part->partIdx);
        process_inodes_recr(ufs, &inode2path, &skip, ROOTINO, "", ft, listFiles, listType);
    }
    
    ufs_uninit(ufs);
    i2i_delete(inode2inode);
    i2p_delete(inode2path);
    skip_delete(skip);
}

static bool is_mount(const char* path) {
    char pdir[FILENAME_MAX];
    struct stat sdir;  /* inode info */
    struct stat spdir; /* parent inode info */
    int res = stat(path, &sdir);
    if (res < 0) return false;
    vfscpy(pdir, path, sizeof(pdir));
    vfscat(pdir, "/..", sizeof(pdir));
    res = stat(pdir, &spdir);
    if (res < 0) return false;
    return    sdir.st_dev != spdir.st_dev  /* different devices */
           || sdir.st_ino == spdir.st_ino; /* root dir case */
}

static int ditool_remove(const char* fpath, const struct stat* sb, int typeflag, struct FTW* ftwbuf) {
#ifndef _WIN32
    fchmodat(AT_FDCWD, fpath, ACCESSPERMS, AT_SYMLINK_NOFOLLOW);
    remove(fpath);
#else
    char zzPath[FILENAME_MAX];
    int len, ret;
    vfscpy(zzPath, fpath, FILENAME_MAX - 1);
    len = strlen(zzPath);
    zzPath[len + 1] = '\0';
    SHFILEOPSTRUCT file_op = {NULL, FO_DELETE, zzPath, "",
        FOF_NOCONFIRMATION | FOF_NOERRORUI | FOF_SILENT,
        false, 0, ""};
    ret = SHFileOperation(&file_op);
    if (ret) {
        return EINVAL;
    }
#endif
    return 0;
}

static void clean_dir(const char* path) {
    printf("---- cleaning '%s'\n", path);
    nftw(path, ditool_remove, 1024, FTW_DEPTH | FTW_PHYS);
    if (is_mount(path)) {
        printf("     - directory %s is a mount point, using as-is\n", path);
    } else if (access(path, F_OK | R_OK | W_OK) == 0) {
        char tmp[32];
        char newName[FILENAME_MAX];
        snprintf(tmp, sizeof(tmp), ".%08X", rand());
        vfscpy(newName, path, sizeof(newName));
        vfscat(newName, tmp, sizeof(newName));
        printf("     - directory %s still exists, trying to rename it to '%s'\n", path, newName);
        rename(path, newName);
    }
}

static const char* to_host_path(const char* path) { /* FIXME: expand path? */
    return (path && strlen(path)) ? path : NULL; //path ? HostPath(path) : HostPath();
}

static bool is_case_insensitive(const char* path) {
    int i;
    char* p;
    char filename[FILENAME_MAX];
    const char* testFile = ".nfsd__CASE__TEST__";
    FILE* fs;
    vfscpy(filename, path, sizeof(filename));
    vfscat(filename, "/", sizeof(filename));
    vfscat(filename, testFile, sizeof(filename));
    fs = fopen(filename, "wb");
    fclose(fs);
    p = filename + strlen(path) + 1;
    while (*p++) {
        *p = tolower(*p);
    }
    return (access(filename, F_OK) == 0);
}


int main(int argc, const char* argv[]) {
    sleep(10);
    if (has_option(argv, argc, "-h") || has_option(argv, argc, "--help")) {
        print_help();
        return 0;
    }
    
    const char* imageFile = to_host_path(get_option(argv, argc, "-im"));
    bool        listParts = has_option(argv, argc, "-lsp");
    const char* partNum   = get_option(argv, argc, "-p");
    bool        listFiles = has_option(argv, argc, "-ls");
    const char* listType  = get_option(argv, argc, "-lst");
    const char* outPath   = to_host_path(get_option(argv, argc, "-out"));
    bool        clean     = has_option(argv, argc, "-clean");
    bool        netboot   = has_option(argv, argc, "-netboot");

    if (imageFile) {
        struct im_t* im = diskimage_init(imageFile);
        if (!diskimage_valid(im)) {
            printf("Can't read '%s' (%s).\n", imageFile, im->error);
            return 1;
        }

        if (listType)
            listFiles = true;
        
        if (listParts)
            diskimage_print(im);
        
        if (listFiles || outPath) {
            if (outPath) {
                if (is_case_insensitive(outPath)) {
                    printf("WARNING: %s is on a case insensitive file system.\n", outPath);
                    printf("         NeXTstep requires a case sensitive file system to run properly.\n");
                    printf("         Use i.e. macOS Disk Utility to create a disk image with a\n");
                    printf("         case sensitive file system for your NFS directory.\n");
                }
                if (clean) clean_dir(outPath);
#ifdef _WIN32
                mkdir(outPath);
#else
                mkdir(outPath, DEFAULT_PERM);
#endif
                if (access(outPath, F_OK | R_OK | W_OK) < 0) {
                    printf("Can't access '%s'\n", outPath);
                    return 1;
                }
            }
            
            int part = partNum ? atoi(partNum) : -1;
            struct part_t* parts = im->parts;
            while (parts) {
                if (part < 0 || part == parts->partIdx) {
                    dump_part(im, parts, outPath, listFiles, listType);
                }
                parts = parts->next;
            }
        }
        diskimage_uninit(im);
        
    } else if (!(netboot)) {
        printf("Missing image file.\n");
        print_help();
        return 1;
    }
    
    if (netboot) {
        if (outPath) {
            printf("---- preparing for netboot\n");
            prepare_netboot(outPath);
        } else {
            printf("Missing output path.\n");
            print_help();
            return 1;
        }
    }
    sleep(10);
    
    printf("---- done.\n");
    return 0;
}
