/* gpio-watch: stampa ogni fronte dei GPIO configurati.
 * Numero Linux, stato vecchio, stato nuovo. Campionamento ~200 us,
 * quindi vede anche un impulso di pochi millisecondi.
 * Sola lettura dei registri EXT_PORT, non cambia mux né direzione.
 */
#include <errno.h>
#include <fcntl.h>
#include <signal.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <time.h>
#include <unistd.h>

#define EXT_PORT 0x70u
#define N_BANKS 5

static const uint32_t BANK_PHYS[N_BANKS] = {
    0xff380000u, /* gpio0 linux 0..31 */
    0xff530000u, /* gpio1 linux 32..63 */
    0xff540000u, /* gpio2 linux 64..95 */
    0xff550000u, /* gpio3 linux 96..127 */
    0xff560000u, /* gpio4 linux 128..159 */
};

struct Pin {
    int linux_num;
    const char *name;
    int header;
    int bank;
    int bit;
    int level;
    int seen;
    uint64_t prev_ms;
    int burst;
    uint64_t burst_ms;
    int collapsed;
};

static struct Pin g_pins[] = {
    {58, "D2", 26},
    {52, "D1", 33},
    {59, "DEMO", 22},
    {53, "MODE", 32},
    {4, "DONE", 40},
    {55, "SQW", 36},
    {51, "LED", 21},
    {73, "MODEM", 27},
    {54, "RTC-SDA", 12},
    {3, "RTC-SCL", 38},
    {118, "USER-LED", 0},
};
#define N_PINS ((int)(sizeof(g_pins) / sizeof(g_pins[0])))

static volatile uint32_t *g_bank[N_BANKS];
static int g_log = -1;
static int g_con = -1;

static uint64_t mono_ms(void) {
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (uint64_t)ts.tv_sec * 1000ull + (uint64_t)ts.tv_nsec / 1000000ull;
}

static void stamp(char *dst, size_t n) {
    struct timespec ts;
    struct tm tm;
    clock_gettime(CLOCK_REALTIME, &ts);
    localtime_r(&ts.tv_sec, &tm);
    snprintf(dst, n, "%02d:%02d:%02d.%03ld",
             tm.tm_hour, tm.tm_min, tm.tm_sec, ts.tv_nsec / 1000000L);
}

static void emit(const char *line) {
    char buf[256];
    int len = snprintf(buf, sizeof(buf), "%s\n", line);
    if (len < 0) return;
    if (len >= (int)sizeof(buf)) len = (int)sizeof(buf) - 1;
    if (g_log >= 0) {
        ssize_t w = write(g_log, buf, (size_t)len);
        (void)w;
    }
    if (g_con >= 0) {
        ssize_t w = write(g_con, buf, (size_t)len);
        (void)w;
    }
}

static int level_of(const struct Pin *p) {
    uint32_t v = g_bank[p->bank][EXT_PORT / 4];
    return (int)((v >> p->bit) & 1u);
}

static int sysfs_level(int n) {
    char path[64];
    char b = 0;
    int fd;
    snprintf(path, sizeof(path), "/sys/class/gpio/gpio%d/value", n);
    fd = open(path, O_RDONLY);
    if (fd < 0) return -1;
    if (read(fd, &b, 1) != 1) {
        close(fd);
        return -1;
    }
    close(fd);
    if (b == '0') return 0;
    if (b == '1') return 1;
    return -1;
}

static void pin_where(const struct Pin *p, char *dst, size_t n) {
    if (p->header > 0)
        snprintf(dst, n, "header %d", p->header);
    else
        snprintf(dst, n, "scheda");
}

static int setup_mem(void) {
    int fd = open("/dev/mem", O_RDWR | O_SYNC);
    int i;
    if (fd < 0) return -1;
    for (i = 0; i < N_BANKS; i++) {
        void *p = mmap(NULL, 4096, PROT_READ, MAP_SHARED, fd, (off_t)BANK_PHYS[i]);
        if (p == MAP_FAILED) {
            close(fd);
            return -1;
        }
        g_bank[i] = (volatile uint32_t *)p;
    }
    close(fd);
    return 0;
}

static void banner(void) {
    char line[240];
    char clk[32];
    int i;
    stamp(clk, sizeof(clk));
    snprintf(line, sizeof(line),
             "%s  gpio-watch avviato. Numero = GPIO Linux. Campione 200 us.",
             clk);
    emit(line);
    for (i = 0; i < N_PINS; i++) {
        struct Pin *p = &g_pins[i];
        char where[32];
        int sys = sysfs_level(p->linux_num);
        int ext = level_of(p);
        uint32_t ver = g_bank[p->bank][0x78 / 4];
        pin_where(p, where, sizeof(where));
        p->level = ext;
        p->seen = 1;
        p->prev_ms = mono_ms();
        if (sys >= 0 && sys != ext) {
            snprintf(line, sizeof(line),
                     "%s  GPIO %d  %s  %s  EXT=%d sysfs=%d  VER=%08x  DIVERSO",
                     clk, p->linux_num, p->name, where, ext, sys, ver);
        } else if (sys >= 0) {
            snprintf(line, sizeof(line),
                     "%s  GPIO %d  %s  %s  ora %d  (sysfs uguale)",
                     clk, p->linux_num, p->name, where, ext);
        } else {
            snprintf(line, sizeof(line),
                     "%s  GPIO %d  %s  %s  ora %d",
                     clk, p->linux_num, p->name, where, ext);
        }
        emit(line);
    }
    emit("in attesa. Ogni fronte: GPIO <linux>  <vecchio> -> <nuovo>");
}

static void on_edge(struct Pin *p, int neu, uint64_t now) {
    char clk[32];
    char where[32];
    char line[240];
    uint64_t dt = now - p->prev_ms;
    stamp(clk, sizeof(clk));
    pin_where(p, where, sizeof(where));
    if (now - p->burst_ms > 200) {
        p->burst = 0;
        p->burst_ms = now;
        if (p->collapsed) {
            p->collapsed = 0;
            snprintf(line, sizeof(line),
                     "%s  GPIO %d  %s  tornato lento, riprendo ogni fronte",
                     clk, p->linux_num, p->name);
            emit(line);
        }
    }
    p->burst++;
    if (p->burst > 40) {
        p->collapsed = 1;
        if (p->burst == 41 || (p->burst % 200) == 0) {
            snprintf(line, sizeof(line),
                     "%s  GPIO %d  %s  %d fronti in 200 ms (ultimo %d -> %d) — riassunto",
                     clk, p->linux_num, p->name, p->burst, p->level, neu);
            emit(line);
        }
        p->level = neu;
        p->prev_ms = now;
        return;
    }
    snprintf(line, sizeof(line),
             "%s  GPIO %d  %d -> %d  %s  %s  durato %llu ms",
             clk, p->linux_num, p->level, neu, p->name, where,
             (unsigned long long)dt);
    emit(line);
    p->level = neu;
    p->prev_ms = now;
}

int main(void) {
    int i;
    uint64_t last_beat;

    if (setup_mem() != 0) {
        fprintf(stderr, "gpio-watch: mmap /dev/mem fallita: %s\n", strerror(errno));
        return 1;
    }
    for (i = 0; i < N_PINS; i++) {
        int n = g_pins[i].linux_num;
        g_pins[i].bank = n / 32;
        g_pins[i].bit = n % 32;
        if (g_pins[i].bank < 0 || g_pins[i].bank >= N_BANKS) return 1;
    }

    if (fork() > 0) {
        printf("gpio-watch partito\nlog: tail -f /tmp/gpio-watch.log\n");
        fflush(stdout);
        _exit(0);
    }
    setsid();

    g_log = open("/tmp/gpio-watch.log", O_WRONLY | O_CREAT | O_TRUNC, 0644);
    g_con = open("/dev/console", O_WRONLY | O_NOCTTY);
    {
        int pidf = open("/tmp/gpio-watch.pid", O_WRONLY | O_CREAT | O_TRUNC, 0644);
        char buf[32];
        int n = snprintf(buf, sizeof(buf), "%d\n", (int)getpid());
        if (pidf >= 0 && n > 0) {
            ssize_t w = write(pidf, buf, (size_t)n);
            (void)w;
            close(pidf);
        }
    }
    signal(SIGPIPE, SIG_IGN);

    banner();
    last_beat = mono_ms();

    for (;;) {
        uint64_t now;
        for (i = 0; i < N_PINS; i++) {
            struct Pin *p = &g_pins[i];
            int a = level_of(p);
            int b = level_of(p);
            if (a != b || a == p->level) continue;
            now = mono_ms();
            on_edge(p, a, now);
        }
        now = mono_ms();
        if (now - last_beat >= 10000) {
            char clk[32];
            char line[240];
            int pos;
            stamp(clk, sizeof(clk));
            pos = snprintf(line, sizeof(line), "%s  vivo ", clk);
            for (i = 0; i < N_PINS && pos > 0 && pos < (int)sizeof(line) - 16; i++) {
                pos += snprintf(line + pos, sizeof(line) - (size_t)pos,
                                "%d=%d ", g_pins[i].linux_num, g_pins[i].level);
            }
            emit(line);
            last_beat = now;
        }
        {
            struct timespec req;
            req.tv_sec = 0;
            req.tv_nsec = 200000L;
            nanosleep(&req, NULL);
        }
    }
}
