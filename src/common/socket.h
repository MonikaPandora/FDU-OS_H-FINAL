#pragma once

#include <common/sem.h>
#include <fs/file.h>
#include <common/list.h>
#include <common/defines.h>
#include <common/spinlock.h>

#define PF_NONE 0
#define PF_LOCAL 1
#define PF_INET 2

#define AF_NONE PF_NONE
#define AF_LOCAL PF_LOCAL
#define AF_INET PF_INET

#define SOCK_STREAM 1
#define SOCK_DGRAM 2
#define SOCK_RAW 3

#define IPPROTO_TCP 6
#define IPPROTO_UDP 17

#define SOCKET_RCVBUF_PAGENO 2
#define SOCKET_SNDBUF_PAGENO 2
#define LOCAL_IP ((127 << 24) | (1))


struct inet_addr{
    u32 addr;
    u16 port;
};

struct socket_addr{
    u32 family;
    union{struct inet_addr* in_addr;};
    union{struct inet_addr* connect_to;};
};

struct socket_rcvbuf{
    u64 pages[SOCKET_RCVBUF_PAGENO];
    u64 r;
    u64 w;
    SpinLock lock;
    Semaphore w_sem;
    Semaphore r_sem;
};

struct socket_sndbuf{
    u64 pages[SOCKET_SNDBUF_PAGENO];
    u64 r;
    u64 w;
    SpinLock lock;
    Semaphore w_sem;
    Semaphore r_sem;
};

struct socket_buf{
    struct socket_rcvbuf* rcv_buf;
    struct socket_sndbuf* snd_buf;
};

bool init_buf(struct socket_buf* buf);
void free_buf(struct socket_buf* buf);
int buf_read(struct socket_buf* buf, u64 addr, usize size, bool send);
int buf_write(struct socket_buf* buf, u64 addr, usize size, bool send);

struct socket{
    int type;
    int proto;
    struct file* fp;
    struct socket_addr s_addr;
    Queue rq;
    ListNode rq_node;
    Semaphore wait_for_connect;
    Semaphore wait_for_exit;
    bool listening;
};

struct socket* sd2socket(int sd);
int socket(int family, int type, int protocal);
int bind(int sd, const struct inet_addr* addr, int addrlen);
int accept(int sd, struct inet_addr* addr, int* addrlen);
int closesocket(int sd);
int connect(int sd, struct inet_addr* addr, int addrlen);
int listen(int sd, int backlog);
int recv(int sd, char* dest, int len, int flags);
int recvfrom(int sd, char* dest, int len, int flags, struct inet_addr* from, int* fromlen);
int send(int sd, const char* src, int len, int flags);
int sendto(int sd, const char* src, int len, int flags, struct inet_addr* to, int* tolen);
