/*
 * Domain Name System
 * 
 * Created by Simon Schubiger on 22.02.2019
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

#include "rpc.h"
#include "dns.h"
#include "ctl.h"


typedef enum {
    REC_A     = 1,  /* Host address */
    REC_CNAME = 5,  /* Canonical name for an alias */
    REC_MX    = 15, /* Mail eXchange */
    REC_NS    = 2,  /* Name Server */
    REC_PTR   = 12, /* Pointer */
    REC_SOA   = 6,  /* Start Of Authority */
    REC_SRV   = 33, /* location of service */
    REC_TXT   = 16, /* Descriptive text */
    
    REC_UNKNOWN = -1,
} vdns_rec_type;

struct vdns_record_t {
    vdns_rec_type type;
    char*         key;
    uint8_t       data[1024];
    size_t        size;
    uint32_t      inaddr;
};

struct vdns_entry_t {
    struct vdns_record_t* rec;
    struct vdns_entry_t* next;
};

static struct vdns_t {
    struct vdns_entry_t* db;
    struct vdns_record_t errNoSuchName;
    mutex_t*             mutex;
    struct udpsocket_t*  udp;
} vdns;

static void vdns_add_record(struct vdns_record_t* rec) {
    struct vdns_entry_t** entry = &vdns.db;

    while (*entry) {
        if (((*entry)->rec->type != rec->type) || 
            strncmp((*entry)->rec->key, rec->key, RPC_MAXPATHLEN)) {
            entry = &(*entry)->next;
            continue;
        }
        printf("[DNS] Duplicate record (%d) %s\n", rec->type, rec->key);
        free((*entry)->rec->key);
        free((*entry)->rec);
        (*entry)->rec = rec;
        return;
    }
    *entry = (struct vdns_entry_t*)malloc(sizeof(struct vdns_entry_t));
    (*entry)->rec = rec;
    (*entry)->next = NULL;
}

static struct vdns_record_t* vdns_find_record(char* key, vdns_rec_type type) {
    struct vdns_entry_t** entry = &vdns.db;
    struct vdns_entry_t* next = NULL;
    
    while (*entry) {
        if (((*entry)->rec->type == type) &&  
            (strncmp((*entry)->rec->key, key, RPC_MAXPATHLEN) == 0)) {
            return (*entry)->rec;
        }
        entry = &(*entry)->next;
    }
    return NULL;
}

static void vdns_delete_db(void) {
    struct vdns_entry_t** entry = &vdns.db;
    struct vdns_entry_t* next = NULL;
    
    while (*entry) {
        free((*entry)->rec->key);
        free((*entry)->rec);
        next = (*entry)->next;
        free((*entry));
        *entry = next;
    }
}


static size_t domain_name(uint8_t* dst, const char* src) {
    size_t   result = strlen(src) + 2;
    uint8_t* len    = dst++;
    *len            = 0;
    while(*src) {
        if(*src == '.') {
            len = dst++;
            *len = 0;
            src++;
            continue;
        }
        *dst++ = tolower(*src++);
        *len = *len + 1;
    }
    *dst++ = '\0';
    return result;
}

static char* alloc_ip_addr_str(uint32_t addr, const char* suffix) {
    char* result = (char*)malloc(16 + strnlen(suffix, RPC_MAXNAMELEN - 16));
    snprintf(result, RPC_MAXNAMELEN, "%d.%d.%d.%d%s", (addr>>24)&0xFF, (addr>>16)&0xFF, (addr>>8)&0xFF, addr&0xFF, suffix);
    return result;
}

static char* alloc_lowercase_name(const char* name, const char* suffix) {
    size_t i;
    size_t size = strnlen(name, RPC_MAXNAMELEN - 16 - 1);
    char* result = (char*)malloc(size + strnlen(suffix, 16) + 1);
    for (i = 0; i < size; i++) {
        result[i] = tolower(name[i]);
    }
    result[size] = '\0';
    strncat(result, suffix, RPC_MAXNAMELEN);
    return result;
}

static void addRecord(uint32_t addr, const char* name) {
    struct vdns_record_t* rec;
    size_t i;
    uint32_t inaddr = htonl(addr);

    rec = (struct vdns_record_t*)malloc(sizeof(struct vdns_record_t));
    rec->type   = REC_A;
    rec->inaddr = addr;
    rec->key    = alloc_ip_addr_str(addr, ".");
    rec->size = 4;
    memcpy(rec->data, &inaddr, rec->size);
    vdns_add_record(rec);
    
    rec = (struct vdns_record_t*)malloc(sizeof(struct vdns_record_t));
    rec->type   = REC_A;
    rec->inaddr = addr;
    rec->key    = alloc_lowercase_name(name, ".");
    inaddr = htonl(addr);
    rec->size = 4;
    memcpy(rec->data, &inaddr, rec->size);
    vdns_add_record(rec);
    
    rec = (struct vdns_record_t*)malloc(sizeof(struct vdns_record_t));
    rec->type   = REC_PTR;
    rec->inaddr = addr;
    rec->key    = alloc_ip_addr_str(SDL_Swap32(addr), ".in-addr.arpa.");
    rec->size   = domain_name(rec->data , name);
    vdns_add_record(rec);
}


void vdns_init(void) {
    uint32_t port;
    vdns.mutex = host_mutex_create();
    vdns.udp   = udpsocket_init(vdns_socketReceived);
    if (vdns.udp) {
        port = udpsocket_open(vdns.udp, PORT_DNS);
        if (port) {
            printf("[DNS] started (UDP: %d -> %d).\n", port, udpsocket_toLocalPort(vdns.udp, port));
        } else {
            printf("[DNS] start failed.\n");
            udpsocket_close(vdns.udp);
            vdns.udp = udpsocket_uninit(vdns.udp);
        }
    } else {
        printf("[DNS] Socket initialisation failed.");
    }
#if 0
    vector<NetInfoNode*> machines = netInfoBind->m_Network.mRoot.find("name", "machines")[0]->mChildren;
    string domain(NAME_DOMAIN);
    for(size_t i = 0; i < machines.size(); i++) {
        string name = machines[i]->getPropValue("name");
        if(name.size() <= domain.size() || name.compare(name.size() - domain.size(), domain.size(), domain))
            name += domain;
        string ip   = machines[i]->getPropValues(machines[i]->mProps, "ip_address")[0];
        in_addr addr;
#ifdef _WIN32
        inet_pton(AF_INET, ip.c_str(), &addr);
#else
        inet_aton(ip.c_str(), &addr);
#endif
        addRecord(ntohl(addr.s_addr), name);
    }
#else
    {
        char hostname[NAME_HOST_MAX];
        memset(hostname, 0, sizeof(hostname));
        gethostname(hostname, sizeof(hostname));
        strcat(hostname, NAME_DOMAIN);
        
        addRecord(CTL_NET|CTL_ALIAS, hostname);
        addRecord(CTL_NET|CTL_HOST,  FQDN_HOST);
        addRecord(CTL_NET|CTL_DNS,   FQDN_DNS);
        addRecord(CTL_NET|CTL_NFSD,  FQDN_NFSD);
    }
#endif
    addRecord(0x7F000001, "localhost");
}

void vdns_uninit(void) {
    if (vdns.udp) {
        udpsocket_close(vdns.udp);
        vdns.udp = udpsocket_uninit(vdns.udp);
        host_mutex_destroy(vdns.mutex);
    }
    vdns_delete_db();
}

static vdns_rec_type to_dot(char* dst, const uint8_t* src, size_t size) {
    int j;
    const uint8_t* end    = &src[size];
    uint8_t        count  = 0;
    uint16_t       result = REC_UNKNOWN;
    while (*src) {
        if (src >= end) return REC_UNKNOWN;
        count = *src++;
        if (count > 63) return REC_UNKNOWN;
        for (j = 0; j < count; j++) {
            if (src >= end) return REC_UNKNOWN;
            *dst++ = tolower(*src++);
        }
        *dst++ = '.';
    }
    src++;
    result = *src++;
    result <<= 8;
    result |= *src;
    return (vdns_rec_type)result;
}

static char *rstrstr(char* s1, char* s2)
{
    size_t  s1len = strlen(s1);
    size_t  s2len = strlen(s2);
    char *s;
    
    if (s2len > s1len)
        return NULL;
    for (s = s1 + s1len - s2len; s >= s1; --s)
        if (strncmp(s, s2, s2len) == 0)
            return s;
    return NULL;
}

static struct vdns_record_t* vdns_query(uint8_t* data, size_t size) {
    struct vdns_record_t* rec;
    size_t n, offset;
    char qname[RPC_MAXNAMELEN];
    char domain[RPC_MAXNAMELEN];
    vdns_rec_type qtype = to_dot(qname, data, size);
    printf("[DNS] query(%d) '%s'\n", qtype, qname);
    
    if (qtype < 0) return NULL;
    
    rec = vdns_find_record(qname, qtype);
    if (rec) {
        return rec;
    }
    
    snprintf(domain, RPC_MAXNAMELEN, "%s.", NAME_DOMAIN);
    offset = strlen(qname) - strlen(domain);
    if (offset >= 0) {
        if (strncmp(qname + offset, domain, strlen(domain)) == 0) {
            return &vdns.errNoSuchName;
        }
    }
    return NULL;
}


int vdns_match(struct mbuf *m, uint32_t addr, int dport) {
    if(m->m_len > 40 &&
       dport == PORT_DNS &&
       addr == (CTL_NET | CTL_DNS))
        return vdns_query((uint8_t*)(&m->m_data[40]), m->m_len-40) != NULL;
    else
        return false;
}

void vdns_udp_map_to_local_port(struct in_addr* ipNBO, uint16_t* dportNBO) {
    switch(ntohs(*dportNBO)) {
        case PORT_DNS:
            /* map port and address for virtual DNS */
            *dportNBO = htons(udpsocket_toLocalPort(vdns.udp, PORT_DNS));
            *ipNBO    = loopback_addr;
            break;
        default:
            break;
    }
}

void vdns_socketReceived(struct csocket_t* pSocket, uint32_t header) {
    host_mutex_lock(vdns.mutex);
    
    struct xdr_t* m_in  = &pSocket->m_Input;
    struct xdr_t* m_out = &pSocket->m_Output;
    uint8_t*      msg   = m_in->data;
    uint8_t*      start = m_out->data;
    int           n     = m_in->size;
    size_t        off   = 12;
    
    struct vdns_record_t* rec = vdns_query(&msg[off], m_in->size - (/*in->getPosition()*/ + off));
    
    if (rec == &vdns.errNoSuchName) {
        /*
         1... .... .... .... = Response: Message is a response
         .000 0... .... .... = Opcode: Standard query (0)
         .... .1.. .... .... = Authoritative: Server is an authority for domain
         .... ..0. .... .... = Truncated: Message is not truncated
         .... ...0 .... .... = Recursion desired: Do not query recursively
         .... .... 0... .... = Recursion available: Server can not do recursive queries
         .... .... .0.. .... = Z: reserved (0)
         .... .... ..0. .... = Answer authenticated: Answer/authority portion was authenticated by the server
         .... .... ...1 .... = Non-authenticated data: Acceptable
         .... .... .... 0011 = Reply code: No such name (3)
         */
        msg[2]=0x84;
        msg[3]=0x13;
        
        /* Change Opcode and flags */
        msg[6]=0;msg[7]   = 0; /* no answers */
        msg[8]=0;msg[9]   = 0; /* NSCOUNT */
        msg[10]=0;msg[11] = 0; /* ARCOUNT */
        
        printf("[DNS] no record found.\n");
    } else {
        /*
         1... .... .... .... = Response: Message is a response
         .000 0... .... .... = Opcode: Standard query (0)
         .... .1.. .... .... = Authoritative: Server is an authority for domain
         .... ..0. .... .... = Truncated: Message is not truncated
         .... ...0 .... .... = Recursion desired: Do not query recursively
         .... .... 0... .... = Recursion available: Server can not do recursive queries
         .... .... .0.. .... = Z: reserved (0)
         .... .... ..0. .... = Answer authenticated: Answer/authority portion was authenticated by the server
         .... .... ...1 .... = Non-authenticated data: Acceptable
         .... .... .... 0000 = Reply code: No error (0)
         */
        
        msg[2]=0x84;
        msg[3]=0x10;
        /* Change Opcode and flags */
        msg[8]=0;msg[9]=0;   /* NSCOUNT */
        msg[10]=0;msg[11]=0; /* ARCOUNT */
        
        if (rec) {
            /* Keep request in message and add answer */
            msg[n++]=0xC0; msg[n++]=off; /* Offset to the domain name */
            
            msg[n++]=0x00;
            msg[n++]=rec->type;  /* Type */
            
            msg[n++]=0x00;msg[n++]=0x01; /* Class 1 */
            msg[n++]=0x00;msg[n++]=0x00;msg[n++]=0x00;msg[n++]=0x3c; /* TTL */
            
            msg[6]=0;msg[7] = 1; /* Num answers */
            uint32_t inaddr = rec->inaddr;
            printf("[DNS] reply '%s' -> %d.%d.%d.%d\n", rec->key, (inaddr>>24)&0xFF, (inaddr>>16)&0xFF, (inaddr>>8)&0xFF, inaddr&0xFF);
            switch(rec->type) {
                case REC_A:
                case REC_PTR:
                    msg[n++]=0x00;msg[n++]=rec->size;
                    memcpy(&msg[n], rec->data, rec->size);
                    n += rec->size;
                    break;
                default:
                    printf("[DNS] unknown query:%d ('%s')\n", rec->type, rec->key);
                    break;
            }
        } else {
            msg[6]=0;msg[7] = 0; /* Num answers */
            printf("[DNS] no record found.\n");
        }
    }
    
    /* Send the answer */
    xdr_write_data(m_out, msg, n);
    m_out->data = start; /* rewind before sending */
    m_out->size = n;     /* and undo alignment    */
#if 0
    for (int i = 0; i < n; i++) {
        printf("%02x ", msg[i]);
    }
    printf("\n");
    for (int i = 0; i < pSocket->m_Output.size; i++) {
        printf("%02x ", pSocket->m_Output.data[i]);
    }
    printf("\n");
#endif
    csocket_send(pSocket);
    
    host_mutex_unlock(vdns.mutex);
}
