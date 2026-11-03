#pragma once
#ifdef _WIN32
#ifndef TCP_NODELAY
#define TCP_NODELAY 1
#endif
#else
#include_next <netinet/tcp.h>
#endif
