/*
 *  netboot.c
 *  Previous
 *
 *  Created by Andreas Grabher on 25.07.2025.
 *
 *  Inspired by ditool.cpp by Simon Schubiger.
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <errno.h>

#include "config.h"
#include "netboot.h"
#include "rpc/vfs.h"
#include "ctl.h"


#define MAX_LINE_LEN 256

static void* read_file_to_buffer(struct vfs_t* ft, const char* path, int extra, int* bufsize, int* datasize, int silent) {
    void* buf = NULL;
    struct path_t file_path;
    int err;
    
    vfscpy(file_path.vfs, path, sizeof(file_path.vfs));
    vfs_to_host_path(ft, &file_path);
    
    err = vfs_access(&file_path, F_OK | W_OK | R_OK);
    if (err) {
        if (!silent) printf("       ! cannot access '%s' (%s)\n", file_path.host, strerror(err));
    } else {
        struct file_t* file = file_open(&file_path, "rb");
        if (file_is_open(file)) {
            long size;
            fseek(file->file, 0, SEEK_END);
            size = ftell(file->file);
            if (size < 0) {
                printf("       ! cannot get size of '%s' (%s)\n", file_path.host, strerror(errno));
            } else if (size > (1024 * 1024)) {
                printf("       ! strange size of '%s' (%ld Byte)\n", file_path.host, size);
            } else {
                int len = size + extra;
                buf = calloc(1, len);
                if (size > 0) {
                    long readsize = file_read(file, 0, buf, size);
                    if (readsize != size || (extra && size != strlen(buf))) {
                        const char* errstr = (readsize != size) ? strerror(errno) : "invalid data";
                        printf("       ! cannot read '%s' (%s)\n", file_path.host, errstr);
                        free(buf);
                        buf = NULL;
                    }
                }
                file_close(&file_path, file);
                *bufsize = len;
                *datasize = size;
            }
        } else {
            printf("       ! cannot open '%s' (%s)\n", file_path.host, strerror(errno));
        }
    }
    return buf;
}

static void write_buffer_to_file(struct vfs_t* ft, const char* path, void* buf, int datasize) {
    struct path_t file_path;
    struct file_t* file;
    
    vfscpy(file_path.vfs, path, sizeof(file_path.vfs));
    vfs_to_host_path(ft, &file_path);

    file = file_open(&file_path, "wb");
    if (file_is_open(file)) {
        if (file_write(file, 0, buf, datasize) != datasize) {
            printf("       ! cannot write '%s' (%s)\n", file_path.host, strerror(errno));
        }
        file_close(&file_path, file);
    } else {
        printf("       ! cannot open '%s' (%s)\n", file_path.host, strerror(errno));
    }
    free(buf);
}

static int add_line(char* buf, int bufsize, const char* line) {
    int len = strlen(line);
    int datasize = strlen(buf);
    int before = datasize;
    if (datasize > 0 && datasize < bufsize && buf[datasize - 1] != '\n') {
        buf[datasize] = '\n';
        datasize++;
    }
    if (datasize + len < bufsize) {
        printf("       - adding line '%s'\n", line);
        memcpy(buf + datasize, line, len);
        datasize += len;
        vfscpy(buf + datasize, "\n", bufsize);
        datasize++;
    } else {
        printf("       ! adding line '%s' failed\n", line);
    }
    return datasize - before;
}

static int remove_line(char* data, const char* line) {
    char* start;
    char* stop;
    int size = 0;
    while ((start = strstr(data, line))) {
        if (start > data && *(start - 1) != '\n') {
            data = strchr(start, '\n');
            if (data) continue;
            else      break;
        }
        stop = strchr(start, '\n');
        if (stop == NULL) {
            stop = start + strlen(start);
        }
        printf("       - removing line '%.*s'\n", (int)(stop - start), start);
        memmove(start, stop + 1, strlen(stop) + 1);
        size += stop + 1 - start;
    }
    return size;
}

static char* ip_addr_str(char* buf, int size, uint32_t addr) {
    snprintf(buf, size, "%d.%d.%d.%d", (addr >> 24) & 0xFF, (addr >> 16) & 0xFF, (addr >> 8) & 0xFF, addr & 0xFF);
    return buf;
}


static void netboot_link_kernel(struct vfs_t* ft, const char* to, const char* from) {
    int err = 0;
    struct path_t path_to;
    struct path_t path_from;
    
    printf("     - linking '%s' -> '%s'\n", from, to);
    
    vfscpy(path_from.vfs, from, sizeof(path_from.vfs));
    vfscpy(path_to.vfs, to, sizeof(path_to.vfs));
    vfs_to_host_path(ft, &path_to);
    
    if (vfs_access(&path_to, F_OK) == 0) {
        err = vfs_remove(&path_to);
        printf("       - replacing '%s'\n", to);
        if (err) {
            printf("       ! cannot remove '%s' (%s)\n", path_to.host, strerror(err));
        }
    }
    if (err == 0) {
        err = vfs_link(&path_from, &path_to, 1);
        if (err) {
            printf("       ! cannot create '%s' (%s)\n", path_to.host, strerror(err));
        }
    }
}

static void netboot_make_resolv(struct vfs_t* ft, const char* file) {
    void* data;
    char ip_addr[32];
    char linebuf[MAX_LINE_LEN];
    int datasize = 0;
    int bufsize = 2 * (sizeof(linebuf) + 3);
    
    printf("     - writing '%s'\n", file);
    
    data = calloc(1, bufsize);
    
    snprintf(linebuf, sizeof(linebuf), "domain %s", NAME_DOMAIN[0] == '.' ? &NAME_DOMAIN[1] : &NAME_DOMAIN[0]);
    datasize += add_line(data, bufsize, linebuf);
    snprintf(linebuf, sizeof(linebuf), "nameserver %s", ip_addr_str(ip_addr, sizeof(ip_addr), CTL_NET | CTL_DNS));
    datasize += add_line(data, bufsize, linebuf);
    write_buffer_to_file(ft, file, data, datasize);
}

static void netboot_patch_hosts(struct vfs_t* ft, const char* file) {
    void* data;
    char ip_addr[32];
    char linebuf[MAX_LINE_LEN];
    int datasize = 0;
    int bufsize = 0;
    int extra = 2 * (sizeof(linebuf) + 3);
    
    printf("     - patching '%s'\n", file);
    
    data = read_file_to_buffer(ft, file, extra, &bufsize, &datasize, 0);
    if (data) {
        datasize -= remove_line(data, ip_addr_str(ip_addr, sizeof(ip_addr), CTL_NET | CTL_HOST));
        datasize -= remove_line(data, ip_addr_str(ip_addr, sizeof(ip_addr), CTL_NET | CTL_NFSD));
        snprintf(linebuf, sizeof(linebuf), "%s\t%s", ip_addr_str(ip_addr, sizeof(ip_addr), CTL_NET | CTL_HOST), NAME_HOST);
        datasize += add_line(data, bufsize, linebuf);
        snprintf(linebuf, sizeof(linebuf), "%s\t%s", ip_addr_str(ip_addr, sizeof(ip_addr), CTL_NET | CTL_NFSD), NAME_NFSD);
        datasize += add_line(data, bufsize, linebuf);
        write_buffer_to_file(ft, file, data, datasize);
    }
}

static void netboot_patch_hostconfig(struct vfs_t* ft, const char* file, const char* template) {
    void* data;
    int datasize = 0;
    int bufsize = 0;
    int extra = 2 * (MAX_LINE_LEN + 3);
    
    printf("     - patching '%s'\n", file);
    
    data = read_file_to_buffer(ft, template, extra, &bufsize, &datasize, 1);
    if (data) {
        printf("       - using template '%s'\n", template);
    } else {
        data = read_file_to_buffer(ft, file, extra, &bufsize, &datasize, 0);
    }
    if (data) {
        datasize -= remove_line(data, "ROUTER");
        datasize -= remove_line(data, "IPNETMASK");
        datasize += add_line(data, bufsize, "ROUTER=-ROUTED-");
        datasize += add_line(data, bufsize, "IPNETMASK=-AUTOMATIC-");
        write_buffer_to_file(ft, file, data, datasize);
    }
}

static void netboot_make_fstab(struct vfs_t* ft, const char* file) {
    void* data;
    char ip_addr[32];
    char linebuf[MAX_LINE_LEN];
    int datasize = 0;
    int bufsize = 2 * sizeof(linebuf) + 3;
    
    printf("     - writing '%s'\n", file);
    
    data = calloc(1, bufsize);
    
    snprintf(linebuf, sizeof(linebuf), "%s:/ / nfs rw,noauto 0 0", NAME_NFSD);
    datasize += add_line(data, bufsize, linebuf);
    snprintf(linebuf, sizeof(linebuf), "%s:/private /private nfs rw,noauto 0 0", NAME_NFSD);
    datasize += add_line(data, bufsize, linebuf);
    write_buffer_to_file(ft, file, data, datasize);
}


void prepare_netboot(const char* path) {
    struct vfs_t* ft = vfs_init(path, "/");
    
    netboot_link_kernel(ft, "/private/tftpboot/mach", "../../sdmach");
    netboot_make_resolv(ft, "/private/etc/resolv.conf");
    netboot_patch_hosts(ft, "/private/etc/hosts");
    netboot_patch_hostconfig(ft, "/private/etc/hostconfig", "/usr/template/client/etc/hostconfig");
    netboot_make_fstab(ft, "/private/etc/fstab");

    vfs_uninit(ft);
}
