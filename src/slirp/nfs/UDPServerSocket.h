#ifndef _DATAGRAMSOCKET_H_
#define _DATAGRAMSOCKET_H_

#include "SocketListener.h"
#include "Socket.h"

class UDPServerSocket
{
public:
	UDPServerSocket(ISocketListener* pListener);
	~UDPServerSocket();
	bool open(int porgNum, uint16_t nPort = 0);
	void close(void);
	int  getPort(void);
	void run(void);

private:
	int              m_nPort;
	int              m_Socket;
	CSocket*         m_pSocket;
	bool             m_bClosed;
	ISocketListener* m_pListener;
};

#endif
