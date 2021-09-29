//
//  NetInfoBindProg.cpp
//  Slirp
//
//  Created by Simon Schubiger on 9/1/21.
//

#include "nfsd.h"
#include "NetInfoBindProg.h"

#include <sstream>

using namespace std;

extern void add_rpc_program(CRPCProg *pRPCProg, uint16_t port = 0);

class NIProps
{
private:
    map<string, string> m_map;
public:
    NIProps(const string& key, const string& val) {
        m_map[key] = val;
    }
    NIProps& operator()(const string& key, const string& val) {
        m_map[key] = val;
        return *this;
    }
    operator map<string, string>() {
        return m_map;
    }
};

static string ip_addr_str(uint32_t addr, size_t count) {
    stringstream ss;
    switch(count) {
        case 3:
            ss << (0xFF&(addr >> 24)) << "." << (0xFF&(addr >> 16)) << "." <<  (0xFF&(addr >> 8));
            break;
        case 4:
            ss << (0xFF&(addr >> 24)) << "." << (0xFF&(addr >> 16)) << "." <<  (0xFF&(addr >> 8)) << "." << (0xFF&(addr));
            break;
    }
    return ss.str();
}

CNetInfoBindProg::CNetInfoBindProg()
    : CRPCProg(PROG_NETINFOBIND, 1, "nibindd")
    , m_Local("local")
    , m_Network("network")
{
    #define RPC_PROG_CLASS  CNetInfoBindProg
    
    SET_PROC(1, REGISTER);
    SET_PROC(2, UNREGISTER);
    SET_PROC(3, GETREGISTER);
    SET_PROC(4, LISTREG);
    SET_PROC(5, CREATEMASTER);
    SET_PROC(6, CREATECLONE);
    SET_PROC(7, DESTROYDOMAIN);
    SET_PROC(8, BIND);
    
    add_rpc_program(&m_Local);
    doRegister(m_Local.mTag, m_Local.getPortUDP(), m_Local.getPortTCP());
    
    add_rpc_program(&m_Network);
    doRegister(m_Network.mTag, m_Network.getPortUDP(), m_Network.getPortTCP());
    
    m_Network.mRoot.add("master", NAME_NFSD "/network");
    m_Network.mRoot.add("trusted_networks", ip_addr_str(CTL_NET, 3));
    NetInfoNode* machines   = m_Network.mRoot.add(NIProps("name","machines"));
    machines->add(NIProps("name",NAME_NFSD)("ip_address",ip_addr_str(CTL_NET|CTL_NFSD, 4))("serves","./network,../network,"NAME_NFSD"/local"));
    machines->add(NIProps("name",NAME_HOST)("ip_address",ip_addr_str(CTL_NET|CTL_HOST, 4))("serves",NAME_HOST"/local")("netgroups",""));
                  
    NetInfoNode* printers   = m_Network.mRoot.add(NIProps("name","printers"));
    NetInfoNode* fax_modems = m_Network.mRoot.add(NIProps("name","fax_modems"));
    NetInfoNode* aliases    = m_Network.mRoot.add(NIProps("name","aliases"));
    NetInfoNode* groups     = m_Network.mRoot.add(NIProps("name","groups"));
    groups->add(NIProps("name","wheel")   ("gid","0") ("passwd","*")("users","root,me"));
    groups->add(NIProps("name","nogroup") ("gid","-2")("passwd","*"));
    groups->add(NIProps("name","daemon")  ("gid","1") ("passwd","*")("users","daemon"));
    groups->add(NIProps("name","sys")     ("gid","2") ("passwd","*"));
    groups->add(NIProps("name","bin")     ("gid","3") ("passwd","*"));
    groups->add(NIProps("name","uucp")    ("gid","4") ("passwd","*"));
    groups->add(NIProps("name","kmem")    ("gid","5") ("passwd","*"));
    groups->add(NIProps("name","news")    ("gid","6") ("passwd","*"));
    groups->add(NIProps("name","ingres")  ("gid","7") ("passwd","*"));
    groups->add(NIProps("name","tty")     ("gid","8") ("passwd","*"));
    groups->add(NIProps("name","operator")("gid","9") ("passwd","*"));
    groups->add(NIProps("name","staff")   ("gid","10")("passwd","*")("users","root,me"));
    groups->add(NIProps("name","other")   ("gid","20")("passwd","*"));
                  
    NetInfoNode* mounts     = m_Network.mRoot.add(NIProps("name","mounts"));
    NetInfoNode* users     = m_Network.mRoot.add(NIProps("name","users"));
    NetInfoNode* user_root =users->add(NIProps("name","root")("_writers_passwd","root")("gid","1")("home","/")("passwd","")("realname","Operator")("shell","/bin/csh")("uid","0"));
    user_root->add(NIProps("name","info")("_writers","root"));
}
                                       
CNetInfoBindProg::~CNetInfoBindProg() {}
 
void CNetInfoBindProg::doRegister(const std::string& tag, uint32_t udpPort, uint32_t tcpPort) {
    m_Register[tag].tcp_port = tcpPort;
    m_Register[tag].udp_port = udpPort;
    
    log("%s tcp:%d udp:%d", tag.c_str(), tcpPort, udpPort);
}

int CNetInfoBindProg::procedureREGISTER() {
    return PRC_NOTIMP;
}
int CNetInfoBindProg::procedureUNREGISTER() {
    return PRC_NOTIMP;
}
int CNetInfoBindProg::procedureGETREGISTER() {
    XDRString tag;
    m_in->read(tag);
    
    std::map<std::string, nibind_addrinfo>::iterator it = m_Register.find(tag.c_str());
    
    if(it == m_Register.end())
        m_out->write(NI_NOTAG);
    else {
        m_out->write(NI_OK);
        m_out->write(it->second.udp_port);
        m_out->write(it->second.tcp_port);
    }

    return PRC_OK;
}
int CNetInfoBindProg::procedureLISTREG() {
    return PRC_NOTIMP;
}
int CNetInfoBindProg::procedureCREATEMASTER() {
    return PRC_NOTIMP;
}
int CNetInfoBindProg::procedureCREATECLONE() {
    return PRC_NOTIMP;
}
int CNetInfoBindProg::procedureDESTROYDOMAIN() {
    return PRC_NOTIMP;
}
int CNetInfoBindProg::procedureBIND() {
    uint32_t  clientAddr;
    XDRString clientTag;
    XDRString serverTag;
    
    m_in->read(&clientAddr);
    m_in->read(clientTag);
    m_in->read(serverTag);

    m_out->write(NI_OK);

    return PRC_OK;
}
