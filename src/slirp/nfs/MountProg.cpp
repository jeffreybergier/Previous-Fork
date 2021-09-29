#include <fstream>
#include <sstream>

#include "MountProg.h"
#include "FileTableNFSD.h"
#include "nfsd.h"
#include "compat.h"

using namespace std;

enum
{
	MNT_OK = 0,
    MNTERR_PERM = 1,
    MNTERR_NOENT = 2,
	MNTERR_IO = 5,
    MNTERR_ACCESS = 13,
	MNTERR_NOTDIR = 20,
    MNTERR_INVAL = 22
};

CMountProg::CMountProg() : CRPCProg(PROG_MOUNT, 3, "mountd"), m_nMountNum(0) {
	m_exportPath[0] = '\0';
	memset(m_clientAddr, 0, sizeof(m_clientAddr));
    #define RPC_PROG_CLASS CMountProg
    SET_PROC(1, MNT);
    SET_PROC(3, UMNT);
    SET_PROC(5, EXPORT);
}

CMountProg::~CMountProg() {
	for (int i = 0; i < MOUNT_NUM_MAX; i++) delete[] m_clientAddr[i];
}

int CMountProg::procedureMNT(void) {
    XDRString path;
    int i, addr_len;

    m_in->read(path);
    log("MNT from %s for '%s'\n", m_param->remoteAddr, path.c_str());
    
    uint64_t handle = nfsd_fts[0]->getFileHandle(path.c_str());
    if(handle) {
        m_out->write(MNT_OK); //OK
        
        uint64_t data[8] = {handle, 0, 0, 0, 0, 0, 0, 0};
        
        if (m_param->version == 1) {
            m_out->write(data, FHSIZE);
        } else {
            m_out->write(FHSIZE_NFS3);
            m_out->write(data, FHSIZE_NFS3);
            m_out->write(0);  //flavor
        }
        
        ++m_nMountNum;
        addr_len = strlen(m_param->remoteAddr);
        
        for (i = 0; i < MOUNT_NUM_MAX; i++) {
            if (m_clientAddr[i] == NULL) { //search an empty space
                m_clientAddr[i] = new char[addr_len + 1];
                strncpy(m_clientAddr[i], m_param->remoteAddr, addr_len + 1);  //remember the client address
                break;
            }
        }
    } else {
        m_out->write(MNTERR_ACCESS);  //permission denied
    }
    
    return PRC_OK;
}

int CMountProg::procedureUMNT(void) {
    XDRString path;
    m_in->read(path);
    log("UNMT from %s for '%s'", m_param->remoteAddr, path.c_str());
    
    for (int i = 0; i < MOUNT_NUM_MAX; i++) {
        if (m_clientAddr[i] != NULL) {
            if (strcmp(m_param->remoteAddr, m_clientAddr[i]) == 0) { //address match
                delete[] m_clientAddr[i];  //remove this address
                m_clientAddr[i] = NULL;
                --m_nMountNum;
                break;
            }
        }
    }
    
    return PRC_OK;
}

int CMountProg::procedureEXPORT(void) {
    log("EXPORT");
    
    VFSPath path = nfsd_fts[0]->getBasePathAlias();
    // dirpath
    m_out->write(1);
    m_out->write(MAXPATHLEN, path.c_str());
    // groups
    m_out->write(1);
    m_out->write(1);
    m_out->write((void*)"*...", 4);
    m_out->write(0);
    
    m_out->write(0);
    m_out->write(0);
    
    return PRC_OK;
}
