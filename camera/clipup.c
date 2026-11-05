/*
 * clipup.so - minimal HTTP/1.0 file uploader for the CG721 (no wget/curl needed).
 *
 * Runs as a constructor so it can be invoked without crt/_start:
 *     LD_PRELOAD=/config/clipup.so busybox true
 * Streams one file (a .mp4 clip or .jpg) to a tiny receiver as the body of a
 * POST /upload?name=<name> request, then _exit(0).
 *
 * Env: UP_HOST (dotted IP, required), UP_PORT (default 8099),
 *      UP_FILE (path, required), UP_NAME (query name; default = basename).
 *
 * Symbol-minimal: imports only the exact libc set that mqttpub.so uses
 * (no memcpy/memset/strlen); built with -fno-builtin so clang can't
 * re-synthesize any of them. MIPS o32 / uClibc.
 *
 * Exit: 0 ok, 2 missing env, 3 open fail, 4 empty, 5 socket fail,
 *       6 connect fail, 7 header write fail, 8 body write fail, 9 non-200.
 */

typedef unsigned int   u32;
typedef unsigned short u16;
typedef unsigned char  u8;
typedef unsigned long  size_t;
typedef long           off_t;

extern int   socket(int, int, int);
extern int   connect(int, const void *, u32);
extern long  write(int, const void *, size_t);
extern long  read(int, void *, size_t);
extern int   close(int);
extern int   open(const char *, int, int);
extern char *getenv(const char *);
extern int   atoi(const char *);
extern void  _exit(int);

#define AF_INET     2
#define SOCK_STREAM 2          /* MIPS */

struct sockaddr_in { u16 sin_family; u16 sin_port; u32 sin_addr; u8 pad[8]; };

static u16 be16(u16 v) { return (u16)((v << 8) | (v >> 8)); }
static size_t slen(const char *s){ size_t n=0; if(s) while(s[n]) n++; return n; }

/* byte copy (no memcpy) */
static void cp(u8 *d, const u8 *s, int n) { for (int i = 0; i < n; i++) d[i] = s[i]; }
/* zero (no memset) */
static void zero(u8 *d, int n) { for (int i = 0; i < n; i++) d[i] = 0; }
/* append a C string into buf at *pos (no strcat/strlen libc) */
static void app(char *buf, int *pos, const char *s) { int p = *pos; while (*s) buf[p++] = *s++; *pos = p; }

/* parse "a.b.c.d" into 4 network-order bytes */
static void parse_ip(const char *s, u8 out[4])
{
    int i = 0, v = 0, have = 0;
    out[0]=out[1]=out[2]=out[3]=0;
    for (;; s++) {
        char c = *s;
        if (c >= '0' && c <= '9') { v = v*10 + (c-'0'); have = 1; }
        else { if (i < 4 && have) out[i++] = (u8)v; v = 0; have = 0; if (!c) break; }
    }
}

/* write all bytes */
static int wall(int fd, const u8 *p, int n)
{
    while (n > 0) { long r = write(fd, p, n); if (r <= 0) return -1; p += r; n -= (int)r; }
    return 0;
}

/* unsigned decimal appended into buf at *pos */
static void uapp(char *buf, int *pos, unsigned v)
{
    char t[12]; int n = 0, p = *pos;
    if (v == 0) { buf[p++] = '0'; *pos = p; return; }
    while (v) { t[n++] = (char)('0' + v % 10); v /= 10; }
    while (n) buf[p++] = t[--n];
    *pos = p;
}

/* last path component of p */
static const char *base_name(const char *p)
{
    const char *b = p;
    for (const char *s = p; *s; s++) if (*s == '/') b = s + 1;
    return b;
}

static u8 g_buf[32768];
static char g_hdr[512];

__attribute__((constructor))
static void run(void)
{
    char *host  = getenv("UP_HOST");
    char *ports = getenv("UP_PORT");
    char *file  = getenv("UP_FILE");
    char *name  = getenv("UP_NAME");
    if (!host || !file) _exit(2);
    int port = (ports && *ports) ? atoi(ports) : 8099;
    if (!name || !*name) name = (char *)base_name(file);

    /* pass 1: measure size by reading once (no lseek needed) */
    int fd = open(file, 0, 0);          /* O_RDONLY */
    if (fd < 0) _exit(3);
    unsigned sz = 0; long rr;
    while ((rr = read(fd, g_buf, sizeof(g_buf))) > 0) sz += (unsigned)rr;
    close(fd);
    if (sz == 0) _exit(4);

    int s = socket(AF_INET, SOCK_STREAM, 0);
    if (s < 0) _exit(5);
    struct sockaddr_in a;
    zero((u8 *)&a, sizeof(a));
    a.sin_family = AF_INET;
    a.sin_port   = be16((u16)port);
    u8 ip[4]; parse_ip(host, ip); cp((u8 *)&a.sin_addr, ip, 4);
    if (connect(s, (const void *)&a, sizeof(a)) < 0) { close(s); _exit(6); }

    /* ---- request head ---- */
    int h = 0;
    app(g_hdr, &h, "POST /upload?name=");
    app(g_hdr, &h, name);
    app(g_hdr, &h, " HTTP/1.0\r\nHost: ");
    app(g_hdr, &h, host);
    g_hdr[h++] = ':'; uapp(g_hdr, &h, (unsigned)port);
    app(g_hdr, &h, "\r\nContent-Type: application/octet-stream\r\nContent-Length: ");
    uapp(g_hdr, &h, (unsigned)sz);
    app(g_hdr, &h, "\r\nConnection: close\r\n\r\n");
    if (wall(s, (const u8 *)g_hdr, h)) { close(fd); close(s); _exit(7); }

    /* ---- body (pass 2: reopen and stream) ---- */
    fd = open(file, 0, 0);
    if (fd < 0) { close(s); _exit(3); }
    long r;
    while ((r = read(fd, g_buf, sizeof(g_buf))) > 0)
        if (wall(s, g_buf, (int)r)) { close(fd); close(s); _exit(8); }
    close(fd);

    /* ---- response (look for "200") ---- */
    int rc = 9;
    long got = read(s, g_buf, sizeof(g_buf) - 1);
    if (got > 0) {
        g_buf[got] = 0;
        for (long i = 0; i + 2 < got; i++)
            if (g_buf[i]=='2' && g_buf[i+1]=='0' && g_buf[i+2]=='0') { rc = 0; break; }
    }
    close(s);
    _exit(rc);
}
