#pragma once
#ifdef _WIN32
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <winsock2.h>
#include <ws2tcpip.h>
static inline int posix_setsockopt(SOCKET s, int level, int optname, const void *optval, int optlen) {
    return setsockopt(s, level, optname, (const char *)optval, optlen);
}
#define setsockopt(s, l, n, v, ol) posix_setsockopt((s), (l), (n), (v), (int)(ol))
#else
#include_next <sys/socket.h>
#endif
