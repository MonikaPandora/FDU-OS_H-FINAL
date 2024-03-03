#include <common/socket.h>
#include <kernel/mem.h>
#include <fs/file.h>
#include <common/string.h>
#include <kernel/printk.h>

// socket buf operations
bool init_buf(struct socket_buf* buf){
    buf->rcv_buf = kalloc(sizeof(struct socket_rcvbuf));
    buf->snd_buf = kalloc(sizeof(struct socket_sndbuf));
    if(buf->rcv_buf == NULL || buf->snd_buf == NULL){
        if(buf->rcv_buf)kfree(buf->rcv_buf);
        return false;
    }

    memset(buf->rcv_buf, 0, sizeof(struct socket_rcvbuf));
    memset(buf->snd_buf, 0, sizeof(struct socket_sndbuf));
    init_sem(&buf->rcv_buf->r_sem, 0);
    init_sem(&buf->rcv_buf->w_sem, 1);
    init_sem(&buf->snd_buf->r_sem, 0);
    init_sem(&buf->snd_buf->w_sem, 1);

    for(int i = 0; i < SOCKET_RCVBUF_PAGENO; i++){
        buf->rcv_buf->pages[i] = (u64)kalloc_page();
        if(buf->rcv_buf->pages[i] == 0){
            goto free;
        }
    }

    for(int i = 0; i < SOCKET_SNDBUF_PAGENO; i++){
        buf->snd_buf->pages[i] = (u64)kalloc_page();
        if(buf->snd_buf->pages[i] == 0){
            goto free;
        }
    }
    return true;
free:
    free_buf(buf);
    return false;
}

void free_buf(struct socket_buf* buf){
    for(int i = 0; i < SOCKET_RCVBUF_PAGENO; i++){
        if(buf->rcv_buf->pages[i]){
            kfree_page((void*)buf->rcv_buf->pages[i]);
        }
    }

    for(int i = 0; i < SOCKET_SNDBUF_PAGENO; i++){
        if(buf->snd_buf->pages[i]){
            kfree_page((void*)buf->snd_buf->pages[i]);
        }
    }

    kfree(buf);
}

int buf_read(struct socket_buf* buf, u64 addr, usize size, bool send){
    int page_no = send ? SOCKET_SNDBUF_PAGENO: SOCKET_RCVBUF_PAGENO;
    u64* pages = send ? buf->snd_buf->pages : buf->rcv_buf->pages;
    u64* r = send ? &buf->snd_buf->r : &buf->rcv_buf->r;
    u64* w = send ? &buf->snd_buf->w : &buf->rcv_buf->w;
    SpinLock* lock = send ? &buf->snd_buf->lock : &buf->rcv_buf->lock;
    Semaphore* r_sem = send ? &buf->snd_buf->r_sem : &buf->rcv_buf->r_sem;
    Semaphore* w_sem = send ? &buf->snd_buf->w_sem : &buf->rcv_buf->w_sem;

    _acquire_spinlock(lock);
    while(*w == *r){
        _release_spinlock(lock);
        if(_wait_sem(r_sem, true) == false){
            return -1;
        }
        _acquire_spinlock(lock);
    }

    usize ret = 0;
    while(ret < size){
        if(*w == *r)break;
        auto page_idx = (*r / PAGE_SIZE) % page_no;
        auto in_page_data_idx = *r % PAGE_SIZE;
        (*r)++;
        ((char*)addr)[ret++] = ((char*)pages[page_idx])[in_page_data_idx];
    }
    post_all_sem(w_sem);
    _release_spinlock(lock);
    return ret;
}

int buf_write(struct socket_buf* buf, u64 addr, usize size, bool send){
    int page_no = send ? SOCKET_SNDBUF_PAGENO: SOCKET_RCVBUF_PAGENO;
    u64* pages = send ? buf->snd_buf->pages : buf->rcv_buf->pages;
    u64* r = send ? &buf->snd_buf->r : &buf->rcv_buf->r;
    u64* w = send ? &buf->snd_buf->w : &buf->rcv_buf->w;
    SpinLock* lock = send ? &buf->snd_buf->lock : &buf->rcv_buf->lock;
    Semaphore* r_sem = send ? &buf->snd_buf->r_sem : &buf->rcv_buf->r_sem;
    Semaphore* w_sem = send ? &buf->snd_buf->w_sem : &buf->rcv_buf->w_sem;

    _acquire_spinlock(lock);

    usize ret = 0;
    while(ret < size){
        if(*w - *r >= (u64)(PAGE_SIZE * page_no)){
            post_all_sem(r_sem);
            _release_spinlock(lock);
            if(!_wait_sem(w_sem, true)){
                return ret;
            }
            _acquire_spinlock(lock);
        }
        else{
            auto page_idx = (*w / PAGE_SIZE) % page_no;
            auto in_page_data_idx = *w % PAGE_SIZE;
            (*w)++;
            ((char*)pages[page_idx])[in_page_data_idx] = ((char*)addr)[ret++];
        }
    }
    post_all_sem(r_sem);
    _release_spinlock(lock);
    return ret;
}


// BSD socket operations
int socket(int family, int type, int protocal){
    ASSERT(family == AF_INET);
    ASSERT(1 <= type&& type <= 3);  // only SOCK_STREAM, SOCK_DGRAM, SOCK_RAW
    ASSERT(protocal == 0);  // only auto select

    auto sk = (struct socket*)kalloc(sizeof(struct socket));
    if(!sk)return -1;
    memset(sk, 0, sizeof(struct socket));
    sk->type = type;
    sk->proto = type == SOCK_DGRAM ? IPPROTO_UDP : IPPROTO_TCP;
    sk->s_addr.family = family;

    sk->fp = file_alloc();
    if(!sk->fp)goto bad;
    auto ret = fdalloc(sk->fp);
    if(ret == -1)goto bad;

    sk->fp->type = FD_SOCKET;
    sk->fp->socket = sk;
    sk->fp->sbuf = (struct socket_buf*)kalloc(sizeof(struct socket_buf));
    if(!sk->fp->sbuf)goto bad;
    if(!init_buf(sk->fp->sbuf))goto bad;

    queue_init(&sk->rq);
    init_sem(&sk->wait_for_connect, 0);
    init_sem(&sk->wait_for_exit, 0);
    return ret;
bad:
    if(!sk)return -1;
    if(sk->fp){
        file_close(sk->fp);
        if(sk->fp->sbuf)kfree(sk->fp->sbuf);
    }
    kfree(sk);
    return -1;
}

struct socket* sd2socket(int sd){
    auto fp = fd2file(sd);
    if(!fp || fp->type != FD_SOCKET)return NULL;
    return fp->socket;
}

struct socket* port2socket[65536];
static SpinLock lock;

int alloc_port(struct socket* sk, const u16* spec_port){
    int ret = -1;
    if(spec_port){
        _acquire_spinlock(&lock);
        if(port2socket[*spec_port] == NULL){
            port2socket[*spec_port] = sk;
            ret = *spec_port;
        }
    }
    else {
        for(int i = 0; i < 65536; i++){
            if(port2socket[i] == NULL){
                port2socket[i] = sk;
                ret = i;
                break;
            }
        }
    }
    _release_spinlock(&lock);
    return ret;
}

bool free_port(u16 port){
    _acquire_spinlock(&lock);
    if(port2socket[port] == NULL){
        _release_spinlock(&lock);
        return false;
    }
    else {
        port2socket[port] = NULL;
        _release_spinlock(&lock);
        return true;
    }
}

int bind(int sd, const struct inet_addr* addr, int addrlen){
    auto sk = sd2socket(sd);
    if(!sk)return -1;
    if(addr){
        if(alloc_port(sk, &addr->port) == -1)return -1;  // port in use
        if(addrlen != sizeof(*(sk->s_addr.in_addr)))return -1;
        if(sk->s_addr.in_addr)free_port(sk->s_addr.in_addr->port);
        else sk->s_addr.in_addr = kalloc(sizeof(struct inet_addr));
        if(sk->s_addr.in_addr == NULL){
            free_port(addr->port);
            return -1;
        }
        memcpy((void*)sk->s_addr.in_addr, (void*)addr, addrlen);
    }
    else if(sk->s_addr.in_addr == NULL){
        sk->s_addr.in_addr = kalloc(sizeof(struct inet_addr));
        struct inet_addr in_addr;
        in_addr.addr = LOCAL_IP;
        auto port = alloc_port(sk, NULL);
        if(port < 0){
            kfree(sk->s_addr.in_addr);
            return -1;
        }
        in_addr.port = port;
        memcpy((void*)sk->s_addr.in_addr, (void*)&in_addr, sizeof(struct inet_addr));
    }
    return 0;
}

int accept(int sd, struct inet_addr* addr, int* addrlen){
    auto sk = sd2socket(sd);
    ASSERT(sk);
    if(sk->type != SOCK_STREAM && sk->type != SOCK_DGRAM)return -1;

    if(!sk->listening){
        printk("accept: try to accept on a non-listening socket\n");
        return -1;
    }

    int new = socket(sk->s_addr.family, sk->type, 0);
    if(new == -1)return -1;

    queue_lock(&sk->rq);
    if(queue_empty(&sk->rq)){
        queue_unlock(&sk->rq);
        if(_wait_sem(&sk->wait_for_connect, true) == false)return -1;
        queue_lock(&sk->rq);
    }
    auto req_sk = container_of(queue_front(&sk->rq), struct socket, rq_node);
    queue_pop(&sk->rq);
    queue_unlock(&sk->rq);

    if(addr){
        memcpy((void*)addr, (void*)req_sk->s_addr.in_addr, sizeof(struct inet_addr));
    }
    if(addrlen)*addrlen = sizeof(struct inet_addr);
    
    bind(new, NULL, sizeof(struct inet_addr));
    auto new_sk = sd2socket(new);
    new_sk->s_addr.connect_to = kalloc(sizeof(struct inet_addr));
    new_sk->s_addr.connect_to->addr = req_sk->s_addr.in_addr->addr;
    new_sk->s_addr.connect_to->port = req_sk->s_addr.in_addr->port;
    memcpy((void*)req_sk->s_addr.connect_to, (void*)new_sk->s_addr.in_addr, sizeof(struct inet_addr));
    post_sem(&req_sk->wait_for_connect);
    return new;
}

int closesocket(int sd){
    auto sk = sd2socket(sd);
    if(!sk)return 0;

    if(sk->s_addr.connect_to){
        post_sem(&port2socket[sk->s_addr.connect_to->port]->wait_for_exit);
        unalertable_wait_sem(&sk->wait_for_exit);
    }

    if(sk->s_addr.in_addr){
        free_port(sk->s_addr.in_addr->port);
        kfree(sk->s_addr.in_addr);
    }
    if(sk->s_addr.connect_to){
        kfree(sk->s_addr.connect_to);
    }
    if(sk->fp){
        if(sk->fp->sbuf)free_buf(sk->fp->sbuf);
        file_close(sk->fp);
    }
    kfree(sk);
    return 0;
}

int connect(int sd, struct inet_addr* addr, int addrlen){
    ASSERT(addrlen == sizeof(struct inet_addr));
    auto sk = sd2socket(sd);
    if(!sk)return -1;
    if(sk->type == SOCK_STREAM){
        // establish TCP connection here
        // TODO
        printk("connect: SOCKE_STREAM is not supported");
        return -1;
    }
    else if(sk->type == SOCK_DGRAM){
        // add address filter
        // my implementation is assigning the connect_to address
        if(sk->s_addr.connect_to){
            printk("connect: can not connect to mutilple addresses");
            return -1;
        }
        sk->s_addr.connect_to = kalloc(sizeof(struct inet_addr));
        if(sk->s_addr.connect_to == NULL){
            printk("connect: no more memory");
            return -1;
        }
        memcpy((void*)sk->s_addr.connect_to, (void*)addr, sizeof(struct inet_addr));
        auto aim_sk = port2socket[addr->port];
        queue_lock(&aim_sk->rq);
        queue_push(&aim_sk->rq, &sk->rq_node);
        queue_unlock(&aim_sk->rq);
        post_sem(&aim_sk->wait_for_connect);
        unalertable_wait_sem(&sk->wait_for_connect);
        return 0;
    }
    else{
        PANIC();
    }
}

int listen(int sd, int backlog){
    (void)backlog;
    auto sk = sd2socket(sd);
    if(!sk)return -1;
    sk->listening = true;
    return 0;
}

int recv(int sd, char* dest, int len, int flags){
    (void)flags;    // now only support blocking mode
    auto sk = sd2socket(sd);
    if(!sk)return -1;
    return buf_read(sk->fp->sbuf, (u64)dest, (usize)len, false);
}

int recvfrom(int sd, char* dest, int len, int flags, struct inet_addr* from, int* fromlen){
    // TODO
    (void)sd;
    (void)dest;
    (void)len;
    (void)flags;
    (void)from;
    (void)fromlen;
    return -1;
}

int send(int sd, const char* src, int len, int flags){
    (void)flags;    // now only support blocking mode
    auto sk = sd2socket(sd);
    if(sk->s_addr.connect_to == NULL){
        printk("send data before connect");
        return -1;
    }
    if(!sk)return -1;
    // auto ret = buf_write(sk->fp->sbuf, (u64)src, (usize)len, true);

    // simulate loopback socket
    auto aim_sk = port2socket[sk->s_addr.connect_to->port];
    auto ret = buf_write(aim_sk->fp->sbuf, (u64)src, (usize)len, false);
    return ret;
}

int sendto(int sd, const char* src, int len, int flags, struct inet_addr* to, int* tolen){
    // TODO
    (void)sd;
    (void)src;
    (void)len;
    (void)flags;
    (void)to;
    (void)tolen;
    return -1;
}