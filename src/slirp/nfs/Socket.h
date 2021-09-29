#ifndef _SOCKET_H_
#define _SOCKET_H_

#include <sys/socket.h>
#include <netinet/in.h>
#include <unistd.h>

#include "SocketListener.h"
#include "XDRStream.h"
#include "host.h"
#include "slirpmain.h"

class CSocket
{
public:
	CSocket(int nType, int serverPort);
	virtual       ~CSocket();
	int            getType(void);
	void           open(int socket, ISocketListener *pListener, struct sockaddr_in *pRemoteAddr = NULL);
	void           close(void);
	void           send(void);
	bool           active(void);
	const char*    getRemoteAddress(void);
	int            getRemotePort(void);
	XDRInput*      getInputStream(void);
	XDROutput*     getOutputStream(void);
	void           run(void);
    int            getServerPort(void);
    
    static uint16_t map_and_htons(int type, uint16_t port);
    static void     map_port(int type, int progNum, uint16_t origPort, uint16_t port);
    
private:
	int                m_nType;
	int                m_Socket;
	struct sockaddr_in m_RemoteAddr;
    ISocketListener*   m_pListener;
	bool               m_bActive;
	thread_t*          m_hThread;
    XDRInput           m_Input;
    XDROutput          m_Output;
    int                m_serverPort;
};

const int INVALID_SOCKET = -1;

#endif
