/*
** tcc/winsock2.h - the part of Winsock mme uses (the debugger's TCP
** connection to an adapter), for tcc 0.9.27, which has no winsock2.h.
*/
#ifndef MME_TCC_WINSOCK2_H
#define MME_TCC_WINSOCK2_H
#include <windows.h>

typedef UINT_PTR SOCKET;
typedef unsigned short u_short;
typedef unsigned long u_long;

#define INVALID_SOCKET	((SOCKET)~0)
#define SOCKET_ERROR	(-1)
#define AF_INET	2
#define SOCK_STREAM	1
#define IPPROTO_TCP	6
#define FD_SETSIZE	64

typedef struct WSAData {
  WORD wVersion;
  WORD wHighVersion;
#ifdef _WIN64
  unsigned short iMaxSockets;
  unsigned short iMaxUdpDg;
  char *lpVendorInfo;
  char szDescription[257];
  char szSystemStatus[129];
#else
  char szDescription[257];
  char szSystemStatus[129];
  unsigned short iMaxSockets;
  unsigned short iMaxUdpDg;
  char *lpVendorInfo;
#endif
} WSADATA;

struct sockaddr {
  u_short sa_family;
  char sa_data[14];
};

struct in_addr {
  u_long s_addr;
};

struct sockaddr_in {
  short sin_family;
  u_short sin_port;
  struct in_addr sin_addr;
  char sin_zero[8];
};

typedef struct fd_set {
  unsigned int fd_count;
  SOCKET fd_array[FD_SETSIZE];
} fd_set;

#ifndef _TIMEVAL_DEFINED
#define _TIMEVAL_DEFINED
struct timeval {
  long tv_sec;
  long tv_usec;
};
#endif

#define FD_ZERO(set)	(((fd_set *)(set))->fd_count = 0)
#define FD_SET(fd, set)	do { \
    if (((fd_set *)(set))->fd_count < FD_SETSIZE) \
      ((fd_set *)(set))->fd_array[((fd_set *)(set))->fd_count++] = (fd); \
  } while (0)

int WINAPI WSAStartup (WORD version, WSADATA *data);
int WINAPI WSACleanup (void);
SOCKET WINAPI socket (int af, int type, int protocol);
int WINAPI connect (SOCKET s, const struct sockaddr *name, int namelen);
int WINAPI closesocket (SOCKET s);
int WINAPI select (int nfds, fd_set *r, fd_set *w, fd_set *e, const struct timeval *timeout);
int WINAPI send (SOCKET s, const char *buf, int len, int flags);
int WINAPI recv (SOCKET s, char *buf, int len, int flags);
u_short WINAPI htons (u_short v);
u_long WINAPI htonl (u_long v);

#endif
