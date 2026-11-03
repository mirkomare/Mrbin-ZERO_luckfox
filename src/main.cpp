// Luckfox / host entry — CORE FSM lives in mrbin_core_main.cpp (app_main).
#ifdef _WIN32
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <winsock2.h>
#endif

#include <stdio.h>
#include <string.h>
#ifndef _WIN32
#include <signal.h>
#include <stdlib.h>
#endif

extern "C" void app_main(void);

int main(int argc, char **argv) {
#ifdef _WIN32
    WSADATA wsa;
    if (WSAStartup(MAKEWORD(2, 2), &wsa) != 0) {
        fprintf(stderr, "WSAStartup failed\n");
        return 1;
    }
#endif
#ifndef _WIN32
    /* A browser drop of /live/stream must return EPIPE from send().
     * Default SIGPIPE kills the whole process, so the next Registra
     * sees a reset connection and Live stays black until respawn. */
    signal(SIGPIPE, SIG_IGN);
#endif
    for (int i = 1; i < argc; ++i) {
        if (strcmp(argv[i], "--force-config") == 0) {
#ifdef _WIN32
            _putenv("MRBIN_FORCE_CONFIG=1");
#else
            setenv("MRBIN_FORCE_CONFIG", "1", 1);
#endif
        } else if (strcmp(argv[i], "--pir") == 0) {
#ifdef _WIN32
            _putenv("MRBIN_PIR=1");
            _putenv("MRBIN_REC_FAST=1");
#else
            setenv("MRBIN_PIR", "1", 1);
            setenv("MRBIN_REC_FAST", "1", 1);
#endif
        } else if (strcmp(argv[i], "--rec-fast") == 0) {
#ifdef _WIN32
            _putenv("MRBIN_REC_FAST=1");
#else
            setenv("MRBIN_REC_FAST", "1", 1);
#endif
        } else if (strcmp(argv[i], "--pir-bench") == 0 && (i + 1) < argc) {
            char buf[48];
            snprintf(buf, sizeof(buf), "MRBIN_PIR_BENCH_SEC=%s", argv[++i]);
#ifdef _WIN32
            _putenv(buf);
#else
            setenv("MRBIN_PIR_BENCH_SEC", argv[i], 1);
#endif
        }
    }
    app_main();
    return 0;
}
