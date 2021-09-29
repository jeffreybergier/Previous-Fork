#ifndef _SERVERSOCKET_H_
#define _SERVERSOCKET_H_

#include "SocketListener.h"
#include "Socket.h"

class TCPServerSocket{
public:
	TCPServerSocket(ISocketListener* pListener);
	~TCPServerSocket();
	bool open(int progNum, uint16_t port = 0);
	void close(void);
	int  getPort(void);
	void run(void);

private:
	uint16_t         m_nPort;
	int              m_ServerSocket;
	bool             m_bClosed;
	ISocketListener *m_pListener;
	thread_t*        m_hThread;
	CSocket**        m_pSockets;
};

#endif
