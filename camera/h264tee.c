/*
 * h264tee.so  -  LD_PRELOAD hook for Ingenic T31 (ZRT) sdkencode.
 *
 * Interposes IMP_Encoder_GetStream(). Tees the encoded NAL packets of BOTH
 * encoder channels out to TCP clients:
 *     port 8900 -> chn 0 (main, 1080p, cloud/on-demand)
 *     port 8901 -> chn 1 (sub,  720p,  continuous) <- use this for Frigate
 *
 * SAFETY: complete-NAL sends with a 0.5s socket timeout (never truncates a
 * NAL, can't stall the encoder on a dead client); SIGPIPE-safe; never crashes
 * sdkencode (defensive, read-only access to the ring); new clients start at an
 * SPS/IDR boundary (H264 and H265).
 *
 * IMPEncoderStream layout (from libimp): +0x00 phyAddr +0x04 virAddr
 *   +0x08 streamSize +0x0c pack* +0x10 packCount +0x14 seq ; pack stride 0x20,
 *   pack[i]: +0x00 offset(rel virAddr) +0x04 length.
 *
 * Env: H264TEE_PORT base port (default 8900); H264TEE_LOG=1 for struct dump.
 * MIPS o32/uClibc constants used explicitly.
 */

typedef unsigned int   u32;
typedef unsigned short u16;
typedef unsigned char  u8;
typedef unsigned long  size_t;
typedef long           ssize_t;

extern void *dlsym(void *handle, const char *symbol);
extern void *dlopen(const char *file, int mode);
extern int   socket(int, int, int);
extern int   setsockopt(int, int, int, const void *, u32);
extern int   bind(int, const void *, u32);
extern int   listen(int, int);
extern int   accept(int, void *, void *);
extern ssize_t send(int, const void *, size_t, int);
extern int   close(int);
extern int   open(const char *, int, int);
extern ssize_t write(int, const void *, size_t);
extern void *memcpy(void *, const void *, size_t);
extern char *getenv(const char *);
extern int   atoi(const char *);
extern int   pthread_create(void *, const void *, void *(*)(void *), void *);
extern void  usleep(unsigned);
extern int   snprintf(char *, size_t, const char *, ...);

#define RTLD_NEXT   ((void *)-1L)
#define RTLD_NOW    0x0002
#define RTLD_GLOBAL 0x0100

#define AF_INET        2
#define SOCK_STREAM    2          /* MIPS */
#define SOL_SOCKET     0xffff     /* MIPS */
#define SO_REUSEADDR   0x0004     /* MIPS */
#define SO_SNDTIMEO    0x1005     /* MIPS */
#define MSG_NOSIGNAL   0x4000
#define O_WRONLY       0x0001
#define O_CREAT        0x0100
#define O_APPEND       0x0008

struct sockaddr_in { u16 sin_family; u16 sin_port; u32 sin_addr; u8 pad[8]; };
struct timeval { long tv_sec; long tv_usec; };

#define NCH 2                     /* channels 0 (main) and 1 (sub) */

int IMP_Encoder_GetStream(int chn, void *stream, int block);  /* fwd decl */
static int (*real_getstream)(int, void *, int) = 0;

static volatile int g_cli[NCH]    = { -1, -1 };
static volatile int g_needidr[NCH] = { 1, 1 };
static u8  g_vps[NCH][512], g_sps[NCH][512], g_pps[NCH][512];  /* cached param sets */
static u32 g_vlen[NCH] = {0,0}, g_slen[NCH] = {0,0}, g_plen[NCH] = {0,0};
static unsigned long g_tid[NCH];
static int g_portbase = 8900;
static int g_log = 0, g_logn = 0, g_started = 0, g_diag = 0;
static int g_dumped[NCH] = { 0, 0 };

static u16 htons_(u16 v) { return (u16)((v << 8) | (v >> 8)); }

static void diag(const char *s){ int n=0; while(s[n])n++; write(2,s,n); }
static void diagp(const char *t, void *p){ char b[64]; int n=snprintf(b,sizeof(b),"%s=%p\n",t,p); write(2,b,n); }

static void resolve_real(void)
{
    void *self = (void *)&IMP_Encoder_GetStream;
    void *p = dlsym(RTLD_NEXT, "IMP_Encoder_GetStream");
    if (g_diag < 3) diagp("[h264tee] RTLD_NEXT", p);
    if (p && p != self) { real_getstream = (int(*)(int,void*,int))p; return; }
    void *h = dlopen("/usr/lib/libimp.so", RTLD_NOW | RTLD_GLOBAL);
    if (h) {
        p = dlsym(h, "IMP_Encoder_GetStream");
        if (g_diag < 3) diagp("[h264tee] dlopen-libimp", p);
        if (p && p != self) { real_getstream = (int(*)(int,void*,int))p; return; }
    }
    if (g_diag < 3) diag("[h264tee] FAILED to resolve real GetStream\n");
}

static int first_nal_byte(const u8 *p, u32 n)
{
    u32 i;
    if (!p) return -1;
    if (n > 0x200000) n = 0x200000;
    for (i = 0; i + 4 < n; i++) {
        if (p[i] == 0 && p[i + 1] == 0) {
            if (p[i + 2] == 1)            return p[i + 3];
            if (p[i + 2] == 0 && p[i + 3] == 1 && i + 4 < n) return p[i + 4];
        }
    }
    return -1;
}
static int is_key_nal(int b)
{
    if (b < 0) return 0;
    int h264 = b & 0x1f;
    int h265 = (b >> 1) & 0x3f;
    if (h264 == 5 || h264 == 7 || h264 == 8) return 1;
    if (h265 == 19 || h265 == 20 || h265 == 21 || h265 == 32 || h265 == 33 || h265 == 34) return 1;
    return 0;
}

/* send a complete buffer to channel ch's client; drop client on error/timeout */
static void tee_send(int ch, const void *buf, u32 len)
{
    int fd = g_cli[ch];
    if (fd < 0 || !buf || !len) return;
    const u8 *p = (const u8 *)buf; u32 left = len;
    while (left) {
        ssize_t r = send(fd, p, left, MSG_NOSIGNAL);
        if (r <= 0) { close(fd); if (g_cli[ch] == fd) g_cli[ch] = -1; g_needidr[ch] = 1; return; }
        p += r; left -= (u32)r;
    }
}

static void *accept_thread(void *arg)
{
    int ch = (int)(long)arg;
    int port = g_portbase + ch;
    int s = socket(AF_INET, SOCK_STREAM, 0);
    if (s < 0) return 0;
    int one = 1;
    setsockopt(s, SOL_SOCKET, SO_REUSEADDR, &one, sizeof(one));
    struct sockaddr_in a;
    memcpy(&a, "\0\0\0\0\0\0\0\0\0\0\0\0\0\0\0\0", 16);
    a.sin_family = AF_INET; a.sin_port = htons_((u16)port); a.sin_addr = 0;
    if (bind(s, &a, sizeof(a)) < 0) { close(s); return 0; }
    if (listen(s, 4) < 0)           { close(s); return 0; }
    for (;;) {
        int c = accept(s, 0, 0);
        if (c < 0) { usleep(200000); continue; }
        struct timeval tv; tv.tv_sec = 0; tv.tv_usec = 500000;
        setsockopt(c, SOL_SOCKET, SO_SNDTIMEO, &tv, sizeof(tv));
        int old = g_cli[ch]; g_cli[ch] = -1;
        if (old >= 0) close(old);
        g_needidr[ch] = 1; g_dumped[ch] = 0;
        g_cli[ch] = c;
    }
    return 0;
}

static void start_once(void)
{
    if (g_started) return;
    g_started = 1;
    char *e;
    if ((e = getenv("H264TEE_PORT")) && *e) g_portbase = atoi(e);
    if ((e = getenv("H264TEE_LOG"))  && *e) g_log = atoi(e);
    int i;
    for (i = 0; i < NCH; i++)
        pthread_create(&g_tid[i], 0, accept_thread, (void *)(long)i);
    { char b[64]; int n=snprintf(b,sizeof(b),"[h264tee] listen :%d(ch0) :%d(ch1)\n",g_portbase,g_portbase+1); write(2,b,n); }
}

int IMP_Encoder_GetStream(int chn, void *stream, int block)
{
    if (!real_getstream) resolve_real();
    if (!real_getstream) { if (g_diag < 3) { diag("[h264tee] real NULL -> -1\n"); g_diag++; } return -1; }

    int ret = real_getstream(chn, stream, block);
    if (g_diag < 6) { char b[72]; int n=snprintf(b,sizeof(b),"[h264tee] call chn=%d ret=%d\n",chn,ret); write(2,b,n); g_diag++; }

    if (ret != 0 || !stream) return ret;
    start_once();
    if (chn < 0 || chn >= NCH) return ret;
    u32 *st  = (u32 *)stream;
    u32 vir  = st[1], ssz = st[2], pk = st[3], pc = st[4];
    if (!pk || !pc || pc > 64) return ret;
    u32 i;

    /* pass 1 (ALWAYS, even with no client): cache VPS/SPS/PPS into per-type
       slots so a full parameter set is ready the instant a client attaches,
       and note whether this AU carries an IDR slice. */
    int has_idr = 0, has_ps = 0;
    for (i = 0; i < pc; i++) {
        u32 *e = (u32 *)(pk + i * 0x20);
        u32 off = e[0], len = e[1];
        if (!len || len > ssz + 0x100000) continue;
        const u8 *dp = (off < ssz) ? (const u8 *)(vir + off) : (const u8 *)off;
        int b = first_nal_byte(dp, len);
        if (b < 0) continue;
        int h264 = b & 0x1f, h265 = (b >> 1) & 0x3f;
        u8 *slot = 0; u32 *slen = 0; u32 cap = 0;
        if      (h265 == 32)                 { slot = g_vps[chn]; slen = &g_vlen[chn]; cap = sizeof(g_vps[chn]); }
        else if (h265 == 33 || h264 == 7)    { slot = g_sps[chn]; slen = &g_slen[chn]; cap = sizeof(g_sps[chn]); }
        else if (h265 == 34 || h264 == 8)    { slot = g_pps[chn]; slen = &g_plen[chn]; cap = sizeof(g_pps[chn]); }
        if (slot && len <= cap) {
            if (off < ssz && off + len > ssz) { u32 f = ssz - off;
                memcpy(slot, (const u8 *)(vir + off), f);
                memcpy(slot + f, (const u8 *)vir, len - f);
            } else memcpy(slot, dp, len);
            *slen = len; has_ps = 1;
        }
        if ((h265 >= 19 && h265 <= 21) || h264 == 5) has_idr = 1;
    }

    if (g_cli[chn] < 0) return ret;

    if (!g_dumped[chn]) {
        g_dumped[chn] = 1;
        char b[160]; int n = snprintf(b, sizeof(b),
            "[h264tee] ch%d vir=%08x ssz=%u pc=%u pack0: %08x %08x vps=%u sps=%u pps=%u\n",
            chn, vir, ssz, pc, ((u32*)pk)[0], ((u32*)pk)[1],
            g_vlen[chn], g_slen[chn], g_plen[chn]);
        write(2, b, n);
    }

    /* a new client starts only at an IDR; if that IDR AU didn't itself carry
       the parameter sets, prepend the cached VPS+SPS+PPS so it can decode. */
    if (g_needidr[chn]) {
        if (!has_idr) return ret;
        if (!has_ps) {
            if (g_vlen[chn]) tee_send(chn, (const u8 *)g_vps[chn], g_vlen[chn]);
            if (g_slen[chn]) tee_send(chn, (const u8 *)g_sps[chn], g_slen[chn]);
            if (g_plen[chn]) tee_send(chn, (const u8 *)g_pps[chn], g_plen[chn]);
        }
        g_needidr[chn] = 0;
    }

    /* forward every pack as raw Annex-B (handle ring wrap) */
    for (i = 0; i < pc; i++) {
        u32 *e = (u32 *)(pk + i * 0x20);
        u32 off = e[0], len = e[1];
        if (!len || len > ssz + 0x100000) continue;
        if (off < ssz) {
            if (off + len > ssz) {
                u32 first = ssz - off;
                tee_send(chn, (const u8 *)(vir + off), first);
                tee_send(chn, (const u8 *)vir, len - first);
            } else {
                tee_send(chn, (const u8 *)(vir + off), len);
            }
        } else {
            tee_send(chn, (const u8 *)off, len);
        }
    }
    return ret;
}
