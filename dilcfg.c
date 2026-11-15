/*
 * dilcfg - DSD TECH diLogger configuration tool for Unix
 *
 * Linux / FreeBSD / OpenBSD. No dependencies beyond libc + POSIX termios.
 * Protocol reverse-engineered from DSD TECH diConfig v1.3.0.0 (Windows):
 * ASCII command + "\r\n", config port fixed 115200 baud 8N1.
 *
 * Modes:
 *   dilcfg /dev/ttyU0                 interactive TUI
 *   dilcfg -f cfgfile /dev/ttyU0      apply config file, then exit
 *   dilcfg -c 'cmd [args]' /dev/ttyU0 one-shot command, replies to stdout
 *
 * License: 0BSD (do whatever).
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <ctype.h>
#include <errno.h>
#include <time.h>
#include <unistd.h>
#include <fcntl.h>
#include <termios.h>
#include <signal.h>
#include <sys/time.h>
#include <sys/select.h>

#define APP "dilcfg 1.0"
#define PROMPT "dil> "

static int g_fd = -1;
static int g_verbose = 0;
static int g_stop = 0;

static void on_sigint(int sig) { (void)sig; g_stop = 1; }

/* ------------------------------------------------------------------ */
/* serial                                                              */
/* ------------------------------------------------------------------ */

static speed_t baud_const(long b)
{
    switch (b) {
    case 1200:    return B1200;
    case 2400:    return B2400;
    case 4800:    return B4800;
    case 9600:    return B9600;
    case 19200:   return B19200;
    case 38400:   return B38400;
    case 57600:   return B57600;
    case 115200:  return B115200;
    case 230400:  return B230400;
#if defined(B460800)
    case 460800:  return B460800;
#endif
#if defined(B921600)
    case 921600:  return B921600;
#endif
#if defined(B1000000)
    case 1000000: return B1000000;
#endif
    default:      return 0;
    }
}

static int serial_open(const char *path, long baud, int stopbits_10, int parity, int databits)
{
    /* stopbits_10: 10 = 1 stop bit, 15 = 1.5, 20 = 2 (matches device codes 1/3/2 -> 1/1.5/2 mapping done by caller) */
    int fd = open(path, O_RDWR | O_NOCTTY);
    if (fd < 0) {
        fprintf(stderr, "open %s: %s\n", path, strerror(errno));
        return -1;
    }
    struct termios tio;
    if (tcgetattr(fd, &tio) != 0) {
        /* not a tty? close and report */
        fprintf(stderr, "tcgetattr %s: %s (is it a serial port?)\n", path, strerror(errno));
        close(fd);
        return -1;
    }
    speed_t sp = baud_const(baud);
    if (sp == 0) { fprintf(stderr, "unsupported baud %ld\n", baud); close(fd); return -1; }

    /* raw mode, written out explicitly: cfmakeraw() is not guaranteed on every BSD */
    tio.c_iflag = 0;
    tio.c_oflag = 0;
    tio.c_lflag = 0;
    tio.c_cflag &= ~(CSIZE | PARENB | PARODD | CSTOPB);
#ifdef CRTSCTS
    tio.c_cflag &= ~CRTSCTS;              /* name varies across BSDs; only clear if defined */
#endif
    tio.c_cflag |= CREAD;
#ifdef CLOCAL
    tio.c_cflag |= CLOCAL;                /* ignore modem control lines */
#endif
    tio.c_cflag |= (databits == 7) ? CS7 : CS8;
    if (parity == 1) tio.c_cflag |= PARENB;                 /* even */
    if (parity == 2) tio.c_cflag |= (PARENB | PARODD);      /* odd  */
    if (stopbits_10 == 20) tio.c_cflag |= CSTOPB;           /* 2 stop bits (1.5 is not expressible in termios; the diLogger config port itself is always 8N1) */
    cfsetispeed(&tio, sp);
    cfsetospeed(&tio, sp);
    tio.c_cc[VMIN] = 0;
    tio.c_cc[VTIME] = 0;
    if (tcsetattr(fd, TCSANOW, &tio) != 0) {
        fprintf(stderr, "tcsetattr: %s\n", strerror(errno));
        close(fd);
        return -1;
    }
    tcflush(fd, TCIOFLUSH);
    return fd;
}

/* write a command line (adds CRLF), then read until idle_ms with no data or first-line timeout */
static int serial_send_and_read(int fd, const char *line, int idle_ms, char *rx, size_t rxcap)
{
    char buf[512];
    size_t n = (size_t)snprintf(buf, sizeof(buf), "%s\r\n", line);
    rx[0] = '\0';
    size_t total = 0;
    ssize_t w = write(fd, buf, n);
    if (w < 0) { fprintf(stderr, "write: %s\n", strerror(errno)); return -1; }
    { struct timespec ts = {0, 10000000}; nanosleep(&ts, NULL); } /* diConfig sleeps 10 ms between writes */

    long overall_us = 1500000;           /* cap total wait */
    int idle_us = idle_ms * 1000;
    long waited = 0;
    int got_any = 0;
    while (waited < overall_us) {
        fd_set rfds; FD_ZERO(&rfds); FD_SET(fd, &rfds);
        struct timeval tv; tv.tv_sec = 0; tv.tv_usec = 50000; /* poll 50ms slices */
        int r = select(fd + 1, &rfds, NULL, NULL, &tv);
        if (r > 0 && FD_ISSET(fd, &rfds)) {
            ssize_t got = read(fd, buf, sizeof(buf));
            if (got > 0) {
                if (total + (size_t)got < rxcap - 1) { memcpy(rx + total, buf, (size_t)got); total += (size_t)got; rx[total] = '\0'; }
                got_any = 1;
                waited = 0;              /* restart idle counter */
            } else if (got < 0 && errno != EAGAIN && errno != EWOULDBLOCK) {
                break;
            }
        }
        if (got_any) {
            /* if we already have data and see an idle gap, stop */
        }
        waited += 50000;
        if (got_any && waited >= idle_us) break;
    }
    return (int)total;
}

/* ------------------------------------------------------------------ */
/* command model                                                       */
/* ------------------------------------------------------------------ */

/* map friendly words to wire commands; returns 0 on success, filling wire[] */
static int build_command(const char *line_in, char wire[], size_t cap)
{
    char line[256];
    snprintf(line, sizeof(line), "%s", line_in);
    /* trim */
    char *p = line;
    while (*p && isspace((unsigned char)*p)) p++;
    size_t L = strlen(p);
    while (L && isspace((unsigned char)p[L-1])) p[--L] = '\0';
    if (L == 0) return -1;

    char key[64] = {0}, val[128] = {0};
    const char *sp = strchr(p, ' ');
    if (sp) {
        size_t kl = (size_t)(sp - p); if (kl >= sizeof(key)) kl = sizeof(key)-1;
        memcpy(key, p, kl); key[kl] = '\0';
        while (*sp && isspace((unsigned char)*sp)) sp++;
        snprintf(val, sizeof(val), "%s", sp);
    } else {
        snprintf(key, sizeof(key), "%s", p);
    }
    for (char *q = key; *q; q++) *q = (char)tolower((unsigned char)*q);
    for (char *q = val; *q; q++) *q = (char)tolower((unsigned char)*q);

    struct { const char *key; const char *wire; } qmap[] = {
        {"baudrate", "param baudrate"}, {"stopbit", "param stopbit"},
        {"parity",   "param parity"},   {"filemb",  "param filemb"},
        {"usetime",  "param usetime"},  {"timestamp","param usetime"},
        {"hexlog",   "param hexlog"},   {"filenew", "param filenew"},
        {"time",     "time"},           {"version", "system version"},
        {"system",   "system version"}, {"reset",   "system reset"},
        {"reboot",   "system reset"},   {"default", "param setdefault"},
        {"setdefault","param setdefault"},
    };
    if (strcmp(key, "raw") == 0) {
        /* verbatim passthrough, do not lowercase */
        const char *sp2 = strchr(p, ' ');
        if (!sp2 || !*++sp2) return -1;
        snprintf(wire, cap, "%s", sp2);
        return 0;
    }
    if (val[0] == '\0') {
        if (!strcmp(key, "sync")) {
            time_t t = time(NULL);
            struct tm *lt = localtime(&t);
            snprintf(wire, cap, "time set %d %d %d %d %d %d",
                     lt->tm_year + 1900, lt->tm_mon + 1, lt->tm_mday,
                     lt->tm_hour, lt->tm_min, lt->tm_sec);
            return 0;
        }
        for (size_t i = 0; i < sizeof(qmap)/sizeof(qmap[0]); i++)
            if (strcmp(key, qmap[i].key) == 0) { snprintf(wire, cap, "%s", qmap[i].wire); return 0; }
        /* raw passthrough (AT terminal style) */
        snprintf(wire, cap, "%s", p);
        return 0;
    }

    if (strcmp(key, "baudrate") == 0) {
        snprintf(wire, cap, "param baudrate %s", val); return 0;
    }
    if (strcmp(key, "stopbit") == 0) {
        /* accept 1, 1.5, 2 (display) or 1, 3, 2 (device codes) */
        if (!strcmp(val, "1"))       snprintf(wire, cap, "param stopbit 1");
        else if (!strcmp(val, "1.5")|| !strcmp(val, "3")) snprintf(wire, cap, "param stopbit 3");
        else if (!strcmp(val, "2"))  snprintf(wire, cap, "param stopbit 2");
        else return -1;
        return 0;
    }
    if (strcmp(key, "parity") == 0) {
        if (!strcmp(val,"none")||!strcmp(val,"0")||!strcmp(val,"no")) snprintf(wire, cap, "param parity 0");
        else if (!strcmp(val,"odd")||!strcmp(val,"1"))  snprintf(wire, cap, "param parity 1");
        else if (!strcmp(val,"even")||!strcmp(val,"2")) snprintf(wire, cap, "param parity 2");
        else return -1;
        return 0;
    }
    if (strcmp(key, "filemb") == 0) {
        snprintf(wire, cap, "param filemb %s", val); return 0;   /* device validates 1..100 */
    }
    if (!strcmp(key, "usetime") || !strcmp(key, "timestamp")) {
        if (!strcmp(val,"on")||!strcmp(val,"1")||!strcmp(val,"yes")) snprintf(wire, cap, "param usetime 1");
        else if (!strcmp(val,"off")||!strcmp(val,"0")||!strcmp(val,"no")) snprintf(wire, cap, "param usetime 0");
        else return -1;
        return 0;
    }
    if (!strcmp(key, "hexlog")) {
        if (!strcmp(val,"on")||!strcmp(val,"1")||!strcmp(val,"yes")) snprintf(wire, cap, "param hexlog 1");
        else if (!strcmp(val,"off")||!strcmp(val,"0")||!strcmp(val,"no")) snprintf(wire, cap, "param hexlog 0");
        else return -1;
        return 0;
    }
    if (!strcmp(key, "filenew")) {
        if (!strcmp(val,"on")||!strcmp(val,"1")||!strcmp(val,"yes")) snprintf(wire, cap, "param filenew 1");
        else if (!strcmp(val,"off")||!strcmp(val,"0")||!strcmp(val,"no")) snprintf(wire, cap, "param filenew 0");
        else return -1;
        return 0;
    }
    if (!strcmp(key, "time")) {
        /* value must be "YYYY M D H Mi S" */
        int y,mo,d,h,mi,s;
        if (sscanf(val, "%d %d %d %d %d %d", &y,&mo,&d,&h,&mi,&s) == 6) {
            snprintf(wire, cap, "time set %d %d %d %d %d %d", y,mo,d,h,mi,s);
            return 0;
        }
        return -1;
    }
    if (!strcmp(key, "sync")) {
        time_t t = time(NULL);
        struct tm *lt = localtime(&t);
        snprintf(wire, cap, "time set %d %d %d %d %d %d",
                 lt->tm_year + 1900, lt->tm_mon + 1, lt->tm_mday,
                 lt->tm_hour, lt->tm_min, lt->tm_sec);
        return 0;
    }
    /* raw */
    snprintf(wire, cap, "%s", p);
    return 0;
}

/* display-only: show 0/1 of boolean settings as off/on (wire unchanged) */
static void booleans_to_onoff(char *buf)
{
    static const char *keys[] = {"log_use_time:", "usetime ",
                                 "hex_log:", "hexlog ",
                                 "filenew:", "filenew "};
    for (size_t k = 0; k < sizeof(keys)/sizeof(keys[0]); k++) {
        size_t klen = strlen(keys[k]);
        char *p = buf;
        while ((p = strstr(p, keys[k])) != NULL) {
            char *d = p + klen;
            if ((*d == '0' || *d == '1') && !isdigit((unsigned char)d[1])) {
                const char *rep = (*d == '1') ? "on" : "off";
                size_t rlen = strlen(rep);
                size_t tail = strlen(d + 1);
                memmove(d + rlen, d + 1, tail + 1);
                memcpy(d, rep, rlen);
                p = d + rlen;
            } else {
                p += klen;
            }
        }
    }
}

static void pretty_print_rx(const char *rx)
{
    /* print with CRLF normalized, indented */
    const char *s = rx;
    while (*s) {
        char linebuf[512]; size_t i = 0;
        while (*s && *s != '\r' && *s != '\n' && i < sizeof(linebuf)-1) linebuf[i++] = *s++;
        linebuf[i] = '\0';
        while (*s == '\r' || *s == '\n') s++;
	if (i) { booleans_to_onoff(linebuf); printf("  < %s\n", linebuf); }
    }
}

/* ------------------------------------------------------------------ */
/* modes                                                               */
/* ------------------------------------------------------------------ */

static int run_config(int fd, const char *path)
{
    FILE *f = fopen(path, "r");
    if (!f) { fprintf(stderr, "open %s: %s\n", path, strerror(errno)); return 1; }
    char line[256], wire[384], rx[2048];
    int rc = 0;
    while (fgets(line, sizeof(line), f)) {
        char *p = line;
        while (*p && isspace((unsigned char)*p)) p++;
        if (*p == '#' || *p == ';' || *p == '\0') continue;
        size_t L = strlen(p); while (L && isspace((unsigned char)p[L-1])) p[--L]='\0';
        if (build_command(p, wire, sizeof(wire)) != 0) {
            fprintf(stderr, "config: bad line: %s\n", p); rc = 1; continue;
        }
        if (g_verbose) printf("  > %s\n", wire);
        int n = serial_send_and_read(fd, wire, 300, rx, sizeof(rx));
        if (n > 0) pretty_print_rx(rx);
        if (strstr(wire, "system reset")) printf("  (device resets)\n");
        if (strstr(wire, "setdefault"))   printf("  (device restores defaults)\n");
    }
    fclose(f);
    return rc;
}

/* ------------------------------------------------------------------ */
/* device snapshot                                                     */
/* ------------------------------------------------------------------ */

typedef struct {
    long baud; int stop_code, parity, mb, ut, hx, fn;
    int y, mo, d, h, mi, se;
    char version[64];
    unsigned have;            /* bit i set = field i parsed */
} devconf;

enum { F_BAUD=1, F_STOP=2, F_PARITY=4, F_MB=8, F_UT=16, F_HX=32,
    F_FN=64, F_TIME=128, F_VER=256 };

static int devconf_fetch(int fd, devconf *c)
{
    static const char *queries[] = {
        "param baudrate", "param stopbit", "param parity", "param filemb",
        "param usetime", "param hexlog", "param filenew",
        "time", "system version"
    };
    char rx[4096]; size_t off = 0; char part[1024]; char *p;
    memset(c, 0, sizeof(*c));
    rx[0] = '\0';
    for (size_t i = 0; i < sizeof(queries)/sizeof(queries[0]); i++) {
        int n = serial_send_and_read(fd, queries[i], 250, part, sizeof(part));
        if (n > 0 && off + (size_t)n < sizeof(rx) - 1) {
            memcpy(rx + off, part, (size_t)n); off += (size_t)n; rx[off] = '\0';
        }
    }
    if ((p = strstr(rx, "baudrate:")))     { sscanf(p + 9,  "%ld", &c->baud);      c->have |= F_BAUD; }
    if ((p = strstr(rx, "stop_bit:")))     { sscanf(p + 9,  "%d", &c->stop_code);  c->have |= F_STOP; }
    if ((p = strstr(rx, "parity:")))       { sscanf(p + 7,  "%d", &c->parity);     c->have |= F_PARITY; }
    if ((p = strstr(rx, "file_mb:")))      { sscanf(p + 8,  "%d", &c->mb);         c->have |= F_MB; }
    if ((p = strstr(rx, "log_use_time:"))) { sscanf(p + 13, "%d", &c->ut);         c->have |= F_UT; }
    if ((p = strstr(rx, "hex_log:")))      { sscanf(p + 8,  "%d", &c->hx);         c->have |= F_HX; }
    if ((p = strstr(rx, "filenew:")))      { sscanf(p + 8,  "%d", &c->fn);         c->have |= F_FN; }
    if ((p = strstr(rx, "system version:"))) { sscanf(p + 15, "%63s", c->version); c->have |= F_VER; }
    for (p = rx; (p = strchr(p, '>')) != NULL; p++)
        if (sscanf(p + 1, " %d-%d-%d %d:%d:%d", &c->y, &c->mo, &c->d, &c->h, &c->mi, &c->se) == 6) {
            c->have |= F_TIME; break;
        }
    return c->have ? 0 : -1;
}

static const char *stop_word(int code)
{
    return code == 3 ? "1.5" : code == 2 ? "2" : code == 1 ? "1" : "?";
}
static const char *parity_word(int p)
{
    return p == 0 ? "none" : p == 1 ? "odd" : p == 2 ? "even" : "?";
}
static const char *bool_word(int v) { return v ? "on" : "off"; }

static int cmd_dump(int fd)
{
    devconf c;
    if (devconf_fetch(fd, &c) != 0) { fprintf(stderr, "device did not answer any query\n"); return 1; }
    printf("device configuration:\n");
    if (c.have & F_BAUD)   printf("  baudrate     : %ld\n", c.baud);
    if (c.have & F_STOP)   printf("  stop bits    : %s\n", stop_word(c.stop_code));
    if (c.have & F_PARITY) printf("  parity       : %s\n", parity_word(c.parity));
    if (c.have & F_MB)     printf("  log file max : %d MB\n", c.mb);
    if (c.have & F_UT)     printf("  timestamp    : %s\n", bool_word(c.ut));
    if (c.have & F_HX)     printf("  hex log      : %s\n", bool_word(c.hx));
    if (c.have & F_FN)     printf("  new file/boot: %s\n", bool_word(c.fn));
    if (c.have & F_TIME)   printf("  device clock : %04d-%02d-%02d %02d:%02d:%02d\n", c.y, c.mo, c.d, c.h, c.mi, c.se);
    if (c.have & F_VER)    printf("  firmware     : %s\n", c.version);
    return 0;
}

static int cmd_save(int fd, const char *path)
{
    devconf c;
    if (devconf_fetch(fd, &c) != 0) { fprintf(stderr, "device did not answer any query\n"); return 1; }
    FILE *f = fopen(path, "w");
    if (!f) { fprintf(stderr, "open %s: %s\n", path, strerror(errno)); return 1; }
    fprintf(f, "# dilcfg save - diLogger configuration read from the device\n");
    if (c.have & F_VER) fprintf(f, "# firmware %s\n", c.version);
    fprintf(f, "\n");
    if (c.have & F_BAUD)   fprintf(f, "baudrate %ld\n",   c.baud);
    if (c.have & F_STOP)   fprintf(f, "stopbit %s\n",     stop_word(c.stop_code));
    if (c.have & F_PARITY) fprintf(f, "parity %s\n",      parity_word(c.parity));
    if (c.have & F_MB)     fprintf(f, "filemb %d\n",      c.mb);
    if (c.have & F_UT)     fprintf(f, "timestamp %s\n",   bool_word(c.ut));
    if (c.have & F_HX)     fprintf(f, "hexlog %s\n",      bool_word(c.hx));
    if (c.have & F_FN)     fprintf(f, "filenew %s\n",     bool_word(c.fn));
    if (c.have & F_TIME) {
        fprintf(f, "\n# device clock was %04d-%02d-%02d %02d:%02d:%02d at save time;\n", c.y, c.mo, c.d, c.h, c.mi, c.se);
        fprintf(f, "# uncomment to push that (past) time back, or use 'sync' for the host clock:\n");
        fprintf(f, "# time %d %d %d %d %d %d\n", c.y, c.mo, c.d, c.h, c.mi, c.se);
    }
    if (fclose(f) != 0) { fprintf(stderr, "close %s: %s\n", path, strerror(errno)); return 1; }
    printf("saved to %s\n", path);
    return 0;
}

static void usage_help(void)
{
    printf(
      "commands: name, optionally followed by a value\n"
      "  baudrate [N]              recording port baud (1200..921600)\n"
      "  stopbit [1|1.5|2]         recording port stop bits\n"
      "  parity [none|odd|even]    recording port parity\n"
      "  filemb [N]                max log file size MB (1..100)\n"
      "  timestamp [on|off]        add timestamp to log entries\n"
      "  hexlog [on|off]           record data as hex\n"
      "  filenew [on|off]          new log file on boot/card insert\n"
      "  time [\"Y M D h m s\"]      read / set device clock\n"
      "  sync                      set device clock from host clock\n"
      "  version                   firmware version\n"
      "  reset                     reboot device\n"
      "  default                   restore factory settings\n"
      "  dump                      show current device configuration\n"
      "  save [file]               write current device config to file (load with -f)\n"
      "  raw <text...>             send <text> verbatim (AT terminal)\n"
      "  verbose [on|off]          echo outgoind commands (default: off)\n"
      "  help, quit\n");
}

static int run_tui(int fd)
{
    char line[256], wire[384], rx[2048];
    printf(APP " — diLogger config port. Type 'help'.\n");
    while (!g_stop) {
        fputs(PROMPT, stdout); fflush(stdout);
        if (!fgets(line, sizeof(line), stdin)) break;
        char *p = line; while (*p && isspace((unsigned char)*p)) p++;
        size_t L = strlen(p); while (L && isspace((unsigned char)p[L-1])) p[--L]='\0';
        if (!*p) continue;
        if (!strcmp(p,"q") || !strcmp(p,"quit") || !strcmp(p,"exit")) break;
        if (!strcmp(p,"h") || !strcmp(p,"help") || !strcmp(p,"?")) { usage_help(); continue; }
        if (!strcmp(p,"dump")) { cmd_dump(fd); continue; }
        if (!strcmp(p,"save") || !strncmp(p,"save ",5)) {
            const char *a = p + 4; while (*a == ' ') a++;
            cmd_save(fd, *a ? a : "dilcfg-save.conf");
            continue;
        }
        if (!strcmp(p,"verbose") || !strncmp(p,"verbose ",8)) {
                const char *a = p + 7; while (*a == ' ') a++;
                if (!*a)                              g_verbose = !g_verbose;
                else if (!strcmp(a,"on")  || !strcmp(a,"1"))  g_verbose = 1;
                else if (!strcmp(a,"off") || !strcmp(a,"0"))  g_verbose = 0;
                else { printf("verbose: on|off\n"); continue; }
                printf("verbose %s\n", g_verbose ? "on" : "off");
                continue;
        }
        const char *eff = p;
        char rawbuf[256];
        if (!strncmp(p, "raw ", 4)) {
            snprintf(rawbuf, sizeof(rawbuf), "%s", p+4);
            eff = rawbuf;
            snprintf(wire, sizeof(wire), "%s", eff);
        } else if (build_command(eff, wire, sizeof(wire)) != 0) {
            printf("bad command/args: %s\n", p);
            continue;
        }
        if (g_verbose) printf("  > %s\n", wire);
        int n = serial_send_and_read(fd, wire, 300, rx, sizeof(rx));
        if (n > 0) pretty_print_rx(rx);
        else if (g_verbose) printf("  < (no response)\n");
    }
    return 0;
}

int main(int argc, char **argv)
{
    const char *cfgfile = NULL, *onecmd = NULL, *port = NULL;
    long baud = 115200;
    int c;
    while ((c = getopt(argc, argv, "f:c:b:qvhV")) != -1) {
        switch (c) {
        case 'f': cfgfile = optarg; break;
        case 'c': onecmd = optarg; break;
        case 'b': baud = atol(optarg); break;
        case 'q': g_verbose = 0; break;  /* kept for compatibility; now the default */
        case 'v': g_verbose = 1; break;
        case 'V': printf(APP "\n"); return 0;
        default:
            fprintf(stderr,
                "usage: %s [-b baud] [-v] [-f config | -c 'cmd'] PORT\n"
                "  e.g. %s /dev/ttyUSB0          (interactive)\n"
                "       %s -f logger.conf /dev/ttyUSB0\n"
                "       %s -c version /dev/ttyS1\n", argv[0], argv[0], argv[0], argv[0]);
            return 2;
        }
    }
    if (optind < argc) port = argv[optind];
    if (!port) { fprintf(stderr, "no serial port given\n"); return 2; }

    signal(SIGINT, on_sigint);
    /* diLogger config port is fixed 115200 8N1 */
    g_fd = serial_open(port, baud, 10, 0, 8);
    if (g_fd < 0) return 1;

    int rc;
    if (cfgfile)      rc = run_config(g_fd, cfgfile);
    else if (onecmd)  {
        char wire[384], rx[2048];
        if (!strcmp(onecmd, "dump")) { rc = cmd_dump(g_fd); goto done; }
        if (!strncmp(onecmd, "save", 4) && (onecmd[4] == '\0' || onecmd[4] == ' ')) {
            const char *a = onecmd + 4; while (*a == ' ') a++;
            rc = cmd_save(g_fd, *a ? a : "dilcfg-save.conf"); goto done;
        }
        if (build_command(onecmd, wire, sizeof(wire)) == 0) {
            if (g_verbose) printf("  > %s\n", wire);
            int n = serial_send_and_read(g_fd, wire, 300, rx, sizeof(rx));
            if (n > 0) pretty_print_rx(rx);
            rc = 0;
        } else {
            fprintf(stderr, "bad command/args: %s\n", onecmd);
            rc = 1;
        }
    }
    else rc = run_tui(g_fd);
done:

    close(g_fd);
    return rc;
}
