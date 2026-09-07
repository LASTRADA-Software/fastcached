// Can WaitReadable answer ">0" with NO pending bytes? Yes, on an RST.
//
// This is the measurement #899's clause 1 asks for -- "measure it; do not read it" --
// and it decides that ticket's severity. Committed so the claim can be re-run rather
// than re-argued, which is the idiom `redis-eof-semantics.py` set.
//
// The question: `EpollSocket`'s peek arm turns any non-zero `recv(MSG_PEEK)` into a
// flat `1`, and its own comment calls a negative peek "a spurious wake-up or an error
// the caller's own Read will surface properly". `ArmDisconnect` reads that `1` as
// DATA PENDING and declines to report a departure. So: is a negative peek reachable
// without a peer that pipelines?
//
// Measured, stable over 5 runs on Linux/epoll:
//
//     epoll_wait -> EPOLLIN|EPOLLERR|EPOLLHUP  (0x19)
//     peek 1     -> -1 ECONNRESET   =>  EpollSocket completes 1  ("data pending")
//     peek 2     ->  0              =>  EpollSocket completes 0  (EOF)
//
// So YES, and it needs no pipelining peer -- an ordinary client crash does it. That
// CLOSES the exit #899's clause 2 offered ("if only reachable behind a pipelining
// peer, widen the documentation rather than the code").
//
// The empty receive buffer is load-bearing and is what the first draft got wrong:
// with a byte already sent, peek 1 returns that byte and the reading says nothing
// about the error path. The RST must land on an EMPTY buffer.
//
// Build and run:  cc -O0 -o /tmp/probe epoll-readable-without-data.c && /tmp/probe

#include <stdio.h>
#include <string.h>
#include <unistd.h>
#include <errno.h>
#include <sys/socket.h>
#include <sys/epoll.h>
#include <netinet/in.h>
#include <netinet/tcp.h>
int main(void) {
    int lsn = socket(AF_INET, SOCK_STREAM, 0);
    struct sockaddr_in a; memset(&a,0,sizeof a);
    a.sin_family=AF_INET; a.sin_addr.s_addr=htonl(INADDR_LOOPBACK); a.sin_port=0;
    bind(lsn,(struct sockaddr*)&a,sizeof a); listen(lsn,1);
    socklen_t al=sizeof a; getsockname(lsn,(struct sockaddr*)&a,&al);

    int cli = socket(AF_INET, SOCK_STREAM, 0);
    connect(cli,(struct sockaddr*)&a,sizeof a);
    int srv = accept(lsn,NULL,NULL);

    // Make the CLIENT close with RST rather than FIN.
    struct linger lg = {1, 0};
    setsockopt(cli, SOL_SOCKET, SO_LINGER, &lg, sizeof lg);
    /* no data sent: the RST must land on an EMPTY receive buffer */
    close(cli);
    usleep(50000);

    int ep = epoll_create1(0);
    struct epoll_event ev = { .events = EPOLLIN, .data.fd = srv };
    epoll_ctl(ep, EPOLL_CTL_ADD, srv, &ev);
    struct epoll_event out[1];
    int n = epoll_wait(ep, out, 1, 500);
    printf("  epoll_wait -> %d event(s), flags=0x%x (EPOLLIN=%d EPOLLERR=%d EPOLLHUP=%d)\n",
           n, n>0?out[0].events:0,
           n>0&&(out[0].events&EPOLLIN)?1:0, n>0&&(out[0].events&EPOLLERR)?1:0, n>0&&(out[0].events&EPOLLHUP)?1:0);

    for (int i = 0; i < 3; ++i) {
        char b; errno = 0;
        ssize_t p = recv(srv, &b, 1, MSG_PEEK);
        printf("  peek %d -> %zd  errno=%d (%s)   EpollSocket would complete: %s\n",
               i+1, p, p<0?errno:0, p<0?strerror(errno):"-",
               p == 0 ? "0  (EOF)" : "1  (\"data pending\")");
    }
    return 0;
}
