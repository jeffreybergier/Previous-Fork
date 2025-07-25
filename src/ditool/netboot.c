#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <errno.h>

#include "config.h"
#include "netboot.h"
#include "vfs.h"
#include "ctl.h"


static char* ip_addr_str(char* buf, int size, uint32_t addr) {
    snprintf(buf, size, "%d.%d.%d.%d", (addr >> 24) & 0xFF, (addr >> 16) & 0xFF, (addr >> 8) & 0xFF, addr & 0xFF);
    return buf;
}

static int add_line_file(FILE* file, const char* line) {
    if (strlen(line)) {
        printf("       - adding line '%s'\n", line);
        if (fputs(line, file) == EOF) return errno;
    }
    if (fputc('\n', file) == EOF) return errno;
    return 0;
}

static void add_line(char* data, int size, const char* line) {
    int len = strlen(data);
    if (len > 0 && len < size && data[len - 1] != '\n') {
        data[len] = '\n';
        len++;
    }
    printf("       - adding line '%s'\n", line);
    memcpy(data + len, line, strlen(line));
    vfscpy(data + len + strlen(line), "\n", size);
}

static void remove_line(char* data, const char* line) {
    char* start;
    char* stop;
    while ((start = strstr(data, line))) {
        stop = strchr(start, '\n');
        if (stop == NULL) {
            stop = start + strlen(start);
        }
        printf("       - removing line '%.*s'\n", (int)(stop - start), start);
        memmove(start, stop + 1, strlen(stop) + 1);
    }
}

static void netboot_link_kernel(struct vfs_t* ft, const char* from, const char* to) {
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
    int err = 0;
    struct path_t path;
    
    printf("     - writing '%s'\n", file);
    
    vfscpy(path.vfs, file, sizeof(path.vfs));
    vfs_to_host_path(ft, &path);
    
    if (err == 0) {
        struct file_t* resolv = file_open(&path, "wb");
        if (file_is_open(resolv)) {
            char buf[256];
            char ip_addr[32];
            snprintf(buf, sizeof(buf), "domain %s", NAME_DOMAIN[0] == '.' ? &NAME_DOMAIN[1] : &NAME_DOMAIN[0]);
            add_line_file(resolv->file, buf);
            snprintf(buf, sizeof(buf), "nameserver %s", ip_addr_str(ip_addr, sizeof(ip_addr), CTL_NET | CTL_DNS));
            add_line_file(resolv->file, buf);
            file_close(&path, resolv);
        } else {
            printf("       ! failed opening '%s' (%s)\n", path.host, strerror(errno));
        }
    }
}

static void netboot_patch_hosts(struct vfs_t* ft, const char* file) {
    int err = 0;
    struct path_t path;
    
    printf("     - patching '%s'\n", file);
    
    vfscpy(path.vfs, file, sizeof(path.vfs));
    vfs_to_host_path(ft, &path);
    
    err = vfs_access(&path, F_OK | W_OK | R_OK);
    if (err == 0) {
        struct file_t* hosts = file_open(&path, "rb");
        if (file_is_open(hosts)) {
            long size;
            char buf[256];
            char ip_addr[32];
            int len = 0;
            char* data = NULL;
            fseek(hosts->file, 0, SEEK_END);
            size = ftell(hosts->file);
            if (size >= 0) {
                len = size + 2 + 2 * sizeof(buf);
                data = (char*)calloc(1, len);
                if (size > 0) {
                    file_read(hosts, 0, data, size);
                    remove_line(data, ip_addr_str(ip_addr, sizeof(ip_addr), CTL_NET | CTL_HOST));
                    remove_line(data, ip_addr_str(ip_addr, sizeof(ip_addr), CTL_NET | CTL_NFSD));
                }
            }
            file_close(&path, hosts);
            if (data) {
                hosts = file_open(&path, "wb");
                if (file_is_open(hosts)) {        
                    snprintf(buf, sizeof(buf), "%s\t%s", ip_addr_str(ip_addr, sizeof(ip_addr), CTL_NET | CTL_HOST), NAME_HOST);
                    add_line(data, len, buf);
                    snprintf(buf, sizeof(buf), "%s\t%s", ip_addr_str(ip_addr, sizeof(ip_addr), CTL_NET | CTL_NFSD), NAME_NFSD);
                    add_line(data, len, buf);
                    file_write(hosts, 0, data, strlen(data));
                    file_close(&path, hosts);
                } else {
                    printf("       ! failed opening '%s' (%s)\n", path.host, strerror(errno));
                }
                free(data);
            }
        } else {
            printf("       ! failed opening '%s' (%s)\n", path.host, strerror(errno));
        }
    } else {
        printf("       ! cannot access '%s' (%s)\n", path.host, strerror(err));
    }
}

static void netboot_patch_hostconfig(struct vfs_t* ft, const char* file, const char* template) {
    int err = 0;
    struct path_t path;
    struct path_t temp;
    
    printf("     - patching '%s'\n", file);
    
    vfscpy(path.vfs, file, sizeof(path.vfs));
    vfs_to_host_path(ft, &path);
    
    vfscpy(temp.vfs, template, sizeof(temp.vfs));
    vfs_to_host_path(ft, &temp);
    
    if (vfs_access(&temp, F_OK | R_OK) == 0) {
        printf("       - using template %s\n", template);
    } else {
        vfscpy(temp.vfs, file, sizeof(temp.vfs));
        vfs_to_host_path(ft, &temp);
    }
    
    err = vfs_access(&temp, F_OK | R_OK);
    if (err == 0) {
        struct file_t* hostconfig = file_open(&temp, "rb");
        if (file_is_open(hostconfig)) {
            long size;
            int len = 0;
            char* data = NULL;
            fseek(hostconfig->file, 0, SEEK_END);
            size = ftell(hostconfig->file);
            if (size >= 0) {
                len = size + 2 + 2 * 256;
                data = (char*)calloc(1, len);
                if (size > 0) {
                    file_read(hostconfig, 0, data, size);
                    remove_line(data, "ROUTER");
                    remove_line(data, "NETMASK");
                }
            }
            file_close(&temp, hostconfig);
            if (data) {
                hostconfig = file_open(&path, "wb");
                if (file_is_open(hostconfig)) {
                    add_line(data, len, "ROUTER=-ROUTED-");
                    add_line(data, len, "IPNETMASK=-AUTOMATIC-");
                    file_write(hostconfig, 0, data, strlen(data));
                    file_close(&path, hostconfig);
                } else {
                    printf("       ! failed opening '%s' (%s)\n", path.host, strerror(errno));
                }
                free(data);
            }
        } else {
            printf("       ! failed opening '%s' (%s)\n", temp.host, strerror(errno));
        }
    } else {
        printf("       ! cannot access '%s' (%s)\n", temp.host, strerror(err));
    }
}

static void netboot_make_fstab(struct vfs_t* ft, const char* file) {
    int err = 0;
    struct path_t path;
    
    printf("     - writing '%s'\n", file);
    
    vfscpy(path.vfs, file, sizeof(path.vfs));
    vfs_to_host_path(ft, &path);
    
    err = vfs_access(&path, F_OK | R_OK | W_OK);
    if (err == 0) {
        char buf[256];
        char ip_addr[32];
        struct file_t* fstab = file_open(&path, "wb");
        if (file_is_open(fstab)) {
            snprintf(buf, sizeof(buf), "%s:/ / nfs rw,noauto 0 0", NAME_NFSD);
            add_line_file(fstab->file, buf);
            snprintf(buf, sizeof(buf), "%s:/private /private nfs rw,noauto 0 0", NAME_NFSD);
            add_line_file(fstab->file, buf);
            file_close(&path, fstab);
        } else {
            printf("       ! failed opening '%s' (%s)\n", path.host, strerror(errno));
        }
    } else {
        printf("       ! cannot access '%s' (%s)\n", path.host, strerror(err));
    }
}


void prepare_netboot(const char* path) {
    struct vfs_t* ft = vfs_init(path, "/");
    
    netboot_link_kernel(ft, "../../sdmach", "/private/tftpboot/mach");
    netboot_make_resolv(ft, "/private/etc/resolv.conf");
    netboot_patch_hosts(ft, "/private/etc/hosts");
    netboot_patch_hostconfig(ft, "/private/etc/hostconfig", "/usr/template/client/etc/hostconfig");
    netboot_make_fstab(ft, "/private/etc/fstab");

    vfs_uninit(ft);
}
