//
//  NetInfo.cpp
//  Slirp
//
//  Created by Simon Schubiger on 9/1/21.
//

#include "nfsd.h"
#include "NetInfoProg.h"
#include <sstream>

using namespace std;

CNetInfoProg::CNetInfoProg(const string& tag)
    : CRPCProg(PROG_NETINFO, 2, string("netinfod:" + tag))
    , mTag(tag)
    , mRoot(mIdMap, nullptr)
{
    #define RPC_PROG_CLASS  CNetInfoProg
    
    SET_PROC(1, STATISTICS);
    SET_PROC(2, ROOT);
    SET_PROC(3, SELF);
    SET_PROC(4, PARENT);
    SET_PROC(5, CREATE);
    SET_PROC(6, DESTROY);
    SET_PROC(7, READ);
    SET_PROC(8, WRITE);
    SET_PROC(9, CHILDREN);
    SET_PROC(10, LOOKUP);
    SET_PROC(11, LIST);
    SET_PROC(12, CREATEPROP);
    SET_PROC(13, DESTROYPROP);
    SET_PROC(14, READPROP);
    SET_PROC(15, WRITEPROP);
    SET_PROC(16, RENAMEPROP);
    SET_PROC(17, LISTPROPS);
    SET_PROC(18, CREATENAME);
    SET_PROC(19, DESTROYNAME);
    SET_PROC(20, READNAME);
    SET_PROC(21, WRITENAME);
    SET_PROC(22, RPARENT);
    SET_PROC(23, LISTALL);
    SET_PROC(24, BIND);
    SET_PROC(25, READALL);
    SET_PROC(26, CRASHED);
    SET_PROC(27, RESYNC);
    SET_PROC(28, LOOKUPREAD);
}

CNetInfoProg::~CNetInfoProg() {}
 
int CNetInfoProg::procedureSTATISTICS() {
    return PRC_NOTIMP;
}

static void read(XDRInput* in, ni_id& ni_id) {
    in->read(&ni_id.object);
    in->read(&ni_id.instance);
}

static void write(XDROutput* out, const ni_id& ni_id) {
    out->write(ni_id.object);
    out->write(ni_id.instance);
}

int CNetInfoProg::procedureROOT() {
    m_out->write(NI_OK);
    write(m_out, mRoot.mId);
    
    return PRC_OK;
}

int CNetInfoProg::procedureSELF() {
    ni_id      ni_id;
    read(m_in, ni_id);
    
    ni_status status(NI_OK);
    NetInfoNode* node = mRoot.find(ni_id, status);
    m_out->write(status);
    if(status == NI_OK)
        write(m_out, ni_id);
    return PRC_OK;
}

int CNetInfoProg::procedurePARENT() {
    ni_id      ni_id;
    read(m_in, ni_id);
    
    ni_status status(NI_OK);
    NetInfoNode* node = mRoot.find(ni_id, status);
    if(node->mParent == nullptr)
        status = NI_NETROOT;
        
    m_out->write(status);
    if(status == NI_OK) {
        m_out->write(node->mParent->mId.object);
        write(m_out, ni_id);
    }
    return PRC_OK;
    
}

int CNetInfoProg::procedureCREATE() {
    return PRC_NOTIMP;
}
int CNetInfoProg::procedureDESTROY() {
    return PRC_NOTIMP;
}
int CNetInfoProg::procedureREAD() {
    ni_id     ni_id;
    
    read(m_in, ni_id);

    ni_status status;
    NetInfoNode* node = mRoot.find(ni_id, status);
    m_out->write(status);
    if(status == NI_OK) {
        write(m_out, ni_id);
        m_out->write(node->mProps.size());
        for (map<string, string>::iterator it = node->mProps.begin(); it != node->mProps.end(); it++) {
            m_out->write(it->first);
            vector<string> props = node->getProps(it->first);
            m_out->write(props.size());
            for(size_t i = 0; i < props.size(); i++)
                m_out->write(props[i]);
        }
    }
    return PRC_OK;
}
int CNetInfoProg::procedureWRITE() {
    return PRC_NOTIMP;
}

int CNetInfoProg::procedureCHILDREN() {
    ni_id     ni_id;
    
    read(m_in, ni_id);

    ni_status status(NI_OK);
    NetInfoNode* node = mRoot.find(ni_id, status);
    m_out->write(status);
    if(status == NI_OK) {
        m_out->write(node->mChildren.size());
        for(size_t i = 0; i < node->mChildren.size(); i++)
            m_out->write(node->mChildren[i]->mId.object);
        
        write(m_out, ni_id);
    }
    return PRC_OK;
}

int CNetInfoProg::procedureLOOKUP() {
    ni_id     ni_id;
    XDRString key;
    XDRString value;

    read(m_in, ni_id);
    m_in->read(key);
    m_in->read(value);

    ni_status status(NI_OK);
    NetInfoNode* node = mRoot.find(ni_id, status);
    m_out->write(status);
    if(status == NI_OK) {
        vector<NetInfoNode*> nodes = node->find(key, value);
        m_out->write(nodes.size());
        for(size_t i = 0; i < nodes.size(); i++)
            m_out->write(nodes[i]->mId.object);
        write(m_out, ni_id);
    }
    return PRC_OK;
}

int CNetInfoProg::procedureLIST() {
    ni_id     ni_id;
    XDRString name;
    
    read(m_in, ni_id);
    m_in->read(name);

    ni_status status(NI_OK);
    NetInfoNode* node = mRoot.find(ni_id, status);
    m_out->write(status);
    if(status == NI_OK) {
        m_out->write(node->mChildren.size());
        for(size_t i = 0; i < node->mChildren.size(); i++) {
            m_out->write(node->mChildren[i]->mId.object);
            vector<string> props = node->mChildren[i]->getProps(name);
            if(props.empty())
                m_out->write(0);
            else {
                m_out->write(1);
                m_out->write(props.size());
                for(size_t i = 0; i < props.size(); i++)
                    m_out->write(props[i]);
            }
        }
        
        write(m_out, ni_id);
    }
    return PRC_OK;
}

int CNetInfoProg::procedureCREATEPROP() {
    return PRC_NOTIMP;
}
int CNetInfoProg::procedureDESTROYPROP() {
    return PRC_NOTIMP;
}
int CNetInfoProg::procedureREADPROP() {
    ni_id     ni_id;
    uint32_t  index;
    
    read(m_in, ni_id);
    m_in->read(&index);
    
    ni_status status;
    NetInfoNode* node = mRoot.find(ni_id, status);
    vector<string> props;
    if(node)
        props = node->getProps(index);
    
    if(node && props.empty())
        status = NI_NOPROP;

    m_out->write(status);

    if(status == NI_OK) {
        m_out->write(props.size());
        for (size_t i = 0; i < props.size(); i++)
            m_out->write(props[i]);
        
        write(m_out, ni_id);
    }
return PRC_OK;
}

int CNetInfoProg::procedureWRITEPROP() {
    return PRC_NOTIMP;
}
int CNetInfoProg::procedureRENAMEPROP() {
    return PRC_NOTIMP;
}
int CNetInfoProg::procedureLISTPROPS() {
    ni_id     ni_id;
    
    read(m_in, ni_id);

    ni_status status;
    NetInfoNode* node = mRoot.find(ni_id, status);
    m_out->write(status);
    if(status == NI_OK) {
        m_out->write(node->mProps.size());
        for (map<string, string>::iterator it = node->mProps.begin(); it != node->mProps.end(); it++)
            m_out->write(it->first);
        
        write(m_out, ni_id);
    }
    return PRC_OK;
}

int CNetInfoProg::procedureCREATENAME() {
    return PRC_NOTIMP;
}
int CNetInfoProg::procedureDESTROYNAME() {
    return PRC_NOTIMP;
}
int CNetInfoProg::procedureREADNAME() {
    return PRC_NOTIMP;
}
int CNetInfoProg::procedureWRITENAME() {
    return PRC_NOTIMP;
}
int CNetInfoProg::procedureRPARENT() {
    m_out->write(NI_NETROOT);
    return PRC_OK;
}
int CNetInfoProg::procedureLISTALL() {
    return PRC_NOTIMP;
}
int CNetInfoProg::procedureBIND() {
    return PRC_NOTIMP;
}
int CNetInfoProg::procedureREADALL() {
    return PRC_NOTIMP;
}
int CNetInfoProg::procedureCRASHED() {
    return PRC_NOTIMP;
}
int CNetInfoProg::procedureRESYNC() {
    return PRC_NOTIMP;
}
int CNetInfoProg::procedureLOOKUPREAD() {
    return PRC_NOTIMP;
}

NetInfoNode* NetInfoNode::add(const map<string, string>& props) {
    NetInfoNode* result = new NetInfoNode(mIdmap, this, props);
    mChildren.push_back(result);
    return result;
}

void NetInfoNode::add(const string& key, const string& value) {
    mProps[key] = value;
}

NetInfoNode* NetInfoNode::find(struct ni_id& ni_id, ni_status& status, bool forWrite) const {
    map<uint32_t,NetInfoNode*>::const_iterator it = mIdmap.find(ni_id.object);
    if(it == mIdmap.end()) {
        status = NI_BADID;
        return nullptr;
    }
    NetInfoNode* result = it->second;
    status = (!(forWrite) || (result->mId.instance == ni_id.instance)) ? NI_OK : NI_STALE;
    ni_id.instance = result->mId.instance;
    return result;
}

vector<NetInfoNode*> NetInfoNode::find(const string& key, const string& value) const {
    vector<NetInfoNode*> result;
    for(size_t i = 0; i < mChildren.size(); i++) {
        if(mChildren[i]->mProps.find(key) != mChildren[i]->mProps.end() &&
           mChildren[i]->mProps[key] == value)
            result.push_back(mChildren[i]);
    }
    return result;
}

static void split_value(const string& value, vector<string>& result) {
    stringstream ss(value);
    std::string prop;

    while (getline (ss, prop, ','))
        result.push_back (prop);
}

vector<string> NetInfoNode::getProps(const string& key) const {
    vector<string> result;
    map<string,string>::const_iterator it = mProps.find(key);
    if(it != mProps.end())
        split_value(it->second, result);

    return result;
}

vector<string> NetInfoNode::getProps(uint32_t index) const {
    vector<string> result;
    for(map<string,string>::const_iterator it = mProps.begin(); it != mProps.end(); it++, index--) {
        if(index == 0) {
            split_value(it->second, result);
            break;
        }
    }

    return result;
}
