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
 *   CHORD [hold_ms] name1 name2 ...  - press left-to-right, release right-to-left
 *   WHEEL CW|CCW            - one data-wheel tick clockwise or counter-clockwise
 *   SLIDER n value          - set physical Slider n (1 - 8) to value (0 - 127)
 *   KNOB n value            - set physical RT Knob n (1 - 8) to value (0 - 127)
 *   VSLIDER value           - set value slider position (0 - 127)
 *   JOYSTICK X|Y value      - set Joystick axis to value (0 - 127)
 *   VECTOR X|Y value        - set Vector Joystick axis (0 - 127)
 *   RIBBON X|Z value        - set Ribbon controller axis (0 - 127) - Z axis unverified
 *   AFTERTOUCH value        - set keybed aftertouch (0 - 127)
 *   PEDAL value             - set assignable PEDAL jack (0 - 127)
 *   FOOTSWITCH value        - set assignable foot SWITCH jack (0 - 127)
 *   DAMPER value            - set DAMPER jack (0 - 127), ramped internally
 *   TEMPO value             - set tempo (0 - 127, maps to ~40-300bpm, non-linear), ramped internally
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
 * Boot-safety: /korg/rw/HD/ScreenRemote/.boot is written at startup and deleted only once the
 * framebuffer, network, and listeners are all up.  If it exists on entry the previous boot did not
 * complete cleanly, so midi_bridge is skipped for that boot.  Delete the file over FTP to re-enable.
 *
 * Front-panel injection (BUTTON, CHORD, the TOUCH commands, WHEEL, SLIDER, KNOB,
 * VSLIDER): as of 1.10.0 these no
 * longer write raw packets to /dev/rtf5.  rtf5 is OA.ko's own OUTBOUND notification FIFO to Eva
 * (created by OA in stg_rtfifo_init, direction OA-RT -> Eva) - genuine hardware events reach OA
 * through a completely separate path (CSTGOmapNKSMsgHandler::ProcessNextNKSEvent() pulling raw
 * NKS4 commands via the exported OmapNKS4InputFifo_ReadCommand and dispatching directly to
 * CSTGFrontPanel::Handle*), so writing a synthetic packet to rtf5 only ever fooled Eva's own
 * UI-mirroring code - it never touched the real OA-side action.  That's exactly why touch/mode
 * buttons "mostly worked" (Eva's own widget hit-testing reconstructed the right behaviour from
 * the mirrored packet) while sequencer transport, tempo, and some MIDI-triggering touch actions
 * did nothing (their real effect lives inside OA and was never reached).
 *
 * nks4_inject.ko (extracted from the embedded buffer below, loaded early at startup - see
 * load_nks4_inject()) calls OA's real CSTGFrontPanel::HandleSwitchEvent / HandleTouchPanel /
 * HandleRotary / HandleAnalogController directly via /proc/.nks4inject, i.e. the exact function
 * a physical press/touch/turn dispatches through - so injected events get bit-for-bit the same
 * response as hardware, independent of whatever mode Eva happens to be in.  If the module fails
 * to load (symbol resolution failure, kill-switch present, etc.) these commands return
 * "ERR NKS4_NOT_LOADED\n" rather than silently falling back to the old, proven-unreliable rtf5
 * path - see g_nks4_loaded.
 */

#define _GNU_SOURCE   /* for sched_setaffinity / CPU_SET */
#include <sched.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include <stdarg.h>
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
#include "midi_bridge_ko.h"
#include "midi_tcp_bin.h"
#include "nks4_inject_ko.h"

/*  Keyboard injection (uinput fallback) */
#define KBD_EV_SYN  0
#define KBD_EV_KEY  1

/*  Version */
#define SCREENREMOTE_VERSION "1.10.0"
#ifndef BUILD_ID
#define BUILD_ID "dev"
#endif

/* Config */
#define SCREENREMOTE_DIR  "/korg/rw/screenremote"
#define VKBD_KO           SCREENREMOTE_DIR "/vkbd.ko"
#define MIDI_BRIDGE_KO    SCREENREMOTE_DIR "/midi_bridge.ko"
#define MIDI_TCP_BIN      SCREENREMOTE_DIR "/midi_tcp"
#define NKS4_INJECT_KO    SCREENREMOTE_DIR "/nks4_inject.ko"

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

/* Pad-tap detection: touch (x,y) in framebuffer pixel space -> PADCHORD.
 * Regions calibrated 2026-07-14 against real hardware: 32 corner taps (4 per
 * pad) captured live with LASTTOUCH via tools/pad_calibration_monitor.py,
 * then quantized to 8 uniform 94x335 boxes via linear regression on the pad
 * centers (raw click widths varied 88-97px from click imprecision; centers
 * fit a clean line, spacing ~99.8px, ~6px gaps between pads) rather than
 * used as-is. All 8 pads share one row (Y range is constant, only X
 * varies), in on-screen left-to-right order matching PADCHORD's pad_index.
 * Detection still stays off by default (g_padmap_enabled=0) - flip
 * PADMAP_ON once ready. See docs/api.md's PADMAP section. */
#define NUM_PAD_REGIONS 8
struct pad_region { int x0, y0, x1, y1; };
static struct pad_region g_pad_regions[NUM_PAD_REGIONS] = {
    {   3, 144,  96, 478 }, { 103, 144, 196, 478 },
    { 203, 144, 296, 478 }, { 303, 144, 396, 478 },
    { 402, 144, 495, 478 }, { 502, 144, 595, 478 },
    { 602, 144, 695, 478 }, { 702, 144, 795, 478 },
};
static int g_padmap_enabled = 0;
static int g_active_pad = -1;          /* pad currently held down, or -1 */
static struct timespec g_chord_down_time;  /* when g_active_pad's trigger was sent */
/* Minimum time (ms) a PADCHORD trigger must be held before its release is
 * sent - see inject_touch()'s pen-up handler for why this exists. */
#define PADCHORD_MIN_HOLD_MS 80
static int g_last_touch_x = -1, g_last_touch_y = -1;   /* for LASTTOUCH calibration query */
/* Diagnostic trail of the last type==1 (pen-down) gate evaluation, for
 * debugging why a real client's tap doesn't fire PADCHORD when our own
 * synthetic TOUCH_DOWN/TOUCH_UP tests do - -1 for each means "no pen-down
 * has been evaluated yet since this daemon started". */
static int g_last_touch_type = -1;
static int g_last_gate_pads_page = -1, g_last_gate_pad_play = -1;
static int g_last_gate_chord_assign = -1, g_last_gate_hit = -1;

/* Minimum real wall-clock gap enforced between successive touch injections
 * (TOUCH's own down+up pair, and each TOUCH_DOWN/TOUCH_MOVE/TOUCH_UP from a
 * client-driven drag) - see touch_pace()'s own comment by inject_touch(). */
#define TOUCH_MIN_INTERVAL_MS 30

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

/* Front-panel injection: nks4_inject.ko calls OA's real CSTGFrontPanel::Handle*
 * methods directly (see header comment above) - this replaces the old /dev/rtf5
 * packet-write approach entirely, not just for touch. */
static int nks4_fd      = -1;  /* fd to /proc/.nks4inject O_WRONLY */
static int g_nks4_loaded = 0;  /* 1 once nks4_inject.ko is loaded and nks4_fd is open */
static int g_tempo_value = -1;   /* last value TEMPO commanded, -1 = unknown (see nks4_analog_ramp) */
static int g_damper_value = -1;  /* last value DAMPER commanded, -1 = unknown */
static int vkbd_fd  = -1;  /* fd to /proc/.vkbd (vkbd.ko virtual keyboard) */
static int kbd_fd   = -1;  /* fd to physical USB keyboard evdev node (fallback) */
static int midi_in_fd    = -1;  /* fd to /proc/.midi_in (MIDI injection) */
static int midi_cap_fd   = -1;  /* TCP socket to midi_tcp child on localhost */
static pid_t midi_cap_pid = -1;
static int g_midi_loaded = 0;
static int g_midi_load_pending = 0;  /* 1 = load midi_bridge once EVA UI is up */
/* Calibration mode: if /korg/rw/screenremote/.fbcurve exists at startup, log the
 * framebuffer non-black% + distinct-color count every second to fbcurve.log and
 * NEVER load midi_bridge.  A totally brick-safe reboot (the hazardous tap never
 * fires) that captures the real loading->UI transition, so the EVA-ready threshold
 * can be set from data instead of a guess.  Remove the flag file to go live. */
static int g_fbcurve_cal = 0;
#define FBCURVE_FLAG SCREENREMOTE_DIR "/.fbcurve"
#define FBCURVE_LOG  SCREENREMOTE_DIR "/fbcurve.log"
#define MIDI_TCP_PORT 9875

/* Button table - flat NKS4 hardware scan code (0-127), NOT the old rtf5 (dev,code)
 * pair.  This is the exact eSTGButtonCode value CSTGFrontPanel::HandleSwitchEvent
 * dispatches on - ground-truthed two independent ways that agree exactly:
 *   1. Live capture via the Kronos's own NKS4 test/calibration mode (every code
 *      below was read directly off the front panel by pressing each physical
 *      button once with test mode active).
 *   2. Cross-checked against OA.ko's own ButtonPressHandler disassembly - e.g.
 *      codes 53/54 (Timbre Track / Audio) land on the two ChangeControlSurfaceMode
 *      switch cases, code 9 (Help) sets a status flag bit, code 44 (Tap Tempo)
 *      calls SendKarmaCCToKG - all consistent with the physical function.
 * INC/DEC codes were captured separately (see their own entry below) after the
 * main pass, not reused from the old, unrelated rtf5 dev/code guess. */
struct btn_def { const char *name; uint32_t code; };
static const struct btn_def btn_table[] = {
    /* Exit / Enter */
    { "EXIT",           8 },
    { "ENTER",          23 },
    /* Mode select (Radio group) */
    { "SETLIST",        7 },
    { "COMBI",          1 },
    { "PROGRAM",        2 },
    { "SEQUENCE",       3 },
    { "SAMPLING",       4 },
    { "GLOBAL",         5 },
    { "DISK",           6 },
    /* Utility */
    { "HELP",           9 },
    { "COMPARE",        10 },
    { "RESET",          75 },   /* ResetControls */
    /* Number pad */
    { "NUM0",           11 },
    { "NUM1",           12 },
    { "NUM2",           13 },
    { "NUM3",           14 },
    { "NUM4",           15 },
    { "NUM5",           16 },
    { "NUM6",           17 },
    { "NUM7",           18 },
    { "NUM8",           19 },
    { "NUM9",           20 },
    { "NUM_DASH",       21 },
    { "NUM_DOT",        22 },
    /* Mix Play buttons (PlayMute 1-8) */
    { "MP1",            58 },
    { "MP2",            59 },
    { "MP3",            60 },
    { "MP4",            61 },
    { "MP5",            62 },
    { "MP6",            63 },
    { "MP7",            64 },
    { "MP8",            65 },
    /* Mix Select buttons */
    { "MS1",            66 },
    { "MS2",            67 },
    { "MS3",            68 },
    { "MS4",            69 },
    { "MS5",            70 },
    { "MS6",            71 },
    { "MS7",            72 },
    { "MS8",            73 },
    /* Bank buttons */
    { "BANK_IA",        24 },
    { "BANK_IB",        25 },
    { "BANK_IC",        26 },
    { "BANK_ID",        27 },
    { "BANK_IE",        28 },
    { "BANK_IF",        29 },
    { "BANK_IG",        30 },
    { "BANK_UA",        31 },
    { "BANK_UB",        32 },
    { "BANK_UC",        33 },
    { "BANK_UD",        34 },
    { "BANK_UE",        35 },
    { "BANK_UF",        36 },
    { "BANK_UG",        37 },
    /* Sequencer controls */
    { "SEQ_PAUSE",      38 },
    { "SEQ_REW",        39 },
    { "SEQ_FF",         40 },
    { "SEQ_LOCATE",     41 },
    { "SEQ_REC",        42 },
    { "SEQ_START",      43 },   /* also STOP - same physical button */
    { "TAP_TEMPO",      44 },
    /* Sampling controls */
    { "SMPL_REC",       45 },
    { "SMPL_START",     46 },
    /* Channel strip / control surface */
    { "MIX_KNOBS",      74 },   /* MixerKnobs */
    /* SOLO fires on release; daemon sends press+release so release triggers it */
    { "SOLO",           76 },
    { "MODULE_CONTROL", 47 },
    { "KARMA_ONOFF",    48 },
    { "KARMA_LATCH",    49 },
    { "DRUM_TRACK",     50 },
    /* Captured twice under two different labels ("PanelSW+/-" in the first pass,
     * "INC/DEC" in a dedicated follow-up capture) - same codes both times, so
     * this is one physical button pair, not two.  INC/DEC is the name both the
     * C# and Python clients already send (BTN_Inc/BTN_Dec, left-panel INC/DEC),
     * so that's the name that ships. */
    { "INC",            51 },
    { "DEC",            52 },
    { "TIMBRE_TRACK",   53 },
    { "AUDIO_TRACK",    54 },
    { "EXT_TRACK",      55 },
    { "RTKNOBS_KARMA",  56 },   /* "RT Knobs/Karma" control-surface page select */
    { "TONE_ADJUST",    57 },
    { "SW1",            77 },
    { "SW2",            78 },
    { NULL, 0u }
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

/* TOUCH_MOVE coalescing for handle_ctrl_persistent_data() - see its own
 * comment for why this exists (a drag's queued TOUCH_MOVE backlog was
 * delaying the release that follows it, each one paying touch_pace()'s
 * blocking 30ms minimum gap before the release even got its turn). Only
 * ever holds ONE line (not a general command queue/buffer) - every other
 * command type still processes immediately in strict order, so nothing
 * that can be long (MIDI_SEND's SysEx payload, CHORD's button list) or
 * order-sensitive is ever deferred or size-truncated. */
static char ctrl_pending_move[96];
static int  ctrl_has_pending_move = 0;

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

/* Clamp v into [lo,hi].  Used everywhere a client-supplied numeric value feeds
 * into an nks4_inject command: we never want to forward an out-of-range value
 * to the kernel module (which has its own defense-in-depth bounds checks, but
 * a client should always see its input snapped to something valid, not ERR'd
 * for a merely-out-of-range-but-otherwise-well-formed request). */
static int clampi(int v, int lo, int hi)
{
    if (v < lo) return lo;
    if (v > hi) return hi;
    return v;
}

/* Write one command line to /proc/.nks4inject (BTN/BTN_DOWN/BTN_UP/TOUCH/ROT/ANALOG
 * - see nks4_inject.c's own header comment for the grammar).  Returns 0 on success,
 * -1 if the module isn't loaded or the write failed.  All callers below are
 * responsible for clamping/validating their arguments BEFORE calling this - this
 * function only formats and writes, it does not re-validate. */
static int nks4_write(const char *fmt, ...)
{
    char buf[80];
    va_list ap;
    int n;

    if (!g_nks4_loaded || nks4_fd < 0)
        return -1;
    va_start(ap, fmt);
    n = vsnprintf(buf, sizeof(buf), fmt, ap);
    va_end(ap);
    if (n < 0 || n >= (int)sizeof(buf))
        return -1;
    return write_all(nks4_fd, buf, (size_t)n) == 0 ? 0 : -1;
}

/* Reads onscreen_touch_pad_mode from /proc/.nks4inject_status - this is
 * CSTGFrontPanel::sInstance[0x104], which nks4_inject.c already exposes
 * read-only (see its own touch_pad_mode_read()). Confirmed (2026-07-14, via
 * Eva decompilation) to be a COMBINED signal, not just "Enable Pad Play":
 * CFormOmnimodePads::OnHide() unconditionally clears it on navigating away
 * from the page, and OnShow() only sets it when the page is shown AND Enable
 * Pad Play is on - so nonzero means both "on the Pads page" and "pad play
 * enabled" at once, and zero safely covers every case PADMAP should NOT
 * fire in (wrong page, pads disabled, or Chord Assign active - OnShow()
 * doesn't arm the flag in that mode either). Read fresh on every touch
 * rather than cached - this is normal-context proc I/O, not RT-context, and
 * the flag can change between taps (mode switches, page navigation). */
static int pad_play_active(void)
{
    int fd;
    char buf[256];
    ssize_t n;
    const char *p;

    fd = open("/proc/.nks4inject_status", O_RDONLY);
    if (fd < 0)
        return 0;
    n = read(fd, buf, sizeof(buf) - 1);
    close(fd);
    if (n <= 0)
        return 0;
    buf[n] = '\0';
    p = strstr(buf, "onscreen_touch_pad_mode=");
    if (!p)
        return 0;
    return atoi(p + strlen("onscreen_touch_pad_mode=")) != 0;
}

/* Framebuffer pixel fingerprint for "are we on the Pads (touch to play)
 * page" - added after sInstance[0x104]/pad_play_active() was live-tested
 * and disproved as a page/mode signal (2026-07-14). fb1_map is a direct
 * mmap() onto the hardware framebuffer (see fb1 init), so this is always
 * live with zero staleness, no polling delay. Calibrated the same way as
 * PADMAP's regions: sampled a point 5px inset from each pad box's top-left
 * corner while confirmed on the Pads page (all 8 read the same uniform
 * pad-box-background value, 219) and confirmed none of the 8 read that
 * value on a different tab (mostly 15, a couple outliers) - requiring ALL
 * 8 to match is a strong discriminator, since another page coincidentally
 * matching all 8 specific coordinates is very unlikely. */
#define PAD_FINGERPRINT_VALUE 219
#define PAD_FINGERPRINT_INSET 5

static int on_pads_page(void)
{
    int i, matches = 0;
    if (!fb1_map)
        return 0;
    for (i = 0; i < NUM_PAD_REGIONS; i++) {
        struct pad_region *r = &g_pad_regions[i];
        int x = r->x0 + PAD_FINGERPRINT_INSET;
        int y = r->y0 + PAD_FINGERPRINT_INSET;
        if (x < 0 || x >= (int)fb_w || y < 0 || y >= (int)fb_h)
            continue;
        if (fb1_map[y * (int)fb1_stride + x] == PAD_FINGERPRINT_VALUE)
            matches++;
    }
    return matches == NUM_PAD_REGIONS;
}

/* Chord Assign / Enable Pad Play / Fixed Velocity toggle indicators -
 * calibrated 2026-07-14 by before/after REGION diffs around each click
 * (bulk pixel dump, since per-pixel PIXEL queries were too slow for a wide
 * scan and the exact click coordinate turned out to be offset from the
 * actual colored indicator). All three are a small red LED-style dot:
 * bright red (palette idx ~50, R~246) when ON, dark/muted red (idx ~59-61,
 * R~105-129) when OFF - confirmed against the user's live toggling of all
 * three, both directions. Checking the palette's R channel at a threshold
 * (not an exact index match) is more robust to minor anti-aliasing/
 * rendering variance than requiring one specific index. Remarkably evenly
 * spaced (399-59=340, 739-399=340) - all three are likely one shared
 * widget template repeated at fixed intervals along the same row. */
#define TOGGLE_BRIGHT_R_THRESHOLD 200
#define CHORD_ASSIGN_PX      59
#define CHORD_ASSIGN_PY     102
#define ENABLE_PAD_PLAY_PX  399
#define ENABLE_PAD_PLAY_PY  103
#define FIXED_VELOCITY_PX   739
#define FIXED_VELOCITY_PY   102

static int toggle_is_on(int x, int y)
{
    unsigned char idx;
    if (!fb1_map || x < 0 || x >= (int)fb_w || y < 0 || y >= (int)fb_h)
        return 0;
    idx = fb1_map[y * (int)fb1_stride + x];
    return (pal_r[idx] >> 8) > TOGGLE_BRIGHT_R_THRESHOLD;
}

static int chord_assign_on(void)    { return toggle_is_on(CHORD_ASSIGN_PX, CHORD_ASSIGN_PY); }
static int enable_pad_play_on(void) { return toggle_is_on(ENABLE_PAD_PLAY_PX, ENABLE_PAD_PLAY_PY); }
static int fixed_velocity_on(void)  { return toggle_is_on(FIXED_VELOCITY_PX, FIXED_VELOCITY_PY); }

/* eSTGAnalogDeviceCode raw byte packing, derived from ShortInvertNkS4AnalogValue's
 * own disassembly: with byte1=0, ebx = byte0*4 exactly (the AND-0x3f8-then-
 * conditionally-OR-4 sequence cancels out for every byte0 parity), and the
 * consuming Handle-, Slider-, and Knob- code reads ebx>>3 as its 0-127 display value.
 * So byte0>>1 == displayed value, i.e. to reach display value V (0-127) send
 * byte0 = V*2.  Confirmed empirically on hardware for knobs, sliders, and the
 * value slider (all three read cleanly, exactly matching this formula).
 *
 * Tempo (device code 26) and Damper (29) DO take the same byte0 = V*2 input,
 * but neither can be jumped to directly the way the others can: a large
 * single-step change produces inconsistent, non-repeatable results, while a
 * smooth monotonic ramp through intermediate values is clean and precisely
 * reproducible (confirmed identical ascending and descending for Tempo -
 * value 0=40bpm ... 127=297bpm, non-linear). See TEMPO's own comment below
 * for the ramping this requires; nks4_analog_write() itself stays a single
 * fire-and-forget call for the other commands, which do not need ramping.
 *
 * Full eSTGAnalogDeviceCode table (0-30, joystick/vector/ribbon/aftertouch/
 * pedal/switch/damper included) lives in nks4_inject.c's own header comment.
 * JOYSTICK/VECTOR/RIBBON/AFTERTOUCH/PEDAL/FOOTSWITCH/DAMPER below all reuse
 * this same helper and formula and are now hardware-confirmed (see each
 * command's own comment below for what was tested) - RIBBON's Z axis is the
 * one remaining untested exception. */
static int nks4_analog_write(int device_code, int value_0_127)
{
    int byte0 = clampi(value_0_127, 0, 127) * 2;
    return nks4_write("ANALOG %d %d 0\n", device_code, byte0);
}

/* Tempo (device code 26) and Damper (29) tolerate the identical byte0 = V*2
 * input every other analog control does, but a direct jump to a target value
 * produces inconsistent, non-repeatable results on real hardware - a smooth
 * monotonic ramp through every intermediate value does not.  Confirmed for
 * Tempo with a full 0-127 sweep in both directions (identical BPM ascending
 * and descending); confirmed for Damper with a 256-step ramp.  This wraps
 * nks4_analog_write() in a step loop, 1 unit per call, so callers of TEMPO/
 * DAMPER get the reliable behaviour without needing to know why.
 *
 * *state tracks the value THIS DAEMON last commanded (init -1 = unknown).  On
 * the first call there is no known starting point to ramp from, so it jumps
 * directly (best effort, same as before this existed).  Every call after
 * that ramps from the last commanded value - a physical hand on the same
 * control between calls would desync this tracking, the same caveat any
 * absolute-position software control over a real analog input has.
 *
 * Blocks the calling thread for the ramp's duration (worst case ~127 steps),
 * same tradeoff CHORD's hold_ms already makes elsewhere in this file. */
static void nks4_analog_ramp(int device_code, int target_0_127, int *state)
{
    int target = clampi(target_0_127, 0, 127);
    int step;

    if (*state < 0) {
        nks4_analog_write(device_code, target);
        *state = target;
        return;
    }
    step = (target >= *state) ? 1 : -1;
    while (*state != target) {
        *state += step;
        nks4_analog_write(device_code, *state);
        usleep(25000);   /* ~25 ms/step - matches the proven-smooth Damper ramp pace */
    }
}

/* A synthetic TOUCH_DOWN immediately followed by TOUCH_UP (or a burst of
 * TOUCH_MOVE steps) reaches HandleTouchPanel only microseconds apart - two
 * back-to-back write()s to /proc/.nks4inject, versus the tens-of-milliseconds
 * a real finger's contact/scan naturally spans. Confirmed on hardware
 * (2026-07-14): a zero-delay TOUCH_DOWN + 13x TOUCH_MOVE + TOUCH_UP dragged
 * across the on-screen Pan knob collapsed into a single tap (selected the
 * field, value unchanged) instead of a drag; the identical sequence paced
 * ~30ms apart correctly scrubbed the value end to end. Same root cause class
 * as DAMPER/TEMPO needing a real-time ramp instead of an instant jump (see
 * nks4_analog_ramp() above) - the touch/gesture state machine on the other
 * end evidently needs real elapsed time between samples, not just correct
 * coordinates. This pacing is the suspected fix for Pads' chord-on-tap too
 * (untested directly - no confirmed Pads screen coordinates yet), since a
 * zero-dwell down+up is exactly the kind of input a debounce/contact-time
 * filter on a small touch target would discard. */
static void touch_pace(void)
{
    static struct timespec last;
    static int have_last;
    struct timespec now;
    long elapsed_ms;

    clock_gettime(CLOCK_MONOTONIC, &now);
    if (have_last) {
        elapsed_ms = (now.tv_sec - last.tv_sec) * 1000L
                   + (now.tv_nsec - last.tv_nsec) / 1000000L;
        if (elapsed_ms < TOUCH_MIN_INTERVAL_MS) {
            usleep((useconds_t)(TOUCH_MIN_INTERVAL_MS - elapsed_ms) * 1000);
            clock_gettime(CLOCK_MONOTONIC, &now);
        }
    }
    last = now;
    have_last = 1;
}

/* Touch-Y -> velocity, calibrated 2026-07-14 against real hardware: 10
 * paired (touch_y, resulting MIDI velocity) samples, gathered by clicking
 * through a client and physically touching the same on-screen spot within
 * ~1s (client-only clicks don't produce MIDI - that's the whole reason this
 * bridge exists), captured live via pad_calibration_monitor.py. Includes
 * both a 7-pad diagonal sweep AND 4 repeated taps on one single pad at
 * different heights (same X) to isolate the effect - both datasets agree:
 * velocity is a GLOBAL linear function of absolute screen Y, independent of
 * which pad or its own box bounds (NOT relative to each pad's own y0/y1 as
 * originally guessed). Linear regression over all 10 points: R^2=0.99. */
#define PAD_VEL_SLOPE     -0.35148
#define PAD_VEL_INTERCEPT  163.771

static int velocity_for_touch_y(int y)
{
    double v = PAD_VEL_SLOPE * (double)y + PAD_VEL_INTERCEPT;
    return clampi((int)(v + (v >= 0.0 ? 0.5 : -0.5)), 1, 127);
}

/* pad_index/vel out params only valid (and only written) when this returns 1. */
static int pad_hit_test(int x, int y, int *pad_index, int *vel)
{
    int i;
    for (i = 0; i < NUM_PAD_REGIONS; i++) {
        struct pad_region *r = &g_pad_regions[i];
        if (x >= r->x0 && x <= r->x1 && y >= r->y0 && y <= r->y1) {
            /* Fixed Velocity mode (toggle-gated, see fixed_velocity_on()):
             * confirmed 2026-07-14 by physically tapping the real Kronos
             * with Fixed Velocity on - the hardware fires every chord at
             * 127 regardless of tap force/position, ignoring the Y-based
             * curve entirely. */
            *vel = fixed_velocity_on() ? 127 : velocity_for_touch_y(y);
            *pad_index = i;
            return 1;
        }
    }
    return 0;
}

static void send_padchord(int pad, int vel)
{
    pad = clampi(pad, 0, 7);
    vel = clampi(vel, 0, 127);
    nks4_write("PADCHORD %d %d %d %d\n", pad, vel, 1, 1);
}

static void inject_touch(int type, int x, int y)
{
    x = clampi(x, 0, (int)fb_w - 1);
    y = clampi(y, 0, (int)fb_h - 1);
    g_last_touch_x = x;
    g_last_touch_y = y;
    g_last_touch_type = type;
    if (g_padmap_enabled) {
        if (type == 1) {                       /* pen-down */
            int pad, vel;
            /* Gated on on_pads_page() (framebuffer pixel fingerprint) plus
             * the Enable Pad Play / Chord Assign toggle indicators (also
             * framebuffer-sampled, calibrated 2026-07-14) - NOT
             * pad_play_active() (the sInstance[0x104] flag), which was
             * live-tested and disproved as a page/mode signal: it read 0
             * while a physical tap was actively producing sound. Still
             * reported via PADMAP_STATE for reference only. Fixed Velocity
             * is read but not yet acted on - see docs/api.md's known gap.
             * Each condition recorded separately (not short-circuited) so
             * PADMAP_STATE can show exactly which one blocked a given tap. */
            g_last_gate_pads_page    = on_pads_page();
            g_last_gate_pad_play     = enable_pad_play_on();
            g_last_gate_chord_assign = chord_assign_on();
            g_last_gate_hit = (g_last_gate_pads_page && g_last_gate_pad_play &&
                                !g_last_gate_chord_assign)
                              ? pad_hit_test(x, y, &pad, &vel) : 0;
            if (g_last_gate_hit) {
                g_active_pad = pad;
                send_padchord(pad, vel);
                clock_gettime(CLOCK_MONOTONIC, &g_chord_down_time);
            }
        } else if (type == 2 && g_active_pad != -1) {   /* pen-up */
            /* Enforce a minimum hold before releasing - live-tested
             * 2026-07-14: a real client's near-instantaneous tap (pen-down
             * immediately followed by pen-up, as sent by TOUCH's own
             * single-shot down+up pair) reached inject_touch() and passed
             * every gate condition (confirmed via PADMAP_STATE) yet
             * produced no MIDI at all, while our own manually-paced test
             * (200ms between down and up) worked correctly - RT_chord_
             * trigger's Note-On apparently needs more processing time than
             * a zero-delay down+up pair gives it before the Note-Off
             * cancels it. This blocks the daemon's main loop briefly
             * (same accepted pattern as touch_pace()'s TOUCH_MIN_INTERVAL_MS
             * sleep below) - acceptable for a rare, user-initiated tap. */
            struct timespec now;
            long held_ms;
            clock_gettime(CLOCK_MONOTONIC, &now);
            held_ms = (now.tv_sec - g_chord_down_time.tv_sec) * 1000L
                    + (now.tv_nsec - g_chord_down_time.tv_nsec) / 1000000L;
            if (held_ms < PADCHORD_MIN_HOLD_MS)
                usleep((useconds_t)(PADCHORD_MIN_HOLD_MS - held_ms) * 1000);
            send_padchord(g_active_pad, 0);
            g_active_pad = -1;
        }
    }
    int x_range = g_touch_x_range;
    int y_range = g_touch_y_range;
    int cx = x + g_touch_x_offset;
    int cy = y + g_touch_y_offset;
    uint32_t h_adc = cx <= 0 ? 0u : (cx >= x_range ? 255u
                   : (uint32_t)(cx * 255 + x_range / 2) / (uint32_t)x_range);
    uint32_t v_adc = cy <= 0 ? 0u : (cy >= y_range ? 255u
                   : (uint32_t)(cy * 255 + y_range / 2) / (uint32_t)y_range);
    touch_pace();
    nks4_write("TOUCH %d %u\n", type, v_adc | (h_adc << 8u));
}

/* Embedded .ko / binary extraction - atomic same-dir temp + rename.
 *
 * Write the payload to "<path>.tmp", verify the FULL write, fsync, then rename() it
 * onto <path>.  rename(2) is atomic and gives the target a fresh inode, so:
 *   - a failed or short write NEVER destroys the existing good file (it stays intact
 *     until the rename succeeds) - unlike unlink-then-write, which left a truncated
 *     binary if the write failed, and this file is execl()'d (midi_tcp) / init_module'd;
 *   - it still dodges ETXTBSY exactly as the old unlink-first did: an orphaned child
 *     still exec'ing the old inode keeps it alive; rename only repoints the directory
 *     entry onto the new inode.
 * We always rewrite (never skip on a size match): a rebuild can change behaviour
 * without changing the -static binary's byte count. */
static void extract_ko(const char *path, const unsigned char *data, unsigned int len)
{
    char tmp[512];
    int fd, ok = 0;

    if (snprintf(tmp, sizeof(tmp), "%s.tmp", path) >= (int)sizeof(tmp))
        return;
    fd = open(tmp, O_WRONLY | O_CREAT | O_TRUNC, 0644);
    if (fd >= 0) {
        unsigned int off = 0;
        ok = 1;
        while (off < len) {
            ssize_t w = write(fd, data + off, len - off);
            if (w <= 0) { ok = 0; break; }   /* short/failed write -> abandon */
            off += (unsigned int)w;
        }
        if (ok && fsync(fd) != 0) ok = 0;
        close(fd);
    }
    if (ok && rename(tmp, path) == 0)
        return;
    unlink(tmp);   /* leave the existing good file in place on any failure */
}

/* Mode button flat scan code -> g_mode.  Codes match btn_table[]'s mode-select
 * group (SETLIST/COMBI/PROGRAM/SEQUENCE/SAMPLING/GLOBAL/DISK). */
static void mode_from_btn(uint32_t code)
{
    switch (code) {
        case 7: g_mode = 1; break;  /* Setlist  */
        case 1: g_mode = 2; break;  /* Combi    */
        case 2: g_mode = 3; break;  /* Program  */
        case 3: g_mode = 4; break;  /* Sequence */
        case 4: g_mode = 5; break;  /* Sampling */
        case 5: g_mode = 6; break;  /* Global   */
        case 6: g_mode = 7; break;  /* Disk     */
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
                              unsigned long *outport_fn)
{
    FILE *f = fopen("/proc/kallsyms", "r");
    char line[256];
    *recv_fn = *reg_fn = *outport_fn = 0;
    if (!f) return;
    while (fgets(line, sizeof(line), f)) {
        unsigned long addr; char type, name[256];
        if (sscanf(line, "%lx %c %255s", &addr, &type, name) != 3) continue;
        if      (!*recv_fn     && strstr(name, "MidiInPortGeneric7Receive"))   *recv_fn     = addr;
        else if (!*reg_fn      && strstr(name, "RegisterMidiInPort"))          *reg_fn      = addr;
        else if (!*outport_fn  && strstr(name, "RegisterMidiOutPort"))         *outport_fn  = addr;
        if (*recv_fn && *reg_fn && *outport_fn) break;
    }
    fclose(f);
}

/* Resolve the 5 OA.ko-internal symbols nks4_inject.ko calls directly (see that
 * module's own header comment for the full ABI/offset derivation).  None of
 * these are EXPORT_SYMBOL'd, so - same convention as resolve_kallsyms() above -
 * the operator (this daemon) resolves them from /proc/kallsyms and passes the
 * live addresses in as insmod params, rather than the module trying
 * kallsyms_lookup_name() itself (not confirmed exported on this kernel). */
/* RT_chord_trigger(uchar,uchar,uchar,uchar) is NOT kallsyms-visible (no
 * standalone symbol - it's a plain, non-exported function with internal
 * linkage), unlike the class methods above. It's the real per-pad "Pads
 * (touch to play)" trigger: reads KARMA's own chord-memory tables and calls
 * Do_KM_note_out_chord_trig() per voice - confirmed hardware-verified,
 * matching the on-screen NOTE/VEL grid exactly for all 8 pads (0-indexed:
 * pad_index N = on-screen "Pad N+1"). Do_KM_note_out_chord_trig() itself
 * IS kallsyms-visible, so we resolve RT_chord_trigger's live address via the
 * fixed file-offset delta between the two in the reference binary (same
 * technique already established in this project for other unexported
 * symbols) - both live in the same .ko, so the delta survives relocation.
 * Ground-truthed against OA_real.ko: RT_chord_trigger=0x00512842,
 * Do_KM_note_out_chord_trig=0x0055e3fa -> delta=0x4bbb8. Re-derive both
 * addresses with `nm` against the current OA.ko if this OS build changes. */
#define RT_CHORD_TRIGGER_DELTA  0x4bbb8UL

static void resolve_nks4_kallsyms(unsigned long *fn_switch, unsigned long *fn_touch,
                                   unsigned long *fn_rotary, unsigned long *fn_analog,
                                   unsigned long *fn_invert, unsigned long *fn_chord)
{
    FILE *f = fopen("/proc/kallsyms", "r");
    unsigned long km_note_out = 0;
    char line[256];
    *fn_switch = *fn_touch = *fn_rotary = *fn_analog = *fn_invert = *fn_chord = 0;
    if (!f) return;
    while (fgets(line, sizeof(line), f)) {
        unsigned long addr; char type, name[256];
        if (sscanf(line, "%lx %c %255s", &addr, &type, name) != 3) continue;
        if      (!*fn_switch && strstr(name, "HandleSwitchEventE14eSTGButtonCodeb"))
            *fn_switch = addr;
        else if (!*fn_touch  && strstr(name, "HandleTouchPanelE24eNKS4TouchPanelEventTypei"))
            *fn_touch  = addr;
        else if (!*fn_rotary && strstr(name, "CSTGFrontPanel12HandleRotaryEi"))
            *fn_rotary = addr;
        else if (!*fn_analog && strstr(name, "HandleAnalogControllerE20eSTGAnalogDeviceCodeht"))
            *fn_analog = addr;
        /* Must NOT match ShortInvertNkS4RawAnalogValue (a different, unrelated
         * function) - the "Raw" variant breaks this exact substring, so plain
         * strstr is already safe here without an extra length/suffix check. */
        else if (!*fn_invert && strstr(name, "ShortInvertNkS4AnalogValue"))
            *fn_invert = addr;
        else if (!km_note_out && strstr(name, "Do_KM_note_out_chord_trig"))
            km_note_out = addr;
        if (*fn_switch && *fn_touch && *fn_rotary && *fn_analog && *fn_invert && km_note_out)
            break;
    }
    fclose(f);
    if (km_note_out > RT_CHORD_TRIGGER_DELTA)
        *fn_chord = km_note_out - RT_CHORD_TRIGGER_DELTA;
}


/* Private mountpoint used to reach sysfs when the system /sys isn't mounted
 * yet (GRUB-hook boot path runs screenremote concurrently with /sbin/init). */
#define OA_SYS_PRIV  SCREENREMOTE_DIR "/.sysfs"

/* Wait until OA reaches MODULE_STATE_LIVE, read via /sys/module/OA/initstate.
 *
 * midi_bridge reads OA symbol addresses and OA in-memory objects (the MIDI
 * in-port array and the out-queue it taps).  Unlike its predecessor it does not
 * patch OA .text, but it must still only load once OA is fully initialised, or
 * those objects aren't built yet.  Loading while OA is still COMING - shown as
 * "OA(P+)" in the oops "Modules linked in" list - reads half-built state.
 * /proc/.oacmd appears while OA is still COMING, so it is NOT a sufficient gate.
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
        /* x/y out of framebuffer bounds are snapped to the nearest edge inside
         * inject_touch() itself; only an unparsable line is rejected. */
        int x = 0, y = 0;
        if (!g_nks4_loaded) { REPLY("ERR NKS4_NOT_LOADED\n", 21); return; }
        if (sscanf(line + 6, "%d %d", &x, &y) == 2) {
            inject_touch(1, x, y);  /* pen-down */
            inject_touch(2, x, y);  /* pen-up */
            REPLY("OK\n", 3);
        } else {
            REPLY("ERR\n", 4);
        }

    } else if (strncmp(line, "TOUCH_DOWN ", 11) == 0) {
        int x = 0, y = 0;
        if (!g_nks4_loaded) { REPLY("ERR NKS4_NOT_LOADED\n", 21); return; }
        if (sscanf(line + 11, "%d %d", &x, &y) == 2) {
            inject_touch(1, x, y);
            REPLY("OK\n", 3);
        } else {
            REPLY("ERR\n", 4);
        }

    } else if (strncmp(line, "TOUCH_MOVE ", 11) == 0) {
        int x = 0, y = 0;
        if (!g_nks4_loaded) { REPLY("ERR NKS4_NOT_LOADED\n", 21); return; }
        if (sscanf(line + 11, "%d %d", &x, &y) == 2) {
            inject_touch(3, x, y);
            REPLY("OK\n", 3);
        } else {
            REPLY("ERR\n", 4);
        }

    } else if (strncmp(line, "TOUCH_UP ", 9) == 0) {
        int x = 0, y = 0;
        if (!g_nks4_loaded) { REPLY("ERR NKS4_NOT_LOADED\n", 21); return; }
        if (sscanf(line + 9, "%d %d", &x, &y) == 2) {
            inject_touch(2, x, y);
            REPLY("OK\n", 3);
        } else {
            REPLY("ERR\n", 4);
        }

    } else if (strncmp(line, "PADCHORD ", 9) == 0) {
        /* Triggers the real per-pad KARMA chord (RT_chord_trigger), the
         * confirmed hardware-verified mechanism behind "Pads (touch to
         * play)" - pad_index 0-7 is 0-indexed, matching on-screen "Pad N+1".
         * velocity 0 releases the chord (Note-Off for every voice);
         * velocity 1-127 plays it. param3=1 enables ScaleByte() proportional
         * velocity rescaling (the incoming velocity becomes the chord's new
         * "max" reference note, with the other voices' velocities scaled to
         * preserve their relative balance) - confirmed correct on hardware;
         * param4 is fixed at 1, not yet shown to need to vary. */
        int pad = 0, vel = 0;
        if (!g_nks4_loaded) { REPLY("ERR NKS4_NOT_LOADED\n", 21); return; }
        if (sscanf(line + 9, "%d %d", &pad, &vel) == 2) {
            send_padchord(pad, vel);
            REPLY("OK\n", 3);
        } else {
            REPLY("ERR\n", 4);
        }

    } else if (strncmp(line, "PADMAP ", 7) == 0) {
        /* Live-set one pad's rectangular hit region in framebuffer pixel
         * space, for calibrating against the real on-screen "Pads (touch to
         * play)" layout without rebuilding/redeploying the daemon - tap
         * around with any client, read back raw coordinates via LASTTOUCH,
         * and narrow each box down interactively. */
        int pad = 0, x0 = 0, y0 = 0, x1 = 0, y1 = 0;
        if (sscanf(line + 7, "%d %d %d %d %d", &pad, &x0, &y0, &x1, &y1) == 5 &&
            pad >= 0 && pad < NUM_PAD_REGIONS) {
            g_pad_regions[pad].x0 = x0;
            g_pad_regions[pad].y0 = y0;
            g_pad_regions[pad].x1 = x1;
            g_pad_regions[pad].y1 = y1;
            REPLY("OK\n", 3);
        } else {
            REPLY("ERR\n", 4);
        }

    } else if (strcmp(line, "PADMAP_LIST") == 0) {
        char resp[512];
        int i, rlen = 0;
        for (i = 0; i < NUM_PAD_REGIONS && rlen < (int)sizeof(resp) - 64; i++) {
            struct pad_region *r = &g_pad_regions[i];
            rlen += snprintf(resp + rlen, sizeof(resp) - (size_t)rlen,
                              "%d %d %d %d %d\n", i, r->x0, r->y0, r->x1, r->y1);
        }
        REPLY(resp, (size_t)rlen);

    } else if (strcmp(line, "PADMAP_ON") == 0) {
        g_padmap_enabled = 1;
        REPLY("OK\n", 3);

    } else if (strcmp(line, "PADMAP_OFF") == 0) {
        g_padmap_enabled = 0;
        g_active_pad = -1;
        REPLY("OK\n", 3);

    } else if (strcmp(line, "LASTTOUCH") == 0) {
        char resp[48];
        int  rlen = snprintf(resp, sizeof(resp), "X=%d Y=%d\n", g_last_touch_x, g_last_touch_y);
        REPLY(resp, (size_t)rlen);

    } else if (strcmp(line, "PADMAP_STATE") == 0) {
        /* Diagnostic: dumps the internal state pad_hit_test()/inject_touch()
         * actually use, for debugging why PADMAP_ON isn't firing PADCHORD. */
        char resp[320];
        int  rlen = snprintf(resp, sizeof(resp),
                              "ENABLED=%d ACTIVE_PAD=%d NKS4_LOADED=%d PAD_PLAY_ACTIVE=%d ON_PADS_PAGE=%d "
                              "ENABLE_PAD_PLAY=%d CHORD_ASSIGN=%d FIXED_VELOCITY=%d "
                              "LAST_TOUCH_TYPE=%d LAST_GATE_PADS_PAGE=%d LAST_GATE_PAD_PLAY=%d "
                              "LAST_GATE_CHORD_ASSIGN=%d LAST_GATE_HIT=%d\n",
                              g_padmap_enabled, g_active_pad, g_nks4_loaded, pad_play_active(),
                              on_pads_page(), enable_pad_play_on(), chord_assign_on(),
                              fixed_velocity_on(), g_last_touch_type, g_last_gate_pads_page,
                              g_last_gate_pad_play, g_last_gate_chord_assign, g_last_gate_hit);
        REPLY(resp, (size_t)rlen);

    } else if (strcmp(line, "PALETTE") == 0) {
        /* Diagnostic: dumps the full 256-entry RGB palette (same table sent
         * to stream clients at handshake) so a raw fb1 index found via
         * PIXEL/REGION can be translated to an actual color - e.g. to
         * confirm a toggle indicator is really "red" and find its on/off
         * palette indices without guessing. */
        static char resp[256 * 6 + 16];
        int i, rlen = 0;
        for (i = 0; i < PAL_ENTRIES; i++) {
            rlen += snprintf(resp + rlen, sizeof(resp) - (size_t)rlen, "%02x%02x%02x",
                              (unsigned)(pal_r[i] >> 8), (unsigned)(pal_g[i] >> 8),
                              (unsigned)(pal_b[i] >> 8));
        }
        rlen += snprintf(resp + rlen, sizeof(resp) - (size_t)rlen, "\n");
        REPLY(resp, (size_t)rlen);

    } else if (strncmp(line, "REGION ", 7) == 0) {
        /* Diagnostic/calibration: hex-dump of a rectangle of raw fb1
         * palette-index bytes in one round trip - for diffing a before/
         * after snapshot around a UI toggle to find exactly which pixels
         * change color, since per-pixel PIXEL queries are too slow for a
         * wide-area scan (one round trip per pixel). Capped at 8192 pixels
         * total to keep the response buffer bounded. */
        int rx0 = 0, ry0 = 0, rx1 = 0, ry1 = 0;
        if (sscanf(line + 7, "%d %d %d %d", &rx0, &ry0, &rx1, &ry1) == 4) {
            int rw, rh, x, y;
            rx0 = clampi(rx0, 0, (int)fb_w - 1);
            rx1 = clampi(rx1, 0, (int)fb_w - 1);
            ry0 = clampi(ry0, 0, (int)fb_h - 1);
            ry1 = clampi(ry1, 0, (int)fb_h - 1);
            if (rx1 < rx0) { int t = rx0; rx0 = rx1; rx1 = t; }
            if (ry1 < ry0) { int t = ry0; ry0 = ry1; ry1 = t; }
            rw = rx1 - rx0 + 1;
            rh = ry1 - ry0 + 1;
            if (fb1_map && (long)rw * (long)rh <= 8192) {
                static char resp[8192 * 2 + 64];
                int rlen = snprintf(resp, sizeof(resp), "W=%d H=%d ", rw, rh);
                for (y = 0; y < rh; y++) {
                    for (x = 0; x < rw; x++) {
                        rlen += snprintf(resp + rlen, sizeof(resp) - (size_t)rlen, "%02x",
                                          (unsigned)fb1_map[(ry0 + y) * (int)fb1_stride + (rx0 + x)]);
                    }
                }
                rlen += snprintf(resp + rlen, sizeof(resp) - (size_t)rlen, "\n");
                REPLY(resp, (size_t)rlen);
            } else {
                REPLY("ERR\n", 4);
            }
        } else {
            REPLY("ERR\n", 4);
        }

    } else if (strncmp(line, "PIXEL ", 6) == 0) {
        /* Diagnostic/calibration: raw palette-index byte at (x,y) in the
         * current fb1 capture - for finding a page-identifying pixel (e.g.
         * something unique to the Pads (touch to play) screen) the same way
         * PADMAP's regions were calibrated, since the sInstance[0x104] flag
         * approach was live-tested and disproved (2026-07-14) - it doesn't
         * track page/mode the way the OnShow/OnHide call pattern suggested. */
        int px = 0, py = 0;
        if (sscanf(line + 6, "%d %d", &px, &py) == 2 &&
            px >= 0 && px < (int)fb_w && py >= 0 && py < (int)fb_h && fb1_map) {
            char resp[24];
            int rlen = snprintf(resp, sizeof(resp), "V=%u\n",
                                 (unsigned)fb1_map[py * (int)fb1_stride + px]);
            REPLY(resp, (size_t)rlen);
        } else {
            REPLY("ERR\n", 4);
        }

    } else if (strncmp(line, "BUTTON ", 7) == 0) {
        const char *bname = line + 7;
        const struct btn_def *b;
        if (!g_nks4_loaded) { REPLY("ERR NKS4_NOT_LOADED\n", 21); return; }
        for (b = btn_table; b->name; b++) {
            if (strcmp(bname, b->name) == 0) break;
        }
        /* Unknown button name: nothing sensible to snap to, so reject outright
         * rather than silently pick some other button. */
        if (b->name) {
            nks4_write("BTN %u\n", b->code);
            mode_from_btn(b->code);
            REPLY("OK\n", 3);
        } else {
            REPLY("ERR\n", 4);
        }

    } else if (strncmp(line, "CHORD ", 6) == 0) {
        char names[8][16];
        const struct btn_def *btns[8];
        int count = 0, hold_ms = 0;
        const char *p = line + 6;
        if (!g_nks4_loaded) { REPLY("ERR NKS4_NOT_LOADED\n", 21); return; }
        while (*p == ' ') p++;
        /* Optional leading number = hold duration in ms; clamp into [0,5000]
         * rather than reject (a client sending "-50" or "999999" almost
         * certainly means "as short/long as allowed", not a malformed request). */
        if (*p >= '0' && *p <= '9') {
            hold_ms = clampi(atoi(p), 0, 5000);
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
                    nks4_write("BTN_DOWN %u\n", btns[i]->code);
                if (hold_ms > 0) usleep(hold_ms * 1000);
                for (int i = count - 1; i >= 0; i--)
                    nks4_write("BTN_UP %u\n", btns[i]->code);
                REPLY("OK\n", 3);
            } else {
                REPLY("ERR\n", 4);   /* one or more unknown button names */
            }
        } else {
            REPLY("ERR\n", 4);       /* fewer than 2 buttons is not a chord */
        }

    } else if (strncmp(line, "WHEEL ", 6) == 0) {
        /* CW/CCW are the only two valid directions - no numeric value to snap,
         * so anything else is rejected outright.  Delta is the exact 32-bit
         * value ground-truthed from a real hardware capture (HandleRotary's
         * EDX is zero-extended from the raw 16-bit NKS4 field, NOT a signed
         * -256 - that distinction is load-bearing, do not "simplify" this to
         * a negative literal). */
        const char *dir = line + 6;
        int delta;
        if (!g_nks4_loaded) { REPLY("ERR NKS4_NOT_LOADED\n", 21); return; }
        if (strcmp(dir, "CW") == 0)       delta = 0x00000100;
        else if (strcmp(dir, "CCW") == 0) delta = 0x0000FF00;
        else { REPLY("ERR\n", 4); return; }
        nks4_write("ROT %d\n", delta);
        REPLY("OK\n", 3);

    } else if (strncmp(line, "SLIDER ", 7) == 0) {
        /* Physical Slider n, device code 16 + (n-1).  idx and val are each
         * snapped into their valid range rather than rejected - only a
         * genuinely malformed line (sscanf fails to find two integers at
         * all) is treated as an error. */
        int idx = 0, val = 0;
        if (!g_nks4_loaded) { REPLY("ERR NKS4_NOT_LOADED\n", 21); return; }
        if (sscanf(line + 7, "%d %d", &idx, &val) == 2) {
            idx = clampi(idx, 1, 8);
            nks4_analog_write(16 + (idx - 1), val);
            REPLY("OK\n", 3);
        } else {
            REPLY("ERR\n", 4);
        }

    } else if (strncmp(line, "KNOB ", 5) == 0) {
        /* Physical RT Knob n, device code 8 + (n-1). Same snap-not-reject
         * policy as SLIDER. */
        int idx = 0, val = 0;
        if (!g_nks4_loaded) { REPLY("ERR NKS4_NOT_LOADED\n", 21); return; }
        if (sscanf(line + 5, "%d %d", &idx, &val) == 2) {
            idx = clampi(idx, 1, 8);
            nks4_analog_write(8 + (idx - 1), val);
            REPLY("OK\n", 3);
        } else {
            REPLY("ERR\n", 4);
        }

    } else if (strncmp(line, "VSLIDER ", 8) == 0) {
        /* Value slider, device code 25.  val is snapped into [0,127]; only a
         * line with no parsable integer at all is rejected. */
        int val = 0;
        if (!g_nks4_loaded) { REPLY("ERR NKS4_NOT_LOADED\n", 21); return; }
        if (sscanf(line + 8, "%d", &val) == 1) {
            nks4_analog_write(25, val);
            REPLY("OK\n", 3);
        } else {
            REPLY("ERR\n", 4);
        }

    /* --- JOYSTICK / VECTOR / RIBBON / AFTERTOUCH / PEDAL / FOOTSWITCH / DAMPER ---
     * Device codes identified from OA.ko's own AnalogControllerHandler dispatch
     * tables and cross-checked against the Kronos Operation Guide's front-panel
     * control list (see docs/api.md Section 10) - all real, standard controls.
     * Wired through the SAME validated call path as SLIDER/KNOB/VSLIDER
     * (ShortInvertNkS4AnalogValue -> HandleAnalogController, byte0 = value*2).
     * Hardware-confirmed by live testing: JOYSTICK and VECTOR (full-radius CW
     * circular sweep, then half-radius CCW), RIBBON X axis (center/max/min
     * sweep - Z axis untested), AFTERTOUCH, PEDAL, and FOOTSWITCH all track
     * large single-step jumps correctly, same as the knob/slider group.
     * DAMPER also confirmed working but is a real, documented exception: large
     * jumps produced inconsistent results across otherwise-identical runs; a
     * slow many-step ramp was smooth and repeatable. Send DAMPER as a gradual
     * ramp only - see that command's own comment below. */

    } else if (strncmp(line, "JOYSTICK ", 9) == 0) {
        /* Standard pitch/mod Joystick (Kronos manual item 12). Hardware-
         * confirmed via a full-radius clockwise circular sweep then a
         * half-radius counter-clockwise sweep, same as VECTOR below. */
        char axis = 0; int val = 0;
        if (!g_nks4_loaded) { REPLY("ERR NKS4_NOT_LOADED\n", 21); return; }
        if (sscanf(line + 9, "%c %d", &axis, &val) == 2 && (axis == 'X' || axis == 'Y')) {
            nks4_analog_write(axis == 'X' ? 1 : 2, val);
            REPLY("OK\n", 3);
        } else {
            REPLY("ERR\n", 4);   /* unrecognised axis - no sensible value to snap to */
        }

    } else if (strncmp(line, "VECTOR ", 7) == 0) {
        /* Vector Joystick (Kronos manual item 9) - a separate physical control
         * from JOYSTICK above, used for Vector Synthesis, not pitch/mod.
         * Hardware-confirmed via a full-radius clockwise circular sweep then a
         * half-radius counter-clockwise sweep. */
        char axis = 0; int val = 0;
        if (!g_nks4_loaded) { REPLY("ERR NKS4_NOT_LOADED\n", 21); return; }
        if (sscanf(line + 7, "%c %d", &axis, &val) == 2 && (axis == 'X' || axis == 'Y')) {
            nks4_analog_write(axis == 'X' ? 5 : 6, val);
            REPLY("OK\n", 3);
        } else {
            REPLY("ERR\n", 4);
        }

    } else if (strncmp(line, "RIBBON ", 7) == 0) {
        /* X axis (finger position) hardware-confirmed via a center/max/center/
         * min/center sweep. Z axis's exact meaning (commonly touch pressure on
         * a ribbon strip) is real (confirmed dispatch entry) but untested -
         * see docs/api.md Section 10. */
        char axis = 0; int val = 0;
        if (!g_nks4_loaded) { REPLY("ERR NKS4_NOT_LOADED\n", 21); return; }
        if (sscanf(line + 7, "%c %d", &axis, &val) == 2 && (axis == 'X' || axis == 'Z')) {
            nks4_analog_write(axis == 'X' ? 3 : 4, val);
            REPLY("OK\n", 3);
        } else {
            REPLY("ERR\n", 4);
        }

    } else if (strncmp(line, "AFTERTOUCH ", 11) == 0) {
        /* Keybed channel aftertouch. Hardware-confirmed via a 0/half/full/
         * half/0 sweep - large single-step jumps work fine. */
        int val = 0;
        if (!g_nks4_loaded) { REPLY("ERR NKS4_NOT_LOADED\n", 21); return; }
        if (sscanf(line + 11, "%d", &val) == 1) {
            nks4_analog_write(7, val);
            REPLY("OK\n", 3);
        } else {
            REPLY("ERR\n", 4);
        }

    } else if (strncmp(line, "PEDAL ", 6) == 0) {
        /* Rear-panel assignable PEDAL jack (continuous, e.g. expression).
         * Hardware-confirmed via a 0/half/full/half/0 sweep - large single-step
         * jumps work fine here, unlike DAMPER below. */
        int val = 0;
        if (!g_nks4_loaded) { REPLY("ERR NKS4_NOT_LOADED\n", 21); return; }
        if (sscanf(line + 6, "%d", &val) == 1) {
            nks4_analog_write(27, val);
            REPLY("OK\n", 3);
        } else {
            REPLY("ERR\n", 4);
        }

    } else if (strncmp(line, "FOOTSWITCH ", 11) == 0) {
        /* Rear-panel assignable foot SWITCH jack. Hardware-confirmed via a
         * single on(127)/off(0) tap, registered correctly with no polarity
         * surprises - a simple on/off switch, unlike the other continuous
         * controls here, but the value is still snapped into [0,127] rather
         * than restricted to those two values - the daemon does not assume
         * the assigned function only cares about on/off. */
        int val = 0;
        if (!g_nks4_loaded) { REPLY("ERR NKS4_NOT_LOADED\n", 21); return; }
        if (sscanf(line + 11, "%d", &val) == 1) {
            nks4_analog_write(28, val);
            REPLY("OK\n", 3);
        } else {
            REPLY("ERR\n", 4);
        }

    } else if (strncmp(line, "DAMPER ", 7) == 0) {
        /* Rear-panel DAMPER jack - sustain, or half-damper position if the
         * current Program/Combi has half-damper response configured.
         *
         * Hardware-confirmed, but with a real quirk: the jack accepts either
         * a simple on/off footswitch (Korg PS-1) or a continuous half-damper
         * pedal (Korg DS-1H), and AnalogDamperHandler evidently uses rate-of-
         * change to tell them apart - NOT a polarity-setting effect (a
         * polarity change was tried first based on the odd initial symptoms
         * and made no difference). A direct jump to a target value produces
         * inconsistent, non-repeatable results on real hardware; a gradual
         * ramp through every intermediate value does not - confirmed via a
         * 256-step sweep, smooth and repeatable both directions. Routed
         * through nks4_analog_ramp() below so callers never have to think
         * about this - send a target value like any other command here. */
        int val = 0;
        if (!g_nks4_loaded) { REPLY("ERR NKS4_NOT_LOADED\n", 21); return; }
        if (sscanf(line + 7, "%d", &val) == 1) {
            nks4_analog_ramp(29, val, &g_damper_value);
            REPLY("OK\n", 3);
        } else {
            REPLY("ERR\n", 4);
        }

    } else if (strncmp(line, "TEMPO ", 6) == 0) {
        /* Tempo, device code 26. Same behavioural class as DAMPER above - a
         * direct jump produces inconsistent results, a gradual ramp does not
         * - routed through the same nks4_analog_ramp() helper.
         *
         * value is 0-127 like every other analog command here, NOT a direct
         * BPM number - it maps onto the documented 40-300bpm range through a
         * confirmed non-linear curve (more resolution at low tempos), not a
         * straight line. Ground-truthed on hardware, identical ascending and
         * descending:
         *   value:  0   16   32   48   64   80   96  112  127
         *   bpm:   40   51   68   92  120  154  196  245  297
         * Interpolate between these points for an intermediate value if a
         * client needs a specific BPM; no closed-form formula is exposed
         * here since the confirmed data is the more reliable source. */
        int val = 0;
        if (!g_nks4_loaded) { REPLY("ERR NKS4_NOT_LOADED\n", 21); return; }
        if (sscanf(line + 6, "%d", &val) == 1) {
            nks4_analog_ramp(26, val, &g_tempo_value);
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
 * complete lines.  Returns -1 if the connection closed.
 *
 * TOUCH_MOVE coalescing: a queued backlog of TOUCH_MOVE commands from a fast
 * drag was delaying the release that follows it - each queued move pays
 * touch_pace()'s blocking 30ms minimum gap before the release even gets its
 * turn, so a 20-move backlog meant ~600ms before the note actually let go
 * (confirmed 2026-07-14). A completed TOUCH_MOVE line is held in
 * ctrl_pending_move instead of processed immediately; any other command
 * (including a later TOUCH_MOVE, which simply overwrites the pending one)
 * flushes it first. This drops stale intermediate positions during a fast
 * drag but never reorders or delays anything else, and TOUCH_DOWN/TOUCH_UP
 * always process immediately in full - only same-type backlog is collapsed. */
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
                    if (ctrl_lb_n > 1) {
                        if (strncmp(ctrl_lb, "TOUCH_MOVE ", 11) == 0 &&
                            ctrl_lb_n - 1 < (int)sizeof(ctrl_pending_move)) {
                            memcpy(ctrl_pending_move, ctrl_lb, (size_t)ctrl_lb_n);
                            ctrl_has_pending_move = 1;
                        } else {
                            if (ctrl_has_pending_move) {
                                process_ctrl_cmd(ctrl_pending_move, -1);
                                ctrl_has_pending_move = 0;
                            }
                            process_ctrl_cmd(ctrl_lb, ctrl_fd);
                        }
                    }
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
    /* Flush a still-pending move once the socket is fully drained - it's
     * the latest known position and there's nothing newer to supersede it. */
    if (ctrl_has_pending_move) {
        process_ctrl_cmd(ctrl_pending_move, ctrl_fd);
        ctrl_has_pending_move = 0;
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

/* EVA-ready gate for the DEFERRED midi_bridge load.  midi_bridge's tap does a
 * lock-xadd on OA's transmit-queue reader count; doing that while EVA is still
 * bringing up the USB codec (the ~67 s "initializing user interface" window)
 * wedges EVA - the observed brick.  So we hold the load until EVA has drawn its
 * UI, detected from the framebuffer.
 *
 * Boot curve, measured on hardware (fb1, 8 bpp, sampled every 37th byte):
 *   loading:  nonblack=0,  distinct=2   (67 s, dead flat - pure black + text)
 *   UI drawn: nonblack>=21, distinct>=17 (Set List, the darkest default screen)
 * The transition is a single-second snap with an enormous margin, so we AND two
 * signals - non-black% (brightness) AND distinct palette-index count
 * (colorfulness, immune to a dark UI) - and require both held for a couple of
 * seconds so no one-frame anomaly can trip it.  Loading fails both by a mile. */
#define EVA_UI_NONBLACK_PCT 10   /* loading=0,  darkest UI=21 */
#define EVA_UI_DISTINCT_MIN 8    /* loading=2,  darkest UI=17 */
#define EVA_UI_READY_CHECKS 2    /* consecutive 1 s passes before loading */
/* Sample fb1 sparsely and report two independent "is EVA's UI up?" measures:
 *   *pct      = percent of sampled pixels that are non-black (brightness)
 *   *distinct = number of distinct 8-bpp palette indices seen (colorfulness)
 * The loading/update screen is >90% black AND uses only a handful of indices
 * (black + progress text/bar); even the darkest real UI (Set List, ~21-32%
 * non-black) uses many indices.  distinct is therefore the sturdier signal - it
 * doesn't collapse when the UI happens to be dark.  Either pointer may be NULL. */
static void fb_metrics(int *pct, int *distinct)
{
    uint32_t i, n = 0, nb = 0, total = fb1_stride * fb_h;
    uint8_t seen[256];
    int d = 0;
    if (pct) *pct = 0;
    if (distinct) *distinct = 0;
    if (!fb1_map || total == 0) return;
    memset(seen, 0, sizeof(seen));
    for (i = 0; i < total; i += 37) {
        uint8_t v = fb1_map[i];
        n++;
        if (v) nb++;
        if (!seen[v]) { seen[v] = 1; d++; }
    }
    if (pct)      *pct = n ? (int)((100u * nb) / n) : 0;
    if (distinct) *distinct = d;
}

/* Load midi_bridge + open its /proc surfaces + start MIDI capture.  Called ONLY
 * after EVA has drawn its UI (see the main loop).  midi_bridge taps OA's transmit
 * queues (AllocReader) and reads the in-port objects at load; doing that while EVA
 * is still initializing the USB codec wedges EVA (observed brick).  Loading it only
 * after the UI is up removes it from that window entirely.  eva_ready=1 is passed
 * because EVA is already up, so injection is enabled immediately (the in-module
 * gate stays as defense-in-depth). */
static void load_midi_bridge(void)
{
    unsigned long recv_fn = 0, reg_fn = 0, outport_fn = 0;
    char params[512];
    long ret;

    /* EVA having drawn its UI implies OA is Live, so this returns at once - but
     * assert it anyway before reading OA's in-memory objects. */
    if (!wait_for_oa_live(50)) {
        fprintf(stderr, "screenremote: OA not Live at midi_bridge load - MIDI disabled\n");
        return;
    }
    resolve_kallsyms(&recv_fn, &reg_fn, &outport_fn);
    if (!outport_fn) {
        fprintf(stderr, "screenremote: no usable MIDI symbols in kallsyms - MIDI disabled\n");
        return;
    }
    snprintf(params, sizeof(params),
             "receive_fn=0x%lx register_fn=0x%lx regoutport=0x%lx eva_ready=1 tap_shared=1",
             recv_fn, reg_fn, outport_fn);
    extract_ko(MIDI_BRIDGE_KO, midi_bridge_ko, midi_bridge_ko_len);
    ret = syscall(SYS_init_module, (void *)midi_bridge_ko,
                  (unsigned long)midi_bridge_ko_len, params);
    if (ret == 0 || errno == EEXIST) {
        int _mi;
        fprintf(stderr, "screenremote: midi_bridge %s (recv=%s reg=%s outport=%s)\n",
                ret == 0 ? "loaded" : "already loaded",
                recv_fn ? "ok" : "none", reg_fn ? "ok" : "none",
                outport_fn ? "ok" : "none");
        for (_mi = 0; _mi < 20 && midi_in_fd < 0; _mi++) {
            usleep(100000);
            midi_in_fd = open("/proc/.midi_in", O_WRONLY);
        }
        g_midi_loaded = (midi_in_fd >= 0);
        start_midi_capture();
        fprintf(stderr, "screenremote: midi_in=%d capture=%d\n",
                midi_in_fd >= 0 ? 1 : 0, midi_cap_fd >= 0 ? 1 : 0);
    } else {
        fprintf(stderr, "screenremote: midi_bridge failed (%ld)\n", ret);
    }
}

/* Load nks4_inject.ko - see its own header comment for what it does and why.
 * Unlike midi_bridge this makes pure additive calls into OA's own
 * CSTGFrontPanel::Handle* methods; it never claims a reader slot on a live
 * queue or touches anything EVA's USB-codec init could still be mutating, so
 * it only needs OA to be Live (kallsyms resolvable, .text relocated) - NOT
 * EVA's UI to be up.  Loaded early (right after vkbd, see main()), well
 * before midi_bridge's EVA-ready gate. */
static void load_nks4_inject(void)
{
    unsigned long fn_switch = 0, fn_touch = 0, fn_rotary = 0, fn_analog = 0, fn_invert = 0;
    unsigned long fn_chord = 0;
    char params[320];
    long ret;

    if (!wait_for_oa_live(50)) {
        fprintf(stderr, "screenremote: OA not Live at nks4_inject load - "
                "front-panel injection disabled\n");
        return;
    }
    resolve_nks4_kallsyms(&fn_switch, &fn_touch, &fn_rotary, &fn_analog, &fn_invert, &fn_chord);
    if (!fn_switch || !fn_touch || !fn_rotary || !fn_analog || !fn_invert) {
        fprintf(stderr, "screenremote: missing NKS4 symbols in kallsyms "
                "(switch=%s touch=%s rotary=%s analog=%s invert=%s) - "
                "front-panel injection disabled\n",
                fn_switch ? "ok" : "none", fn_touch ? "ok" : "none",
                fn_rotary ? "ok" : "none", fn_analog ? "ok" : "none",
                fn_invert ? "ok" : "none");
        return;
    }
    /* fn_chord (PADCHORD/RT_chord_trigger) is best-effort: derived via a
     * fixed-offset delta from Do_KM_note_out_chord_trig rather than a direct
     * kallsyms symbol, so it can legitimately come back 0 if that anchor
     * symbol is ever renamed/missing. PADCHORD just won't work in that case
     * (inject_chord() checks fn_chord itself) - doesn't block the other 5. */
    snprintf(params, sizeof(params),
             "fn_switch=0x%lx fn_touch=0x%lx fn_rotary=0x%lx fn_analog=0x%lx fn_invert=0x%lx "
             "fn_chord=0x%lx",
             fn_switch, fn_touch, fn_rotary, fn_analog, fn_invert, fn_chord);
    if (!fn_chord)
        fprintf(stderr, "screenremote: fn_chord not resolved - PADCHORD will be unavailable\n");
    extract_ko(NKS4_INJECT_KO, nks4_inject_ko, nks4_inject_ko_len);
    ret = syscall(SYS_init_module, (void *)nks4_inject_ko,
                  (unsigned long)nks4_inject_ko_len, params);
    if (ret == 0 || errno == EEXIST) {
        int _ni;
        for (_ni = 0; _ni < 20 && nks4_fd < 0; _ni++) {
            usleep(100000);
            nks4_fd = open("/proc/.nks4inject", O_WRONLY);
        }
        g_nks4_loaded = (nks4_fd >= 0);
        fprintf(stderr, "screenremote: nks4_inject %s (fd=%s)\n",
                ret == 0 ? "loaded" : "already loaded",
                nks4_fd >= 0 ? "ok" : "open failed");
    } else {
        fprintf(stderr, "screenremote: nks4_inject failed (%ld)\n", ret);
    }
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
 * whether midi_bridge printed "out tap ..." / "ready" or a warning before a
 * freeze, to localise any regression during the risky module-load window. */
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
 * stray midi_tcp keeps port 9875 and a /proc/.midi_ring reader open - and because
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

/* Pin ourselves - and, by inheritance across fork+exec, our streaming children and
 * the midi_tcp child - to physical CORE 0 (logical CPUs 0,1).  On the Kronos (Atom
 * D2550, 2 cores + HT: core0=CPU0,1 / core1=CPU2,3) the RT audio engine, EVA, and
 * boot-time PCM sample loading all run on core 1, which pins at 100% during boot.
 * The scheduler otherwise places screenremote's fb-streaming on core 1 too; that
 * extra load during the ~4 s boot-settling window starves the RT engine, rtf0 backs
 * up, and EVA freezes (confirmed: no crash without a client, i.e. without the
 * streaming load).  Keeping our load on the otherwise-idle core 0 removes it from
 * the RT core entirely.  Best-effort: a failure just leaves default scheduling. */
static void pin_off_rt_core(void)
{
    cpu_set_t set;
    CPU_ZERO(&set);
    CPU_SET(0, &set);
    CPU_SET(1, &set);
    if (sched_setaffinity(0, sizeof(set), &set) != 0)
        fprintf(stderr, "screenremote: sched_setaffinity(core0) failed: %s\n",
                strerror(errno));
    else
        fprintf(stderr, "screenremote: pinned to core 0 (CPUs 0,1), off the RT core (2,3)\n");
}

/*  Main */
int main(void)
{
    pid_t kmsg_pid = -1;
    pin_off_rt_core();   /* keep our load off the RT/sample-loading core - do this
                          * before any fork so children inherit the affinity */
    int stream_listen, ctrl_listen, disc_fd = -1, client_fd = -1;
    /* ctrl_fd and ctrl_lb* are file-scope globals (see top of file) */
    uint8_t client_mode = MODE_CHANGE, client_fps = FPS_MAX;
    time_t last_mirror_chk = 0, last_net_chk = 0, last_eva_chk = 0;
    time_t g_start_time = time(NULL);  /* for FBCURVE relative timestamps */
    int eva_ready_streak = 0;          /* consecutive EVA-UI-detected checks */
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
     * on entry the previous boot did not complete cleanly - skip midi_bridge
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
     * run a Korg OS update or the cleaner safely (bring the unit up with no kernel
     * modules at all), and to recover a wedged unit.  midi_bridge no longer patches
     * OA .text - its OA-unload notifier just releases its queue reader slot - so
     * the old "Preparing to Install" teardown freeze is gone; this kill-switch
     * remains as defense-in-depth.  Checked here because this daemon is what loads
     * the modules (init_module(2) from embedded buffers); a kernel-side FS check is
     * unreliable in init_module context on the RTAI kernel. */
    int load_mods = 1;
    {
        struct stat _ns;
        if (stat("/korg/rw/HD/_nomod", &_ns) == 0 && S_ISDIR(_ns.st_mode)) {
            load_mods = 0;
            fprintf(stderr, "screenremote: kill-switch /korg/rw/HD/_nomod present - "
                    "not loading any kernel modules (vkbd, midi_bridge)\n");
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
     * load NO kernel modules at all this boot (not vkbd, not midi_bridge), so the
     * unit is guaranteed to reach a usable state.  Remove /korg/rw/screenremote/.boot
     * (or fix the cause) to re-enable module loading on the next boot. */
    if (load_mods && !boot_flag_found) {
        extract_ko(VKBD_KO, vkbd_ko, vkbd_ko_len);
        syscall(SYS_init_module, (void *)vkbd_ko, (unsigned long)vkbd_ko_len, "");

        for (int _vi = 0; _vi < 20 && vkbd_fd < 0; _vi++) {
            usleep(100000);
            vkbd_fd = open("/proc/.vkbd", O_WRONLY);
        }

        /* Front-panel injection module: safe to load this early too (see
         * load_nks4_inject()'s own comment for why it doesn't need to wait for
         * EVA like midi_bridge does) - BUTTON/TOUCH/WHEEL/SLIDER/KNOB/VSLIDER
         * are unavailable (ERR NKS4_NOT_LOADED) until this succeeds. */
        load_nks4_inject();
    }

    /* MIDI injection module: DEFERRED, not loaded here.
     *
     * midi_bridge taps OA's transmit queues (AllocReader) and reads the in-port
     * objects at load time.  Doing that while EVA is still "initializing user
     * interface" - i.e. mid-init of the USB codec whose queues midi_bridge taps -
     * wedges EVA (observed brick: EVA blocks and never reaches the UI, even with
     * MIDI injection fully gated, so the load itself is the hazard, not injection).
     *
     * So we only flag it here and let the main loop load it once EVA has drawn its
     * UI (framebuffer no longer mostly-black - see load_midi_bridge / EVA-ready
     * latch).  vkbd above is safe to load now: it registers a uinput device and
     * never touches OA.  OA-Live is implied by EVA having drawn its UI, so no
     * separate wait_for_oa_live is needed on this path. */
    if (access(FBCURVE_FLAG, F_OK) == 0) {
        g_fbcurve_cal = 1;
        FILE *cf = fopen(FBCURVE_LOG, "w");  /* fresh log per boot */
        if (cf) fclose(cf);
        fprintf(stderr, "screenremote: FBCURVE calibration mode - logging "
                "framebuffer curve, midi_bridge will NOT load\n");
    } else if (load_mods && !boot_flag_found) {
        g_midi_load_pending = 1;
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

        /* Deferred midi_bridge load: hold the load until EVA has finished the
         * ~67 s "initializing user interface" window (its tap wedges EVA if it
         * fires mid codec-init).  EVA-done is signalled by the framebuffer both
         * brightening AND gaining colors; require both for EVA_UI_READY_CHECKS
         * consecutive seconds.  One-shot: cleared after the first load attempt.
         * Runs regardless of client connections.  In calibration mode we only
         * log the curve and never load (a brick-safe reboot for tuning). */
        if (g_fbcurve_cal && now - last_eva_chk >= 1) {
            last_eva_chk = now;
            int pct = 0, dist = 0;
            fb_metrics(&pct, &dist);
            FILE *cf = fopen(FBCURVE_LOG, "a");
            if (cf) {
                fprintf(cf, "t=%ld nonblack=%d distinct=%d\n",
                        (long)(now - g_start_time), pct, dist);
                fclose(cf);
            }
        } else if (g_midi_load_pending && !g_midi_loaded && now - last_eva_chk >= 1) {
            last_eva_chk = now;
            int pct = 0, dist = 0;
            fb_metrics(&pct, &dist);
            if (pct >= EVA_UI_NONBLACK_PCT && dist >= EVA_UI_DISTINCT_MIN) {
                if (++eva_ready_streak >= EVA_UI_READY_CHECKS) {
                    fprintf(stderr, "screenremote: EVA UI up (nonblack=%d distinct=%d) "
                                    "- loading midi_bridge\n", pct, dist);
                    load_midi_bridge();
                    g_midi_load_pending = 0;
                }
            } else {
                eva_ready_streak = 0;
            }
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

        /* Control command - normally only accepted from the authenticated stream
         * client's IP (g_ctrl_allowed_ip is a single global "owner" slot: a new
         * stream connection replaces the old one and reassigns it, which silently
         * invalidates any other ctrl session - by design, so only whoever is
         * currently watching the mirror can also control it). A short allowlist
         * of read-only, one-shot diagnostic queries is exempted from that check
         * (accepted from any IP, regardless of who currently owns ctrl) so a
         * separate observer (e.g. a calibration monitor) can read live state
         * concurrently with the owning client's own session - mutating commands
         * (TOUCH, PADMAP, BUTTON, etc.) still require ownership as before. */
        if (ctrl_listen >= 0 && FD_ISSET(ctrl_listen, &rfds)) {
            struct sockaddr_in cpeer;
            socklen_t cplen = sizeof(cpeer);
            int cfd = accept(ctrl_listen, (struct sockaddr *)&cpeer, &cplen);
            if (cfd >= 0) {
                /* Read the first line up front (needed either way: to check
                 * against the read-only allowlist, or to decide CTRL_PERSIST
                 * vs one-shot for an owned connection). */
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
                    struct timeval zero = {0, 0};
                    setsockopt(cfd, SOL_SOCKET, SO_RCVTIMEO, &zero, sizeof(zero));

                    int is_readonly = (strcmp(firstline, "LASTTOUCH") == 0 ||
                                        strcmp(firstline, "PADMAP_LIST") == 0 ||
                                        strcmp(firstline, "PADMAP_STATE") == 0 ||
                                        strncmp(firstline, "PIXEL ", 6) == 0 ||
                                        strncmp(firstline, "REGION ", 7) == 0 ||
                                        strcmp(firstline, "PALETTE") == 0 ||
                                        strcmp(firstline, "STATE") == 0 ||
                                        strcmp(firstline, "VERSION") == 0 ||
                                        strcmp(firstline, "SYSINFO") == 0);
                    int owned = (g_ctrl_allowed_ip != 0 &&
                                 cpeer.sin_addr.s_addr == g_ctrl_allowed_ip);

                    if (is_readonly) {
                        /* Always one-shot regardless of ownership - never
                         * upgrades to CTRL_PERSIST, so it can't steal ctrl_fd
                         * from the owning client. */
                        process_ctrl_cmd(firstline, cfd);
                        close(cfd);
                    } else if (owned) {
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
                    } else {
                        fprintf(stderr, "screenremote: ctrl rejected from %s\n",
                                inet_ntoa(cpeer.sin_addr));
                        close(cfd);
                    }
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
    if (nks4_fd >= 0) { close(nks4_fd); nks4_fd = -1; }
    if (vkbd_fd  >= 0) { close(vkbd_fd);  vkbd_fd  = -1; }
    if (kbd_fd   >= 0) { ioctl(kbd_fd, UI_DEV_DESTROY); close(kbd_fd); kbd_fd = -1; }
    if (midi_in_fd  >= 0) { close(midi_in_fd);  midi_in_fd = -1; }
    if (midi_cap_fd >= 0) { close(midi_cap_fd); midi_cap_fd = -1; }
    if (midi_cap_pid > 0) { kill(midi_cap_pid, SIGTERM); waitpid(midi_cap_pid, NULL, 0); }
    if (g_midi_loaded) {
        syscall(__NR_delete_module, "midi_bridge", O_NONBLOCK);
        g_midi_loaded = 0;
    }
    syscall(__NR_delete_module, "vkbd", O_NONBLOCK);
    fb0_close();
    fprintf(stderr, "screenremote: exited cleanly\n");
    return 0;
}
