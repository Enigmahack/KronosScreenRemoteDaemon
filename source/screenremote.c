/*
 * screenremote.c — Kronos framebuffer streaming daemon
 * Not yet tested on Nautilus, but should work with minor tweaks if needed.
 *
 * Streams /dev/fb1 (8bpp, 800×600) over TCP port 7373 (default; set by config).
 * Mirrors fb1 to /dev/fb0 (VGA out) when /korg/rw/screenremote/.mirror_enable exists.
 *
 * Stream handshake (TCP port 7373):
 *   Client → Server: MAGIC[4]="KSCR" + 0x02(ver) + mode(1) + fps(1) + ulen(1) + plen(1)
 *                    + username[ulen] + password[plen]  (password validated against screenremote.cfg)
 *     mode: 0x01=Pull (server sends at fps), 0x02=Change (server sends on fb change)
 *   Server → Client: MAGIC[4] + status(1)  [+ width_LE16 + height_LE16 + 256×RGB8 if status=0x00]
 *     status: 0x00=ok  0x01=auth_fail  0x02=ftp_service_unavailable
 *   Frames: [len_LE32][pixel_data]  (full) or dirty-rect PackBits RLE
 *
 * Control port 7374 (text line commands, newline-terminated):
 *   CTRL_PERSIST           — open a persistent session; server keeps the connection open
 *   MIRROR_ON / MIRROR_OFF — enable / disable VGA mirror output
 *   TOUCH nx ny            — tap at normalised float coords (press + release)
 *   TOUCH_DOWN nx ny       — pen-down only
 *   TOUCH_MOVE nx ny       — pen-move (client coalesces consecutive moves)
 *   TOUCH_UP nx ny         — pen-up only
 *   BUTTON name            — press + release a named front-panel button (see btn_table[])
 *   CHORD name1 name2      — press name1, press name2, release name2, release name1
 *   WHEEL CW|CCW           — one data-wheel tick clockwise or counter-clockwise
 *   SLIDER n value         — set CC slider/knob n (1–8) to value (0–127)
 *                            Effect depends on active Control Assign page:
 *                            RT KNOBS/KARMA → moves knob n; slider-active → moves slider n
 *   VSLIDER value          — set value slider position (0–127)
 *   KEY code val           — raw key inject: code 1–511, val 0=release 1=press
 *   REFRESH                — force full-frame resend (clears shadow_valid)
 *   SS_TIMEOUT n           — set screensaver timeout at runtime (seconds; 0 = disable)
 *   STATE                  → MODE=N\n  (0=init 1=Setlist 2=Combi 3=Program 4=Sequence 5=Sampling 6=Global 7=Disk)
 *   VERSION                → VER=x.x.x BUILD=xxx\n
 *   SYSINFO                → multi-line key=value block terminated by OK\n
 *                            (UPTIME, LOAD, MEM_*, CPU_*, AUDIO_*, DISK_*, USB_*, TEMP*, FAN*, MODE)
 *
 * UDP discovery port 7372 (fixed, never configurable):
 *   Client sends: "KSCR?" (5+ bytes)
 *   Daemon replies: "KSCR SP=<stream_port> CP=<ctrl_port>\n"
 *
 * Access control: ctrl commands are only accepted from the authenticated stream client's IP.
 *   Control connections are rejected if no authenticated stream client is connected.
 *
 * Startup: /korg/rw/kronosmods_init launches this via OA.clonos.rc (rooted) or inittab/clontab.
 * Config: /korg/rw/screenremote/screenremote.cfg  (stream_port, ctrl_port, screensaver_timeout)
 * All runtime files live under /korg/rw/screenremote/ (binary, extracted .ko modules, config, flag files).
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include <signal.h>
#include <errno.h>
#include <fcntl.h>
#include <unistd.h>
#include <time.h>
#include <sys/mman.h>
#include <sys/ioctl.h>
#include <sys/socket.h>
#include <sys/select.h>
#include <sys/stat.h>
#include <sys/statvfs.h>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <arpa/inet.h>
#include <ifaddrs.h>
#include <linux/fb.h>
#include <linux/uinput.h>
#include <sys/syscall.h>
#include "palette_data.h"
#include "vkbd_ko.h"

/* ── keyboard injection (uinput fallback) ───────────────────── */
#define KBD_EV_SYN  0
#define KBD_EV_KEY  1

/* ── Version ─────────────────────────────────────────────────── */
#define SCREENREMOTE_VERSION "1.6.1"
#ifndef BUILD_ID
#define BUILD_ID "dev"
#endif

/* ── Config ─────────────────────────────────────────────────── */
#define SCREENREMOTE_DIR  "/korg/rw/screenremote"
#define VKBD_KO           SCREENREMOTE_DIR "/vkbd.ko"

#define FB_SRC       "/dev/fb1"
#define FB_DST       "/dev/fb0"
#define STREAM_PORT  7373       /* default; overridden by config file */
#define CTRL_PORT    7374       /* default; overridden by config file */
#define DISC_PORT    7372       /* fixed UDP discovery port — never configurable */
#define CFG_PATH     SCREENREMOTE_DIR "/screenremote.cfg"
#define MIRROR_FLAG  SCREENREMOTE_DIR "/.mirror_enable"

static int  g_stream_port = STREAM_PORT;
static int  g_ctrl_port   = CTRL_PORT;
#define PAL_ENTRIES  256
#define FPS_MAX      15
#define MODE_PULL    0x01
#define MODE_CHANGE  0x02

static const uint8_t MAGIC[4] = {'K','S','C','R'};

/* ── Framebuffer state ──────────────────────────────────────── */
static int       fb1_fd = -1,  fb0_fd = -1;
static uint8_t  *fb1_map = NULL, *fb0_map = NULL;
static uint32_t  fb1_stride, fb0_stride;
static uint32_t  fb_w, fb_h;           /* 800, 600 */
static uint32_t  frame_bytes;          /* fb_w * fb_h */

static uint16_t  pal_r[PAL_ENTRIES];   /* raw palette — used for streaming handshake */
static uint16_t  pal_g[PAL_ENTRIES];
static uint16_t  pal_b[PAL_ENTRIES];
static uint16_t  pal_t[PAL_ENTRIES];

static uint8_t  *shadow  = NULL;       /* last-sent frame, fb_w*fb_h bytes */
static uint8_t  *staging = NULL;       /* current-tick capture of fb1, fb_w*fb_h bytes */
static uint8_t  *rle_buf = NULL;       /* PackBits encode scratch, 2*frame_bytes */
static int       shadow_valid = 0;

/* ── Mirror state ───────────────────────────────────────────── */
static int       mirror_on = 0;

/* ── Screensaver (VGA/fb0 only) ─────────────────────────────── */
#define SS_TIMEOUT_DEF  300   /* seconds idle before blanking; 0=disabled */
#define SS_CHECK_S        5   /* how often to sample fb1 for changes */
#define SS_SAMPLE_N      16   /* pixel positions sampled per check */

static int      g_ss_timeout  = SS_TIMEOUT_DEF;
static int      ss_active     = 0;    /* 1 = fb0 is currently blanked */
static time_t   ss_last_chg   = 0;   /* last time fb1 pixels changed */
static time_t   last_ss_chk   = 0;
static uint8_t  ss_prev[SS_SAMPLE_N];
static int      ss_prev_valid = 0;

/* ── Mode state ─────────────────────────────────────────────── */
static uint32_t g_mode        = 0;   /* 0=init 1=Setlist 2=Combi 3=Program
                                         4=Sequence 5=Sampling 6=Global 7=Disk */

/* ── Sysinfo CPU snapshot (populated on-demand, only while connected) ─ */
typedef struct {
    unsigned long user, nice, sys, idle, iowait, irq, softirq;
} cpu_snap_t;
#define SI_NCPU 4
static cpu_snap_t g_si_prev[SI_NCPU + 1]; /* [0]=aggregate  [1..4]=per-cpu  */
static int        g_si_prev_valid = 0;

/* ── Touch / button injection ───────────────────────────────── */
static int touch_fd = -1;  /* fd to /dev/rtf5 O_WRONLY — injects into Eva's FIFO */
static int vkbd_fd  = -1;  /* fd to /proc/.vkbd (vkbd.ko virtual keyboard) */
static int kbd_fd   = -1;  /* fd to physical USB keyboard evdev node (fallback) */

/* Button table — pkt[2]=dev, pkt[3]=code, pkt[4]=0x7f/0x00 for press/release */
struct btn_def { const char *name; uint32_t dev; uint32_t code; };
static const struct btn_def btn_table[] = {
    /* Exit / Enter */
    { "EXIT",       0x06u, 0x02u },
    { "ENTER",      0x06u, 0x10u },
    /* Mode select (Radio group) */
    { "SETLIST",    0x07u, 0x0eu },
    { "COMBI",      0x07u, 0x08u },
    { "PROGRAM",    0x07u, 0x09u },
    { "SEQUENCE",   0x07u, 0x0au },
    { "SAMPLING",   0x07u, 0x0bu },
    { "GLOBAL",     0x07u, 0x0cu },
    { "DISK",       0x07u, 0x0du },
    /* Utility */
    { "HELP",       0x08u, 0x00u },
    { "COMPARE",    0x08u, 0x01u },
    { "RESET",      0x08u, 0x02u },
    /* Number pad */
    { "NUM_DASH",   0x06u, 0x05u },
    { "NUM0",       0x06u, 0x06u },
    { "NUM_DOT",    0x06u, 0x04u },
    { "NUM1",       0x06u, 0x07u },
    { "NUM2",       0x06u, 0x08u },
    { "NUM3",       0x06u, 0x09u },
    { "NUM4",       0x06u, 0x0au },
    { "NUM5",       0x06u, 0x0bu },
    { "NUM6",       0x06u, 0x0cu },
    { "NUM7",       0x06u, 0x0du },
    { "NUM8",       0x06u, 0x0eu },
    { "NUM9",       0x06u, 0x0fu },
    /* Value increment/decrement */
    { "INC",        0x06u, 0x00u },
    { "DEC",        0x06u, 0x01u },
    /* Mix Play buttons */
    { "MP1",        0x04u, 0x00u },
    { "MP2",        0x04u, 0x01u },
    { "MP3",        0x04u, 0x02u },
    { "MP4",        0x04u, 0x03u },
    { "MP5",        0x04u, 0x04u },
    { "MP6",        0x04u, 0x05u },
    { "MP7",        0x04u, 0x06u },
    { "MP8",        0x04u, 0x07u },
    /* Mix Select buttons */
    { "MS1",        0x05u, 0x00u },
    { "MS2",        0x05u, 0x01u },
    { "MS3",        0x05u, 0x02u },
    { "MS4",        0x05u, 0x03u },
    { "MS5",        0x05u, 0x04u },
    { "MS6",        0x05u, 0x05u },
    { "MS7",        0x05u, 0x06u },
    { "MS8",        0x05u, 0x07u },
    /* Bank buttons */
    { "BANK_IA",    0x09u, 0x00u },
    { "BANK_IB",    0x09u, 0x01u },
    { "BANK_IC",    0x09u, 0x02u },
    { "BANK_ID",    0x09u, 0x03u },
    { "BANK_IE",    0x09u, 0x04u },
    { "BANK_IF",    0x09u, 0x05u },
    { "BANK_IG",    0x09u, 0x06u },
    { "BANK_UA",    0x09u, 0x07u },
    { "BANK_UB",    0x09u, 0x08u },
    { "BANK_UC",    0x09u, 0x09u },
    { "BANK_UD",    0x09u, 0x0au },
    { "BANK_UE",    0x09u, 0x0bu },
    { "BANK_UF",    0x09u, 0x0cu },
    { "BANK_UG",    0x09u, 0x0du },
    /* Sequencer controls */
    { "SEQ_START",  0x0bu, 0x00u },
    { "SEQ_REC",    0x0bu, 0x01u },
    { "SEQ_LOCATE", 0x0bu, 0x02u },
    { "SEQ_FF",     0x0bu, 0x03u },
    { "SEQ_REW",    0x0bu, 0x04u },
    { "SEQ_PAUSE",  0x0bu, 0x05u },
    { "TAP_TEMPO",  0x0bu, 0x06u },
    /* Sampling controls */
    { "SMPL_REC",   0x0au, 0x00u },
    { "SMPL_START", 0x0au, 0x01u },
    /* Channel strip */
    { "MIX_KNOBS",  0x07u, 0x05u },
    /* SOLO fires on release; daemon sends press+release so release triggers it */
    { "SOLO",       0x07u, 0x06u },
    { NULL, 0u, 0u }
};

/* ── Access control ─────────────────────────────────────────── */
/* IP (network byte order) of the current stream client. Only that host is
 * allowed to send control commands. 0 = no stream client yet → allow any. */
static uint32_t g_ctrl_allowed_ip = 0;

/* Currently bound listen address (network byte order). Tracked so we can
 * detect IP changes and rebind the stream/ctrl listeners. */
static uint32_t g_bound_ip = INADDR_ANY;

/* ── Persistent control connection ──────────────────────────── */
static int  ctrl_fd     = -1;   /* accepted persistent ctrl socket, -1 if none */
static char ctrl_lb[2048];      /* partial line accumulation buffer */
static int  ctrl_lb_n   = 0;    /* bytes in ctrl_lb */

/* ── Signal flag ────────────────────────────────────────────── */
static volatile sig_atomic_t g_exit = 0;

static void sig_exit(int sig) { (void)sig; g_exit = 1; }

/* ── Kernel message log (dmesg) ─────────────────────────────── */
/* ── I/O helper ─────────────────────────────────────────────── */
static int write_all(int fd, const void *buf, size_t n)
{
    const uint8_t *p = buf;
    while (n > 0) {
        ssize_t r = write(fd, p, n);
        if (r <= 0) return -1;
        p += r; n -= (size_t)r;
    }
    return 0;
}

/* ── Embedded SHA-512 + SHA-512 crypt ($6$) ─────────────────────────────────
 * crypt() was removed from glibc libc.a in 2.28; embed it to avoid -lcrypt. */

#define SR_R64(x,n)     (((x)>>(n))|((x)<<(64-(n))))
#define SR_CH(e,f,g)    (((e)&(f))^((~(e))&(g)))
#define SR_MAJ(a,b,c)   (((a)&(b))^((a)&(c))^((b)&(c)))
#define SR_BSIG0(x)     (SR_R64(x,28)^SR_R64(x,34)^SR_R64(x,39))
#define SR_BSIG1(x)     (SR_R64(x,14)^SR_R64(x,18)^SR_R64(x,41))
#define SR_SSIG0(x)     (SR_R64(x, 1)^SR_R64(x, 8)^((x)>>7))
#define SR_SSIG1(x)     (SR_R64(x,19)^SR_R64(x,61)^((x)>>6))

static const uint64_t S512K[80] = {
    UINT64_C(0x428a2f98d728ae22),UINT64_C(0x7137449123ef65cd),
    UINT64_C(0xb5c0fbcfec4d3b2f),UINT64_C(0xe9b5dba58189dbbc),
    UINT64_C(0x3956c25bf348b538),UINT64_C(0x59f111f1b605d019),
    UINT64_C(0x923f82a4af194f9b),UINT64_C(0xab1c5ed5da6d8118),
    UINT64_C(0xd807aa98a3030242),UINT64_C(0x12835b0145706fbe),
    UINT64_C(0x243185be4ee4b28c),UINT64_C(0x550c7dc3d5ffb4e2),
    UINT64_C(0x72be5d74f27b896f),UINT64_C(0x80deb1fe3b1696b1),
    UINT64_C(0x9bdc06a725c71235),UINT64_C(0xc19bf174cf692694),
    UINT64_C(0xe49b69c19ef14ad2),UINT64_C(0xefbe4786384f25e3),
    UINT64_C(0x0fc19dc68b8cd5b5),UINT64_C(0x240ca1cc77ac9c65),
    UINT64_C(0x2de92c6f592b0275),UINT64_C(0x4a7484aa6ea6e483),
    UINT64_C(0x5cb0a9dcbd41fbd4),UINT64_C(0x76f988da831153b5),
    UINT64_C(0x983e5152ee66dfab),UINT64_C(0xa831c66d2db43210),
    UINT64_C(0xb00327c898fb213f),UINT64_C(0xbf597fc7beef0ee4),
    UINT64_C(0xc6e00bf33da88fc2),UINT64_C(0xd5a79147930aa725),
    UINT64_C(0x06ca6351e003826f),UINT64_C(0x142929670a0e6e70),
    UINT64_C(0x27b70a8546d22ffc),UINT64_C(0x2e1b21385c26c926),
    UINT64_C(0x4d2c6dfc5ac42aed),UINT64_C(0x53380d139d95b3df),
    UINT64_C(0x650a73548baf63de),UINT64_C(0x766a0abb3c77b2a8),
    UINT64_C(0x81c2c92e47edaee6),UINT64_C(0x92722c851482353b),
    UINT64_C(0xa2bfe8a14cf10364),UINT64_C(0xa81a664bbc423001),
    UINT64_C(0xc24b8b70d0f89791),UINT64_C(0xc76c51a30654be30),
    UINT64_C(0xd192e819d6ef5218),UINT64_C(0xd69906245565a910),
    UINT64_C(0xf40e35855771202a),UINT64_C(0x106aa07032bbd1b8),
    UINT64_C(0x19a4c116b8d2d0c8),UINT64_C(0x1e376c085141ab53),
    UINT64_C(0x2748774cdf8eeb99),UINT64_C(0x34b0bcb5e19b48a8),
    UINT64_C(0x391c0cb3c5c95a63),UINT64_C(0x4ed8aa4ae3418acb),
    UINT64_C(0x5b9cca4f7763e373),UINT64_C(0x682e6ff3d6b2b8a3),
    UINT64_C(0x748f82ee5defb2fc),UINT64_C(0x78a5636f43172f60),
    UINT64_C(0x84c87814a1f0ab72),UINT64_C(0x8cc702081a6439ec),
    UINT64_C(0x90befffa23631e28),UINT64_C(0xa4506cebde82bde9),
    UINT64_C(0xbef9a3f7b2c67915),UINT64_C(0xc67178f2e372532b),
    UINT64_C(0xca273eceea26619c),UINT64_C(0xd186b8c721c0c207),
    UINT64_C(0xeada7dd6cde0eb1e),UINT64_C(0xf57d4f7fee6ed178),
    UINT64_C(0x06f067aa72176fba),UINT64_C(0x0a637dc5a2c898a6),
    UINT64_C(0x113f9804bef90dae),UINT64_C(0x1b710b35131c471b),
    UINT64_C(0x28db77f523047d84),UINT64_C(0x32caab7b40c72493),
    UINT64_C(0x3c9ebe0a15c9bebc),UINT64_C(0x431d67c49c100d4c),
    UINT64_C(0x4cc5d4becb3e42b6),UINT64_C(0x597f299cfc657e2a),
    UINT64_C(0x5fcb6fab3ad6faec),UINT64_C(0x6c44198c4a475817)
};

typedef struct { uint64_t h[8]; uint64_t total; unsigned blen; uint8_t buf[128]; } s512_ctx;

static void s512_compress(s512_ctx *c, const uint8_t *blk)
{
    uint64_t w[80], a,b,cc,d,e,f,g,h,t1,t2; int i;
    for (i=0;i<16;i++)
        w[i]=((uint64_t)blk[i*8]<<56)|((uint64_t)blk[i*8+1]<<48)
            |((uint64_t)blk[i*8+2]<<40)|((uint64_t)blk[i*8+3]<<32)
            |((uint64_t)blk[i*8+4]<<24)|((uint64_t)blk[i*8+5]<<16)
            |((uint64_t)blk[i*8+6]<<8)|blk[i*8+7];
    for (i=16;i<80;i++) w[i]=SR_SSIG1(w[i-2])+w[i-7]+SR_SSIG0(w[i-15])+w[i-16];
    a=c->h[0];b=c->h[1];cc=c->h[2];d=c->h[3];
    e=c->h[4];f=c->h[5];g=c->h[6];h=c->h[7];
    for (i=0;i<80;i++){
        t1=h+SR_BSIG1(e)+SR_CH(e,f,g)+S512K[i]+w[i];
        t2=SR_BSIG0(a)+SR_MAJ(a,b,cc);
        h=g;g=f;f=e;e=d+t1;d=cc;cc=b;b=a;a=t1+t2;
    }
    c->h[0]+=a;c->h[1]+=b;c->h[2]+=cc;c->h[3]+=d;
    c->h[4]+=e;c->h[5]+=f;c->h[6]+=g;c->h[7]+=h;
}

static void s512_init(s512_ctx *c)
{
    c->h[0]=UINT64_C(0x6a09e667f3bcc908);c->h[1]=UINT64_C(0xbb67ae8584caa73b);
    c->h[2]=UINT64_C(0x3c6ef372fe94f82b);c->h[3]=UINT64_C(0xa54ff53a5f1d36f1);
    c->h[4]=UINT64_C(0x510e527fade682d1);c->h[5]=UINT64_C(0x9b05688c2b3e6c1f);
    c->h[6]=UINT64_C(0x1f83d9abfb41bd6b);c->h[7]=UINT64_C(0x5be0cd19137e2179);
    c->total=0;c->blen=0;
}

static void s512_feed(s512_ctx *c, const void *data, size_t len)
{
    const uint8_t *p=(const uint8_t*)data;
    while(len){ size_t sp=128-c->blen,tk=len<sp?len:sp;
        memcpy(c->buf+c->blen,p,tk);
        c->blen+=(unsigned)tk;c->total+=tk;p+=tk;len-=tk;
        if(c->blen==128){s512_compress(c,c->buf);c->blen=0;}
    }
}

static void s512_done(s512_ctx *c, uint8_t out[64])
{
    uint64_t bits=c->total*8; uint8_t pad=0x80; int i;
    uint8_t lb[16]={0,0,0,0,0,0,0,0,
        (uint8_t)(bits>>56),(uint8_t)(bits>>48),(uint8_t)(bits>>40),(uint8_t)(bits>>32),
        (uint8_t)(bits>>24),(uint8_t)(bits>>16),(uint8_t)(bits>>8),(uint8_t)bits};
    s512_feed(c,&pad,1); pad=0;
    while(c->blen!=112) s512_feed(c,&pad,1);
    s512_feed(c,lb,16);
    for(i=0;i<8;i++){
        out[i*8+0]=(uint8_t)(c->h[i]>>56);out[i*8+1]=(uint8_t)(c->h[i]>>48);
        out[i*8+2]=(uint8_t)(c->h[i]>>40);out[i*8+3]=(uint8_t)(c->h[i]>>32);
        out[i*8+4]=(uint8_t)(c->h[i]>>24);out[i*8+5]=(uint8_t)(c->h[i]>>16);
        out[i*8+6]=(uint8_t)(c->h[i]>>8); out[i*8+7]=(uint8_t)c->h[i];
    }
}

/* SHA-512 crypt — Drepper algorithm, $6$ hashes only */
static const char S6B64[] =
    "./0123456789ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz";

static void s6b64(char **p, unsigned b2, unsigned b1, unsigned b0, int n)
{
    unsigned v=(b2<<16)|(b1<<8)|b0;
    while(n--){*(*p)++=S6B64[v&0x3f];v>>=6;}
}

static char s6_out[128];

static char *local_crypt_sha512(const char *key, const char *setting)
{
    s512_ctx ctx;
    uint8_t dA[64],dB[64],dP[64],dS[64],C[64];
    uint8_t pstr[128],sstr[64];
    const char *sp,*ep; size_t klen,slen;
    unsigned rounds=5000,i,n; int cust=0; char *op;

    if(strncmp(setting,"$6$",3)!=0) return NULL;
    sp=setting+3;
    if(strncmp(sp,"rounds=",7)==0){
        sp+=7; rounds=(unsigned)strtoul(sp,(char**)&ep,10);
        if(rounds<1000) rounds=1000;
        if(rounds>999999999u) rounds=999999999u;
        cust=1; sp=ep; if(*sp=='$') sp++;
    }
    ep=sp; while(*ep&&*ep!='$'&&(ep-sp)<16) ep++;
    slen=(size_t)(ep-sp); klen=strlen(key);
    if(klen>128) klen=128;

    /* Digest B = SHA512(P + P) */
    s512_init(&ctx); s512_feed(&ctx,key,klen); s512_feed(&ctx,key,klen);
    s512_done(&ctx,dB);

    /* Digest A = SHA512(P + S + dB[0..klen-1] + bit-alt(P,dB)) */
    s512_init(&ctx);
    s512_feed(&ctx,key,klen); s512_feed(&ctx,sp,slen);
    for(n=(unsigned)klen;n>64;n-=64) s512_feed(&ctx,dB,64);
    s512_feed(&ctx,dB,n);
    for(n=(unsigned)klen;n;n>>=1)
        s512_feed(&ctx,(n&1)?(const void*)dB:(const void*)key,
                  (n&1)?64:klen);
    s512_done(&ctx,dA);

    /* P-string: SHA512(P repeated klen times), tiled to klen bytes */
    s512_init(&ctx);
    for(i=0;i<(unsigned)klen;i++) s512_feed(&ctx,key,klen);
    s512_done(&ctx,dP);
    for(i=0;i<(unsigned)klen;i++) pstr[i]=dP[i%64];

    /* S-string: SHA512(S repeated 16+dA[0] times), tiled to slen bytes */
    s512_init(&ctx);
    for(i=0;i<16+(unsigned)dA[0];i++) s512_feed(&ctx,sp,slen);
    s512_done(&ctx,dS);
    for(i=0;i<(unsigned)slen;i++) sstr[i]=dS[i%64];

    /* Rounds */
    memcpy(C,dA,64);
    for(i=0;i<rounds;i++){
        s512_init(&ctx);
        if(i&1) s512_feed(&ctx,pstr,klen); else s512_feed(&ctx,C,64);
        if(i%3) s512_feed(&ctx,sstr,slen);
        if(i%7) s512_feed(&ctx,pstr,klen);
        if(i&1) s512_feed(&ctx,C,64); else s512_feed(&ctx,pstr,klen);
        s512_done(&ctx,C);
    }

    /* Encode output */
    op=s6_out;
    *op++='$';*op++='6';*op++='$';
    if(cust) op+=sprintf(op,"rounds=%u$",rounds);
    memcpy(op,sp,slen); op+=slen; *op++='$';
    s6b64(&op,C[ 0],C[21],C[42],4); s6b64(&op,C[22],C[43],C[ 1],4);
    s6b64(&op,C[44],C[ 2],C[23],4); s6b64(&op,C[ 3],C[24],C[45],4);
    s6b64(&op,C[25],C[46],C[ 4],4); s6b64(&op,C[47],C[ 5],C[26],4);
    s6b64(&op,C[ 6],C[27],C[48],4); s6b64(&op,C[28],C[49],C[ 7],4);
    s6b64(&op,C[50],C[ 8],C[29],4); s6b64(&op,C[ 9],C[30],C[51],4);
    s6b64(&op,C[31],C[52],C[10],4); s6b64(&op,C[53],C[11],C[32],4);
    s6b64(&op,C[12],C[33],C[54],4); s6b64(&op,C[34],C[55],C[13],4);
    s6b64(&op,C[56],C[14],C[35],4); s6b64(&op,C[15],C[36],C[57],4);
    s6b64(&op,C[37],C[58],C[16],4); s6b64(&op,C[59],C[17],C[38],4);
    s6b64(&op,C[18],C[39],C[60],4); s6b64(&op,C[40],C[61],C[19],4);
    s6b64(&op,C[62],C[20],C[41],4); s6b64(&op,0,C[63],0,2);
    *op='\0';
    return s6_out;
}

/* ── Embedded MD5 for $1$ hashes ─────────────────────────────────────────── */
typedef struct { uint32_t s[4]; uint64_t total; unsigned blen; uint8_t buf[64]; } md5_ctx;

static const uint8_t MD5SH[64]={
    7,12,17,22,7,12,17,22,7,12,17,22,7,12,17,22,
    5,9,14,20,5,9,14,20,5,9,14,20,5,9,14,20,
    4,11,16,23,4,11,16,23,4,11,16,23,4,11,16,23,
    6,10,15,21,6,10,15,21,6,10,15,21,6,10,15,21
};
static const uint32_t MD5K[64]={
    0xd76aa478u,0xe8c7b756u,0x242070dbu,0xc1bdceeeu,
    0xf57c0fafu,0x4787c62au,0xa8304613u,0xfd469501u,
    0x698098d8u,0x8b44f7afu,0xffff5bb1u,0x895cd7beu,
    0x6b901122u,0xfd987193u,0xa679438eu,0x49b40821u,
    0xf61e2562u,0xc040b340u,0x265e5a51u,0xe9b6c7aau,
    0xd62f105du,0x02441453u,0xd8a1e681u,0xe7d3fbc8u,
    0x21e1cde6u,0xc33707d6u,0xf4d50d87u,0x455a14edu,
    0xa9e3e905u,0xfcefa3f8u,0x676f02d9u,0x8d2a4c8au,
    0xfffa3942u,0x8771f681u,0x6d9d6122u,0xfde5380cu,
    0xa4beea44u,0x4bdecfa9u,0xf6bb4b60u,0xbebfbc70u,
    0x289b7ec6u,0xeaa127fau,0xd4ef3085u,0x04881d05u,
    0xd9d4d039u,0xe6db99e5u,0x1fa27cf8u,0xc4ac5665u,
    0xf4292244u,0x432aff97u,0xab9423a7u,0xfc93a039u,
    0x655b59c3u,0x8f0ccc92u,0xffeff47du,0x85845dd1u,
    0x6fa87e4fu,0xfe2ce6e0u,0xa3014314u,0x4e0811a1u,
    0xf7537e82u,0xbd3af235u,0x2ad7d2bbu,0xeb86d391u
};
#define MD5ROL(x,n) (((x)<<(n))|((x)>>(32-(n))))

static void md5_compress(md5_ctx *c, const uint8_t *blk)
{
    uint32_t W[16],A,B,C,D,f,tmp; unsigned i,g;
    for(i=0;i<16;i++)
        W[i]=((uint32_t)blk[i*4+3]<<24)|((uint32_t)blk[i*4+2]<<16)
            |((uint32_t)blk[i*4+1]<<8)|blk[i*4];
    A=c->s[0]; B=c->s[1]; C=c->s[2]; D=c->s[3];
    for(i=0;i<64;i++){
        if(i<16){      f=(B&C)|(~B&D);  g=i; }
        else if(i<32){ f=(B&D)|(C&~D);  g=(5*i+1)%16; }
        else if(i<48){ f=B^C^D;         g=(3*i+5)%16; }
        else{          f=C^(B|~D);      g=(7*i)%16; }
        tmp=D; D=C; C=B; B+=MD5ROL(A+f+MD5K[i]+W[g],MD5SH[i]); A=tmp;
    }
    c->s[0]+=A; c->s[1]+=B; c->s[2]+=C; c->s[3]+=D;
}
static void md5_init(md5_ctx *c)
{
    c->s[0]=0x67452301u; c->s[1]=0xEFCDAB89u;
    c->s[2]=0x98BADCFEu; c->s[3]=0x10325476u;
    c->total=0; c->blen=0;
}
static void md5_feed(md5_ctx *c, const void *data, size_t len)
{
    const uint8_t *p=(const uint8_t*)data;
    while(len){ size_t sp=64-c->blen,tk=len<sp?len:sp;
        memcpy(c->buf+c->blen,p,tk);
        c->blen+=(unsigned)tk; c->total+=tk; p+=tk; len-=tk;
        if(c->blen==64){md5_compress(c,c->buf); c->blen=0;}
    }
}
static void md5_done(md5_ctx *c, uint8_t out[16])
{
    uint64_t bits=c->total*8; uint8_t pad=0x80; int i;
    uint8_t lb[8]={(uint8_t)bits,(uint8_t)(bits>>8),(uint8_t)(bits>>16),(uint8_t)(bits>>24),
                   (uint8_t)(bits>>32),(uint8_t)(bits>>40),(uint8_t)(bits>>48),(uint8_t)(bits>>56)};
    md5_feed(c,&pad,1); pad=0;
    while(c->blen!=56) md5_feed(c,&pad,1);
    md5_feed(c,lb,8);
    for(i=0;i<4;i++){
        out[i*4]=(uint8_t)c->s[i];   out[i*4+1]=(uint8_t)(c->s[i]>>8);
        out[i*4+2]=(uint8_t)(c->s[i]>>16); out[i*4+3]=(uint8_t)(c->s[i]>>24);
    }
}

static char m1_out[64];

static char *local_crypt_md5(const char *key, const char *setting)
{
    md5_ctx ctx,ctx1;
    uint8_t dA[16],dB[16]; uint8_t zero=0;
    const char *sp,*ep; size_t klen,slen;
    unsigned i,n; char *op;

    if(strncmp(setting,"$1$",3)!=0) return NULL;
    sp=setting+3;
    ep=sp; while(*ep&&*ep!='$'&&(ep-sp)<8) ep++;
    slen=(size_t)(ep-sp); klen=strlen(key);
    if(klen>128) klen=128;

    /* Digest B = MD5(P + S + P) */
    md5_init(&ctx1);
    md5_feed(&ctx1,key,klen); md5_feed(&ctx1,sp,slen); md5_feed(&ctx1,key,klen);
    md5_done(&ctx1,dB);

    /* Digest A = MD5(P + "$1$" + S + dB[0..klen-1] + bit-alt('\0', P[0])) */
    md5_init(&ctx);
    md5_feed(&ctx,key,klen); md5_feed(&ctx,"$1$",3); md5_feed(&ctx,sp,slen);
    for(n=(unsigned)klen;n>16;n-=16) md5_feed(&ctx,dB,16);
    md5_feed(&ctx,dB,n);
    for(n=(unsigned)klen;n;n>>=1)
        if(n&1) md5_feed(&ctx,&zero,1);
        else    md5_feed(&ctx,key,1);
    md5_done(&ctx,dA);

    /* 1000 rounds */
    for(i=0;i<1000;i++){
        md5_init(&ctx1);
        if(i&1) md5_feed(&ctx1,key,klen); else md5_feed(&ctx1,dA,16);
        if(i%3) md5_feed(&ctx1,sp,slen);
        if(i%7) md5_feed(&ctx1,key,klen);
        if(i&1) md5_feed(&ctx1,dA,16); else md5_feed(&ctx1,key,klen);
        md5_done(&ctx1,dA);
    }

    op=m1_out;
    *op++='$'; *op++='1'; *op++='$';
    memcpy(op,sp,slen); op+=slen; *op++='$';
    s6b64(&op,dA[ 0],dA[ 6],dA[12],4);
    s6b64(&op,dA[ 1],dA[ 7],dA[13],4);
    s6b64(&op,dA[ 2],dA[ 8],dA[14],4);
    s6b64(&op,dA[ 3],dA[ 9],dA[15],4);
    s6b64(&op,dA[ 4],dA[10],dA[ 5],4);
    s6b64(&op,0,0,dA[11],2);
    *op='\0';
    return m1_out;
}

static char *local_crypt(const char *key, const char *setting)
{
    if(strncmp(setting,"$1$",3)==0) return local_crypt_md5(key,setting);
    if(strncmp(setting,"$6$",3)==0) return local_crypt_sha512(key,setting);
    return NULL;
}

/* ── Berkeley DB 4.x hash reader for vsftpd virtual users ─────────────────
 * pam_userdb stores plaintext passwords: key=username, value=password.
 * Magic 0x00061561; metadata at fixed offsets (same for DB 2–6).          */

/* Parse /etc/pam.d/vsftpd for the db= path used by pam_userdb.so.
 * Appends ".db" suffix. Falls back to common on-disk paths. Returns 1 on success. */
static int find_vsftpd_db(char *out, size_t outsz)
{
    static const char *const fallbacks[] = {
        "/etc/vsftpd/login.db", "/etc/vsftpd/virtual_users.db",
        "/etc/vsftpd/vsftpd_login.db", NULL
    };
    FILE *f = fopen("/etc/pam.d/vsftpd", "r");
    if (f) {
        char line[256];
        while (fgets(line, sizeof(line), f)) {
            char *p = strstr(line, "pam_userdb");
            if (!p) continue;
            p = strstr(p, "db=");
            if (!p) continue;
            p += 3;
            char *end = p;
            while (*end && *end != ' ' && *end != '\t' && *end != '\n' && *end != '\r') end++;
            size_t len = (size_t)(end - p);
            if (len > 0 && len + 4 < outsz) {
                memcpy(out, p, len);
                memcpy(out + len, ".db", 4);
                fclose(f);
                return 1;
            }
        }
        fclose(f);
    }
    /* Try well-known fallback paths */
    int i; for (i = 0; fallbacks[i]; i++) {
        FILE *tf = fopen(fallbacks[i], "rb");
        if (tf) { fclose(tf); strncpy(out, fallbacks[i], outsz - 1); out[outsz-1] = '\0'; return 1; }
    }
    return 0;
}

/* Validate user:pass against a Berkeley DB hash file (pam_userdb plaintext).
 * Returns 0=match, 1=wrong password, -1=not found or parse error. */
static int bdb_check_pass(const char *db_path, const char *user, const char *pass)
{
    uint8_t meta[512], page[4096];
    uint32_t pagesize, lastpgno, magic, pgno;
    size_t ulen = strlen(user), plen = strlen(pass);
    int result = -1;
    FILE *f = fopen(db_path, "rb");
    if (!f) return -1;

    if (fread(meta, 1, sizeof(meta), f) < sizeof(meta)) { fclose(f); return -1; }
    magic = (uint32_t)meta[12] | ((uint32_t)meta[13]<<8)
          | ((uint32_t)meta[14]<<16) | ((uint32_t)meta[15]<<24);
    if (magic != 0x00061561u) { fclose(f); return -1; }

    pagesize = (uint32_t)meta[20] | ((uint32_t)meta[21]<<8)
             | ((uint32_t)meta[22]<<16) | ((uint32_t)meta[23]<<24);
    lastpgno = (uint32_t)meta[32] | ((uint32_t)meta[33]<<8)
             | ((uint32_t)meta[34]<<16) | ((uint32_t)meta[35]<<24);
    if (pagesize < 512 || pagesize > sizeof(page)) { fclose(f); return -1; }

    for (pgno = 1; pgno <= lastpgno && result < 0; pgno++) {
        if (fseek(f, (long)pgno * (long)pagesize, SEEK_SET) != 0) continue;
        if (fread(page, 1, pagesize, f) < pagesize) continue;
        if (page[25] != 13) continue;  /* P_HASH = 13 */

        unsigned n = (unsigned)((uint16_t)page[20] | ((uint16_t)page[21]<<8));
        if (n < 2 || (n & 1)) continue;  /* must have even item count (key+value pairs) */

        /* inp[i] at page[26 + i*2].  Offsets descend: inp[0] > inp[1] > ...
         * LEN_HITEM: len[0] = pagesize - inp[0]; len[i] = inp[i-1] - inp[i]. */
        unsigned j; for (j = 0; j + 1 < n && result < 0; j += 2) {
            uint16_t koff = (uint16_t)page[26+j*2]   | ((uint16_t)page[26+j*2+1]<<8);
            uint16_t doff = (uint16_t)page[26+j*2+2] | ((uint16_t)page[26+j*2+3]<<8);
            uint16_t klen = (j == 0) ? (uint16_t)(pagesize - koff)
                          : ((uint16_t)page[26+j*2-2] | ((uint16_t)page[26+j*2-1]<<8)) - koff;
            uint16_t dlen = koff - doff;  /* = inp[j] - inp[j+1] */

            if (koff < 26 || doff < 26 || koff >= pagesize || doff >= pagesize) continue;
            if (klen < 2 || dlen < 2) continue;
            if (page[koff] != 1 || page[doff] != 1) continue;  /* H_KEYDATA = 1 */

            /* key bytes: page[koff+1 .. koff+klen-1], length = klen-1 */
            if ((uint16_t)(klen - 1) == (uint16_t)ulen &&
                memcmp(page + koff + 1, user, ulen) == 0) {
                result = ((uint16_t)(dlen - 1) == (uint16_t)plen &&
                          memcmp(page + doff + 1, pass, plen) == 0) ? 0 : 1;
            }
        }
    }
    fclose(f);
    return result;
}

/* Check /korg/rw/Startup/KronosNet.conf — Korg's UI-managed credential store.
 * Format: line 1 = username, line 2 = password (plain text, updated by the UI).
 * Returns 0=match, 1=username matched but wrong password, -1=not this user/unavailable. */
static int kronosnet_auth(const char *user, const char *pass)
{
    char stored_user[128] = "", stored_pass[256] = "";
    FILE *f = fopen("/korg/rw/Startup/KronosNet.conf", "r");
    if (!f) return -1;
    if (fgets(stored_user, sizeof(stored_user), f))
        fgets(stored_pass, sizeof(stored_pass), f);
    fclose(f);
    stored_user[strcspn(stored_user, "\r\n")] = '\0';
    stored_pass[strcspn(stored_pass, "\r\n")] = '\0';
    if (stored_user[0] == '\0') return -1;
    if (strcmp(stored_user, user) != 0) return -1;  /* not the KronosNet user — try system auth */
    return (strcmp(stored_pass, pass) == 0) ? 0 : 1;
}

/* Validate credentials.  Priority:
 *   1. KronosNet.conf  (Korg UI-managed, covers the 'kronos' / network user)
 *   2. /etc/shadow + /etc/passwd  (system users: root, stg, pocky, …)
 *   3. vsftpd virtual-user Berkeley DB  (legacy fallback)
 * Returns 0=ok, -1=wrong password, -2=lookup error.
 * *out_reason describes the failure (never NULL on error). */
static int shadow_auth(const char *user, const char *pass, const char **out_reason)
{
    /* ── KronosNet.conf ── */
    int kr = kronosnet_auth(user, pass);
    if (kr == 0) { *out_reason = NULL; return 0; }
    if (kr == 1) { *out_reason = "wrong password"; return -1; }

    /* ── /etc/shadow then /etc/passwd ── */
    FILE *f;
    char line[512], hash[256] = "";
    size_t ulen = strlen(user);
    char *result;

    f = fopen("/etc/shadow", "r");
    if (f) {
        while (fgets(line, sizeof(line), f)) {
            if (strncmp(line, user, ulen) == 0 && line[ulen] == ':') {
                char *h = line + ulen + 1, *end = strchr(h, ':');
                if (end) *end = '\0';
                h[strcspn(h, "\r\n")] = '\0';
                strncpy(hash, h, sizeof(hash) - 1);
                break;
            }
        }
        fclose(f);
    }
    if (hash[0] == '\0') {
        f = fopen("/etc/passwd", "r");
        if (f) {
            while (fgets(line, sizeof(line), f)) {
                if (strncmp(line, user, ulen) == 0 && line[ulen] == ':') {
                    char *h = line + ulen + 1, *end = strchr(h, ':');
                    if (end) *end = '\0';
                    h[strcspn(h, "\r\n")] = '\0';
                    strncpy(hash, h, sizeof(hash) - 1);
                    break;
                }
            }
            fclose(f);
        }
    }

    if (hash[0] != '\0' && hash[0] != 'x' && hash[0] != '!' && hash[0] != '*') {
        if (hash[0] != '$') { *out_reason = "unrecognised hash format"; return -2; }
        result = local_crypt(pass, hash);
        if (!result) { *out_reason = "unsupported hash algorithm"; return -2; }
        if (strcmp(result, hash) != 0) { *out_reason = "wrong password"; return -1; }
        *out_reason = NULL;
        return 0;
    }
    if (hash[0] == '!' || hash[0] == '*') { *out_reason = "account locked"; return -1; }

    /* ── vsftpd Berkeley DB fallback ── */
    char db_path[256];
    if (find_vsftpd_db(db_path, sizeof(db_path))) {
        int r = bdb_check_pass(db_path, user, pass);
        if (r == 0) { *out_reason = NULL; return 0; }
        if (r == 1) { *out_reason = "wrong password"; return -1; }
    }

    *out_reason = "user not found";
    return -2;
}

static void log_access(const char *ip, int accepted, const char *reason)
{
    time_t t = time(NULL);
    char ts[32];
    struct tm *ti = localtime(&t);
    strftime(ts, sizeof(ts), "%Y-%m-%d %H:%M:%S", ti);
    FILE *f = fopen(SCREENREMOTE_DIR "/access.log", "a");
    if (!f) return;
    fprintf(f, "%s  %-15s  %s%s%s\n", ts, ip,
            accepted ? "ACCEPTED" : "REJECTED",
            reason   ? "  "       : "",
            reason   ? reason     : "");
    fclose(f);
}

/* ── Apply palette to fb0 ───────────────────────────────────── */
static void apply_palette_to_fb0(void)
{
    struct fb_cmap cmap;
    if (fb0_fd < 0) return;
    cmap.start  = 0;
    cmap.len    = PAL_ENTRIES;
    cmap.red    = pal_r;
    cmap.green  = pal_g;
    cmap.blue   = pal_b;
    cmap.transp = pal_t;
    ioctl(fb0_fd, FBIOPUTCMAP, &cmap);
}

/* ── Framebuffer open/close ─────────────────────────────────── */
static int fb1_open(void)
{
    struct fb_fix_screeninfo ffix;
    struct fb_var_screeninfo fvar;

    fb1_fd = open(FB_SRC, O_RDONLY);
    if (fb1_fd < 0) { perror("open " FB_SRC); return -1; }

    if (ioctl(fb1_fd, FBIOGET_FSCREENINFO, &ffix) < 0 ||
        ioctl(fb1_fd, FBIOGET_VSCREENINFO, &fvar) < 0) {
        perror("fb1 ioctl"); return -1;
    }
    fb_w        = fvar.xres;
    fb_h        = fvar.yres;
    fb1_stride  = ffix.line_length;
    frame_bytes = fb_w * fb_h;

    {
        int pi;
        for (pi = 0; pi < PAL_ENTRIES; pi++) {
            pal_r[pi] = (uint16_t)kronos_palette[pi][0] << 8;
            pal_g[pi] = (uint16_t)kronos_palette[pi][1] << 8;
            pal_b[pi] = (uint16_t)kronos_palette[pi][2] << 8;
            pal_t[pi] = 0;
        }
    }

    fb1_map = mmap(NULL, fb1_stride * fb_h,
                   PROT_READ, MAP_SHARED, fb1_fd, 0);
    if (fb1_map == MAP_FAILED) { perror("mmap fb1"); return -1; }

    fprintf(stderr, "screenremote: fb1 %ux%u bpp=8 stride=%u\n",
            fb_w, fb_h, fb1_stride);
    return 0;
}

static int fb0_open(void)
{
    struct fb_fix_screeninfo ffix;

    if (fb0_fd >= 0) return 0;

    fb0_fd = open(FB_DST, O_RDWR);
    if (fb0_fd < 0) { perror("open " FB_DST); return -1; }

    if (ioctl(fb0_fd, FBIOGET_FSCREENINFO, &ffix) < 0) {
        perror("fb0 FBIOGET_FSCREENINFO"); goto fail;
    }
    fb0_stride = ffix.line_length;

    fb0_map = mmap(NULL, fb0_stride * fb_h,
                   PROT_WRITE, MAP_SHARED, fb0_fd, 0);
    if (fb0_map == MAP_FAILED) { perror("mmap fb0"); goto fail; }

    apply_palette_to_fb0();

    /* Hide the fbcon text cursor so it doesn't blink over the mirrored frame */
    { int t = open("/dev/tty0", O_WRONLY); if (t >= 0) { write(t, "\033[?25l", 6); close(t); } }

    fprintf(stderr, "screenremote: fb0 mirror enabled (stride=%u)\n", fb0_stride);
    return 0;

fail:
    close(fb0_fd); fb0_fd = -1; fb0_map = NULL;
    return -1;
}

static void fb0_close(void)
{
    if (fb0_map) {
        /* Zero the framebuffer before releasing so fbcon can resume drawing
         * dmesg output on a clean slate rather than leaving the last frame
         * frozen on screen. */
        memset(fb0_map, 0, fb0_stride * fb_h);
        munmap(fb0_map, fb0_stride * fb_h);
        fb0_map = NULL;
    }
    if (fb0_fd >= 0) { close(fb0_fd); fb0_fd = -1; }
    /* Restore the fbcon text cursor now that we've released the framebuffer */
    { int t = open("/dev/tty0", O_WRONLY); if (t >= 0) { write(t, "\033[?25h", 6); close(t); } }
    fprintf(stderr, "screenremote: fb0 mirror disabled\n");
}

/* ── Screensaver helpers ─────────────────────────────────────── */
static void ss_sample(uint8_t *out)
{
    int i;
    uint32_t step = frame_bytes / SS_SAMPLE_N;
    for (i = 0; i < SS_SAMPLE_N; i++) {
        uint32_t off = i * step;
        uint32_t row = off / fb1_stride;
        uint32_t col = off % fb1_stride;
        out[i] = (row < fb_h && col < fb_w) ? fb1_map[row * fb1_stride + col] : 0;
    }
}

static void ss_reset(time_t now)
{
    ss_last_chg   = now;
    ss_prev_valid = 0;
    ss_active     = 0;
}

/* ── Mirror ─────────────────────────────────────────────────── */
static void do_mirror(void)
{
    uint32_t y;
    if (!fb0_map || !fb1_map) return;
    for (y = 0; y < fb_h; y++)
        memcpy(fb0_map + y * fb0_stride,
               fb1_map + y * fb1_stride,
               fb_w);
}

static void check_mirror_flag(void)
{
    struct stat st;
    int want = (stat(MIRROR_FLAG, &st) == 0);
    if (want == mirror_on) return;
    if (want) { if (fb0_open() == 0) { mirror_on = 1; ss_reset(time(NULL)); } }
    else      { fb0_close(); mirror_on = 0; ss_active = 0; }
}

/* ── Network ────────────────────────────────────────────────── */
static int network_ok(void)
{
    struct ifaddrs *ifa, *p;
    int ok = 0;
    if (getifaddrs(&ifa) < 0) return 0;
    for (p = ifa; p && !ok; p = p->ifa_next) {
        struct sockaddr_in *sin;
        uint32_t a;
        if (!p->ifa_addr || p->ifa_addr->sa_family != AF_INET) continue;
        sin = (struct sockaddr_in *)p->ifa_addr;
        a   = ntohl(sin->sin_addr.s_addr);
        if ((a >> 24) == 127)   continue;
        if ((a >> 16) == 0xA9FE) continue;
        ok = 1;
    }
    freeifaddrs(ifa);
    return ok;
}

/* Returns the first usable LAN IPv4 address in network byte order, or INADDR_ANY. */
static uint32_t find_lan_ip(void)
{
    struct ifaddrs *ifa, *p;
    uint32_t found = INADDR_ANY;
    if (getifaddrs(&ifa) < 0) return INADDR_ANY;
    for (p = ifa; p; p = p->ifa_next) {
        struct sockaddr_in *sin;
        uint32_t a;
        if (!p->ifa_addr || p->ifa_addr->sa_family != AF_INET) continue;
        sin = (struct sockaddr_in *)p->ifa_addr;
        a   = ntohl(sin->sin_addr.s_addr);
        if ((a >> 24) == 127)    continue;
        if ((a >> 16) == 0xA9FE) continue;
        found = sin->sin_addr.s_addr;   /* already network byte order */
        break;
    }
    freeifaddrs(ifa);
    return found;
}

/* ── Config ──────────────────────────────────────────────────── */
static void read_config(void)
{
    FILE *f = fopen(CFG_PATH, "r");
    if (!f) return;
    char line[128];
    while (fgets(line, sizeof(line), f)) {
        int v;
        if      (sscanf(line, "stream_port=%d", &v) == 1 && v > 0 && v <= 65535)
            g_stream_port = v;
        else if (sscanf(line, "ctrl_port=%d",   &v) == 1 && v > 0 && v <= 65535)
            g_ctrl_port = v;
        else if (sscanf(line, "screensaver_timeout=%d", &v) == 1 && v >= 0)
            g_ss_timeout = v;
    }
    fclose(f);
    fprintf(stderr, "screenremote: config loaded: stream=%d ctrl=%d\n",
            g_stream_port, g_ctrl_port);
}

/* bind_ip: network-byte-order address to bind to (INADDR_ANY = 0 = all interfaces) */
static int make_listen(int port, uint32_t bind_ip)
{
    int fd, yes = 1;
    struct sockaddr_in a;
    fd = socket(AF_INET, SOCK_STREAM, 0);
    if (fd < 0) return -1;
    setsockopt(fd, SOL_SOCKET, SO_REUSEADDR, &yes, sizeof(yes));
    memset(&a, 0, sizeof(a));
    a.sin_family      = AF_INET;
    a.sin_port        = htons((uint16_t)port);
    a.sin_addr.s_addr = bind_ip;
    if (bind(fd, (struct sockaddr *)&a, sizeof(a)) < 0 ||
        listen(fd, 1) < 0) {
        close(fd); return -1;
    }
    return fd;
}

static int make_udp_disc(void)
{
    int fd, yes = 1;
    struct sockaddr_in a;
    fd = socket(AF_INET, SOCK_DGRAM, 0);
    if (fd < 0) return -1;
    setsockopt(fd, SOL_SOCKET, SO_REUSEADDR, &yes, sizeof(yes));
    memset(&a, 0, sizeof(a));
    a.sin_family      = AF_INET;
    a.sin_port        = htons(DISC_PORT);
    a.sin_addr.s_addr = INADDR_ANY;
    if (bind(fd, (struct sockaddr *)&a, sizeof(a)) < 0) {
        close(fd); return -1;
    }
    return fd;
}

/* ── Protocol ───────────────────────────────────────────────── */
/* Cork/uncork helper: batches header + body into one TCP burst. */
#define TCP_CORK_ON(fd)  do { int _c=1; setsockopt((fd),IPPROTO_TCP,TCP_CORK,&_c,sizeof(_c)); } while(0)
#define TCP_CORK_OFF(fd) do { int _c=0; setsockopt((fd),IPPROTO_TCP,TCP_CORK,&_c,sizeof(_c)); } while(0)

/* Send a flat frame_bytes buffer preceded by a 4-byte LE length header.
 * Reads only from RAM (buf), not from device memory. */
static int send_frame_buf(int fd, const uint8_t *buf)
{
    uint8_t hdr[4];
    hdr[0] = frame_bytes        & 0xFF;
    hdr[1] = (frame_bytes >>  8) & 0xFF;
    hdr[2] = (frame_bytes >> 16) & 0xFF;
    hdr[3] = (frame_bytes >> 24) & 0xFF;
    TCP_CORK_ON(fd);
    if (write_all(fd, hdr, 4) < 0 || write_all(fd, buf, frame_bytes) < 0) {
        TCP_CORK_OFF(fd);
        return -1;
    }
    TCP_CORK_OFF(fd);
    return 0;
}

/* Pull mode: send directly from device memory fb1_map (no staging/shadow). */
static int send_frame(int fd)
{
    uint8_t hdr[4];
    uint32_t y;
    hdr[0] = frame_bytes        & 0xFF;
    hdr[1] = (frame_bytes >>  8) & 0xFF;
    hdr[2] = (frame_bytes >> 16) & 0xFF;
    hdr[3] = (frame_bytes >> 24) & 0xFF;
    TCP_CORK_ON(fd);
    if (write_all(fd, hdr, 4) < 0) goto fail;
    if (fb1_stride == fb_w) {
        if (write_all(fd, fb1_map, frame_bytes) < 0) goto fail;
    } else {
        for (y = 0; y < fb_h; y++)
            if (write_all(fd, fb1_map + y * fb1_stride, fb_w) < 0) goto fail;
    }
    TCP_CORK_OFF(fd);
    return 0;
fail:
    TCP_CORK_OFF(fd);
    return -1;
}

static int do_handshake(int fd, uint8_t *mode_out, uint8_t *fps_out,
                        const struct sockaddr_in *peer)
{
    uint8_t  hdr[9];   /* KSCR(4) + ver(1) + mode(1) + fps(1) + ulen(1) + plen(1) */
    uint8_t  rsp[4 + 1 + 2 + 2 + PAL_ENTRIES * 3];
    char     user[65], pass[129];
    struct timeval tv5 = {5, 0}, tvz = {0, 0};
    uint8_t  fail[5];
    uint8_t  ulen, plen;
    int      i, j, auth;
    const char *peer_ip = inet_ntoa(peer->sin_addr);

    memcpy(fail, MAGIC, 4);

    setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, &tv5, sizeof(tv5));

    if (recv(fd, hdr, sizeof(hdr), MSG_WAITALL) != (ssize_t)sizeof(hdr)) {
        log_access(peer_ip, 0, "incomplete hello");
        return -1;
    }
    if (memcmp(hdr, MAGIC, 4) != 0 || hdr[4] != 0x02) {
        fail[4] = 0x01;
        write_all(fd, fail, 5);
        log_access(peer_ip, 0, "bad magic/version");
        return -1;
    }

    ulen = hdr[7];
    plen = hdr[8];
    if (ulen == 0 || ulen > 64 || plen > 128) {
        fail[4] = 0x01;
        write_all(fd, fail, 5);
        log_access(peer_ip, 0, "bad credential lengths");
        return -1;
    }

    if (recv(fd, user, ulen, MSG_WAITALL) != ulen ||
        (plen > 0 && recv(fd, pass, plen, MSG_WAITALL) != (ssize_t)plen)) {
        log_access(peer_ip, 0, "credential read error");
        return -1;
    }
    user[ulen] = '\0';
    pass[plen] = '\0';

    setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, &tvz, sizeof(tvz));

    const char *auth_reason = NULL;
    auth = shadow_auth(user, pass, &auth_reason);
    memset(pass, 0, sizeof(pass));

    if (auth != 0) {
        char logbuf[192];
        snprintf(logbuf, sizeof(logbuf), "user=%s: %s", user,
                 auth_reason ? auth_reason : "wrong password");
        fail[4] = (auth == -2) ? 0x02 : 0x01;
        write_all(fd, fail, 5);
        log_access(peer_ip, 0, logbuf);
        return -1;
    }

    *mode_out = hdr[5];
    *fps_out  = hdr[6] ? hdr[6] : FPS_MAX;
    if (*fps_out > FPS_MAX) *fps_out = FPS_MAX;

    i = 0;
    memcpy(rsp + i, MAGIC, 4);              i += 4;
    rsp[i++] = 0x00;                        /* status ok */
    rsp[i++] = (uint8_t)(fb_w & 0xFF);     /* width LE16 */
    rsp[i++] = (uint8_t)(fb_w >> 8);
    rsp[i++] = (uint8_t)(fb_h & 0xFF);     /* height LE16 */
    rsp[i++] = (uint8_t)(fb_h >> 8);
    for (j = 0; j < PAL_ENTRIES; j++) {    /* palette: 256 × RGB8 */
        rsp[i++] = (uint8_t)(pal_r[j] >> 8);
        rsp[i++] = (uint8_t)(pal_g[j] >> 8);
        rsp[i++] = (uint8_t)(pal_b[j] >> 8);
    }
    log_access(peer_ip, 1, NULL);
    return write_all(fd, rsp, i);
}

static void inject_key(int code, int val)
{
    /* struct input_event on 32-bit Linux: { long tv_sec(4); long tv_usec(4);
     * unsigned short type(2); unsigned short code(2); int value(4); } = 16 bytes.
     * Using explicit long so the layout stays correct if built on 64-bit. */
    struct { long tv_sec; long tv_usec; unsigned short type; unsigned short code; int value; } ev;
    char buf[16];
    int n;

    /* Primary: vkbd.ko proc interface — loaded at startup, reopen if stale */
    if (vkbd_fd < 0)
        vkbd_fd = open("/proc/.vkbd", O_WRONLY);
    if (vkbd_fd >= 0) {
        n = snprintf(buf, sizeof(buf), "%d %d\n", code, val);
        if (write(vkbd_fd, buf, n) >= 0) return;
        close(vkbd_fd); vkbd_fd = -1;
    }

    /* Fallback: /dev/uinput virtual device.  Writing to /dev/input/eventX does NOT
     * inject events — that requires uinput.  Open and configure on first use. */
    if (kbd_fd < 0) {
        struct uinput_user_dev uidev;
        int i;
        kbd_fd = open("/dev/uinput", O_WRONLY | O_NONBLOCK);
        if (kbd_fd < 0) return;

        ioctl(kbd_fd, UI_SET_EVBIT,  EV_KEY);
        ioctl(kbd_fd, UI_SET_EVBIT,  EV_SYN);
        for (i = 1; i < KEY_CNT; i++)
            ioctl(kbd_fd, UI_SET_KEYBIT, i);

        memset(&uidev, 0, sizeof(uidev));
        snprintf(uidev.name, UINPUT_MAX_NAME_SIZE, "Kronos SR Fallback");
        uidev.id.bustype = BUS_USB;
        uidev.id.vendor  = 0x0001;
        uidev.id.product = 0x0002;
        uidev.id.version = 0x0001;
        if (write(kbd_fd, &uidev, sizeof(uidev)) < 0 ||
            ioctl(kbd_fd, UI_DEV_CREATE) < 0) {
            close(kbd_fd); kbd_fd = -1; return;
        }
        usleep(100000);   /* allow kernel to register the device */
    }

    memset(&ev, 0, sizeof(ev));
    ev.type  = KBD_EV_KEY;
    ev.code  = (unsigned short)code;
    ev.value = val;
    if (write(kbd_fd, &ev, sizeof(ev)) < 0) {
        ioctl(kbd_fd, UI_DEV_DESTROY);
        close(kbd_fd); kbd_fd = -1;
        return;
    }
    ev.type = KBD_EV_SYN; ev.code = 0; ev.value = 0;
    (void)write(kbd_fd, &ev, sizeof(ev));
}

static void send_rtf5_event(uint32_t dev, uint32_t code, uint32_t value)
{
    if (touch_fd < 0)
        touch_fd = open("/dev/rtf5", O_WRONLY);
    if (touch_fd >= 0) {
        uint32_t pkt[5] = { 0x00010014u, 0u, dev, code, value };
        (void)write(touch_fd, pkt, 20);
    }
}

static void inject_touch(int type, int x, int y)
{
    if (x < 0) x = 0;
    if (x >= (int)fb_w) x = (int)fb_w - 1;
    if (y < 0) y = 0;
    if (y >= (int)fb_h) y = (int)fb_h - 1;
    uint32_t h_adc = 10u + (uint32_t)(x * 236u) / 799u;
    uint32_t v_adc = 8u  + (uint32_t)(y * 237u) / 599u;
    send_rtf5_event(0x11u, (uint32_t)type, v_adc | (h_adc << 8u));
}

/* ── Embedded .ko extraction ─────────────────────────────────── */
static void extract_ko(const char *path, const unsigned char *data, unsigned int len)
{
    struct stat st;
    /* Skip write if file already exists with the correct size. */
    if (stat(path, &st) == 0 && (unsigned int)st.st_size == len)
        return;
    int fd = open(path, O_WRONLY | O_CREAT | O_TRUNC, 0644);
    if (fd >= 0) { write(fd, data, len); close(fd); }
}

/* ── Mode button keycode → g_mode ───────────────────────────── */
static void mode_from_btn(uint32_t dev, uint32_t code)
{
    if (dev != 0x07u) return;
    switch (code) {
        case 0x0eu: g_mode = 1; break;  /* Setlist  */
        case 0x08u: g_mode = 2; break;  /* Combi    */
        case 0x09u: g_mode = 3; break;  /* Program  */
        case 0x0au: g_mode = 4; break;  /* Sequence */
        case 0x0bu: g_mode = 5; break;  /* Sampling */
        case 0x0cu: g_mode = 6; break;  /* Global   */
        case 0x0du: g_mode = 7; break;  /* Disk     */
    }
}

/* Parse a "cpu ..." or "cpuN ..." line from /proc/stat into a cpu_snap_t. */
static void si_parse_cpu(const char *line, cpu_snap_t *s)
{
    memset(s, 0, sizeof(*s));
    sscanf(line, "%*s %lu %lu %lu %lu %lu %lu %lu",
           &s->user, &s->nice, &s->sys, &s->idle,
           &s->iowait, &s->irq, &s->softirq);
}

/* Build a SYSINFO response into out[outsz].  Returns bytes written.
 * Updates g_si_prev so successive calls yield accurate CPU deltas.
 * Must only be called when a client is connected (no background polling). */
static int sysinfo_collect(char *out, int outsz)
{
    cpu_snap_t  cur[SI_NCPU + 1];
    int         cpu_pct[SI_NCPU + 1];
    int         ncpu = 0, i, n = 0;
    FILE       *f;
    char        line[256];

    /* ── /proc/stat — CPU delta ───────────────────────────────── */
    memset(cur, 0, sizeof(cur));
    f = fopen("/proc/stat", "r");
    if (f) {
        while (fgets(line, sizeof(line), f)) {
            int idx;
            if (strncmp(line, "cpu ", 4) == 0) {
                si_parse_cpu(line, &cur[0]);
            } else if (sscanf(line, "cpu%d ", &idx) == 1 &&
                       idx >= 0 && idx < SI_NCPU) {
                si_parse_cpu(line, &cur[idx + 1]);
                if (idx + 1 > ncpu) ncpu = idx + 1;
            }
        }
        fclose(f);
    }
    for (i = 0; i <= ncpu; i++) {
        if (!g_si_prev_valid) {
            cpu_pct[i] = -1;
        } else {
            unsigned long busy =
                (cur[i].user    - g_si_prev[i].user)    +
                (cur[i].nice    - g_si_prev[i].nice)    +
                (cur[i].sys     - g_si_prev[i].sys)     +
                (cur[i].irq     - g_si_prev[i].irq)     +
                (cur[i].softirq - g_si_prev[i].softirq);
            unsigned long idle =
                (cur[i].idle    - g_si_prev[i].idle)    +
                (cur[i].iowait  - g_si_prev[i].iowait);
            unsigned long tot = busy + idle;
            cpu_pct[i] = (tot > 0) ? (int)(busy * 100UL / tot) : 0;
        }
    }
    memcpy(g_si_prev, cur, sizeof(g_si_prev));
    g_si_prev_valid = 1;

    /* ── /proc/uptime ─────────────────────────────────────────── */
    {
        unsigned long up = 0;
        f = fopen("/proc/uptime", "r");
        if (f) { fscanf(f, "%lu", &up); fclose(f); }
        n += snprintf(out + n, outsz - n, "UPTIME=%lu\n", up);
    }

    /* ── /proc/loadavg ────────────────────────────────────────── */
    {
        float l1 = 0, l5 = 0, l15 = 0;
        f = fopen("/proc/loadavg", "r");
        if (f) { fscanf(f, "%f %f %f", &l1, &l5, &l15); fclose(f); }
        n += snprintf(out + n, outsz - n, "LOAD=%.2f %.2f %.2f\n", l1, l5, l15);
    }

    /* ── /proc/meminfo ────────────────────────────────────────── */
    {
        unsigned long total = 0, mem_free = 0, bufs = 0, cached = 0;
        f = fopen("/proc/meminfo", "r");
        if (f) {
            while (fgets(line, sizeof(line), f)) {
                unsigned long v = 0;
                if      (sscanf(line, "MemTotal: %lu",  &v) == 1) total    = v;
                else if (sscanf(line, "MemFree: %lu",   &v) == 1) mem_free = v;
                else if (sscanf(line, "Buffers: %lu",   &v) == 1) bufs     = v;
                else if (sscanf(line, "Cached: %lu",    &v) == 1) cached   = v;
            }
            fclose(f);
        }
        n += snprintf(out + n, outsz - n,
            "MEM_TOTAL_KB=%lu\nMEM_FREE_KB=%lu\nMEM_AVAIL_KB=%lu\n",
            total, mem_free, mem_free + bufs + cached);
    }

    /* ── CPU percentages ──────────────────────────────────────── */
    n += snprintf(out + n, outsz - n, "CPU_PCT=%d\n", cpu_pct[0]);
    for (i = 0; i < ncpu; i++)
        n += snprintf(out + n, outsz - n, "CPU%d_PCT=%d\n", i, cpu_pct[i + 1]);

    /* ── /proc/KorgUsbAudio ───────────────────────────────────── */
    {
        unsigned int  sr = 0, ich = 0, och = 0;
        unsigned long rto = 0, midi_rt = 0;
        const char   *p;
        f = fopen("/proc/KorgUsbAudio", "r");
        if (f) {
            while (fgets(line, sizeof(line), f)) {
                if (sscanf(line, "audio sr=%u ich=%u och=%u", &sr, &ich, &och) == 3)
                    ;
                else if ((p = strstr(line, "rto=")) != NULL)
                    sscanf(p + 4, "%lu", &rto);
                if ((p = strstr(line, "midi output called rt=")) != NULL)
                    sscanf(p + 22, "%lu", &midi_rt);
            }
            fclose(f);
        }
        n += snprintf(out + n, outsz - n,
            "AUDIO_SR=%u\nAUDIO_OUT_CH=%u\nAUDIO_RTO=%lu\nAUDIO_MIDI_RT=%lu\n",
            sr, och, rto, midi_rt);
    }

    /* ── /korg/rw disk space ──────────────────────────────────── */
    {
        struct statvfs sv;
        if (statvfs("/korg/rw", &sv) == 0) {
            unsigned long free_mb  = (unsigned long)((unsigned long long)sv.f_bavail * sv.f_bsize >> 20);
            unsigned long total_mb = (unsigned long)((unsigned long long)sv.f_blocks * sv.f_bsize >> 20);
            n += snprintf(out + n, outsz - n,
                "DISK_FREE_MB=%lu\nDISK_TOTAL_MB=%lu\n", free_mb, total_mb);
        }
    }

    /* ── /korg/rw2 disk space (second internal SSD) ───────────── */
    {
        struct statvfs sv;
        if (statvfs("/korg/rw2", &sv) == 0) {
            unsigned long free_mb  = (unsigned long)((unsigned long long)sv.f_bavail * sv.f_bsize >> 20);
            unsigned long total_mb = (unsigned long)((unsigned long long)sv.f_blocks * sv.f_bsize >> 20);
            n += snprintf(out + n, outsz - n,
                "RW2_FREE_MB=%lu\nRW2_TOTAL_MB=%lu\n", free_mb, total_mb);
        }
    }

    /* ── USB drives (/proc/mounts — /dev/sdc+ are USB storage) ── */
    /* sda=SSD1, sdb=SSD2 are internal; sdc and above are external */
    {
        FILE *mf = fopen("/proc/mounts", "r");
        int usb_n = 0;
        if (mf) {
            char mline[512];
            while (usb_n < 2 && fgets(mline, sizeof(mline), mf)) {
                char dev[64], mnt[128], fst[32];
                if (sscanf(mline, "%63s %127s %31s", dev, mnt, fst) != 3) continue;
                if (strncmp(dev, "/dev/sd", 7) != 0 || dev[7] < 'c' || dev[7] > 'z') continue;
                struct statvfs sv;
                if (statvfs(mnt, &sv) != 0) continue;
                unsigned long free_mb  = (unsigned long)((unsigned long long)sv.f_bavail * sv.f_bsize >> 20);
                unsigned long total_mb = (unsigned long)((unsigned long long)sv.f_blocks * sv.f_bsize >> 20);
                n += snprintf(out + n, outsz - n,
                    "USB%d_MNT=%s\nUSB%d_FREE_MB=%lu\nUSB%d_TOTAL_MB=%lu\n",
                    usb_n, mnt, usb_n, free_mb, usb_n, total_mb);
                usb_n++;
            }
            fclose(mf);
        }
        n += snprintf(out + n, outsz - n, "USB_COUNT=%d\n", usb_n);
    }

    /* ── Hardware monitor (W83627UHG) ─────────────────────────── */
    /* hwmon index is non-deterministic across boots/module loads.
     * Try hwmon0..4 with both the old /device/ sub-path and the
     * newer direct layout. */
    {
        char  tpath[80];
        int   tv;
        /* Temperatures — use whichever hwmon responds for temp1 */
        int   hwmon_base = -1;
        int   use_device = 0;
        for (int hi = 0; hi <= 4 && hwmon_base < 0; hi++) {
            /* try new ABI first (direct), then old ABI (/device/) */
            snprintf(tpath, sizeof(tpath),
                     "/sys/class/hwmon/hwmon%d/temp1_input", hi);
            f = fopen(tpath, "r");
            if (f) { fclose(f); hwmon_base = hi; use_device = 0; continue; }
            snprintf(tpath, sizeof(tpath),
                     "/sys/class/hwmon/hwmon%d/device/temp1_input", hi);
            f = fopen(tpath, "r");
            if (f) { fclose(f); hwmon_base = hi; use_device = 1; }
        }
        if (hwmon_base >= 0) {
            const char *sub = use_device ? "/device" : "";
            for (int i = 1; i <= 3; i++) {
                snprintf(tpath, sizeof(tpath),
                         "/sys/class/hwmon/hwmon%d%s/temp%d_input",
                         hwmon_base, sub, i);
                f = fopen(tpath, "r");
                if (f) {
                    tv = 0; fscanf(f, "%d", &tv); fclose(f);
                    n += snprintf(out + n, outsz - n, "TEMP%d=%d\n", i, tv / 1000);
                }
            }
            snprintf(tpath, sizeof(tpath),
                     "/sys/class/hwmon/hwmon%d%s/fan1_input",
                     hwmon_base, sub);
            f = fopen(tpath, "r");
            if (f) { tv = 0; fscanf(f, "%d", &tv); fclose(f);
                n += snprintf(out + n, outsz - n, "FAN1_RPM=%d\n", tv); }
        }
    }

    /* ── Mode ─────────────────────────────────────────────────── */
    n += snprintf(out + n, outsz - n, "MODE=%u\n", (unsigned)g_mode);

    n += snprintf(out + n, outsz - n, "OK\n");
    return n;
}

/* Process one already-parsed command line.
 * fd >= 0: write response to fd.  fd == -1: fire-and-forget, no response. */
static void process_ctrl_cmd(const char *line, int fd)
{
#define REPLY(msg, len) do { if (fd >= 0) write_all(fd, (msg), (len)); } while (0)

    if (strcmp(line, "MIRROR_ON") == 0) {
        int f = open(MIRROR_FLAG, O_CREAT | O_WRONLY, 0644);
        if (f >= 0) close(f);
        check_mirror_flag();
        REPLY("OK\n", 3);

    } else if (strcmp(line, "MIRROR_OFF") == 0) {
        unlink(MIRROR_FLAG);
        check_mirror_flag();
        REPLY("OK\n", 3);

    } else if (strncmp(line, "TOUCH ", 6) == 0) {
        int x = 0, y = 0;
        if (sscanf(line + 6, "%d %d", &x, &y) == 2) {
            inject_touch(1, x, y);  /* pen-down */
            inject_touch(2, x, y);  /* pen-up */
            REPLY("OK\n", 3);
        }

    } else if (strncmp(line, "TOUCH_DOWN ", 11) == 0) {
        int x = 0, y = 0;
        if (sscanf(line + 11, "%d %d", &x, &y) == 2) {
            inject_touch(1, x, y);
            REPLY("OK\n", 3);
        }

    } else if (strncmp(line, "TOUCH_MOVE ", 11) == 0) {
        int x = 0, y = 0;
        if (sscanf(line + 11, "%d %d", &x, &y) == 2) {
            inject_touch(3, x, y);
            REPLY("OK\n", 3);
        }

    } else if (strncmp(line, "TOUCH_UP ", 9) == 0) {
        int x = 0, y = 0;
        if (sscanf(line + 9, "%d %d", &x, &y) == 2) {
            inject_touch(2, x, y);
            REPLY("OK\n", 3);
        }

    } else if (strncmp(line, "BUTTON ", 7) == 0) {
        const char *bname = line + 7;
        const struct btn_def *b;
        for (b = btn_table; b->name; b++) {
            if (strcmp(bname, b->name) == 0) break;
        }
        if (b->name) {
            send_rtf5_event(b->dev, b->code, 0x7fu);
            send_rtf5_event(b->dev, b->code, 0x00u);
            mode_from_btn(b->dev, b->code);
            REPLY("OK\n", 3);
        } else {
            REPLY("ERR\n", 4);
        }

    } else if (strncmp(line, "CHORD ", 6) == 0) {
        char names[8][16];
        const struct btn_def *btns[8];
        int count = 0, hold_ms = 0;
        const char *p = line + 6;
        while (*p == ' ') p++;
        /* Optional leading number = hold duration in ms (max 5000) */
        if (*p >= '0' && *p <= '9') {
            hold_ms = atoi(p);
            if (hold_ms > 5000) hold_ms = 5000;
            while (*p && *p != ' ') p++;
        }
        while (count < 8) {
            while (*p == ' ') p++;
            if (*p == '\0') break;
            int i = 0;
            while (*p && *p != ' ' && i < 15) names[count][i++] = *p++;
            names[count][i] = '\0';
            btns[count] = NULL;
            const struct btn_def *b;
            for (b = btn_table; b->name; b++) {
                if (strcmp(names[count], b->name) == 0) { btns[count] = b; break; }
            }
            count++;
        }
        if (count >= 2) {
            int ok = 1;
            for (int i = 0; i < count; i++) { if (!btns[i]) { ok = 0; break; } }
            if (ok) {
                for (int i = 0; i < count; i++)
                    send_rtf5_event(btns[i]->dev, btns[i]->code, 0x7fu);
                if (hold_ms > 0) usleep(hold_ms * 1000);
                for (int i = count - 1; i >= 0; i--)
                    send_rtf5_event(btns[i]->dev, btns[i]->code, 0x00u);
                REPLY("OK\n", 3);
            } else {
                REPLY("ERR\n", 4);
            }
        }

    } else if (strncmp(line, "WHEEL ", 6) == 0) {
        /* Data wheel: 16-byte packet, device=0x0d, field3=0x0100(CW) or 0xFF00(CCW) */
        const char *dir = line + 6;
        uint32_t field3;
        if (strcmp(dir, "CW") == 0)       field3 = 0x00000100u;
        else if (strcmp(dir, "CCW") == 0) field3 = 0x0000FF00u;
        else { REPLY("ERR\n", 4); return; }

        if (touch_fd < 0)
            touch_fd = open("/dev/rtf5", O_WRONLY);
        if (touch_fd >= 0) {
            uint32_t pkt[4];
            pkt[0] = 0x00010010u;
            pkt[1] = 0x00000000u;
            pkt[2] = 0x0000000du;
            pkt[3] = field3;
            (void)write(touch_fd, pkt, 16);
        }
        REPLY("OK\n", 3);

    } else if (strncmp(line, "SLIDER ", 7) == 0) {
        int idx = 0, val = 0;
        if (sscanf(line + 7, "%d %d", &idx, &val) == 2 &&
                idx >= 1 && idx <= 8 && val >= 0 && val <= 127) {
            send_rtf5_event(0x0eu, (uint32_t)(idx - 1), (uint32_t)val);
            REPLY("OK\n", 3);
        } else {
            REPLY("ERR\n", 4);
        }

    } else if (strncmp(line, "VSLIDER ", 8) == 0) {
        int val = 0;
        if (sscanf(line + 8, "%d", &val) == 1 && val >= 0 && val <= 127) {
            send_rtf5_event(0x0fu, 0x09u, (uint32_t)val);
            REPLY("OK\n", 3);
        } else {
            REPLY("ERR\n", 4);
        }

    } else if (strncmp(line, "KEY ", 4) == 0) {
        int code = 0, val = 0;
        if (sscanf(line + 4, "%d %d", &code, &val) == 2 &&
                code > 0 && code < 512 && (val == 0 || val == 1)) {
            inject_key(code, val);
            REPLY("OK\n", 3);
        } else {
            REPLY("ERR\n", 4);
        }

    } else if (strcmp(line, "REFRESH") == 0) {
        /* Force change-driven mode to resend the current frame on the next tick */
        shadow_valid = 0;
        REPLY("OK\n", 3);

    } else if (strcmp(line, "STATE") == 0) {
        char resp[16];
        int  rlen = snprintf(resp, sizeof(resp), "MODE=%u\n", (unsigned)g_mode);
        REPLY(resp, (size_t)rlen);

    } else if (strcmp(line, "VERSION") == 0) {
        char resp[64];
        int  rlen = snprintf(resp, sizeof(resp), "VER=%s BUILD=%s\n",
                             SCREENREMOTE_VERSION, BUILD_ID);
        REPLY(resp, (size_t)rlen);

    } else if (strcmp(line, "SYSINFO") == 0) {
        char si[2048];
        int  silen = sysinfo_collect(si, (int)sizeof(si));
        REPLY(si, (size_t)silen);

    } else if (strncmp(line, "SS_TIMEOUT ", 11) == 0) {
        int v;
        if (sscanf(line + 11, "%d", &v) == 1 && v >= 0) {
            g_ss_timeout = v;
            ss_reset(time(NULL));
            REPLY("OK\n", 3);
        } else {
            REPLY("ERR\n", 4);
        }
    }

#undef REPLY
}

/* Called when ctrl_fd (O_NONBLOCK) has data.  Reads available bytes, processes
 * complete lines.  Returns -1 if the connection closed. */
static int handle_ctrl_persistent_data(void)
{
    char buf[128];
    ssize_t n = recv(ctrl_fd, buf, sizeof(buf), 0);
    if (n <= 0) return -1;

    int i;
    for (i = 0; i < (int)n; i++) {
        char c = buf[i];
        if (ctrl_lb_n < (int)sizeof(ctrl_lb) - 1)
            ctrl_lb[ctrl_lb_n++] = c;
        if (c == '\n') {
            ctrl_lb[ctrl_lb_n - 1] = '\0';
            if (ctrl_lb_n > 1)
                process_ctrl_cmd(ctrl_lb, -1);
            ctrl_lb_n = 0;
        }
    }
    return 0;
}

/* ── PackBits RLE encoder ───────────────────────────────────────
 * Standard PackBits (Apple/TIFF variant):
 *   header 0x00–0x7F → n+1 literal bytes follow
 *   header 0x81–0xFF → repeat next byte (257-n) times
 *   header 0x80      → NOP (never emitted here)
 * Literal scan breaks on runs of 3+ identical bytes.
 * Worst-case output: n + ceil(n/128) bytes (~1.008× input). */
static uint32_t packbits_encode(const uint8_t *src, uint32_t n, uint8_t *dst)
{
    uint32_t si = 0, di = 0;
    while (si < n) {
        uint32_t run = 1;
        while (si + run < n && run < 128 && src[si + run] == src[si])
            run++;

        if (run >= 2) {
            /* Run token: header = 257-run (range 0xFF down to 0x81) */
            dst[di++] = (uint8_t)(257u - run);
            dst[di++] = src[si];
            si += run;
        } else {
            /* Literal run: gather bytes until a 3+ run begins */
            uint32_t lit = 1;
            while (si + lit < n && lit < 128) {
                uint32_t r2 = 1;
                while (si + lit + r2 < n && r2 < 3 &&
                       src[si + lit + r2] == src[si + lit])
                    r2++;
                if (r2 >= 3) break;
                lit++;
            }
            /* Literal token: header = lit-1 (range 0x00 to 0x7F) */
            dst[di++] = (uint8_t)(lit - 1u);
            memcpy(dst + di, src + si, lit);
            di += lit;
            si += lit;
        }
    }
    return di;
}

/* ── Dirty row scan ─────────────────────────────────────────────
 * Find the first and last changed row by comparing staging vs shadow
 * line by line.  Both buffers must be valid before calling this. */
static void find_dirty_rows(uint32_t *first_out, uint32_t *count_out)
{
    uint32_t first = fb_h, last = 0, y;
    for (y = 0; y < fb_h; y++) {
        if (memcmp(staging + y * fb_w, shadow + y * fb_w, fb_w) != 0) {
            if (y < first) first = y;
            last = y;
        }
    }
    if (first > last) { *first_out = 0; *count_out = 0; return; }
    *first_out = first;
    *count_out = last - first + 1;
}

/* Send a dirty-rect update with PackBits RLE payload.
 * Wire format: [payload_len LE32][first_row LE16][row_count LE16][rle_bytes…]
 * Invariant: payload_len < frame_bytes so clients can discriminate by len.
 * Falls back to send_frame_buf() if RLE expands to >= frame_bytes (degenerate). */
static int send_dirty_rect(int fd, uint32_t first_row, uint32_t row_count)
{
    uint32_t raw_size = row_count * fb_w;
    uint32_t enc_size = packbits_encode(staging + first_row * fb_w, raw_size, rle_buf);
    uint32_t payload  = 4u + enc_size;

    if (payload >= frame_bytes)
        return send_frame_buf(fd, staging);

    uint8_t hdr[8];
    hdr[0] = payload         & 0xFF;
    hdr[1] = (payload >>  8) & 0xFF;
    hdr[2] = (payload >> 16) & 0xFF;
    hdr[3] = (payload >> 24) & 0xFF;
    hdr[4] = first_row        & 0xFF;
    hdr[5] = (first_row >> 8) & 0xFF;
    hdr[6] = row_count        & 0xFF;
    hdr[7] = (row_count >> 8) & 0xFF;
    TCP_CORK_ON(fd);
    if (write_all(fd, hdr, 8) < 0 ||
        write_all(fd, rle_buf, enc_size) < 0) {
        TCP_CORK_OFF(fd);
        return -1;
    }
    TCP_CORK_OFF(fd);
    return 0;
}

/* ── Frame capture + change detection ──────────────────────────
 * Copies fb1 → staging in one device-memory pass, then compares
 * staging (RAM) against shadow (RAM).  Returns 1 if the frame
 * changed or shadow is not yet valid.
 *
 * After a successful send, swap staging ↔ shadow so shadow holds
 * the just-sent frame without an extra copy. */
static int capture_to_staging(void)
{
    if (fb1_stride == fb_w) {
        memcpy(staging, fb1_map, frame_bytes);
    } else {
        uint32_t y;
        for (y = 0; y < fb_h; y++)
            memcpy(staging + y * fb_w,
                   fb1_map + y * fb1_stride, fb_w);
    }
    if (!shadow_valid) return 1;
    return memcmp(staging, shadow, frame_bytes) != 0;
}

/* ── Main ───────────────────────────────────────────────────── */
int main(void)
{
    int stream_listen, ctrl_listen, disc_fd = -1, client_fd = -1;
    /* ctrl_fd and ctrl_lb* are file-scope globals (see top of file) */
    uint8_t client_mode = MODE_CHANGE, client_fps = FPS_MAX;
    time_t last_mirror_chk = 0, last_net_chk = 0;
    struct timespec last_frame = {0, 0};
    ss_last_chg = time(NULL);

    signal(SIGPIPE, SIG_IGN);
    signal(SIGTERM, sig_exit);
    signal(SIGINT,  sig_exit);

    read_config();

    /* Ensure runtime directory exists (binary lives here, so it always should) */
    mkdir(SCREENREMOTE_DIR, 0755);

    /* Extract and load virtual keyboard early so Eva discovers it before any client connects.
     * Use init_module(2) directly — system() needs /bin/sh which does not exist on non-rooted
     * Kronos units. Loading from the embedded buffer avoids any shell or external binary. */
    extract_ko(VKBD_KO, vkbd_ko, vkbd_ko_len);
    syscall(SYS_init_module, (void *)vkbd_ko, (unsigned long)vkbd_ko_len, "");

    for (int _vi = 0; _vi < 20 && vkbd_fd < 0; _vi++) {
        usleep(100000);
        vkbd_fd = open("/proc/.vkbd", O_WRONLY);
    }

    /* On the non-rooted boot path, screenremote starts before /sbin/init via the
     * init=/korg/kronos_init GRUB hook, so OmapVideoModule.ko (which creates /dev/fb1)
     * may not be loaded yet.  Poll until the device node appears (up to 30 s). */
    {
        int _probe, _fi;
        for (_fi = 0; _fi < 300; _fi++) {
            _probe = open(FB_SRC, O_RDONLY);
            if (_probe >= 0) { close(_probe); break; }
            usleep(100000);
        }
    }
    if (fb1_open() < 0) return 1;

    shadow = malloc(frame_bytes);
    if (!shadow) { perror("malloc shadow"); return 1; }
    staging = malloc(frame_bytes);
    if (!staging) { perror("malloc staging"); return 1; }
    rle_buf = malloc(frame_bytes * 2);
    if (!rle_buf) { perror("malloc rle_buf"); return 1; }

    check_mirror_flag();

    /* Wait for usable network */
    while (!network_ok()) {
        fprintf(stderr, "screenremote: waiting for network...\n");
        check_mirror_flag();
        sleep(5);
    }

    {
        uint32_t lan_ip = find_lan_ip();
        stream_listen = make_listen(g_stream_port, lan_ip);
        ctrl_listen   = make_listen(g_ctrl_port,   lan_ip);
        if (stream_listen < 0 || ctrl_listen < 0) {
            fprintf(stderr, "screenremote: failed to bind stream=%d ctrl=%d — change ports in %s\n",
                    g_stream_port, g_ctrl_port, CFG_PATH);
            return 1;
        }
        g_bound_ip = lan_ip;
        if (lan_ip != INADDR_ANY) {
            struct in_addr ia; ia.s_addr = lan_ip;
            fprintf(stderr, "screenremote: bound to %s\n", inet_ntoa(ia));
        }
    }
    disc_fd = make_udp_disc();
    fprintf(stderr, "screenremote: listening on %d (stream) %d (ctrl) %d (discovery)\n",
            g_stream_port, g_ctrl_port, DISC_PORT);

    for (;;) {
        fd_set rfds;
        struct timeval tv;
        time_t now = time(NULL);
        int maxfd, r;

        if (g_exit) break;

        /* Periodic mirror flag check */
        if (now - last_mirror_chk >= 1) {
            check_mirror_flag();
            last_mirror_chk = now;
        }

        /* Periodic network check — re-bind if IP changed (DHCP, link down/up) */
        if (now - last_net_chk >= 10) {
            last_net_chk = now;
            uint32_t cur_ip = find_lan_ip();
            if (cur_ip != g_bound_ip) {
                if (client_fd >= 0) { close(client_fd); client_fd = -1; }
                if (ctrl_fd  >= 0) { close(ctrl_fd);  ctrl_fd = -1; ctrl_lb_n = 0; }
                shadow_valid      = 0;
                g_ctrl_allowed_ip = 0;
                g_si_prev_valid   = 0;

                if (stream_listen >= 0) { close(stream_listen); stream_listen = -1; }
                if (ctrl_listen   >= 0) { close(ctrl_listen);   ctrl_listen   = -1; }

                if (cur_ip == INADDR_ANY) {
                    g_bound_ip = INADDR_ANY;
                    fprintf(stderr, "screenremote: network down, listeners closed\n");
                } else {
                    struct in_addr dbg;
                    dbg.s_addr = cur_ip;
                    fprintf(stderr, "screenremote: IP changed → %s, rebinding\n",
                            inet_ntoa(dbg));
                    stream_listen = make_listen(g_stream_port, cur_ip);
                    ctrl_listen   = make_listen(g_ctrl_port,   cur_ip);
                    if (stream_listen >= 0 && ctrl_listen >= 0) {
                        g_bound_ip = cur_ip;
                        fprintf(stderr, "screenremote: rebound to %s\n", inet_ntoa(dbg));
                    } else {
                        if (stream_listen >= 0) { close(stream_listen); stream_listen = -1; }
                        if (ctrl_listen   >= 0) { close(ctrl_listen);   ctrl_listen   = -1; }
                        g_bound_ip = INADDR_ANY;
                        fprintf(stderr, "screenremote: rebind failed, retry next tick\n");
                    }
                }
            }
        }

        /* Mirror update (screensaver blanks fb0 after idle timeout) */
        if (mirror_on) {
            if (g_ss_timeout > 0 && now - last_ss_chk >= SS_CHECK_S) {
                uint8_t cur[SS_SAMPLE_N];
                ss_sample(cur);
                if (!ss_prev_valid || memcmp(cur, ss_prev, SS_SAMPLE_N) != 0) {
                    ss_last_chg = now;
                    ss_active   = 0;
                    memcpy(ss_prev, cur, SS_SAMPLE_N);
                    ss_prev_valid = 1;
                }
                last_ss_chk = now;
            }
            if (g_ss_timeout > 0 && (now - ss_last_chg) >= g_ss_timeout) {
                if (!ss_active && fb0_map) {
                    memset(fb0_map, 0, fb0_stride * fb_h);
                    ss_active = 1;
                }
            } else {
                ss_active = 0;
                do_mirror();
            }
        }

        /* Change-driven frame send */
        if (client_fd >= 0 && client_mode == MODE_CHANGE) {
            struct timespec ts;
            long frame_ns = 1000000000L / client_fps;
            clock_gettime(CLOCK_MONOTONIC, &ts);
            long elapsed = (ts.tv_sec  - last_frame.tv_sec)  * 1000000000L
                         + (ts.tv_nsec - last_frame.tv_nsec);
            if (elapsed >= frame_ns) {
                last_frame = ts;

                int do_capture = 1;

                if (do_capture && capture_to_staging()) {
                    int send_ok;
                    if (!shadow_valid) {
                        send_ok = send_frame_buf(client_fd, staging) == 0;
                    } else {
                        uint32_t first_row, row_count;
                        find_dirty_rows(&first_row, &row_count);
                        if (row_count == 0 || row_count >= fb_h) {
                            send_ok = send_frame_buf(client_fd, staging) == 0;
                        } else {
                            send_ok = send_dirty_rect(client_fd, first_row, row_count) == 0;
                        }
                    }
                    if (!send_ok) {
                        close(client_fd); client_fd = -1; shadow_valid = 0;
                        g_ctrl_allowed_ip = 0;
                    } else {
                        /* Swap staging ↔ shadow: shadow now holds the sent frame. */
                        uint8_t *tmp = shadow; shadow = staging; staging = tmp;
                        shadow_valid = 1;
                    }
                }
            }
        }

        /* Build select set — listeners may be -1 during rebind */
        FD_ZERO(&rfds);
        maxfd = -1;
        if (stream_listen >= 0) {
            FD_SET(stream_listen, &rfds);
            if (stream_listen > maxfd) maxfd = stream_listen;
        }
        if (ctrl_listen >= 0) {
            FD_SET(ctrl_listen, &rfds);
            if (ctrl_listen > maxfd) maxfd = ctrl_listen;
        }
        if (disc_fd >= 0) {
            FD_SET(disc_fd, &rfds);
            if (disc_fd > maxfd) maxfd = disc_fd;
        }
        if (client_fd >= 0 && client_mode == MODE_PULL) {
            FD_SET(client_fd, &rfds);
            if (client_fd > maxfd) maxfd = client_fd;
        }
        if (ctrl_fd >= 0) {
            FD_SET(ctrl_fd, &rfds);
            if (ctrl_fd > maxfd) maxfd = ctrl_fd;
        }

        /* Adaptive timeout: wait only until the next frame is due.
         * If we're already behind (frame work exceeded the budget),
         * use tv=0 so select returns immediately instead of adding
         * an extra 1/fps delay on top of every slow frame. */
        if ((client_fd >= 0 && client_mode == MODE_CHANGE) || mirror_on) {
            long us;
            if (client_fd >= 0 && client_mode == MODE_CHANGE) {
                struct timespec ts_now;
                clock_gettime(CLOCK_MONOTONIC, &ts_now);
                long ns_since  = (ts_now.tv_sec  - last_frame.tv_sec)  * 1000000000L
                               + (ts_now.tv_nsec - last_frame.tv_nsec);
                long ns_remain = 1000000000L / (client_fps ? client_fps : FPS_MAX) - ns_since;
                us = ns_remain > 0 ? ns_remain / 1000 : 0;
            } else {
                us = 1000000L / (client_fps ? client_fps : FPS_MAX);
            }
            tv.tv_sec  = 0;
            tv.tv_usec = us;
        } else {
            tv.tv_sec  = 1;
            tv.tv_usec = 0;
        }

        if (maxfd < 0) { usleep(500000); continue; }
        r = select(maxfd + 1, &rfds, NULL, NULL, &tv);
        if (r < 0) continue;

        /* New streaming client */
        if (stream_listen >= 0 && FD_ISSET(stream_listen, &rfds)) {
            struct timeval send_to = {5, 0};
            struct sockaddr_in peer;
            socklen_t plen = sizeof(peer);
            int new_fd = accept(stream_listen, (struct sockaddr *)&peer, &plen);
            if (new_fd >= 0) {
                if (client_fd >= 0) close(client_fd);
                shadow_valid = 0;
                setsockopt(new_fd, SOL_SOCKET, SO_SNDTIMEO,
                           &send_to, sizeof(send_to));
                { int nd = 1; setsockopt(new_fd, IPPROTO_TCP, TCP_NODELAY, &nd, sizeof(nd)); }
                /* Large send buffer — bypasses net.core.wmem_max (requires root).
                 * Default wmem_max on 2.6.32 is often 128 KB; sending 480 KB/frame
                 * would force 3-4 write-block-wait-ACK cycles per frame (~40 ms each)
                 * if the buffer is smaller than the frame.  With 512 KB the kernel
                 * can absorb the full frame in one write call and send asynchronously. */
#ifndef SO_SNDBUFFORCE
#define SO_SNDBUFFORCE 32
#endif
                { int sb = 512 * 1024; setsockopt(new_fd, SOL_SOCKET, SO_SNDBUFFORCE, &sb, sizeof(sb)); }
                if (do_handshake(new_fd, &client_mode, &client_fps, &peer) < 0) {
                    close(new_fd);
                } else {
                    client_fd = new_fd;
                    g_ctrl_allowed_ip = peer.sin_addr.s_addr;
                    clock_gettime(CLOCK_MONOTONIC, &last_frame);
                    fprintf(stderr, "screenremote: client connected mode=%u fps=%u from %s\n",
                            client_mode, client_fps, inet_ntoa(peer.sin_addr));
                    /* Send the current framebuffer immediately on connect.
                     * Change-driven mode only sends when the screen changes, so a
                     * newly connected client would see a blank display until the
                     * Kronos UI moves.  Sending once here ensures the client always
                     * has a valid initial frame regardless of screen activity. */
                    if (client_mode == MODE_CHANGE) {
                        capture_to_staging();
                        if (send_frame_buf(client_fd, staging) < 0) {
                            close(client_fd); client_fd = -1;
                            g_ctrl_allowed_ip = 0;
                        } else {
                            uint8_t *tmp = shadow; shadow = staging; staging = tmp;
                            shadow_valid = 1;
                        }
                    }
                }
            }
        }

        /* Control command — only accept from the stream client's IP (or any if no
         * stream client has ever connected yet). */
        if (ctrl_listen >= 0 && FD_ISSET(ctrl_listen, &rfds)) {
            struct sockaddr_in cpeer;
            socklen_t cplen = sizeof(cpeer);
            int cfd = accept(ctrl_listen, (struct sockaddr *)&cpeer, &cplen);
            if (cfd >= 0) {
                if (g_ctrl_allowed_ip != 0 &&
                    cpeer.sin_addr.s_addr == g_ctrl_allowed_ip) {
                    /* Read the first line to decide: "CTRL_PERSIST" → persistent
                     * connection (replaces any previous ctrl_fd); anything else →
                     * one-shot command+response then close (used by QueryAsync). */
                    char firstline[2048]; int fl = 0;
                    struct timeval rto = {0, 200000};  /* 200 ms read timeout */
                    setsockopt(cfd, SOL_SOCKET, SO_RCVTIMEO, &rto, sizeof(rto));
                    while (fl < (int)sizeof(firstline) - 1) {
                        char c;
                        if (recv(cfd, &c, 1, 0) != 1) { close(cfd); cfd = -1; break; }
                        if (c == '\n') break;
                        firstline[fl++] = c;
                    }
                    if (cfd >= 0) {
                        firstline[fl] = '\0';
                        /* clear the receive timeout now */
                        struct timeval zero = {0, 0};
                        setsockopt(cfd, SOL_SOCKET, SO_RCVTIMEO, &zero, sizeof(zero));

                        if (strcmp(firstline, "CTRL_PERSIST") == 0) {
                            /* Persistent connection: replace any previous ctrl_fd */
                            if (ctrl_fd >= 0) close(ctrl_fd);
                            ctrl_fd   = cfd;
                            ctrl_lb_n = 0;
                            /* Mark non-blocking so handle_ctrl_persistent_data() never stalls */
                            int flags = fcntl(ctrl_fd, F_GETFL, 0);
                            fcntl(ctrl_fd, F_SETFL, flags | O_NONBLOCK);
                            fprintf(stderr, "screenremote: persistent ctrl from %s\n",
                                    inet_ntoa(cpeer.sin_addr));
                        } else {
                            /* One-shot: firstline already read; process it and close */
                            process_ctrl_cmd(firstline, cfd);
                            close(cfd);
                        }
                    }
                } else {
                    fprintf(stderr, "screenremote: ctrl rejected from %s\n",
                            inet_ntoa(cpeer.sin_addr));
                    close(cfd);
                }
            }
        }

        /* Persistent ctrl connection: process any available command lines */
        if (ctrl_fd >= 0 && FD_ISSET(ctrl_fd, &rfds)) {
            if (handle_ctrl_persistent_data() < 0) {
                fprintf(stderr, "screenremote: persistent ctrl disconnected\n");
                close(ctrl_fd);
                ctrl_fd        = -1;
                ctrl_lb_n      = 0;
                g_si_prev_valid = 0;
            }
        }

        /* Discovery probe */
        if (disc_fd >= 0 && FD_ISSET(disc_fd, &rfds)) {
            char buf[16];
            struct sockaddr_in from;
            socklen_t fromlen = sizeof(from);
            ssize_t n = recvfrom(disc_fd, buf, sizeof(buf) - 1, 0,
                                 (struct sockaddr *)&from, &fromlen);
            if (n >= 5 && memcmp(buf, "KSCR?", 5) == 0) {
                char resp[48];
                int rlen = snprintf(resp, sizeof(resp), "KSCR SP=%d CP=%d\n",
                                    g_stream_port, g_ctrl_port);
                sendto(disc_fd, resp, rlen, 0, (struct sockaddr *)&from, fromlen);
            }
        }

        /* Pull mode: client requests a frame */
        if (client_fd >= 0 && client_mode == MODE_PULL &&
            FD_ISSET(client_fd, &rfds)) {
            uint8_t cmd;
            ssize_t n = recv(client_fd, &cmd, 1, 0);
            if (n == 1 && cmd == 0xFF) {
                if (send_frame(client_fd) < 0) {
                    close(client_fd); client_fd = -1;
                    g_ctrl_allowed_ip = 0;
                }
            } else {
                close(client_fd); client_fd = -1;
                g_ctrl_allowed_ip = 0;
                fprintf(stderr, "screenremote: client disconnected\n");
            }
        }
    }

    /* Clean shutdown: clear fb0 so fbcon can resume, close sockets. */
    if (client_fd >= 0) close(client_fd);
    if (ctrl_fd   >= 0) close(ctrl_fd);
    if (stream_listen >= 0) close(stream_listen);
    if (ctrl_listen   >= 0) close(ctrl_listen);
    if (disc_fd >= 0) close(disc_fd);
    if (touch_fd >= 0) { close(touch_fd); touch_fd = -1; }
    if (vkbd_fd  >= 0) { close(vkbd_fd);  vkbd_fd  = -1; }
    if (kbd_fd   >= 0) { ioctl(kbd_fd, UI_DEV_DESTROY); close(kbd_fd); kbd_fd = -1; }
    fb0_close();
    fprintf(stderr, "screenremote: exited cleanly\n");
    return 0;
}
