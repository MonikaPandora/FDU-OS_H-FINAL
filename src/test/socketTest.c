#include <common/socket.h>
#include "test.h"
#include <kernel/proc.h>
#include <kernel/printk.h>
#include <kernel/sched.h>
#include <common/string.h>

extern struct socket* port2socket[];
void client(){
    // socket and bind
    int sd = socket(AF_INET, SOCK_DGRAM, 0);
    struct inet_addr c_addr;
    c_addr.addr = LOCAL_IP;
    c_addr.port = 40000;
    bind(sd, &c_addr, sizeof(struct inet_addr));
    auto sk = sd2socket(sd);
    ASSERT(sk->s_addr.in_addr);
    auto port = sk->s_addr.in_addr->port;
    ASSERT(port == 40000);
    ASSERT(port2socket[port] == sk);
    printk("created client: %d, bind on port: %u\n", sd, sk->s_addr.in_addr->port);
    // connect
    struct inet_addr s_addr;
    s_addr.port = 0;
    s_addr.addr = LOCAL_IP;
    connect(sd, &s_addr, sizeof(struct inet_addr));
    ASSERT(sk->s_addr.connect_to->port == 1);
    printk("client connect to server port 0\n");
    // recv and send
    char buf[512];
    recv(sd, buf, 512, 0);
    printk("client received %s\n", buf);

    char* hello = "Hello server!";
    send(sd, hello, strlen(hello) + 1, 0);
    printk("client send [%s] to server\n", hello);
    // closesocket
    closesocket(sd);
    ASSERT(port2socket[port] == NULL);
    printk("client exited\n");
    exit(0);
}

void server(){
    // socket and bind
    int sd = socket(AF_INET, SOCK_DGRAM, 0);
    bind(sd, NULL, 0);
    auto sk = sd2socket(sd);
    ASSERT(sk);
    ASSERT(sk->s_addr.in_addr);
    auto port = sk->s_addr.in_addr->port;
    ASSERT(port2socket[port] == sk);
    printk("created server: %d, bind on port: %u\n", sd, sk->s_addr.in_addr->port);
    // listen and accept
    struct inet_addr c_addr;
    int addrlen = 0;
    int new_sd = accept(sd, &c_addr, &addrlen);
    ASSERT(new_sd == -1);
    listen(sd, 1);
    new_sd = accept(sd, &c_addr, &addrlen);
    ASSERT(addrlen == sizeof(struct inet_addr));
    auto new_sk = sd2socket(new_sd);
    auto new_port = new_sk->s_addr.in_addr->port;
    ASSERT(port2socket[new_port] == new_sk);
    printk("server received connection from local port %u\n", c_addr.port);
    printk("server's new sd: %d, new port: %u\n", new_sd, new_port);
    ASSERT(new_sk->s_addr.connect_to->port == c_addr.port);
    // send and recv
    char* hello = "Hello client!";
    send(new_sd, hello, strlen(hello) + 1, 0);
    printk("server send [%s] to client\n", hello);
    char buf[512];
    recv(new_sd, buf, 512, 0);
    printk("server recv %s\n", buf);
    // closesocket
    closesocket(new_sd);
    closesocket(sd);
    ASSERT(port2socket[port] == NULL);
    printk("server exited\n");
    exit(0);
}

void socket_test_single(){
    printk("single pair socket test starting\n");
    auto s = create_proc();
    auto c = create_proc();
    start_proc(s, server, 0);

    int count = 0;
    while(count < 10){
        yield();
        count++;
    }

    start_proc(c, client, 0);

    int code;
    printk("%d exit\n", wait(&code));
    ASSERT(code != -1);
    printk("%d exit\n", wait(&code));
    ASSERT(code != -1);

    for(int i = 0; i < 65536; i++){
        ASSERT(port2socket[i] == NULL);
    }
    printk("single pair socket test pass\n");
}

void client_multi(u64 port){
    for(int i = 0; i < 2; i++){
        int sd = socket(AF_INET, SOCK_DGRAM, 0);
        struct inet_addr addr;
        addr.addr = LOCAL_IP;
        addr.port = port;
        bind(sd, &addr, sizeof(struct inet_addr));
        addr.port = 60000 - i;
        connect(sd, &addr, sizeof(struct inet_addr));
        char buf[512];
        memset(buf, port, 512);
        send(sd, buf, 512, 0);
        while(recv(sd, buf, 3, 0) != 3){
            yield();
        }
        ASSERT(strncmp(buf, "OK", 3) == 0);
        closesocket(sd);
    }
    exit(0);
}

void server_multi(u64 port){
    int sd = socket(AF_INET, SOCK_DGRAM, 0);
    struct inet_addr addr;
    addr.addr = LOCAL_IP;
    addr.port = port;
    bind(sd, &addr, sizeof(struct inet_addr));
    listen(sd, 10);
    int new_sds[10];
    struct inet_addr addrs[10];
    int addrlen;
    char buf[512];
    for(int i = 0; i < 10; i++){
        new_sds[i] = accept(sd, &addrs[i], &addrlen);
        ASSERT(new_sds[i] == i+ 1);
        ASSERT(addrlen == sizeof(struct inet_addr));
        auto sk = sd2socket(new_sds[i]);
        ASSERT(port2socket[sk->s_addr.in_addr->port] == sk);
        ASSERT(sk->s_addr.connect_to);
        recv(new_sds[i], buf, 512, 0);
        for(int j = 0; j < 512; j++){
            ASSERT(buf[j] == sk->s_addr.connect_to->port);
        }
        buf[0] = 'O';
        buf[1] = 'K';
        buf[2] = '\0';
        send(new_sds[i], buf, 3, 0);
        closesocket(new_sds[i]);
    }

    closesocket(sd);
    exit(0);
}

void socket_test_multi(){
    printk("multi pair socket test starting\n");
    int n_server = 2;
    int n_client = 10;
    int pids[12];
    for(int i = 0; i < n_server; i++){
        auto p = create_proc();
        pids[i] = start_proc(p, server_multi, 60000 - i);
    }

    int k = 0;
    while(k < 50){
        yield();
        k++;
    }

    for(int i = 0; i < n_client; i++){
        auto p = create_proc();
        pids[i + n_server] = start_proc(p, client_multi, i);
    }

    int pid;
    int code;
    int exited[12];
    memset(exited, 0, 12 * sizeof(int));
    while((pid = wait(&code)) != -1){
        ASSERT(code == 0);
        for(int j = 0; j < 13; j++){
            if(pid == pids[j]){
                exited[j] = 1;
                break;
            }
        }
    }
    for(int i = 0; i < 12; i++){
        ASSERT(exited[i]);
    }
    printk("multi pair socket test pass\n");
}

void socket_test(){
    socket_test_single();
    socket_test_multi();
}