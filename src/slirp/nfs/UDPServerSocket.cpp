#include <sys/socket.h>

#include "UDPServerSocket.h"
#include "nfsd.h"

UDPServerSocket::UDPServerSocket(ISocketListener *pListener) : m_nPort(0), m_Socket(0), m_pSocket(NULL), m_bClosed(true), m_pListener(pListener) {}

UDPServerSocket::~UDPServerSocket() {
	close();
}

bool UDPServerSocket::open(int progNum, uint16_t nPort) {
	struct sockaddr_in localAddr;

	close();

    m_Socket = socket(AF_INET, SOCK_DGRAM, 0);
    if (m_Socket == INVALID_SOCKET)
        return false;
    
    socklen_t size = 64 * 1024;
    socklen_t len  = sizeof(size);
    setsockopt(m_Socket, SOL_SOCKET, SO_SNDBUF, &size, len);
    setsockopt(m_Socket, SOL_SOCKET, SO_RCVBUF, &size, len);
    memset(&localAddr, 0, sizeof(localAddr));
    localAddr.sin_family = AF_INET;
    localAddr.sin_port = CSocket::map_and_htons(SOCK_DGRAM, nPort);
    localAddr.sin_addr = loopback_addr;
    if (bind(m_Socket, (struct sockaddr *)&localAddr, sizeof(struct sockaddr)) < 0) {
        ::close(m_Socket);
        return false;
    }
    
    size = sizeof(localAddr);
    if(getsockname(m_Socket, (struct sockaddr *)&localAddr, &size) < 0) {
        ::close(m_Socket);
        return false;
    }
    
    m_nPort = nPort == 0 ? ntohs(localAddr.sin_port) : nPort;
    CSocket::map_port(SOCK_DGRAM, progNum, nPort, ntohs(localAddr.sin_port));

	m_bClosed = false;
	m_pSocket = new CSocket(SOCK_DGRAM, getPort());
	m_pSocket->open(m_Socket, m_pListener);  //wait for receiving data
	return true;
}

void UDPServerSocket::close(void)
{
	if (m_bClosed)
		return;

	m_bClosed = true;
	::close(m_Socket);
    
    if      (m_nPort == PORT_DNS)             nfsd_ports.udp.dns     = 0;
    else if (m_nPort == PORT_PORTMAP)         nfsd_ports.udp.portmap = 0;
    else if (m_nPort == nfsd_ports.udp.mount) nfsd_ports.udp.mount   = 0;
    else if (m_nPort == PORT_NFS)             nfsd_ports.udp.nfs     = 0;
    
	delete m_pSocket;
}

int UDPServerSocket::getPort(void) {
	return m_nPort;
}
