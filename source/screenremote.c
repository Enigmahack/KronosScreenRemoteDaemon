/*
 * screenremote.c  - Kronos framebuffer streaming daemon
 * Not yet tested on Nautilus, but should work with minor tweaks if needed.
 *
 * Streams /dev/fb1 (8bpp, 800x600) over TCP port 7373 (default; set by config).
 * Mirrors fb1 to /dev/fb0 (VGA out) when /korg/rw/screenremote/.mirror_enable exists.
 *
 * Stream handshake (TCP port 7373):
 *   Client -> Server: MAGIC[4]="KSCR" + 0x02(ver) + mode(1) + fps(1) + ulen(1) + plen(1)
 *                    + username[ulen] + password[plen]
 *     mode: 0x01=Pull (server sends at fps), 0x02=Change (server sends on fb change)
 *   Server -> Client: MAGIC[4] + status(1)  [+ width_LE16 + height_LE16 + 256 xRGB8 if status=0x00]
 *     status: 0x00=ok  0x01=auth_fail  0x02=user_not_found
 *   Frames: [len_LE32][pixel_data]  (full) or dirty-rect PackBits RLE
 *
 * Control port 7374 (text line commands, newline-terminated):
 *   CTRL_PERSIST            - open a persistent session; server keeps the connection open
 *   MIRROR_ON / MIRROR_OFF  - enable / disable VGA mirror output
 *   TOUCH nx ny             - tap at normalised float coords (press + release)
 *   TOUCH_DOWN nx ny        - pen-down only
 *   TOUCH_MOVE nx ny        - pen-move (client coalesces consecutive moves)
 *   TOUCH_UP nx ny          - pen-up only
 *   BUTTON name             - press + release a named front-panel button (see btn_table[])
 *   CHORD name1 name2       - press name1, press name2, release name2, release name1
 *   WHEEL CW|CCW            - one data-wheel tick clockwise or counter-clockwise
 *   SLIDER n value          - set CC slider/knob n (1 - 8) to value (0 - 127)
 *                            Effect depends on active Control Assign page:
 *                            RT KNOBS/KARMA -> moves knob n; slider-active -> moves slider n
 *   VSLIDER value           - set value slider position (0 - 127)
 *   KEY code val            - raw key inject: code 1 - 511, val 0=release 1=press
 *   REFRESH                 - force full-frame resend (clears shadow_valid)
 *   MIDI_SEND hex           - inject raw MIDI bytes (hex pairs, spaces allowed)
 *   SYSEX hex               - send SysEx (must start F0), capture response (up to 5 s)
 *                           -> SYSEX_RESP hex\n  or  ERR TIMEOUT\n
 *   MIDI_STATUS             -> MIDI_LOADED=n\nMIDI_IN=n\nMIDI_CAPTURE=n\nOK\n
 *   SS_TIMEOUT n            - set screensaver timeout at runtime (seconds; 0 = disable)
 *   STATE                   -> MODE=N\n  (0=init 1=Setlist 2=Combi 3=Program 4=Sequence 5=Sampling 6=Global 7=Disk)
 *   VERSION                 -> VER=x.x.x BUILD=xxx\n
 *   SYSINFO                 -> multi-line key=value block terminated by OK\n
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
 *
 * Boot-safety: /korg/rw/screenremote/.boot is written at startup and deleted only once the
 * framebuffer, network, and listeners are all up.  If it exists on entry the previous boot did not
 * complete cleanly, so midi_inject is skipped for that boot.  Delete the file over FTP to re-enable.
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
#include <sys/mount.h>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <arpa/inet.h>
#include <net/if.h>
#include <ifaddrs.h>
#include <sys/wait.h>
#include <linux/fb.h>
#include <linux/uinput.h>
#include <sys/syscall.h>
#include "palette_data.h"
#include "vkbd_ko.h"
#include "midi_inject_ko.h"
#include "midi_tcp_bin.h"

/*  Keyboard injection (uinput fallback) */
#define KBD_EV_SYN  0
#define KBD_EV_KEY  1

/*  Version */
#define SCREENREMOTE_VERSION "1.8.0"
#ifndef BUILD_ID
#define BUILD_ID "dev"
#endif

/* Config */
#define SCREENREMOTE_DIR  "/korg/rw/screenremote"
#define VKBD_KO           SCREENREMOTE_DIR "/vkbd.ko"
#define MIDI_INJECT_KO    SCREENREMOTE_DIR "/midi_inject.ko"
#define MIDI_TCP_BIN      SCREENREMOTE_DIR "/midi_tcp"

#define FB_SRC       "/dev/fb1"
#define FB_DST       "/dev/fb0"
#define STREAM_PORT  7373       /* default; overridden by config file */
#define CTRL_PORT    7374       /* default; overridden by config file */
#define DISC_PORT    7372       /* fixed UDP discovery port - never configurable */
#define CFG_PATH     SCREENREMOTE_DIR "/screenremote.cfg"
#define MIRROR_FLAG  SCREENREMOTE_DIR "/.mirror_enable"

/* FTP-visible log/diagnostic folder.  /korg/rw/HD maps to the FTP root on an
 * unrooted Kronos, and this is the official ScreenRemote log folder (matches the
 * install scripts' diag_dir).  Created and chowned to the Kronos user (uid/gid
 * 500) at startup so files we drop here - notably the .boot self-recovery flag -
 * can be deleted over FTP without shell access. */
#define LOG_DIR      "/korg/rw/HD/ScreenRemote"
#define BOOT_FLAG    LOG_DIR "/.boot"

/* Kronos FTP/user account owns the log folder so its contents are FTP-deletable. */
#define KRONOS_UID   500
#define KRONOS_GID   500

static int  g_stream_port = STREAM_PORT;
static int  g_ctrl_port   = CTRL_PORT;
#define PAL_ENTRIES  256
#define FPS_MAX      15
#define MODE_PULL    0x01
#define MODE_CHANGE  0x02

static const uint8_t MAGIC[4] = {'K','S','C','R'};

/*  Framebuffer state */
static int       fb1_fd = -1,  fb0_fd = -1;
static uint8_t  *fb1_map = NULL, *fb0_map = NULL;
static uint32_t  fb1_stride, fb0_stride;
static uint32_t  fb_w, fb_h;           /* 800, 600 */
static uint32_t  frame_bytes;          /* fb_w * fb_h */

static uint16_t  pal_r[PAL_ENTRIES];   /* raw palette - used for streaming handshake */
static uint16_t  pal_g[PAL_ENTRIES];
static uint16_t  pal_b[PAL_ENTRIES];
static uint16_t  pal_t[PAL_ENTRIES];

static uint8_t  *shadow  = NULL;       /* last-sent frame, fb_w*fb_h bytes */
static uint8_t  *staging = NULL;       /* current-tick capture of fb1, fb_w*fb_h bytes */
static uint8_t  *rle_buf = NULL;       /* PackBits encode scratch, 2*frame_bytes */
static int       shadow_valid = 0;

/* Mirror state */
static int       mirror_on = 0;

/* Screensaver (VGA/fb0 only) */
#define SS_TIMEOUT_DEF  300   /* seconds idle before blanking; 0=disabled */
#define SS_CHECK_S        5   /* how often to sample fb1 for changes */
#define SS_SAMPLE_N      16   /* pixel positions sampled per check */

static int      g_ss_timeout  = SS_TIMEOUT_DEF;
static int      ss_active     = 0;    /* 1 = fb0 is currently blanked */
static time_t   ss_last_chg   = 0;   /* last time fb1 pixels changed */
static time_t   last_ss_chk   = 0;
static uint8_t  ss_prev[SS_SAMPLE_N];
static int      ss_prev_valid = 0;

/* Touch calibration */
static int g_touch_x_offset = 10;   /* pixels added to x before ADC scaling      */
static int g_touch_x_range  = 813;  /* total pixel span -> ADC 0-255             */
static int g_touch_y_offset = 20;   /* pixels added to y before ADC scaling      */
static int g_touch_y_range  = 638;  /* total pixel span -> ADC 0-255             */

/* Mode state */
static uint32_t g_mode        = 0;   /* 0=init 1=Setlist 2=Combi 3=Program
                                         4=Sequence 5=Sampling 6=Global 7=Disk */

/* Sysinfo CPU snapshot (populated on-demand, only while connected) */
typedef struct {
    unsigned long user, nice, sys, idle, iowait, irq, softirq;
} cpu_snap_t;
#define SI_NCPU 4
static cpu_snap_t g_si_prev[SI_NCPU + 1]; /* [0]=aggregate  [1..4]=per-cpu  */
static int        g_si_prev_valid = 0;

/* Touch / button injection */
static int touch_fd = -1;  /* fd to /dev/rtf5 O_WRONLY - injects into Eva's FIFO */
static int vkbd_fd  = -1;  /* fd to /proc/.vkbd (vkbd.ko virtual keyboard) */
static int kbd_fd   = -1;  /* fd to physical USB keyboard evdev node (fallback) */
static int midi_in_fd    = -1;  /* fd to /proc/.midi_in (MIDI injection) */
static int midi_cap_fd   = -1;  /* TCP socket to midi_tcp child on localhost */
static pid_t midi_cap_pid = -1;
static int g_midi_loaded = 0;
#define MIDI_TCP_PORT 9875

/* Button table - pkt[2]=dev, pkt[3]=code, pkt[4]=0x7f/0x00 for press/release */
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

/* Access control */
/* IP (network byte order) of the current stream client. Only that host is
 * allowed to send control commands. 0 = no stream client, 1 = reject all. */
static uint32_t g_ctrl_allowed_ip = 0;

/* Currently bound listen address (network byte order). Tracked so we can
 * detect IP changes and rebind the stream/ctrl listeners. */
static uint32_t g_bound_ip = INADDR_ANY;

/* Persistent control connection */
static int  ctrl_fd     = -1;   /* accepted persistent ctrl socket, -1 if none */
static char ctrl_lb[2048];      /* partial line accumulation buffer */
static int  ctrl_lb_n   = 0;    /* bytes in ctrl_lb */

/* Signal flag */
static volatile sig_atomic_t g_exit = 0;

static void sig_exit(int sig) { (void)sig; g_exit = 1; }

/* Kernel message log (dmesg) */
/* I/O helper */
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

/* Emergency fallback authentication
 * When KronosNet.conf does not recognise the user, username "kronos" with the
 * device's PublicID (read from /proc/id, dashes optional) is accepted for
 * screen connect only (not FTP).  No directory flag required. */
static char g_pubid[17]; /* 16 hex chars + NUL, populated at startup */

static void read_pubid(void)
{
    FILE *f = fopen("/proc/id", "r");
    char raw[32];
    int i, o = 0;
    g_pubid[0] = '\0';
    if (!f) return;
    if (!fgets(raw, sizeof(raw), f)) { fclose(f); return; }
    fclose(f);
    for (i = 0; raw[i] && o < 16; i++)
        if (raw[i] != '-' && raw[i] != '\n' && raw[i] != '\r')
            g_pubid[o++] = raw[i];
    g_pubid[o] = '\0';
}

static void strip_dashes(const char *in, char *out, int outsz)
{
    int o = 0;
    while (*in && o < outsz - 1) {
        if (*in != '-') out[o++] = *in;
        in++;
    }
    out[o] = '\0';
}

/* Check /korg/rw/Startup/KronosNet.conf - Korg's UI-managed credential store.
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
    if (strcmp(stored_user, user) != 0) return -1;  /* not the KronosNet user */
    return (strcmp(stored_pass, pass) == 0) ? 0 : 1;
}

/* Validate credentials.  Priority:
 *   1. KronosNet.conf  (Korg UI-managed, covers the 'kronos' / network user)
 *   2. PublicID fallback  (emergency recovery for screenconnect)
 * Returns 0=ok, -1=wrong password, -2=lookup error.
 * *out_reason describes the failure (never NULL on error). */
static int check_auth(const char *user, const char *pass, const char **out_reason)
{
    /* KronosNet.conf */
    int kr = kronosnet_auth(user, pass);
    if (kr == 0) { *out_reason = NULL; return 0; }
    if (kr == 1) { *out_reason = "wrong password"; return -1; }

    /* PublicID fallback */
    if (strcmp(user, "kronos") == 0 && g_pubid[0]) {
        char pass_stripped[17];
        strip_dashes(pass, pass_stripped, sizeof(pass_stripped));
        if (strcmp(pass_stripped, g_pubid) == 0) {
            *out_reason = NULL;
            return 0;
        }
        *out_reason = "wrong password";
        return -1;
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

/* Apply palette to fb0 */
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

/* Framebuffer open/close */
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
    if (fvar.bits_per_pixel != 8) {
        fprintf(stderr, "screenremote: fb1 bpp=%u, expected 8\n",
                fvar.bits_per_pixel);
        return -1;
    }
    fb_w        = fvar.xres;
    fb_h        = fvar.yres;
    fb1_stride  = ffix.line_length;
    if (fb_w == 0 || fb_h == 0 || fb1_stride < fb_w) {
        fprintf(stderr, "screenremote: fb1 bad geometry %ux%u stride=%u\n",
                fb_w, fb_h, fb1_stride);
        return -1;
    }
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

/* Screensaver helpers */
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

/* Mirror */
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

/* Network */
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

/* Config */
static void read_config(void)
{
    FILE *f = fopen(CFG_PATH, "r");
    if (!f) return;
    char line[128];
    while (fgets(line, sizeof(line), f)) {
        int v;
        if (sscanf(line, "stream_port=%d", &v) == 1 && v > 0 && v <= 65535)
           g_stream_port = v;
        else if (sscanf(line, "ctrl_port=%d",   &v) == 1 && v > 0 && v <= 65535)
           g_ctrl_port = v;
        else if (sscanf(line, "screensaver_timeout=%d", &v) == 1 && v >= 0)
           g_ss_timeout = v;
        else if (sscanf(line, "touch_x_offset=%d", &v) == 1)
           g_touch_x_offset = v;
        else if (sscanf(line, "touch_x_range=%d", &v) == 1 && v > 0)
           g_touch_x_range = v;
        else if (sscanf(line, "touch_y_offset=%d", &v) == 1)
           g_touch_y_offset = v;
        else if (sscanf(line, "touch_y_range=%d", &v) == 1 && v > 0)
           g_touch_y_range = v;
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
    /* Backlog >1: a burst of one-shot ctrl connections (the client opens a fresh
     * connection per MIDI_SEND) must not be refused while the loop services the
     * previous one. */
    if (bind(fd, (struct sockaddr *)&a, sizeof(a)) < 0 ||
        listen(fd, 16) < 0) {
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

/* Protocol */
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
    auth = check_auth(user, pass, &auth_reason);
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
    if (*mode_out != MODE_PULL && *mode_out != MODE_CHANGE) {
        fail[4] = 0x01;
        write_all(fd, fail, 5);
        log_access(peer_ip, 0, "invalid stream mode");
        return -1;
    }
    *fps_out  = hdr[6] ? hdr[6] : FPS_MAX;
    if (*fps_out > FPS_MAX) *fps_out = FPS_MAX;

    i = 0;
    memcpy(rsp + i, MAGIC, 4);              i += 4;
    rsp[i++] = 0x00;                       /* status ok */
    rsp[i++] = (uint8_t)(fb_w & 0xFF);     /* width LE16 */
    rsp[i++] = (uint8_t)(fb_w >> 8);
    rsp[i++] = (uint8_t)(fb_h & 0xFF);     /* height LE16 */
    rsp[i++] = (uint8_t)(fb_h >> 8);
    for (j = 0; j < PAL_ENTRIES; j++) {    /* palette: 256  x RGB8 */
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

    /* Primary: vkbd.ko proc interface - loaded at startup, reopen if stale */
    if (vkbd_fd < 0)
        vkbd_fd = open("/proc/.vkbd", O_WRONLY);
    if (vkbd_fd >= 0) {
        n = snprintf(buf, sizeof(buf), "%d %d\n", code, val);
        if (write(vkbd_fd, buf, n) >= 0) return;
        close(vkbd_fd); vkbd_fd = -1;
    }

    /* Fallback: /dev/uinput virtual device.  Writing to /dev/input/eventX does NOT
     * inject events - that requires uinput.  Open and configure on first use. */
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
    int x_range = g_touch_x_range;
    int y_range = g_touch_y_range;
    int cx = x + g_touch_x_offset;
    int cy = y + g_touch_y_offset;
    uint32_t h_adc = cx <= 0 ? 0u : (cx >= x_range ? 255u
                   : (uint32_t)(cx * 255 + x_range / 2) / (uint32_t)x_range);
    uint32_t v_adc = cy <= 0 ? 0u : (cy >= y_range ? 255u
                   : (uint32_t)(cy * 255 + y_range / 2) / (uint32_t)y_range);
    send_rtf5_event(0x11u, (uint32_t)type, v_adc | (h_adc << 8u));
}

/* Embedded .ko extraction */
static void extract_ko(const char *path, const unsigned char *data, unsigned int len)
{
    struct stat st;
    /* Skip write if file already exists with the correct size. */
    if (stat(path, &st) == 0 && (unsigned int)st.st_size == len)
        return;
    int fd = open(path, O_WRONLY | O_CREAT | O_TRUNC, 0644);
    if (fd >= 0) { write(fd, data, len); close(fd); }
}

/* Mode button keycode - g_mode */
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
#define SI_APPEND(...) do { \
    if (n < outsz) n += snprintf(out + n, outsz - n, __VA_ARGS__); \
    if (n >= outsz) n = outsz - 1; \
} while (0)

    /*  /proc/stat - CPU delta */
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

    /*  /proc/uptime */
    {
        unsigned long up = 0;
        f = fopen("/proc/uptime", "r");
        if (f) { fscanf(f, "%lu", &up); fclose(f); }
        SI_APPEND("UPTIME=%lu\n", up);
    }

    /*  /proc/loadavg */
    {
        float l1 = 0, l5 = 0, l15 = 0;
        f = fopen("/proc/loadavg", "r");
        if (f) { fscanf(f, "%f %f %f", &l1, &l5, &l15); fclose(f); }
        SI_APPEND("LOAD=%.2f %.2f %.2f\n", l1, l5, l15);
    }

    /*  /proc/meminfo */
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
        SI_APPEND("MEM_TOTAL_KB=%lu\nMEM_FREE_KB=%lu\nMEM_AVAIL_KB=%lu\n",
            total, mem_free, mem_free + bufs + cached);
    }

    /*  CPU percentages */
    SI_APPEND("CPU_PCT=%d\n", cpu_pct[0]);
    for (i = 0; i < ncpu; i++)
        SI_APPEND("CPU%d_PCT=%d\n", i, cpu_pct[i + 1]);

    /*  /proc/KorgUsbAudio */
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
        SI_APPEND("AUDIO_SR=%u\nAUDIO_OUT_CH=%u\nAUDIO_RTO=%lu\nAUDIO_MIDI_RT=%lu\n",
            sr, och, rto, midi_rt);
    }

    /*  /korg/rw disk space (SSD 1) */
    {
        struct statvfs sv;
        if (statvfs("/korg/rw", &sv) == 0) {
            unsigned long free_mb  = (unsigned long)((unsigned long long)sv.f_bavail * sv.f_bsize >> 20);
            unsigned long total_mb = (unsigned long)((unsigned long long)sv.f_blocks * sv.f_bsize >> 20);
            SI_APPEND("DISK_FREE_MB=%lu\nDISK_TOTAL_MB=%lu\n", free_mb, total_mb);
        }
    }

    /*  /korg/rw2 disk space (SSD 2) */
    {
        struct statvfs sv;
        if (statvfs("/korg/rw2", &sv) == 0) {
            unsigned long free_mb  = (unsigned long)((unsigned long long)sv.f_bavail * sv.f_bsize >> 20);
            unsigned long total_mb = (unsigned long)((unsigned long long)sv.f_blocks * sv.f_bsize >> 20);
            SI_APPEND("RW2_FREE_MB=%lu\nRW2_TOTAL_MB=%lu\n", free_mb, total_mb);
        }
    }

    /* USB drives (/proc/mounts - /dev/sdc+ are USB storage) */
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
                SI_APPEND("USB%d_MNT=%s\nUSB%d_FREE_MB=%lu\nUSB%d_TOTAL_MB=%lu\n",
                    usb_n, mnt, usb_n, free_mb, usb_n, total_mb);
                usb_n++;
            }
            fclose(mf);
        }
        SI_APPEND("USB_COUNT=%d\n", usb_n);
    }

    /* Hardware monitor */
    /* hwmon index is non-deterministic across boots/module loads.
     * Try hwmon0..4 with both the old /device/ sub-path and the
     * newer direct layout. */
    {
        char  tpath[80];
        int   tv;
        /* Temperatures - use whichever hwmon responds for temp1 */
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
                    SI_APPEND("TEMP%d=%d\n", i, tv / 1000);
                }
            }
            snprintf(tpath, sizeof(tpath),
                     "/sys/class/hwmon/hwmon%d%s/fan1_input",
                     hwmon_base, sub);
            f = fopen(tpath, "r");
            if (f) { tv = 0; fscanf(f, "%d", &tv); fclose(f);
                SI_APPEND("FAN1_RPM=%d\n", tv); }
        }
    }

    /* Mode */
    SI_APPEND("MODE=%u\n", (unsigned)g_mode);

    SI_APPEND("OK\n");
#undef SI_APPEND
    return n;
}

/* MIDI helpers */

static void resolve_kallsyms(unsigned long *recv_fn,    unsigned long *reg_fn,
                              unsigned long *dispatch_fn)
{
    FILE *f = fopen("/proc/kallsyms", "r");
    char line[256];
    *recv_fn = *reg_fn = *dispatch_fn = 0;
    if (!f) return;
    while (fgets(line, sizeof(line), f)) {
        unsigned long addr; char type, name[256];
        if (sscanf(line, "%lx %c %255s", &addr, &type, name) != 3) continue;
        if      (!*recv_fn     && strstr(name, "MidiInPortGeneric7Receive"))   *recv_fn     = addr;
        else if (!*reg_fn      && strstr(name, "RegisterMidiInPort"))          *reg_fn      = addr;
        else if (!*dispatch_fn && strstr(name, "ReadNextMessageEPhj"))         *dispatch_fn = addr;
        if (*recv_fn && *reg_fn && *dispatch_fn) break;
    }
    fclose(f);
}


/* Private mountpoint used to reach sysfs when the system /sys isn't mounted
 * yet (GRUB-hook boot path runs screenremote concurrently with /sbin/init). */
#define OA_SYS_PRIV  SCREENREMOTE_DIR "/.sysfs"

/* Wait until OA reaches MODULE_STATE_LIVE, read via /sys/module/OA/initstate.
 *
 * midi_inject reads OA symbol addresses and patches OA .text (the MIDI
 * trampolines), so it must only be loaded once OA is fully initialised.
 * Loading it while OA is still COMING - shown as "OA(P+)" in the oops "Modules
 * linked in" list - faults in the module loader (module_put) during
 * init_module.  /proc/.oacmd appears while OA is still COMING, so it is NOT a
 * sufficient gate.
 *
 * CRUCIAL: we must NOT probe module state via /proc/modules on this kernel.
 * Reading it makes m_show() call module_refcount() on every module, which
 * dereferences the per-cpu refptr of a COMING module whose per-cpu area isn't
 * mapped yet - an instant kernel oops (troubleshooting/boot_kmsg.log:
 * "module_refcount+0x25").  sysfs initstate is refcount-safe: the kernel's
 * show_initstate() reads only mod->state ("live"/"coming"/"going").
 *
 * On the GRUB-hook path /sys may not be mounted, so fall back to mounting a
 * private sysfs at OA_SYS_PRIV (a mountpoint we own - neither depends on nor
 * collides with the system /sys mount).  Returns 1 once OA is live, 0 on
 * timeout.  Polls in 100 ms steps. */
static int wait_for_oa_live(int max_deciseconds)
{
    const char *path = "/sys/module/OA/initstate";
    char privpath[256];
    int mounted_priv = 0;
    int i, result = 0;

    /* Prefer the system /sys if already mounted; otherwise mount our own. */
    if (access("/sys/module", F_OK) != 0) {
        mkdir(OA_SYS_PRIV, 0700);
        if (mount("sysfs", OA_SYS_PRIV, "sysfs", 0, NULL) == 0) {
            mounted_priv = 1;
            snprintf(privpath, sizeof(privpath),
                     "%s/module/OA/initstate", OA_SYS_PRIV);
            path = privpath;
        }
    }

    for (i = 0; i < max_deciseconds; i++) {
        int fd = open(path, O_RDONLY);
        if (fd >= 0) {
            char buf[16];
            int n = (int)read(fd, buf, sizeof(buf) - 1);
            close(fd);
            if (n > 0) {
                buf[n] = '\0';
                if (strncmp(buf, "live", 4) == 0) { result = 1; break; }
            }
        }
        usleep(100000);
    }

    if (mounted_priv) {
        umount(OA_SYS_PRIV);
        rmdir(OA_SYS_PRIV);
    }
    return result;
}


static int hex_decode(const char *hex, uint8_t *out, int maxlen)
{
    int len = 0;
    while (*hex && len < maxlen) {
        while (*hex == ' ') hex++;
        if (!*hex) break;
        unsigned int b;
        if (sscanf(hex, "%2x", &b) != 1) return -1;
        out[len++] = (uint8_t)b;
        hex += 2;
    }
    return len;
}

/* ---- CC injection throttle ----------------------------------------------
 * Continuous controllers can be moved fast enough to flood OA.ko's MIDI-in
 * queue.  Mod (CC#1) and breath (CC#2) are typically routed as AMS sources to
 * many engine parameters, so OA spends real CPU per change and its *in-order*
 * queue backs up - delaying other MIDI (e.g. pitch bend) stuck behind it.
 *
 * For a continuous controller only the latest value matters, so we rate-limit
 * per (status,controller): inject at most once per CC_MIN_INTERVAL_MS, keep
 * only the most recent value, and always deliver the final position via the
 * flush in the main loop.  Notes, pitch bend and SysEx are never throttled. */
#define CC_SLOTS            32
#define CC_MIN_INTERVAL_MS  7

struct cc_slot {
    int             key;        /* (status<<8)|controller; 0 = free slot      */
    uint8_t         val;        /* latest value seen                          */
    int             pending;    /* 1 = val not yet injected                   */
    struct timespec last;       /* time of last actual injection              */
};
static struct cc_slot cc_tab[CC_SLOTS];   /* zero-init: all slots free (key 0)*/

static long ts_ms_diff(const struct timespec *a, const struct timespec *b)
{
    return (a->tv_sec - b->tv_sec) * 1000L + (a->tv_nsec - b->tv_nsec) / 1000000L;
}

static void cc_inject(int status, int ctrl, uint8_t val)
{
    uint8_t mb[3] = { (uint8_t)status, (uint8_t)ctrl, val };
    if (midi_in_fd >= 0) (void)write(midi_in_fd, mb, 3);
}

/* If `mb` is a 3-byte Control Change, rate-limit it and return 1 (handled,
 * possibly deferred).  Returns 0 for anything else so the caller injects it
 * normally and immediately. */
static int cc_throttle(const uint8_t *mb, int mlen)
{
    int key, i, slot = -1, free_i = -1;
    struct timespec now;

    if (mlen != 3 || (mb[0] & 0xF0) != 0xB0) return 0;
    key = ((int)mb[0] << 8) | mb[1];          /* always >= 0xB000, never 0 */

    clock_gettime(CLOCK_MONOTONIC, &now);
    for (i = 0; i < CC_SLOTS; i++) {
        if (cc_tab[i].key == key) { slot = i; break; }
        if (cc_tab[i].key == 0 && free_i < 0) free_i = i;
    }
    if (slot < 0) {
        if (free_i < 0) return 0;             /* table full: inject normally */
        slot = free_i;
        cc_tab[slot].key  = key;
        cc_tab[slot].last = now;
        cc_tab[slot].last.tv_sec -= 1;        /* force first message to inject */
        cc_tab[slot].pending = 0;
    }

    cc_tab[slot].val = mb[2];
    if (ts_ms_diff(&now, &cc_tab[slot].last) >= CC_MIN_INTERVAL_MS) {
        cc_inject(mb[0], mb[1], mb[2]);
        cc_tab[slot].last    = now;
        cc_tab[slot].pending = 0;
    } else {
        cc_tab[slot].pending = 1;             /* hold; landed by cc_flush() */
    }
    return 1;
}

/* Inject any held CC whose hold interval has elapsed.  Called once per main
 * loop iteration.  Returns 1 if a value is still being held (so the caller can
 * shorten its select() timeout and land the final position promptly). */
static int cc_flush(void)
{
    struct timespec now;
    int i, still_pending = 0;

    clock_gettime(CLOCK_MONOTONIC, &now);
    for (i = 0; i < CC_SLOTS; i++) {
        if (cc_tab[i].key == 0 || !cc_tab[i].pending) continue;
        if (ts_ms_diff(&now, &cc_tab[i].last) >= CC_MIN_INTERVAL_MS) {
            cc_inject(cc_tab[i].key >> 8, cc_tab[i].key & 0xFF, cc_tab[i].val);
            cc_tab[i].last    = now;
            cc_tab[i].pending = 0;
        } else {
            still_pending = 1;
        }
    }
    return still_pending;
}

#define SYSEX_CAP_SIZE 65536
static uint8_t sysex_capbuf[SYSEX_CAP_SIZE];
static char    sysex_hexbuf[SYSEX_CAP_SIZE * 2 + 32];

/* Async SysEx capture state
 * Instead of blocking the main select() loop while waiting for a
 * MIDI response (up to 5 s), we send the SysEx, add midi_cap_fd
 * to the select set, and collect the response across iterations.
 * Touch/button/frame processing continues uninterrupted. */
static int            sysex_pending     = 0;    /* 1 = capture in progress          */
static int            sysex_resp_fd     = -1;   /* fd to send response to when done */
static int            sysex_cap_offset  = 0;    /* bytes captured so far            */
static int            sysex_got_first   = 0;    /* received at least one byte       */
static int            sysex_in_f0       = 0;    /* inside F0...F7 block             */
static struct timespec sysex_t0;                /* capture start time               */
#define SYSEX_INITIAL_TIMEOUT_MS  5000
#define SYSEX_TAIL_TIMEOUT_MS     1000

/* Bring up the loopback interface via ioctl.  midi_tcp listens on 127.0.0.1,
 * which the Kronos does not configure by default.  Done with ioctl rather than
 * system("ifconfig ...") because non-rooted units have no /bin/sh - the same
 * reason module loading uses init_module(2) directly. */
static void bring_up_loopback(void)
{
    struct ifreq ifr;
    struct sockaddr_in *sin = (struct sockaddr_in *)&ifr.ifr_addr;
    int s = socket(AF_INET, SOCK_DGRAM, 0);
    if (s < 0) return;

    memset(&ifr, 0, sizeof(ifr));
    strncpy(ifr.ifr_name, "lo", IFNAMSIZ - 1);

    sin->sin_family      = AF_INET;
    sin->sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    ioctl(s, SIOCSIFADDR, &ifr);

    if (ioctl(s, SIOCGIFFLAGS, &ifr) == 0) {
        ifr.ifr_flags |= IFF_UP | IFF_RUNNING;
        ioctl(s, SIOCSIFFLAGS, &ifr);
    }
    close(s);
}

static void start_midi_capture(void)
{
    pid_t pid;
    int i;

    extract_ko(MIDI_TCP_BIN, midi_tcp_bin, midi_tcp_bin_len);
    chmod(MIDI_TCP_BIN, 0755);

    /* Ensure loopback is up - Kronos doesn't configure it by default */
    bring_up_loopback();

    pid = fork();
    if (pid < 0) return;

    if (pid == 0) {
        execl(MIDI_TCP_BIN, "midi_tcp", "-s", NULL);
        _exit(127);
    }

    midi_cap_pid = pid;
    fprintf(stderr, "screenremote: midi_tcp pid=%d port=%d\n",
            (int)pid, MIDI_TCP_PORT);

    for (i = 0; i < 30 && midi_cap_fd < 0; i++) {
        struct sockaddr_in sa;
        int fd;
        usleep(200000);
        fd = socket(AF_INET, SOCK_STREAM, 0);
        if (fd < 0) continue;
        memset(&sa, 0, sizeof(sa));
        sa.sin_family = AF_INET;
        sa.sin_port = htons(MIDI_TCP_PORT);
        sa.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
        if (connect(fd, (struct sockaddr *)&sa, sizeof(sa)) == 0) {
            int nd = 1;
            setsockopt(fd, IPPROTO_TCP, TCP_NODELAY, &nd, sizeof(nd));
            midi_cap_fd = fd;
        } else {
            close(fd);
        }
    }
}


/* Start an async SysEx capture: send the message and let the main loop
 * collect the response via sysex_poll().  Returns 1 if started, 0 on error. */
static int sysex_start_async(const uint8_t *sysex, int sysex_len, int resp_fd)
{
    if (midi_cap_fd < 0 || sysex_pending)
        return 0;

    send(midi_cap_fd, sysex, sysex_len, 0);

    sysex_pending    = 1;
    sysex_resp_fd    = resp_fd;
    sysex_cap_offset = 0;
    sysex_got_first  = 0;
    sysex_in_f0      = 0;
    clock_gettime(CLOCK_MONOTONIC, &sysex_t0);
    return 1;
}

static void sysex_send_response(void)
{
    if (sysex_resp_fd >= 0 && sysex_cap_offset > 0) {
        int rlen = sprintf(sysex_hexbuf, "SYSEX_RESP ");
        int i;
        for (i = 0; i < sysex_cap_offset; i++)
            rlen += sprintf(sysex_hexbuf + rlen, "%02X", sysex_capbuf[i]);
        sysex_hexbuf[rlen++] = '\n';
        write_all(sysex_resp_fd, sysex_hexbuf, rlen);
    } else if (sysex_resp_fd >= 0) {
        write_all(sysex_resp_fd, "ERR TIMEOUT\n", 12);
    }
}

static void sysex_finish(void)
{
    sysex_send_response();
    /* Close one-shot fd; persistent ctrl_fd stays open */
    if (sysex_resp_fd >= 0 && sysex_resp_fd != ctrl_fd)
        close(sysex_resp_fd);
    sysex_resp_fd    = -1;
    sysex_pending    = 0;
    sysex_cap_offset = 0;
    sysex_in_f0      = 0;
}

/* Called from the main loop when select() reports midi_cap_fd readable,
 * or on each iteration to check the timeout.  Non-blocking.
 *
 * midi_tcp now forwards all parsed MIDI messages (not only SysEx responses),
 * so this function always drains midi_cap_fd to prevent socket-buffer buildup.
 * When a SysEx capture is pending it filters for F0...F7 blocks only;
 * interleaved channel messages and real-time bytes are discarded. */
static void sysex_poll(int readable)
{
    if (readable && midi_cap_fd >= 0) {
        uint8_t tmp[4096];
        int n;
        /* Drain the whole socket buffer each call: midi_tcp forwards all MIDI
         * out, so a burst could exceed one 4 KB read and stall midi_tcp if we
         * consumed only one chunk per iteration. */
        while ((n = recv(midi_cap_fd, tmp, sizeof(tmp), MSG_DONTWAIT)) > 0) {
            if (sysex_pending) {
                int j;
                for (j = 0; j < n; j++) {
                    uint8_t b = tmp[j];
                    /* Real-time bytes may appear anywhere; skip without
                     * affecting the SysEx parse state. */
                    if (b >= 0xF8) continue;
                    /* SysEx start: begin capturing; discard any partial
                     * previous attempt if responses were back-to-back. */
                    if (b == 0xF0) {
                        sysex_in_f0      = 1;
                        sysex_cap_offset = 0;
                    }
                    if (sysex_in_f0 && sysex_cap_offset < SYSEX_CAP_SIZE) {
                        sysex_capbuf[sysex_cap_offset++] = b;
                        if (!sysex_got_first) {
                            sysex_got_first = 1;
                            clock_gettime(CLOCK_MONOTONIC, &sysex_t0);
                        }
                    }
                    /* SysEx end: deliver the captured response, then keep
                     * draining the rest of the buffer (now discarded). */
                    if (b == 0xF7 && sysex_in_f0) {
                        sysex_finish();
                        break;
                    }
                    /* Any other status byte that isn't a real-time or SysEx
                     * byte resets SysEx tracking (stray non-SysEx MIDI). */
                    if ((b & 0x80) && b != 0xF0)
                        sysex_in_f0 = 0;
                }
            }
            if (n < (int)sizeof(tmp))
                break;   /* socket buffer drained */
        }
    }

    if (!sysex_pending) return;

    /* Timeout check */
    struct timespec now;
    clock_gettime(CLOCK_MONOTONIC, &now);
    long ms = (now.tv_sec - sysex_t0.tv_sec) * 1000
            + (now.tv_nsec - sysex_t0.tv_nsec) / 1000000;
    long limit = sysex_got_first ? SYSEX_TAIL_TIMEOUT_MS : SYSEX_INITIAL_TIMEOUT_MS;
    if (ms >= limit)
        sysex_finish();
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

    } else if (strncmp(line, "MIDI_SEND ", 10) == 0) {
        if (midi_in_fd < 0) { REPLY("ERR MIDI_NOT_LOADED\n", 20); }
        else {
            uint8_t mb[4096];
            int mlen = hex_decode(line + 10, mb, sizeof(mb));
            if (mlen > 0) {
                /* Continuous CC is rate-limited/coalesced so a fast controller
                 * sweep can't flood OA's MIDI queue and stall other messages.
                 * Everything else (notes, bend, multi-byte) injects as-is. */
                if (!cc_throttle(mb, mlen))
                    (void)write(midi_in_fd, mb, mlen);
                REPLY("OK\n", 3);
            } else {
                REPLY("ERR BAD_HEX\n", 12);
            }
        }

    } else if (strncmp(line, "SYSEX ", 6) == 0) {
        if (midi_in_fd < 0 || midi_cap_fd < 0) {
            REPLY("ERR MIDI_NOT_LOADED\n", 20);
        } else if (sysex_pending) {
            REPLY("ERR SYSEX_BUSY\n", 15);
        } else {
            uint8_t sb[4096];
            int slen = hex_decode(line + 6, sb, sizeof(sb));
            if (slen <= 0 || sb[0] != 0xF0) {
                REPLY("ERR BAD_SYSEX\n", 14);
            } else {
                /* Start async capture - response sent by sysex_poll/sysex_finish
                 * in a later select iteration.  fd is kept open until then. */
                if (!sysex_start_async(sb, slen, fd))
                    REPLY("ERR SYSEX_FAIL\n", 15);
                /* Caller must NOT close fd if sysex_pending && sysex_resp_fd == fd */
            }
        }

    } else if (strcmp(line, "MIDI_STATUS") == 0) {
        char resp[256];
        int rlen = snprintf(resp, sizeof(resp),
            "MIDI_LOADED=%d\nMIDI_IN=%d\nMIDI_CAPTURE=%d\nOK\n",
            g_midi_loaded, midi_in_fd >= 0 ? 1 : 0, midi_cap_fd >= 0 ? 1 : 0);
        REPLY(resp, rlen);
    }

#undef REPLY
}

/* Called when ctrl_fd (O_NONBLOCK) has data.  Reads available bytes, processes
 * complete lines.  Returns -1 if the connection closed. */
static int handle_ctrl_persistent_data(void)
{
    char buf[2048];
    ssize_t n;

    /* Drain everything currently buffered.  ctrl_fd is O_NONBLOCK, so a burst of
     * commands (e.g. rapid MIDI_SEND) is consumed in one iteration instead of
     * 128 bytes at a time - otherwise latency accumulates across select loops. */
    for (;;) {
        n = recv(ctrl_fd, buf, sizeof(buf), 0);
        if (n > 0) {
            int i;
            for (i = 0; i < (int)n; i++) {
                char c = buf[i];
                if (ctrl_lb_n < (int)sizeof(ctrl_lb) - 1)
                    ctrl_lb[ctrl_lb_n++] = c;
                if (c == '\n') {
                    ctrl_lb[ctrl_lb_n - 1] = '\0';
                    if (ctrl_lb_n > 1)
                        process_ctrl_cmd(ctrl_lb, ctrl_fd);
                    ctrl_lb_n = 0;
                }
            }
            if (n < (ssize_t)sizeof(buf))
                break;                      /* socket buffer drained */
        } else if (n == 0) {
            return -1;                      /* peer closed */
        } else {
            if (errno == EAGAIN || errno == EWOULDBLOCK)
                break;                      /* nothing left to read */
            return -1;                      /* real error */
        }
    }
    return 0;
}

/* PackBits RLE encoder
 * Standard PackBits (Apple/TIFF variant):
 *   header 0x00-0x7F -> n+1 literal bytes follow
 *   header 0x81-0xFF -> repeat next byte (257-n) times
 *   header 0x80      -> NOP (never emitted here)
 * Literal scan breaks on runs of 3+ identical bytes.
 * Worst-case output: n + ceil(n/128) bytes (~1.008 x input). */
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

/* Dirty row scan
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
 * Wire format: [payload_len LE32][first_row LE16][row_count LE16][rle_bytes]
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

/* Frame capture + change detection
 * Copies fb1 -> staging in one device-memory pass, then compares
 * staging (RAM) against shadow (RAM).  Returns 1 if the frame
 * changed or shadow is not yet valid.
 *
 * After a successful send, swap staging -> shadow so shadow holds
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

/* ---- Boot kernel-log capture (stock non-rooted diagnosis) ---------------
 * Stock (non-rooted) Kronos units have no dmesg/klogd, so a freeze during
 * early-boot module loading normally leaves no retrievable evidence - and
 * rooting the unit to get logs makes the modules load late enough that the
 * freeze no longer reproduces (it masks the very bug we're chasing).
 *
 * syslog(2)/klogctl action 3 (SYSLOG_ACTION_READ_ALL) reads the ENTIRE kernel
 * ring buffer non-destructively; it works for root with no dmesg_restrict on
 * 2.6.32 and is independent of console `loglevel` (loglevel only gates what
 * reaches the console, never what the ring stores).  We snapshot the ring to
 * the FTP-visible HD partition and fsync it.  A forked drainer re-snapshots
 * every 150 ms across the risky init_module() calls: if one wedges the box,
 * the last fsync'd snapshot still holds the printks up to the hang - e.g.
 * whether midi_inject printed "hooked 0x..." (froze in the .text patch / IPI) or
 * "prologue mismatch" (address drift) before the freeze. */
#define BOOT_KMSG_DIR  LOG_DIR
#define BOOT_KMSG_LOG  BOOT_KMSG_DIR "/boot_kmsg.log"

/* Create the FTP-visible log folder and hand it to the Kronos user (uid/gid
 * 500) so everything we write there - the .boot flag, boot_kmsg.log - can be
 * read and deleted over FTP on an unrooted unit (no shell access).  Deleting a
 * file needs write+exec on the containing directory, so the directory ownership
 * is what matters; we chown it to 500:500 with mode 0775. */
static void ensure_log_dir(void)
{
    mkdir("/korg/rw/HD", 0755);
    mkdir(LOG_DIR, 0775);
    chmod(LOG_DIR, 0775);
    chown(LOG_DIR, KRONOS_UID, KRONOS_GID);
}

static void dump_kmsg(void)
{
    static char kbuf[131072];
    long n = syscall(SYS_syslog, 3 /*READ_ALL*/, kbuf, (long)sizeof(kbuf));
    if (n <= 0) return;
    /* Write-then-rename so the live log is always a COMPLETE snapshot: if the box
     * freezes mid-write, BOOT_KMSG_LOG still holds the previous full snapshot
     * rather than a truncated one. */
    int fd = open(BOOT_KMSG_LOG ".tmp", O_WRONLY | O_CREAT | O_TRUNC, 0644);
    if (fd < 0) return;
    if (write_all(fd, kbuf, (size_t)n) == 0) {
        fsync(fd);
        close(fd);
        rename(BOOT_KMSG_LOG ".tmp", BOOT_KMSG_LOG);
    } else {
        close(fd);
    }
}

/* Fork a child that keeps BOOT_KMSG_LOG current until the parent kills it.
 * Returns the child pid, or -1 on error. */
static pid_t start_kmsg_drainer(void)
{
    pid_t pid;
    ensure_log_dir();
    dump_kmsg();                 /* one snapshot up front in case fork fails */
    pid = fork();
    if (pid != 0) return pid;    /* parent (pid>0) or error (-1) */
    signal(SIGTERM, SIG_DFL);    /* parent's handler would not exit our loop */
    for (;;) { dump_kmsg(); usleep(150000); }
    _exit(0);                    /* unreachable */
}

/* Reap the forked helper children (kmsg drainer + midi_tcp capture) before an
 * EARLY exit from main().  The normal shutdown path at the bottom of main already
 * does this, but the early "return 1" paths (fb1 never appears, malloc failure,
 * bind failure) run AFTER both children are forked and would otherwise orphan
 * them to init: the drainer rewrites boot_kmsg.log every 150 ms forever, and a
 * stray midi_tcp keeps port 9875 and a /proc/.midi_ring reader open — and because
 * the ring read advances a single global cursor, a second reader steals the
 * first's MIDI bytes.  kmsg_pid is main-local so it is passed in; midi_cap_pid is
 * a file-scope global. */
static void stop_helper_children(pid_t kmsg_pid)
{
    if (kmsg_pid > 0) {
        kill(kmsg_pid, SIGTERM);
        waitpid(kmsg_pid, NULL, 0);
    }
    if (midi_cap_pid > 0) {
        kill(midi_cap_pid, SIGTERM);
        waitpid(midi_cap_pid, NULL, 0);
        midi_cap_pid = -1;
    }
}

/*  Main */
int main(void)
{
    pid_t kmsg_pid = -1;
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
    read_pubid();

    /* Ensure runtime directory exists (binary lives here, so it always should) */
    mkdir(SCREENREMOTE_DIR, 0755);

    /* Ensure the FTP-visible log folder exists and is owned by the Kronos user
     * BEFORE writing the .boot flag there - on an unrooted unit this is the only
     * way the user can delete .boot (over FTP) to re-enable MIDI after a failed
     * boot. */
    ensure_log_dir();

    /* Truncate access log on each boot to avoid filling /korg/rw over time */
    { FILE *lf = fopen(SCREENREMOTE_DIR "/access.log", "w"); if (lf) fclose(lf); }

    /* Boot-safety flag: write .boot at startup; delete it only after full successful
     * startup (listeners bound, OS confirmed operational).  If .boot already exists
     * on entry the previous boot did not complete cleanly - skip midi_inject hooks
     * so the unit can recover.  Lives in the FTP-visible log folder
     * (/korg/rw/HD/ScreenRemote/.boot) so the user can remove it over FTP - no
     * shell access needed on an unrooted Kronos - to re-enable MIDI next boot. */
    int boot_flag_found = 0;
    {
        struct stat _bf;
        if (stat(BOOT_FLAG, &_bf) == 0) {
            boot_flag_found = 1;
            fprintf(stderr, "screenremote: .boot flag present from failed previous boot - "
                    "MIDI hooks disabled for safe recovery (remove %s to re-enable)\n",
                    BOOT_FLAG);
        }
        int _bfd = open(BOOT_FLAG, O_WRONLY | O_CREAT | O_TRUNC, 0666);
        if (_bfd >= 0) {
            fchown(_bfd, KRONOS_UID, KRONOS_GID);  /* FTP-deletable/readable */
            fsync(_bfd);
            close(_bfd);
        }
        /* The marker MUST be durable on disk BEFORE any init_module() call: if a
         * module load freezes the RTAI box, the next boot has to see .boot to skip
         * the dangerous loads and self-recover.  open()+close() alone leaves it in
         * the page cache (ext3 commit interval up to seconds), so a hard freeze can
         * lose it -> permanent boot loop.  sync() forces it out now. */
        sync();
    }

    /* Kill-switch: if the folder /korg/rw/HD/_nomod exists, load NO kernel modules.
     * FTP reaches only /korg/rw/HD, so this folder can be created over FTP
     * (mkdir _nomod) + reboot to bring a unit up with no kernel hooks.  Required to
     * run a Korg OS update or the cleaner safely (midi_inject's OA .text hooks
     * otherwise freeze the system when OA is torn down at "Preparing to Install"),
     * and to recover a wedged unit.  Checked here because this daemon is what loads
     * the modules (init_module(2) from embedded buffers); a kernel-side FS check is
     * unreliable in init_module context on the RTAI kernel. */
    int load_mods = 1;
    {
        struct stat _ns;
        if (stat("/korg/rw/HD/_nomod", &_ns) == 0 && S_ISDIR(_ns.st_mode)) {
            load_mods = 0;
            fprintf(stderr, "screenremote: kill-switch /korg/rw/HD/_nomod present - "
                    "not loading any kernel modules (vkbd, midi_inject)\n");
        }
    }

    /* Capture the kernel ring buffer across the risky init_module() window so a
     * freeze on a stock non-rooted unit still leaves retrievable evidence at
     * /korg/rw/HD/screenremote/boot_kmsg.log.  Started before any module load;
     * stopped once startup succeeds (see below). */
    if (load_mods && !boot_flag_found)
        kmsg_pid = start_kmsg_drainer();

    /* Extract and load virtual keyboard early so Eva discovers it before any client connects.
     * Use init_module(2) directly - system() needs /bin/sh which does not exist on non-rooted
     * Kronos units. Loading from the embedded buffer avoids any shell or external binary.
     *
     * Gated by !boot_flag_found: if the previous boot didn't complete cleanly we
     * load NO kernel modules at all this boot (not vkbd, not midi_inject), so the
     * unit is guaranteed to reach a usable state.  Remove /korg/rw/screenremote/.boot
     * (or fix the cause) to re-enable module loading on the next boot. */
    if (load_mods && !boot_flag_found) {
        extract_ko(VKBD_KO, vkbd_ko, vkbd_ko_len);
        syscall(SYS_init_module, (void *)vkbd_ko, (unsigned long)vkbd_ko_len, "");

        for (int _vi = 0; _vi < 20 && vkbd_fd < 0; _vi++) {
            usleep(100000);
            vkbd_fd = open("/proc/.vkbd", O_WRONLY);
        }
    }

    /* Load MIDI injection module */
    if (load_mods && !boot_flag_found) {
        unsigned long recv_fn = 0, reg_fn = 0;
        unsigned long dispatch_fn = 0;

        /* midi_inject reads OA symbol addresses and patches OA .text.  It MUST NOT
         * be loaded while OA is still MODULE_STATE_COMING ("OA(P+)" in the oops
         * module list): doing so races the module loader and faults in module_put
         * during init_module, killing the daemon before its init runs (see
         * troubleshooting/boot_kmsg.log).  On the GRUB-hook boot path screenremote
         * starts as soon as /proc/.oacmd appears, but that entry is created while
         * OA is still COMING - so wait here until OA is actually Live.  On the
         * rooted path OA is already Live when this runs, so the wait returns at
         * once.  Timeout is generous; if OA never goes Live we skip MIDI rather
         * than load into an unsafe state. */
        if (!wait_for_oa_live(600)) {
            fprintf(stderr, "screenremote: OA not Live after 60s - "
                    "skipping midi_inject to avoid init_module race\n");
            goto midi_done;
        }

        resolve_kallsyms(&recv_fn, &reg_fn, &dispatch_fn);
        if (dispatch_fn) {
            char params[512];
            snprintf(params, sizeof(params),
                     "receive_fn=0x%lx register_fn=0x%lx midi_dispatch_fn=0x%lx",
                     recv_fn, reg_fn, dispatch_fn);
            extract_ko(MIDI_INJECT_KO, midi_inject_ko, midi_inject_ko_len);
            long ret = syscall(SYS_init_module,
                               (void *)midi_inject_ko,
                               (unsigned long)midi_inject_ko_len,
                               params);
            if (ret == 0 || errno == EEXIST) {
                /* midi_inject_init() always returns 0 to the kernel now (see the
                 * comment above its final return) - a negative return here would
                 * crash module_put() on this kernel, so the module degrades
                 * internally instead of failing the load.  That means "the
                 * syscall succeeded" no longer implies "MIDI actually works";
                 * /proc/.midi_in's openability below is the real signal, so
                 * g_midi_loaded is set from that, not from ret. */
                fprintf(stderr, "screenremote: midi_inject %s "
                        "(recv=%s reg=%s dispatch=%s)\n",
                        ret == 0 ? "loaded" : "already loaded",
                        recv_fn      ? "ok" : "none",
                        reg_fn       ? "ok" : "none",
                        dispatch_fn  ? "ok" : "none");

                for (int _mi = 0; _mi < 20 && midi_in_fd < 0; _mi++) {
                    usleep(100000);
                    midi_in_fd = open("/proc/.midi_in", O_WRONLY);
                }
                g_midi_loaded = (midi_in_fd >= 0);
                start_midi_capture();
                fprintf(stderr, "screenremote: midi_in=%d capture=%d\n",
                        midi_in_fd >= 0 ? 1 : 0, midi_cap_fd >= 0 ? 1 : 0);
            } else {
                fprintf(stderr, "screenremote: midi_inject failed (%ld)\n", ret);
            }
        } else {
            fprintf(stderr, "screenremote: no usable MIDI symbols in kallsyms - MIDI disabled\n");
        }
    midi_done: ;
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
    if (fb1_open() < 0) { stop_helper_children(kmsg_pid); return 1; }

    shadow = malloc(frame_bytes);
    if (!shadow) { perror("malloc shadow"); stop_helper_children(kmsg_pid); return 1; }
    staging = malloc(frame_bytes);
    if (!staging) { perror("malloc staging"); stop_helper_children(kmsg_pid); return 1; }
    rle_buf = malloc(frame_bytes * 2);
    if (!rle_buf) { perror("malloc rle_buf"); stop_helper_children(kmsg_pid); return 1; }

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
            fprintf(stderr, "screenremote: failed to bind stream=%d ctrl=%d - change ports in %s\n",
                    g_stream_port, g_ctrl_port, CFG_PATH);
            stop_helper_children(kmsg_pid);
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

    /* Startup complete: framebuffer open, network up, listeners bound.
     * OS is fully operational - clear the boot flag so the next boot
     * knows this one succeeded and loads MIDI normally. */
    unlink(BOOT_FLAG);

    /* Startup survived the module-load window: take a final ring-buffer snapshot
     * (captures the successful "hooked 0x..."/"ready" lines) and stop the drainer
     * so boot_kmsg.log reflects the boot window, not runtime churn. */
    if (kmsg_pid > 0) {
        kill(kmsg_pid, SIGTERM);
        waitpid(kmsg_pid, NULL, 0);
        kmsg_pid = -1;
        dump_kmsg();
    }

    for (;;) {
        fd_set rfds;
        struct timeval tv;
        time_t now = time(NULL);
        int maxfd, r;

        if (g_exit) break;

        /* Land any rate-limited CC whose hold interval elapsed.  cc_pending stays
         * set while a controller's final value is still held, so we shorten the
         * select() timeout below to deliver it promptly. */
        int cc_pending = cc_flush();

        /* Periodic mirror flag check */
        if (now - last_mirror_chk >= 1) {
            check_mirror_flag();
            last_mirror_chk = now;
        }

        /* Periodic network check - re-bind if IP changed (DHCP, link down/up) */
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
                    fprintf(stderr, "screenremote: IP changed -> %s, rebinding\n",
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
                        /* Swap staging -> shadow: shadow now holds the sent frame. */
                        uint8_t *tmp = shadow; shadow = staging; staging = tmp;
                        shadow_valid = 1;
                    }
                }
            }
        }

        /* Build select set - listeners may be -1 during rebind */
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
        if (midi_cap_fd >= 0) {
            FD_SET(midi_cap_fd, &rfds);
            if (midi_cap_fd > maxfd) maxfd = midi_cap_fd;
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

        /* A held CC must land within the throttle interval even when the screen
         * is idle (MODE_PULL parks select() for up to 1 s). */
        if (cc_pending) {
            long max_us = CC_MIN_INTERVAL_MS * 1000L;
            if (tv.tv_sec > 0 || tv.tv_usec > max_us) {
                tv.tv_sec  = 0;
                tv.tv_usec = max_us;
            }
        }

        if (maxfd < 0) { usleep(500000); continue; }
        r = select(maxfd + 1, &rfds, NULL, NULL, &tv);
        if (r < 0) continue;

        /* Drain midi_tcp output every iteration: collects SysEx responses
         * when a capture is pending, discards other MIDI bytes otherwise.
         * Always non-blocking (MSG_DONTWAIT inside sysex_poll). */
        sysex_poll(midi_cap_fd >= 0 && FD_ISSET(midi_cap_fd, &rfds));

        /* New streaming client */
        if (stream_listen >= 0 && FD_ISSET(stream_listen, &rfds)) {
            struct timeval send_to = {5, 0};
            struct sockaddr_in peer;
            socklen_t plen = sizeof(peer);
            int new_fd = accept(stream_listen, (struct sockaddr *)&peer, &plen);
            if (new_fd >= 0) {
                setsockopt(new_fd, SOL_SOCKET, SO_SNDTIMEO,
                           &send_to, sizeof(send_to));
                { int nd = 1; setsockopt(new_fd, IPPROTO_TCP, TCP_NODELAY, &nd, sizeof(nd)); }
#ifndef SO_SNDBUFFORCE
#define SO_SNDBUFFORCE 32
#endif
                { int sb = 512 * 1024; setsockopt(new_fd, SOL_SOCKET, SO_SNDBUFFORCE, &sb, sizeof(sb)); }
                if (do_handshake(new_fd, &client_mode, &client_fps, &peer) < 0) {
                    close(new_fd);
                } else {
                    if (client_fd >= 0) close(client_fd);
                    shadow_valid = 0;
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

        /* Control command - only accept from the authenticated stream client's IP. */
        if (ctrl_listen >= 0 && FD_ISSET(ctrl_listen, &rfds)) {
            struct sockaddr_in cpeer;
            socklen_t cplen = sizeof(cpeer);
            int cfd = accept(ctrl_listen, (struct sockaddr *)&cpeer, &cplen);
            if (cfd >= 0) {
                if (g_ctrl_allowed_ip != 0 &&
                    cpeer.sin_addr.s_addr == g_ctrl_allowed_ip) {
                    /* Read the first line to decide: "CTRL_PERSIST" -> persistent
                     * connection (replaces any previous ctrl_fd); anything else ->
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
                            /* One-shot: firstline already read; process it and close.
                             * Exception: SYSEX starts an async capture - fd stays open
                             * until sysex_finish() sends the response and closes it. */
                            process_ctrl_cmd(firstline, cfd);
                            if (!(sysex_pending && sysex_resp_fd == cfd))
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
                if (sysex_pending && sysex_resp_fd == ctrl_fd) {
                    sysex_resp_fd = -1;
                    sysex_pending = 0;
                    sysex_cap_offset = 0;
                }
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
                char resp[64];
                int rlen = snprintf(resp, sizeof(resp), "KSCR SP=%d CP=%d MIDI=%d\n",
                                    g_stream_port, g_ctrl_port, g_midi_loaded);
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
    if (kmsg_pid > 0) { kill(kmsg_pid, SIGTERM); waitpid(kmsg_pid, NULL, 0); kmsg_pid = -1; }
    if (client_fd >= 0) close(client_fd);
    if (ctrl_fd   >= 0) close(ctrl_fd);
    if (stream_listen >= 0) close(stream_listen);
    if (ctrl_listen   >= 0) close(ctrl_listen);
    if (disc_fd >= 0) close(disc_fd);
    if (touch_fd >= 0) { close(touch_fd); touch_fd = -1; }
    if (vkbd_fd  >= 0) { close(vkbd_fd);  vkbd_fd  = -1; }
    if (kbd_fd   >= 0) { ioctl(kbd_fd, UI_DEV_DESTROY); close(kbd_fd); kbd_fd = -1; }
    if (midi_in_fd  >= 0) { close(midi_in_fd);  midi_in_fd = -1; }
    if (midi_cap_fd >= 0) { close(midi_cap_fd); midi_cap_fd = -1; }
    if (midi_cap_pid > 0) { kill(midi_cap_pid, SIGTERM); waitpid(midi_cap_pid, NULL, 0); }
    if (g_midi_loaded) {
        syscall(__NR_delete_module, "midi_inject", O_NONBLOCK);
        g_midi_loaded = 0;
    }
    syscall(__NR_delete_module, "vkbd", O_NONBLOCK);
    fb0_close();
    fprintf(stderr, "screenremote: exited cleanly\n");
    return 0;
}
