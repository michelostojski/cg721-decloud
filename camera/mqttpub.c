/*
 * mqttpub.so - minimal MQTT 3.1.1 publisher for the CG721 (no wget/curl needed).
 *
 * Runs as a constructor so it can be invoked without crt/_start:
 *     LD_PRELOAD=/config/mqttpub.so busybox true
 * Reads config from the environment, connects to the broker, PUBLISHes one
 * retained message (topic = payload file contents), then _exit(0).
 *
 * Env: MQTT_HOST (dotted IP), MQTT_PORT (default 1883), MQTT_TOPIC,
 *      MQTT_FILE (path to JPEG), MQTT_USER, MQTT_PASS (optional).
 *
 * MIPS o32 / uClibc. Build like the hook (links camera libc).
 */

typedef unsigned int   u32;
typedef unsigned short u16;
typedef unsigned char  u8;
typedef unsigned long  size_t;

extern int   socket(int, int, int);
extern int   connect(int, const void *, u32);
extern long  write(int, const void *, size_t);
extern long  read(int, void *, size_t);
extern int   close(int);
extern int   open(const char *, int, int);
extern char *getenv(const char *);
extern int   atoi(const char *);
extern void  _exit(int);
extern void *memcpy(void *, const void *, size_t);

#define AF_INET     2
#define SOCK_STREAM 2          /* MIPS */

struct sockaddr_in { u16 sin_family; u16 sin_port; u32 sin_addr; u8 pad[8]; };

static u16 be16(u16 v) { return (u16)((v << 8) | (v >> 8)); }
static size_t slen(const char *s){ size_t n=0; if(s) while(s[n]) n++; return n; }

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

/* append an MQTT "remaining length" varint; return bytes written */
static int remlen(u8 *o, int len)
{
    int i = 0;
    do { u8 b = len & 0x7f; len >>= 7; if (len) b |= 0x80; o[i++] = b; } while (len);
    return i;
}

static u8 g_payload[262144];

__attribute__((constructor))
static void run(void)
{
    char *host  = getenv("MQTT_HOST");
    char *ports = getenv("MQTT_PORT");
    char *topic = getenv("MQTT_TOPIC");
    char *file  = getenv("MQTT_FILE");
    char *user  = getenv("MQTT_USER");
    char *pass  = getenv("MQTT_PASS");
    if (!host || !topic || !file) _exit(2);
    int port = (ports && *ports) ? atoi(ports) : 1883;

    /* read the JPEG */
    int fd = open(file, 0, 0);
    if (fd < 0) _exit(3);
    int plen = 0, r;
    while ((r = (int)read(fd, g_payload + plen, sizeof(g_payload) - plen)) > 0) {
        plen += r;
        if (plen >= (int)sizeof(g_payload)) break;
    }
    close(fd);
    if (plen <= 0) _exit(4);

    /* connect */
    int s = socket(AF_INET, SOCK_STREAM, 0);
    if (s < 0) _exit(5);
    struct sockaddr_in a;
    memcpy(&a, "\0\0\0\0\0\0\0\0\0\0\0\0\0\0\0\0", 16);
    a.sin_family = AF_INET;
    a.sin_port   = be16((u16)port);
    u8 ip[4]; parse_ip(host, ip); memcpy(&a.sin_addr, ip, 4);
    if (connect(s, &a, sizeof(a)) < 0) _exit(6);

    /* ---- CONNECT ---- */
    const char *cid = "cg721pub";
    int ulen = (int)slen(user), plen2 = (int)slen(pass);
    u8 flags = 0x02;                         /* clean session */
    if (user && ulen) flags |= 0x80;
    if (pass && plen2) flags |= 0x40;
    u8 vh[10]; int vi = 0;
    vh[vi++]=0; vh[vi++]=4; vh[vi++]='M'; vh[vi++]='Q'; vh[vi++]='T'; vh[vi++]='T';
    vh[vi++]=4;                              /* protocol level 3.1.1 */
    vh[vi++]=flags;
    vh[vi++]=0; vh[vi++]=60;                 /* keepalive 60s */
    int pl = 2 + (int)slen(cid);
    if (flags & 0x80) pl += 2 + ulen;
    if (flags & 0x40) pl += 2 + plen2;
    int body = vi + pl;
    u8 hdr[8]; int hi = 0; hdr[hi++] = 0x10; hi += remlen(hdr + hi, body);
    if (wall(s, hdr, hi) || wall(s, vh, vi)) _exit(7);
    u8 f2[4]; int ci = (int)slen(cid);
    f2[0]=ci>>8; f2[1]=ci&0xff; wall(s,f2,2); wall(s,(const u8*)cid,ci);
    if (flags & 0x80) { f2[0]=ulen>>8; f2[1]=ulen&0xff; wall(s,f2,2); wall(s,(const u8*)user,ulen); }
    if (flags & 0x40) { f2[0]=plen2>>8; f2[1]=plen2&0xff; wall(s,f2,2); wall(s,(const u8*)pass,plen2); }

    /* read CONNACK (best-effort) */
    u8 ack[4]; read(s, ack, 4);

    /* ---- PUBLISH (QoS0, retain) ---- */
    int tl = (int)slen(topic);
    int rem = 2 + tl + plen;                 /* topic len field + topic + payload */
    u8 ph[8]; int pi = 0; ph[pi++] = 0x31;   /* PUBLISH | retain */
    pi += remlen(ph + pi, rem);
    if (wall(s, ph, pi)) _exit(8);
    u8 tlh[2]; tlh[0]=tl>>8; tlh[1]=tl&0xff;
    if (wall(s, tlh, 2) || wall(s, (const u8*)topic, tl)) _exit(9);
    if (wall(s, g_payload, plen)) _exit(10);

    close(s);
    _exit(0);
}
