#pragma once

#include <unistd.h>
#include <time.h>
#include <sys/socket.h>
#include <sys/types.h>
#include <unistd.h>
#include <fcntl.h>
#include <sys/ioctl.h>
#include <stdint.h>


namespace bronx{

// Hook 层:用 dlsym(RTLD_NEXT) 把 libc 的阻塞系统调用(sleep/read/write/recv/send/
// connect/accept...)换成"协程让出版"。业务照常写同步调用,实际遇到 EAGAIN 时不阻塞线程,
// 而是注册 epoll 事件 + yield,事件就绪再恢复重试。返回语义跟原调用一致。
//
//  - 线程级开关(thread_local):只有开了 hook 的线程(worker)才走协程版,普通线程还是原生阻塞。
//  - 原函数存在 *_f 指针里(sleep_f/read_f...),要绕过 hook 就直接调它们。
//  - close 会触发 abortAll,路由到 fd 的 owner manager 唤醒挂着的协程。

bool is_hook_enable();
void set_hook_enable(bool flag);          // worker 线程默认开
void hook_init();                         // 把 *_f 指到 libc 原实现

}



// extern "C" 禁止名字修饰,保证符号能正确覆盖 libc 的同名函数。
// 每个被 hook 的函数,都有一个同签名的 *_f 指针存着 libc 原实现。
extern "C"{

// sleep
using sleep_fun = unsigned int(*)(unsigned int seconds);
extern sleep_fun sleep_f;

using usleep_fun = int(*)(useconds_t usec);
extern usleep_fun usleep_f;

using nanosleep_fun = int(*)(const struct timespec *req, struct timespec *rem);
extern nanosleep_fun nanosleep_f;

// socket
using socket_fun = int(*)(int domain, int type, int protocol);
extern socket_fun socket_f;

using connect_fun = int(*)(int sockfd, const struct sockaddr *addr, socklen_t addrlen);
extern connect_fun connect_f;
extern int connect_with_timeout(int sockfd, const struct sockaddr *addr, socklen_t addrlen, uint64_t timeout_ms);

using accept_fun = int(*)(int sockfd, struct sockaddr *addr, socklen_t *addrlen);
extern accept_fun accept_f;

// read
using read_fun = ssize_t(*)(int fd, void *buf, size_t count);
extern read_fun read_f;

using readv_fun = ssize_t(*)(int fd, const struct iovec *iov, int iovcnt);
extern readv_fun readv_f;

using recv_fun = ssize_t(*)(int sockfd, void *buf, size_t len, int flags);
extern recv_fun recv_f;

using recvfrom_fun = ssize_t(*)(int sockfd, void *buf, size_t len, int flags, struct sockaddr *src_addr, socklen_t *addrlen);
extern recvfrom_fun recvfrom_f;

using recvmsg_fun = ssize_t(*)(int sockfd, struct msghdr *msg, int flags);
extern recvmsg_fun recvmsg_f;

// write
using write_fun = ssize_t(*)(int fd, const void *buf, size_t count);
extern write_fun write_f;

using writev_fun = ssize_t(*)(int fd, const struct iovec *iov, int iovcnt);
extern writev_fun writev_f;

using send_fun = ssize_t(*)(int sockfd, const void *buf, size_t len, int flags);
extern send_fun send_f;

using sendto_fun = ssize_t(*)(int sockfd, const void *buf, size_t len, int flags, const struct sockaddr *dest_addr, socklen_t addrlen);
extern sendto_fun sendto_f;

using sendmsg_fun = ssize_t(*)(int sockfd, const struct msghdr *msg, int flags);
extern sendmsg_fun sendmsg_f;


using close_fun = int(*)(int fd);
extern close_fun close_f;

// socketop
using fcntl_fun = int(*)(int fd, int cmd, ... /* arg */ );
extern fcntl_fun fcntl_f;

using ioctl_fun = int(*)(int fd, unsigned long request, ...);
extern ioctl_fun ioctl_f;

// using getsockopt_fun = int(*)(int sockfd, int level, int optname, void *optval, socklen_t *optlen);
// extern getsockopt_fun getsockopt_f;

using setsockopt_fun = int(*)(int sockfd, int level, int optname, const void *optval, socklen_t optlen);
extern setsockopt_fun setsockopt_f;

}
