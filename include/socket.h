#ifndef SOCKET_H
#define SOCKET_H

#include <stdint.h>
#include "idt.h"

/* socket.h - Unix domain sockets. Milestone 28.
 *
 * Milestone 27 ran real Wine as far as `wine --version` and found that
 * the thing stopping it was not a missing syscall but a missing
 * *subsystem*. Wine is a client/server system: ntdll talks to wineserver
 * over a Unix domain socket, and every process, thread, handle and
 * synchronisation object lives on the far side of it. Without sockets
 * nothing past ntdll's own initialisation can work.
 *
 * So: AF_UNIX, SOCK_STREAM, and the one feature that makes it more than
 * a pipe - SCM_RIGHTS, passing an open file descriptor to the process at
 * the other end. That is how wineserver hands a client the descriptor
 * behind a Windows HANDLE, and a socket layer without it would look
 * complete and be useless for the thing it was built for.
 *
 * There is no networking here and none is implied. AF_INET is refused. */

/* --- the ABI, as Linux defines it --------------------------------------- */
#define AF_UNIX      1
#define AF_INET      2
#define SOCK_STREAM  1
#define SOCK_DGRAM   2
#define SOCK_CLOEXEC   0x80000
#define SOCK_NONBLOCK  0x800

#define MSG_PEEK      0x02
#define MSG_DONTWAIT  0x40
#define MSG_NOSIGNAL  0x4000
#define MSG_CMSG_CLOEXEC 0x40000000

#define SOL_SOCKET   1
#define SCM_RIGHTS   1

#define SHUT_RD   0
#define SHUT_WR   1
#define SHUT_RDWR 2

/* socketcall()'s sub-call numbers. On i386 every socket operation comes
 * through one syscall (102) with the operation in the first argument and
 * a pointer to its arguments in the second - a 1990s space saving that
 * every i386 libc still speaks. */
#define SYS_SOCKET      1
#define SYS_BIND        2
#define SYS_CONNECT     3
#define SYS_LISTEN      4
#define SYS_ACCEPT      5
#define SYS_GETSOCKNAME 6
#define SYS_GETPEERNAME 7
#define SYS_SOCKETPAIR  8
#define SYS_SEND        9
#define SYS_RECV       10
#define SYS_SENDTO     11
#define SYS_RECVFROM   12
#define SYS_SHUTDOWN   13
#define SYS_SETSOCKOPT 14
#define SYS_GETSOCKOPT 15
#define SYS_SENDMSG    16
#define SYS_RECVMSG    17
#define SYS_ACCEPT4    18

/* struct sockaddr_un: a family word and a path. */
typedef struct {
    uint16_t sun_family;
    char     sun_path[108];
} sockaddr_un_t;

/* struct msghdr and struct cmsghdr, i386 layout. */
typedef struct {
    uint32_t msg_name;
    uint32_t msg_namelen;
    uint32_t msg_iov;
    uint32_t msg_iovlen;
    uint32_t msg_control;
    uint32_t msg_controllen;
    uint32_t msg_flags;
} k_msghdr_t;

typedef struct {
    uint32_t cmsg_len;
    int32_t  cmsg_level;
    int32_t  cmsg_type;
} k_cmsghdr_t;

struct socket;
typedef struct socket socket_t;

/* Everything below is called from kernel/posix.c, which owns the
 * descriptor table. A socket is reached through a descriptor like any
 * other object; these are the operations behind it. */

/* The whole of socketcall(). `args` is the user pointer to the sub-call's
 * argument block. Needs `regs` because a receive that has to wait blocks
 * the calling task, and blocking means saving its trap frame. */
int32_t socket_syscall(uint32_t call, uint32_t* args, registers_t* regs);

/* Called from read()/write()/close() when the descriptor turns out to be
 * a socket - a Unix socket is readable and writable as an ordinary file
 * descriptor, and Wine uses it both ways. */
int32_t socket_read(socket_t* s, void* buf, uint32_t len, registers_t* regs);
int32_t socket_write(socket_t* s, const void* buf, uint32_t len);
void    socket_close(socket_t* s);

/* Per-process setup and teardown: sockets belong to the program, and a
 * program that exits holding one must not leak it into the next. */
void socket_process_begin(void);
void socket_process_end(void);

/* For the shell's `sockinfo`. */
uint32_t socket_open_count(void);
uint32_t socket_bytes_moved(void);
uint32_t socket_fds_passed(void);

#endif
