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
 *   STATE                   -> MODE=N EDITCTX=E\n
 *                            MODE (0=init/undetected 1=Setlist 2=Combi 3=Program 4=Sequence
 *                              5=Sampling 6=Global 7=Disk) and EDITCTX (0=none 1=Program-edit-
 *                              from-Combi 2=Program-edit-from-Sequence) are read live via
 *                              get_mode_state() - see that function for the full priority
 *                              order. Primary source is eva_mode.ko, a small kernel module
 *                              loaded at startup that reads Eva's own live CModeManager state
 *                              directly out of its process memory (exact - no thresholds, and
 *                              the only source that can report EDITCTX=2, fully calibrated
 *                              live against pixel ground truth 2026-07-17, see
 *                              docs/EVA_ModeManager_probe.md). Falls back to framebuffer pixel
 *                              detection (detect_ui_mode()/detect_program_edit_context(),
 *                              mode_detect_refs.h) when eva_mode.ko isn't loaded or hasn't
 *                              resolved yet (e.g. early boot before Eva has started) - pixel
 *                              detection's own fallback to the last BUTTON-commanded mode
 *                              applies in that path only. Either way this is the daemon's own
 *                              source of truth, not an echo of client-side screen comparison.
 *   MODE_DETAIL             -> SOURCE=eva|pixel MODE=N EDITCTX=E EDITSLOT=S EVA_LOADED=0|1
 *                              EVA_RESOLVED=0|1\n
 *                            Richer counterpart to STATE (same spirit as PADMAP_STATE
 *                              alongside PADMAP_*): SOURCE says which of the two paths above
 *                              actually answered MODE/EDITCTX this call; EDITSLOT is the
 *                              timbre/track index being Program-edited (-1 if EDITCTX=0, or
 *                              if eva_mode.ko didn't resolve); EVA_LOADED/EVA_RESOLVED let a
 *                              caller confirm eva_mode.ko is in play rather than the pixel
 *                              fallback without needing to infer it from SOURCE alone.
 *   VERSION                 -> VER=x.x.x BUILD=xxx\n
 *   SYSINFO                 -> multi-line key=value block terminated by OK\n
 *                            (UPTIME, LOAD, MEM_*, CPU_*, AUDIO_*, DISK_*, USB_*, TEMP*, FAN*, MODE, EDITCTX)
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
 * try_load_nks4_inject()) calls OA's real CSTGFrontPanel::HandleSwitchEvent / HandleTouchPanel /
 * HandleRotary / HandleAnalogController directly via /proc/.nks4inject, i.e. the exact function
 * a physical press/touch/turn dispatches through - so injected events get bit-for-bit the same
 * response as hardware, independent of whatever mode Eva happens to be in.  See g_nks4_loaded.
 *
 * rtf5 fallback (g_rtf5_fallback_active): if nks4_inject.ko is ever given up on for a boot
 * (permanent load failure, or OA never reaching Live within NKS4_LOAD_DEADLINE_S of the deferred
 * retry starting - see the main loop), BUTTON, CHORD, the TOUCH commands, WHEEL, SLIDER, and
 * VSLIDER fall back to writing raw packets to /dev/rtf5, OA's own OUTBOUND notification FIFO to
 * Eva (stg_rtfifo_init, direction OA-RT -> Eva) - the pre-1.10.0 injection mechanism.  This is a
 * DEGRADED path, not a substitute: genuine hardware events reach OA through a completely separate
 * route (CSTGOmapNKSMsgHandler::ProcessNextNKSEvent() -> CSTGFrontPanel::Handle*), so a synthetic
 * rtf5 packet only ever fools Eva's own UI-mirroring code - it never touches the real OA-side
 * action.  That's why touch/mode buttons "mostly work" this way (Eva's own widget hit-testing
 * reconstructs the right behaviour from the mirrored packet) while sequencer transport
 * (SEQ_START/SEQ_REC/SEQ_LOCATE/SEQ_FF/SEQ_REW/SEQ_PAUSE), TAP_TEMPO, SMPL_REC/SMPL_START, and
 * some MIDI-triggering touch actions silently do nothing even though the daemon still replies OK -
 * their real effect lives inside OA and is never reached.  KNOB/JOYSTICK/VECTOR/RIBBON/
 * AFTERTOUCH/PEDAL/FOOTSWITCH/DAMPER/TEMPO/PADCHORD have no rtf5 equivalent at all (added after
 * rtf5 was retired) and still return "ERR NKS4_NOT_LOADED\n" in fallback mode; a BUTTON/CHORD name
 * that exists in btn_table[] but has no historical rtf5 (dev,code) mapping (see rtf5_btn_table[])
 * returns "ERR RTF5_UNSUPPORTED\n" instead.  Entering fallback is logged to both stderr and
 * RTF5_FALLBACK_LOG (FTP-visible under LOG_DIR) so a degraded boot is never silent.
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
#include <dirent.h>
#include <linux/fb.h>
#include <linux/uinput.h>
#include <sys/syscall.h>
#include "palette_data.h"
#include "mode_detect_refs.h"
#include "vkbd_ko.h"
#include "midi_bridge_ko.h"
#include "midi_tcp_bin.h"
#include "nks4_inject_ko.h"
#include "eva_mode_ko.h"

/* Optional, interim compile-time boot-splash fallback - see
 * tools/extract_boot_splash.py's --header option and load_boot_splash()
 * below. Unlike the other embedded-header includes above, this one is
 * genuinely optional: a fresh checkout with no generated
 * boot_splash_data.h (the normal case - it's gitignored, see .gitignore)
 * builds identically to before this feature existed, just without the
 * fallback. __has_include is a GCC extension available since GCC 5; this
 * project already requires GCC. */
#if __has_include("boot_splash_data.h")
#include "boot_splash_data.h"
#define HAVE_EMBEDDED_BOOT_SPLASH 1
#endif

/*  Keyboard injection (uinput fallback) */
#define KBD_EV_SYN  0
#define KBD_EV_KEY  1

/*  Version */
#define SCREENREMOTE_VERSION "1.11.2"
#ifndef BUILD_ID
#define BUILD_ID "dev"
#endif

/* Config */
#define SCREENREMOTE_DIR  "/korg/rw/screenremote"
#define VKBD_KO           SCREENREMOTE_DIR "/vkbd.ko"
#define MIDI_BRIDGE_KO    SCREENREMOTE_DIR "/midi_bridge.ko"
#define MIDI_TCP_BIN      SCREENREMOTE_DIR "/midi_tcp"
#define NKS4_INJECT_KO    SCREENREMOTE_DIR "/nks4_inject.ko"
#define EVA_MODE_KO       SCREENREMOTE_DIR "/eva_mode.ko"

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
#define RTF5_FALLBACK_LOG  LOG_DIR "/rtf5_fallback.log"

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

/* Boot-splash compositing state (see apply_boot_splash()/load_boot_splash()/
 * send_frame(), all further down) - declared up here alongside the other
 * framebuffer state since send_frame() (pull mode's direct sender, defined
 * not far below) needs g_boot_splash directly, ahead of where the rest of
 * the boot-gate machinery is declared. update_boot_state() frees
 * g_boot_splash the moment the gate clears. */
static uint8_t  *g_boot_splash      = NULL;
static uint32_t  g_boot_splash_w    = 0;
static uint32_t  g_boot_splash_rows = 0;

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
/* Backstop (s): force-release a held pad if nothing has released it by hand
 * (overlapping TOUCH_DOWN and PADMAP_OFF already do). Covers the case a
 * client can't: it simply disconnects between TOUCH_DOWN and TOUCH_UP with no
 * further command ever arriving, which would otherwise strand a chord
 * sounding forever (found 2026-07-16, see the main loop's watchdog check). */
#define PADCHORD_MAX_HOLD_S 10
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
static int g_eva_mode_loaded  = 0;   /* 1 once eva_mode.ko is loaded (see load_eva_mode_module()) */

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
static int g_nks4_load_pending = 0;  /* 1 = early load found OA not-yet-Live; retry from main loop */

/* rtf5 degraded fallback - see header comment above for what this is and its known
 * limitations.  Only entered once nks4_inject.ko has been permanently given up on for
 * this boot (see nks4_give_up() / the main loop's retry block). */
static int touch_fd = -1;              /* fd to /dev/rtf5 O_WRONLY, opened on first use */
static int g_rtf5_fallback_active = 0; /* 1 once nks4_inject has been given up on and rtf5 is in use */
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

/* rtf5 fallback button table - the pre-1.10.0 (dev,code) pairs written straight into
 * OA's rtf5 packet, from the last commit before rtf5 removal (904531c).  This is a
 * COMPLETELY SEPARATE code space from btn_table[]'s flat NKS4 scan codes above - do not
 * cross-reference the two.  Only names that existed before nks4_inject replaced rtf5 are
 * here; buttons added afterwards (MODULE_CONTROL, KARMA_ONOFF, KARMA_LATCH, DRUM_TRACK,
 * TIMBRE_TRACK, AUDIO_TRACK, EXT_TRACK, RTKNOBS_KARMA, TONE_ADJUST, SW1, SW2) have no rtf5
 * equivalent and fall through to "ERR RTF5_UNSUPPORTED\n" in fallback mode - see
 * rtf5_find_btn(). */
struct rtf5_btn_def { const char *name; uint32_t dev; uint32_t code; };
static const struct rtf5_btn_def rtf5_btn_table[] = {
    { "EXIT",       0x06u, 0x02u },
    { "ENTER",      0x06u, 0x10u },
    { "SETLIST",    0x07u, 0x0eu },
    { "COMBI",      0x07u, 0x08u },
    { "PROGRAM",    0x07u, 0x09u },
    { "SEQUENCE",   0x07u, 0x0au },
    { "SAMPLING",   0x07u, 0x0bu },
    { "GLOBAL",     0x07u, 0x0cu },
    { "DISK",       0x07u, 0x0du },
    { "HELP",       0x08u, 0x00u },
    { "COMPARE",    0x08u, 0x01u },
    { "RESET",      0x08u, 0x02u },
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
    { "INC",        0x06u, 0x00u },
    { "DEC",        0x06u, 0x01u },
    { "MP1",        0x04u, 0x00u },
    { "MP2",        0x04u, 0x01u },
    { "MP3",        0x04u, 0x02u },
    { "MP4",        0x04u, 0x03u },
    { "MP5",        0x04u, 0x04u },
    { "MP6",        0x04u, 0x05u },
    { "MP7",        0x04u, 0x06u },
    { "MP8",        0x04u, 0x07u },
    { "MS1",        0x05u, 0x00u },
    { "MS2",        0x05u, 0x01u },
    { "MS3",        0x05u, 0x02u },
    { "MS4",        0x05u, 0x03u },
    { "MS5",        0x05u, 0x04u },
    { "MS6",        0x05u, 0x05u },
    { "MS7",        0x05u, 0x06u },
    { "MS8",        0x05u, 0x07u },
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
    { "SEQ_START",  0x0bu, 0x00u },
    { "SEQ_REC",    0x0bu, 0x01u },
    { "SEQ_LOCATE", 0x0bu, 0x02u },
    { "SEQ_FF",     0x0bu, 0x03u },
    { "SEQ_REW",    0x0bu, 0x04u },
    { "SEQ_PAUSE",  0x0bu, 0x05u },
    { "TAP_TEMPO",  0x0bu, 0x06u },
    { "SMPL_REC",   0x0au, 0x00u },
    { "SMPL_START", 0x0au, 0x01u },
    { "MIX_KNOBS",  0x07u, 0x05u },
    { "SOLO",       0x07u, 0x06u },
    { NULL, 0u, 0u }
};

static const struct rtf5_btn_def *rtf5_find_btn(const char *name)
{
    const struct rtf5_btn_def *b;
    for (b = rtf5_btn_table; b->name; b++)
        if (strcmp(name, b->name) == 0) return b;
    return NULL;
}

/* Access control */
/* IP (network byte order) of the current stream client - the only host allowed
 * to send mutating control commands (read-only diagnostic queries are exempt,
 * see the ctrl accept path).  0 = no stream client connected (all ownership
 * checks fail closed). */
static uint32_t g_ctrl_allowed_ip = 0;

/* IP that established the current persistent ctrl_fd (CTRL_PERSIST), captured
 * once at accept time. handle_ctrl_persistent_data() services ctrl_fd on every
 * select() wakeup without re-checking who owns it, so without comparing this
 * against the (possibly since-changed) g_ctrl_allowed_ip before each service
 * call, a client that loses ownership - superseded by a new stream client,
 * disconnected, or bumped by an IP rebind - keeps injecting BUTTON/TOUCH/etc.
 * through its still-open persistent session indefinitely (found 2026-07-16). */
static uint32_t g_ctrl_persist_ip = 0;

/* Currently bound listen address (network byte order). Tracked so we can
 * detect IP changes and rebind the stream/ctrl listeners. */
static uint32_t g_bound_ip = INADDR_ANY;

/* Largest control line we accept.  Sized to hold the biggest legitimate command
 * as contiguous hex: MIDI_SEND / SYSEX carry up to a 4096-byte payload (the cap
 * their decode buffers mb[]/sb[] enforce), i.e. "MIDI_SEND " + 4096*2 hex chars,
 * plus slack for the command word and a few spaces.  A line longer than this is
 * rejected with "ERR LINE_TOO_LONG\n" rather than silently truncated - the old
 * 2048-byte buffers quietly dispatched a corrupted command for any large
 * payload, which the mb[4096]/sb[4096] decode buffers implied was supported. */
#define CTRL_LINE_MAX 8320

/* Persistent control connection */
static int  ctrl_fd     = -1;   /* accepted persistent ctrl socket, -1 if none */
static char ctrl_lb[CTRL_LINE_MAX]; /* partial line accumulation buffer */
static int  ctrl_lb_n   = 0;    /* bytes in ctrl_lb (excludes the newline) */
static int  ctrl_lb_overflow = 0;   /* 1 = current line exceeded ctrl_lb; reject at newline */

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

/* Mark an fd close-on-exec. The daemon is single-threaded and forks+execs exactly
 * once (start_midi_capture()'s midi_tcp child), so calling this right after every
 * fd-creating call - rather than O_CLOEXEC/SOCK_CLOEXEC flags, which depend on
 * toolchain/kernel flag support this static -m32 build shouldn't have to assume -
 * is race-free in practice: no other thread can fork+exec between fd creation and
 * this call.  Without it, every listener/proc/device fd open at that moment
 * (stream_listen, ctrl_listen, disc_fd, any connected client_fd/ctrl_fd, vkbd_fd,
 * nks4_fd, midi_in_fd, fb1_fd/fb0_fd, kbd_fd, touch_fd) leaks into midi_tcp for its
 * entire life: a crashed/killed screenremote leaves that orphan still holding the
 * listen ports, so a restart's bind() fails EADDRINUSE (SO_REUSEADDR does not
 * override an actively-bound listener) and the daemon can't come back up; a client
 * connected at fork time also has its socket held open (CLOSE_WAIT) after it
 * disconnects, since midi_tcp still references it. */
static void set_cloexec(int fd)
{
    if (fd >= 0)
        fcntl(fd, F_SETFD, FD_CLOEXEC);
}

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
    set_cloexec(fb1_fd);

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
    struct fb_var_screeninfo fvar;

    if (fb0_fd >= 0) return 0;

    fb0_fd = open(FB_DST, O_RDWR);
    if (fb0_fd < 0) { perror("open " FB_DST); return -1; }
    set_cloexec(fb0_fd);

    if (ioctl(fb0_fd, FBIOGET_FSCREENINFO, &ffix) < 0) {
        perror("fb0 FBIOGET_FSCREENINFO"); goto fail;
    }
    fb0_stride = ffix.line_length;

    /* do_mirror()/the screensaver blank write fb_w bytes/row for fb_h rows -
     * fb1's geometry, assumed to also hold for fb0 (VGA out) without ever
     * checking. If VGA is ever in a mode smaller than 800x600, fb0_stride*fb_h
     * exceeds what the device actually has mapped: an out-of-bounds mmap
     * write / SIGBUS the moment MIRROR_ON is enabled (found 2026-07-16). */
    if (ioctl(fb0_fd, FBIOGET_VSCREENINFO, &fvar) < 0) {
        perror("fb0 FBIOGET_VSCREENINFO"); goto fail;
    }
    if (fvar.xres < fb_w || fvar.yres < fb_h) {
        fprintf(stderr, "screenremote: fb0 is %ux%u, smaller than fb1's %ux%u - "
                "refusing to mirror (would write out of bounds)\n",
                fvar.xres, fvar.yres, fb_w, fb_h);
        goto fail;
    }

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
    /* stream and ctrl are both TCP listeners, so equal ports make the second
     * bind() fail and the daemon exit at startup with a config error.  Catch the
     * misconfiguration here and fall back to defaults so the unit still comes up.
     * (DISC_PORT is UDP, so it never collides with these TCP ports.) */
    if (g_ctrl_port == g_stream_port) {
        fprintf(stderr, "screenremote: ctrl_port == stream_port (%d) in config - "
                "invalid, reverting both to defaults %d/%d\n",
                g_stream_port, STREAM_PORT, CTRL_PORT);
        g_stream_port = STREAM_PORT;
        g_ctrl_port   = CTRL_PORT;
    }
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
    set_cloexec(fd);
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
    set_cloexec(fd);
    return fd;
}

/* Protocol */
/* Cork/uncork helper: batches header + body into one TCP burst. */
#define TCP_CORK_ON(fd)  do { int _c=1; setsockopt((fd),IPPROTO_TCP,TCP_CORK,&_c,sizeof(_c)); } while(0)
#define TCP_CORK_OFF(fd) do { int _c=0; setsockopt((fd),IPPROTO_TCP,TCP_CORK,&_c,sizeof(_c)); } while(0)

/* Little-endian header packing - the on-wire byte order for every length/geometry
 * field the streaming protocol emits.  Factored out so the three frame senders
 * below don't each open-code the same byte splat. */
static void put_le32(uint8_t *p, uint32_t v)
{
    p[0] = v         & 0xFF;
    p[1] = (v >>  8) & 0xFF;
    p[2] = (v >> 16) & 0xFF;
    p[3] = (v >> 24) & 0xFF;
}
static void put_le16(uint8_t *p, uint16_t v)
{
    p[0] = v        & 0xFF;
    p[1] = (v >> 8) & 0xFF;
}

/* Send a flat frame_bytes buffer preceded by a 4-byte LE length header.
 * Reads only from RAM (buf), not from device memory. */
static int send_frame_buf(int fd, const uint8_t *buf)
{
    uint8_t hdr[4];
    put_le32(hdr, frame_bytes);
    TCP_CORK_ON(fd);
    if (write_all(fd, hdr, 4) < 0 || write_all(fd, buf, frame_bytes) < 0) {
        TCP_CORK_OFF(fd);
        return -1;
    }
    TCP_CORK_OFF(fd);
    return 0;
}

/* Forward decl - defined near the boot-splash compositing code further
 * down (needs g_boot_active/g_boot_splash, declared later in the file);
 * send_frame() below is this file's other frame-send path and needs the
 * same "how many rows are splash right now" answer apply_boot_splash()
 * uses, so the two can't disagree. */
static uint32_t boot_splash_active_rows(void);

/* Pull mode: send directly from device memory fb1_map (no staging/shadow) -
 * this is KronosScreenRemote's own DEFAULT connection mode (AppSettings.cs
 * PullMode=true), so unlike capture_to_staging()'s change-mode path, this
 * one is what most clients actually see. While the boot gate is active and
 * a splash is loaded, its rows are written from g_boot_splash instead of
 * fb1_map - everything from there down is still a direct, uncopied read
 * from fb1_map, so this stays zero-copy for the overwhelmingly common case
 * (BOOT=0, splash_rows=0) and only pays an extra small write while the
 * gate is closed. Found missing 2026-07-19: an earlier version of the
 * splash compositing feature only patched capture_to_staging(), so change
 * mode composited correctly but pull mode - live-tested during a real
 * boot - still showed a plain black-and-text frame with no logo, since it
 * never went through staging/apply_boot_splash() at all. */
static int send_frame(int fd)
{
    uint8_t hdr[4];
    uint32_t y, splash_rows = boot_splash_active_rows();
    put_le32(hdr, frame_bytes);
    TCP_CORK_ON(fd);
    if (write_all(fd, hdr, 4) < 0) goto fail;
    if (splash_rows > 0 &&
            write_all(fd, g_boot_splash, (size_t)fb_w * splash_rows) < 0)
        goto fail;
    if (fb1_stride == fb_w) {
        if (write_all(fd, fb1_map + splash_rows * fb1_stride,
                       frame_bytes - splash_rows * fb_w) < 0)
            goto fail;
    } else {
        for (y = splash_rows; y < fb_h; y++)
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
    set_cloexec(vkbd_fd);
    if (vkbd_fd >= 0) {
        n = snprintf(buf, sizeof(buf), "%d %d\n", code, val);
        /* snprintf returns the would-be length; clamp to what actually fits so a
         * truncated format can never make write() read past buf.  (Callers bound
         * code/val today, but this keeps inject_key safe if reused.) */
        if (n < 0) return;
        if (n >= (int)sizeof(buf)) n = (int)sizeof(buf) - 1;
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
        set_cloexec(kbd_fd);

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

/* UI mode detection - server-side port of KronosScreenRemote's
 * Detection/ModeDetector.cs and Detection/CombiProgramEditDetector.cs, so the
 * daemon becomes the source of truth for MODE/EDITCTX instead of every client
 * re-deriving it from the streamed pixels on its own. Same reference pixel
 * sets (mode_detect_refs.h, extracted from the client's own embedded PNGs),
 * same per-channel tolerance and match-fraction thresholds - kept in lockstep
 * deliberately so daemon and client never disagree about what "Combi mode"
 * looks like.
 *
 * One deviation from the client: the client's LUT bakes in the user's
 * brightness/contrast/gamma tone curve (MainWindow.Streaming.cs RebuildLut),
 * which the daemon has no equivalent of. We score directly against pal_r/g/b
 * - the raw hardware palette also used by toggle_is_on() above - i.e. the
 * client's un-adjusted case. If a firmware/panel revision's native palette
 * drifts enough to matter, widen MODE_MATCH_TOLERANCE rather than trying to
 * reproduce the client's tone curve here. */
#define MODE_MATCH_TOLERANCE   30      /* +/- per channel, matches client's ColorTolerance */
#define MODE_MATCH_THRESHOLD   0.85    /* matches client's ModeDetector.ModeThreshold */
#define EDIT_MATCH_THRESHOLD   0.98    /* matches client's CombiProgramEditDetector.MatchThreshold */

static double mode_ref_score(const struct mode_px_ref *refs, int count)
{
    int i, matches = 0;
    if (!fb1_map || !refs || count <= 0)
        return 0.0;
    for (i = 0; i < count; i++) {
        const struct mode_px_ref *p = &refs[i];
        unsigned char idx;
        int lr, lg, lb;
        if (p->x >= fb_w || p->y >= fb_h)
            continue;
        idx = fb1_map[(int)p->y * (int)fb1_stride + (int)p->x];
        lr = pal_r[idx] >> 8;
        lg = pal_g[idx] >> 8;
        lb = pal_b[idx] >> 8;
        if (abs(lr - p->r) <= MODE_MATCH_TOLERANCE &&
                abs(lg - p->g) <= MODE_MATCH_TOLERANCE &&
                abs(lb - p->b) <= MODE_MATCH_TOLERANCE)
            matches++;
    }
    return (double)matches / (double)count;
}

/* Returns 1-7 (Setlist..Disk) for a confidently-matched mode banner, or 0 if
 * none scores above threshold (e.g. mid-transition, or a dialog covers the
 * top-left corner). Picks the single highest-scoring mode, same as the
 * client's ModeDetector.Identify(). */
static int detect_ui_mode(void)
{
    int m, best_mode = 0;
    double best_score = MODE_MATCH_THRESHOLD;
    for (m = 1; m <= 7; m++) {
        double s = mode_ref_score(g_mode_refs[m], g_mode_ref_counts[m]);
        if (s > best_score) { best_score = s; best_mode = m; }
    }
    return best_mode;
}

#define EDITCTX_NONE     0
#define EDITCTX_COMBI    1
#define EDITCTX_SEQUENCE 2

/* Program-edit-in-context sub-state: MODE=3 (Program) but the top-right
 * tempo-area widget at (696,39) is showing "COMBI" or "SEQ" instead of a
 * tempo, meaning this Program is being edited from within a Combi/Song
 * rather than standalone. See mode_detect_refs.h for why the Sequence case
 * has no reference data yet (client-side never implemented it either) - it
 * is wired up here so filling in g_seq_edit_ref later is a data-only change. */
static int detect_program_edit_context(void)
{
    if (mode_ref_score(g_combi_edit_ref, g_combi_edit_ref_count) >= EDIT_MATCH_THRESHOLD)
        return EDITCTX_COMBI;
    if (g_seq_edit_ref_count > 0 &&
            mode_ref_score(g_seq_edit_ref, g_seq_edit_ref_count) >= EDIT_MATCH_THRESHOLD)
        return EDITCTX_SEQUENCE;
    return EDITCTX_NONE;
}

/* Authoritative MODE= value: screen detection first, falling back to the
 * last BUTTON-commanded mode (g_mode) only while detection is inconclusive.
 * Keeps STATE/SYSINFO answering something sane through a brief transition
 * frame instead of dropping back to 0/init every time the banner redraws.
 *
 * This is now the FALLBACK path, used only when eva_mode.ko isn't loaded or
 * hasn't resolved yet - see get_mode_state() below, which prefers the
 * eva_mode.ko reading (exact, no thresholds) and only calls this when that
 * source is unavailable (e.g. early boot, before Eva has started). */
static int current_ui_mode(void)
{
    int m = detect_ui_mode();
    return m ? m : (int)g_mode;
}

/* eva_mode.ko's raw Eva ESysMode ordinal (0-6, Eva's own arbitrary C++ enum
 * declaration order - see docs/EVA_ModeManager_probe.md) translated to this
 * daemon's public MODE=1..7 wire numbering (see STATE's doc comment above
 * and docs/api.md). Calibrated live against pixel ground truth 2026-07-17,
 * every one of the 7 values independently confirmed. Index = raw SYS_MODE. */
static const int g_eva_sysmode_to_pub[7] = {
    3, /* 0 = Program  */
    2, /* 1 = Combi    */
    6, /* 2 = Global   */
    7, /* 3 = Disk     */
    4, /* 4 = Sequence */
    5, /* 5 = Sampling */
    1, /* 6 = Setlist  */
};

/* Reads eva_mode.ko's /proc/.eva_mode - a single line, read fresh every
 * call (no caching), same live-read philosophy as on_pads_page()/
 * toggle_is_on() against fb1_map. Returns 1 and fills *out_mode (public
 * 1-7 numbering, already translated) / *out_editctx (0-2, no translation
 * needed - eva_mode.ko's EDITCTX_RAW already matches this daemon's
 * EDITCTX_NONE/COMBI/SEQUENCE numbering) on a resolved reading; returns 0
 * (leaving both untouched) if eva_mode.ko isn't loaded, hasn't resolved yet
 * (Eva not up), or reports a raw SYS_MODE outside the calibrated 0-6 range.
 * out_slot is optional (pass NULL if the caller doesn't need it) - the
 * timbre/track index being Program-edited, -1 when EDITCTX is none.
 * out_pid is likewise optional - eva_mode.ko's EVA_PID, for callers that
 * want to check Eva's process age (see eva_uptime_seconds()) without a
 * second independent /proc scan. */
static int eva_mode_read(int *out_mode, int *out_editctx, int *out_slot, int *out_pid)
{
    int fd, resolved = 0, sysmode = -1, editctx = 0, slot = -1, pid = -1;
    char buf[128];
    ssize_t n;

    if (!g_eva_mode_loaded)
        return 0;
    fd = open("/proc/.eva_mode", O_RDONLY);
    if (fd < 0)
        return 0;
    n = read(fd, buf, sizeof(buf) - 1);
    close(fd);
    if (n <= 0)
        return 0;
    buf[n] = '\0';

    if (sscanf(buf, "RESOLVED=%d EVA_PID=%d SYS_MODE=%d EDITCTX_RAW=%d EDITCTX_SLOT=%d",
               &resolved, &pid, &sysmode, &editctx, &slot) != 5 || !resolved)
        return 0;
    if (sysmode < 0 || sysmode >= (int)(sizeof(g_eva_sysmode_to_pub) / sizeof(g_eva_sysmode_to_pub[0])))
        return 0;
    /* editctx gets the same treatment as sysmode above: out-of-range means
     * something about the pointer chain read garbage, so treat the WHOLE
     * reading as unresolved (both fields fall back to pixel detection
     * together in get_mode_state()) rather than passing through a half-
     * trusted value alongside a validated mode - "only lever pixel
     * detection as a fallback if EVA fails for any reason" applies to a bad
     * editctx exactly as much as a bad sysmode. */
    if (editctx < EDITCTX_NONE || editctx > EDITCTX_SEQUENCE)
        return 0;

    *out_mode    = g_eva_sysmode_to_pub[sysmode];
    *out_editctx = editctx;
    if (out_slot)
        *out_slot = slot;
    if (out_pid)
        *out_pid = pid;
    return 1;
}

/* Single source of truth for MODE=/EDITCTX= everywhere they're reported
 * (STATE, SYSINFO, MODE_DETAIL): prefers eva_mode.ko's direct memory read
 * (exact - no color thresholds, and the only source that can ever report
 * EDITCTX_SEQUENCE, since no pixel reference bitmap exists for it - see
 * mode_detect_refs.h), falling back to framebuffer pixel detection when
 * eva_mode.ko isn't loaded or hasn't resolved (e.g. early boot before Eva
 * has started, or the kill-switch at /korg/rw/HD/_nomod prevented it from
 * loading at all). *out_source is "eva" or "pixel", for MODE_DETAIL. */
static void get_mode_state(int *out_mode, int *out_editctx, const char **out_source)
{
    if (eva_mode_read(out_mode, out_editctx, NULL, NULL)) {
        *out_source = "eva";
        return;
    }
    *out_mode    = current_ui_mode();
    *out_editctx = detect_program_edit_context();
    *out_source  = "pixel";
}

/* ── Boot-state gate ──────────────────────────────────────────────────────
 * Goal: never let a client send an interactive command (touch/button/wheel/
 * MIDI/etc.) while the Kronos OS is still booting - only the read-only
 * allowlist (see is_readonly_cmd()) is answered during that window, and the
 * decision is made entirely server-side (a client, especially one that just
 * connected, has no way to tell "still booting" from "genuinely idle" on its
 * own - that determination belongs to the daemon, not something every client
 * has to re-derive).
 *
 * Root cause this defends against: eva_mode.ko's RESOLVED=1 only means the
 * sm_poMMI->CMMI::modeManager pointer chain didn't hit NULL/out-of-bounds -
 * it does NOT mean the CModeManager C++ object has finished constructing.
 * Freshly-allocated-but-not-yet-constructed heap memory reads back as small
 * ints, and SYS_MODE=0/EDITCTX_RAW=1 (-> MODE=3 EDITCTX=1, "Program edit
 * while in Combi") is exactly what that looks like - a false-confident
 * reading, not a real one. See docs/EVA_ModeManager_probe.md.
 *
 * Two independent, layered signals clear the gate; either is sufficient:
 *
 *   1. Debounced EVA_RESOLVED - eva_mode.ko must report RESOLVED=1 with the
 *      SAME (SYS_MODE, EDITCTX_RAW) pair continuously for EVA_DEBOUNCE_MS.
 *      A transient/garbage reading restarts the streak on every value
 *      change, so real construction-in-progress churn is filtered before
 *      the gate trusts it. Fast (typically clears within a second or two of
 *      Eva actually finishing init) but not airtight - see the header
 *      comment for the residual risk (static garbage that happens to read
 *      the same value across the whole window).
 *
 *   2. Eva process-age override - independent of eva_mode.ko entirely (a
 *      pure /proc scan, same "lowest PID" tie-break eva_mode.ko itself uses
 *      to dodge the documented transient-same-named-process bug - see
 *      docs/EVA_ModeManager_probe.md's "gotcha" section). Once the real Eva
 *      process has been alive for EVA_BOOT_UPTIME_OVERRIDE_S, force the gate
 *      open regardless of (1) - by then any construction-in-progress window
 *      is certainly long past. Also the ONLY path that can ever clear the
 *      gate when eva_mode.ko isn't loaded at all (kill-switch, or a load
 *      failure) - without it, a unit missing eva_mode.ko would stay
 *      permanently read-only, which would be a regression, not a safety
 *      win.
 *
 * g_boot_active is a one-way latch: once cleared it stays cleared for the
 * rest of this process's life. The OS doesn't "re-boot" without restarting
 * screenremote itself (which resets every global fresh), so there is no
 * legitimate reason for a mode/page change during normal operation to ever
 * re-arm it. */
#define EVA_DEBOUNCE_MS            600   /* min continuous stable RESOLVED=1 streak before trusted */
#define EVA_CONFIRM_DELAY_MS       500   /* extra settle time + single re-check once debounce first passes */
#define EVA_BOOT_UPTIME_OVERRIDE_S 180   /* Eva alive this long => gate opens unconditionally */

static int g_boot_active = 1;   /* fail-safe: read-only until proven otherwise */

/* Debounce streak state for signal (1) above. */
static int             g_eva_streak_mode    = -1;
static int             g_eva_streak_editctx = -1;
static int             g_eva_streak_active  = 0;
static struct timespec g_eva_streak_since;

/* Scans /proc for the lowest-PID process with comm=="Eva" - same tie-break
 * eva_mode.ko's find_eva_mm() uses, for the same reason (a transient
 * same-named forked child, e.g. from a failed execl() in one of Eva's own
 * USB/disk housekeeping helpers, always has a strictly higher PID than the
 * real long-lived UI process - see docs/EVA_ModeManager_probe.md). Returns
 * -1 if no such process exists yet (Eva hasn't been exec'd, i.e. very early
 * boot). Deliberately independent of eva_mode.ko - this is the fallback
 * path used when that module isn't loaded at all.
 *
 * Reads comm out of /proc/<pid>/stat's parenthesized field 2, NOT
 * /proc/<pid>/comm - that file doesn't exist on this kernel (added in
 * Linux 2.6.33; the Kronos runs 2.6.32.11-korg, see CLAUDE.md). Matches
 * the FIRST '(' to the LAST ')' rather than splitting on whitespace, since
 * comm can itself contain spaces/parens - same technique eva_uptime_seconds()
 * below uses for the fields *after* comm, and what proc(5) itself
 * recommends. */
static int find_eva_pid(void)
{
    DIR *d = opendir("/proc");
    struct dirent *de;
    int best = -1;

    if (!d)
        return -1;
    while ((de = readdir(d)) != NULL) {
        int pid;
        char path[64], buf[256], *lp, *rp;
        FILE *f;

        if (de->d_name[0] < '0' || de->d_name[0] > '9')
            continue;
        pid = atoi(de->d_name);
        if (best >= 0 && pid >= best)
            continue;   /* already have a lower candidate */
        snprintf(path, sizeof(path), "/proc/%d/stat", pid);
        f = fopen(path, "r");
        if (!f)
            continue;
        buf[0] = '\0';
        fgets(buf, sizeof(buf), f);
        fclose(f);
        lp = strchr(buf, '(');
        rp = strrchr(buf, ')');
        if (!lp || !rp || rp <= lp)
            continue;
        if ((size_t)(rp - lp - 1) == 3 && strncmp(lp + 1, "Eva", 3) == 0)
            best = pid;
    }
    closedir(d);
    return best;
}

/* Seconds the given pid has been alive, via /proc/<pid>/stat field 22
 * (starttime, in USER_HZ ticks since boot per proc(5) - sysconf(_SC_CLK_TCK)
 * is always 100 on Linux regardless of the kernel's internal HZ, so no
 * kernel-version-specific constant is needed) against /proc/uptime. Returns
 * -1 if the process doesn't exist or either file can't be read/parsed. */
static long eva_uptime_seconds(int pid)
{
    char path[64], buf[512];
    FILE *f;
    ssize_t n;
    int fd;
    char *rparen;
    unsigned long long starttime = 0;
    double sys_uptime = 0;
    long clk_tck;
    int i;
    char *p;

    if (pid <= 0)
        return -1;

    snprintf(path, sizeof(path), "/proc/%d/stat", pid);
    fd = open(path, O_RDONLY);
    if (fd < 0)
        return -1;
    n = read(fd, buf, sizeof(buf) - 1);
    close(fd);
    if (n <= 0)
        return -1;
    buf[n] = '\0';

    /* comm (field 2) is inside parens and may itself contain spaces/parens -
     * skip to the LAST ')' before splitting on whitespace, same technique
     * proc(5) itself recommends. */
    rparen = strrchr(buf, ')');
    if (!rparen)
        return -1;
    p = rparen + 1;
    /* Fields after comm, 1-indexed from field 3 (state) at *p: starttime is
     * field 22, i.e. the 20th whitespace-separated token starting from p. */
    for (i = 3; i < 22; i++) {
        p = strchr(p, ' ');
        if (!p)
            return -1;
        p++;
    }
    if (sscanf(p, "%llu", &starttime) != 1)
        return -1;

    f = fopen("/proc/uptime", "r");
    if (!f)
        return -1;
    if (fscanf(f, "%lf", &sys_uptime) != 1) {
        fclose(f);
        return -1;
    }
    fclose(f);

    clk_tck = sysconf(_SC_CLK_TCK);
    if (clk_tck <= 0)
        clk_tck = 100;

    {
        double eva_start_s = (double)starttime / (double)clk_tck;
        double up = sys_uptime - eva_start_s;
        return (up < 0) ? 0 : (long)up;
    }
}

/* Called once per main-loop iteration (see main()) - cheap once latched
 * (g_boot_active == 0 short-circuits immediately), so no throttling needed
 * even though this runs far more often than eva_mode.ko's own /proc/.eva_mode
 * changes. See the block comment above for the two signals and why either
 * clears the gate. */
static void update_boot_state(void)
{
    int mode = 0, editctx = 0, pid = -1, resolved;
    struct timespec now;

    if (!g_boot_active)
        return;   /* one-way latch - nothing left to do */

    resolved = eva_mode_read(&mode, &editctx, NULL, &pid);
    clock_gettime(CLOCK_MONOTONIC, &now);

    if (resolved) {
        if (!g_eva_streak_active || mode != g_eva_streak_mode || editctx != g_eva_streak_editctx) {
            g_eva_streak_active  = 1;
            g_eva_streak_mode    = mode;
            g_eva_streak_editctx = editctx;
            g_eva_streak_since   = now;
        } else {
            long elapsed_ms = (now.tv_sec  - g_eva_streak_since.tv_sec)  * 1000 +
                               (now.tv_nsec - g_eva_streak_since.tv_nsec) / 1000000;
            if (elapsed_ms >= EVA_DEBOUNCE_MS) {
                /* Debounce just passed - one more deliberate pause-and-recheck
                 * before committing, on top of the debounce window itself.
                 * Blocks the main loop for EVA_CONFIRM_DELAY_MS, but this
                 * fires exactly once per boot and nothing interactive is
                 * being served yet anyway (that's the whole point of the
                 * gate still being closed) - a one-time, boot-only stall,
                 * not a recurring cost. */
                int cmode = 0, ceditctx = 0, cresolved;
                fprintf(stderr, "screenremote: boot gate: eva_mode.ko debounced stable "
                                 "(MODE=%d EDITCTX=%d), confirming after %dms...\n",
                                 mode, editctx, EVA_CONFIRM_DELAY_MS);
                usleep(EVA_CONFIRM_DELAY_MS * 1000);
                cresolved = eva_mode_read(&cmode, &ceditctx, NULL, NULL);
                if (cresolved && cmode == mode && ceditctx == editctx) {
                    g_boot_active = 0;
                    free(g_boot_splash); g_boot_splash = NULL;   /* no longer needed - see apply_boot_splash() */
                    fprintf(stderr, "screenremote: boot gate cleared (eva_mode.ko confirmed stable, "
                                     "MODE=%d EDITCTX=%d)\n", mode, editctx);
                    return;
                }
                fprintf(stderr, "screenremote: boot gate: confirmation check failed (RESOLVED=%d "
                                 "MODE=%d EDITCTX=%d) - resetting streak\n", cresolved, cmode, ceditctx);
                g_eva_streak_active = 0;
            }
        }
    } else {
        g_eva_streak_active = 0;
    }

    /* Fallback/safety-net signal - independent of eva_mode.ko and of the
     * debounce state above. */
    if (pid < 0)
        pid = find_eva_pid();
    if (pid > 0) {
        long up = eva_uptime_seconds(pid);
        if (up >= EVA_BOOT_UPTIME_OVERRIDE_S) {
            g_boot_active = 0;
            free(g_boot_splash); g_boot_splash = NULL;
            fprintf(stderr, "screenremote: boot gate cleared (Eva pid=%d alive %lds)\n", pid, up);
        }
    }
}

/* Commands answered regardless of ctrl ownership AND regardless of the boot
 * gate above - see docs/api.md's "Read-only allowlist exception". Shared by
 * the ctrl-accept path (ownership bypass) and process_ctrl_cmd() (boot-gate
 * bypass) so the two can never drift apart. */
static int is_readonly_cmd(const char *line)
{
    return strcmp(line, "LASTTOUCH") == 0 ||
           strcmp(line, "PADMAP_LIST") == 0 ||
           strcmp(line, "PADMAP_STATE") == 0 ||
           strncmp(line, "PIXEL ", 6) == 0 ||
           strncmp(line, "REGION ", 7) == 0 ||
           strcmp(line, "PALETTE") == 0 ||
           strcmp(line, "STATE") == 0 ||
           strcmp(line, "MODE_DETAIL") == 0 ||
           strcmp(line, "VERSION") == 0 ||
           strcmp(line, "SYSINFO") == 0;
}

/* ── Boot splash compositing ──────────────────────────────────────────────
 * During boot, /dev/fb1 itself only ever contains the green footer band's
 * loading text (rows ~527-540) - the KORG wordmark, engine badges, and
 * "KRONOS / MUSIC WORKSTATION" title lockup above it are rendered by the
 * touch panel's OWN separate firmware/MCU directly onto the LCD, never
 * written to fb1 at all (confirmed against OmapNKS4Module's video.cpp -
 * SendFillData, the panel-local progress-bar opcode, never carries pixel
 * data, and SendPixelDataRegion, the only opcode that does, only ever
 * forwards whatever fb1 actually contains). So a client watching the raw
 * fb1 stream during boot sees only scattered text on an otherwise-black
 * frame - nothing like what's really on the physical screen.
 *
 * This composites a static copy of that background (extracted once, by
 * hand, from a local KRONOS_Vxxxxx.VSB firmware image via
 * tools/extract_boot_splash.py - see that script's own header for why it's
 * neither run at build time nor committed anywhere: the bitmap is Korg's
 * copyrighted artwork, not a protocol fact, matching kronosology's own
 * stance in docs/modules/KRONOS_V06R06.VSB.md) under the live fb1 capture's
 * top rows (row count comes from the asset file's own header, extracted as
 * ~526 - up to but not including the green footer band), leaving every row
 * from there down (the footer band and anything else) exactly as fb1
 * itself reports it. Purely
 * cosmetic - has no bearing on the BOOT= gate itself (see
 * update_boot_state() above), which is what actually decides whether
 * commands are accepted; this only affects what the video stream looks
 * like while that gate is closed. */
#define BOOT_SPLASH_PATH  LOG_DIR "/boot_splash.bin"   /* FTP-visible - user deploys by hand, see docs/api.md */
#define BOOT_SPLASH_MAGIC "KSPL"
#define BOOT_SPLASH_VERSION 1
#define BOOT_SPLASH_MAX_ROWS 600   /* sanity cap - never more than a full frame */

/* Validates a complete in-memory boot-splash asset - KSPL header followed
 * immediately by its pixel bytes, exactly BOOT_SPLASH_PATH's own on-disk
 * layout (see docs/api.md's "Boot splash" section) - and, if it checks
 * out, mallocs a copy of just the pixel portion into g_boot_splash/_w/
 * _rows. Shared by both sources load_boot_splash() below tries (the
 * on-device file, and the optional compile-time embedded fallback) so
 * validation can never drift between the two - a caller only needs to
 * hand it bytes+length, regardless of where they came from. `source` is
 * just for the log line. Returns 1 on success, 0 on any rejection
 * (already logged to stderr). */
static int parse_boot_splash_buf(const uint8_t *buf, size_t len, const char *source)
{
    uint32_t w, rows, pixels;
    uint8_t *copy;

    if (len < 9) {
        fprintf(stderr, "screenremote: %s too short for a boot-splash header, ignoring\n", source);
        return 0;
    }
    if (memcmp(buf, BOOT_SPLASH_MAGIC, 4) != 0 || buf[4] != BOOT_SPLASH_VERSION) {
        fprintf(stderr, "screenremote: %s has wrong magic/version, ignoring\n", source);
        return 0;
    }
    w    = buf[5] | (buf[6] << 8);
    rows = buf[7] | (buf[8] << 8);
    if (w != fb_w || rows == 0 || rows > BOOT_SPLASH_MAX_ROWS || rows > fb_h) {
        fprintf(stderr, "screenremote: %s dimensions %ux%u don't match fb1 %ux%u, ignoring\n",
                source, w, rows, fb_w, fb_h);
        return 0;
    }
    pixels = w * rows;
    if (len != 9 + (size_t)pixels) {
        fprintf(stderr, "screenremote: %s size doesn't match its own header, ignoring\n", source);
        return 0;
    }
    copy = malloc(pixels);
    if (!copy)
        return 0;
    memcpy(copy, buf + 9, pixels);

    g_boot_splash      = copy;
    g_boot_splash_w    = w;
    g_boot_splash_rows = rows;
    fprintf(stderr, "screenremote: boot splash loaded from %s (%ux%u)\n", source, w, rows);
    return 1;
}

/* Called once at startup, after fb1_open() has established fb_w/fb_h.
 * Prefers the on-device file (BOOT_SPLASH_PATH) so it can be swapped out
 * live during calibration without a daemon rebuild; falls back to the
 * compile-time embedded copy (if this build has one - see
 * HAVE_EMBEDDED_BOOT_SPLASH above) when that file is missing or fails
 * validation. Neither source available is not an error - compositing
 * simply stays off and the client sees the plain (mostly-black) live fb1
 * capture during boot, same as before this feature existed. */
static void load_boot_splash(void)
{
    int fd;
    struct stat st;
    uint8_t *filebuf;

    fd = open(BOOT_SPLASH_PATH, O_RDONLY);
    if (fd >= 0) {
        if (fstat(fd, &st) == 0 && st.st_size > 0 && st.st_size <= 8 * 1024 * 1024) {
            filebuf = malloc((size_t)st.st_size);
            if (filebuf) {
                if (read(fd, filebuf, (size_t)st.st_size) == st.st_size &&
                        parse_boot_splash_buf(filebuf, (size_t)st.st_size, BOOT_SPLASH_PATH)) {
                    free(filebuf);
                    close(fd);
                    return;
                }
                free(filebuf);
            }
        }
        close(fd);
    }

#ifdef HAVE_EMBEDDED_BOOT_SPLASH
    parse_boot_splash_buf(boot_splash_data, boot_splash_data_len, "<embedded boot_splash_data.h>");
#endif
}

/* How many leading rows of an outgoing frame should come from the boot
 * splash instead of live fb1 right now - shared by every frame-send path
 * (capture_to_staging()'s change-mode compositor below, AND send_frame()'s
 * pull-mode direct sender - see its own comment for why it needs this too)
 * so none of them can independently disagree about whether/how much to
 * substitute. 0 means "use fb1 for every row", the steady-state case once
 * the boot gate clears. Never touches fb1_map itself - eva_mode.ko/
 * pixel-based mode detection (both of which read fb1_map directly) keep
 * seeing the real, truthful capture regardless of what any outgoing
 * stream shows. */
static uint32_t boot_splash_active_rows(void)
{
    if (!g_boot_active || !g_boot_splash || g_boot_splash_w != fb_w)
        return 0;
    return g_boot_splash_rows;
}

/* Composites the loaded splash (if any) into the top rows of an already-
 * fb1-filled staging buffer. Called from capture_to_staging() only. */
static void apply_boot_splash(uint8_t *buf)
{
    uint32_t rows = boot_splash_active_rows();
    if (rows)
        memcpy(buf, g_boot_splash, (size_t)g_boot_splash_w * rows);
}

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

/* rtf5 fallback wire writers - see the header comment's "rtf5 fallback" section and
 * rtf5_btn_table[] above.  Only ever called once g_rtf5_fallback_active is set; touch_fd
 * is opened lazily on first use (mirrors nks4_fd's own open-on-demand pattern) and left
 * open across calls, since /dev/rtf5 may not exist yet the first time this fires. */
static void send_rtf5_event(uint32_t dev, uint32_t code, uint32_t value)
{
    if (touch_fd < 0)
        touch_fd = open("/dev/rtf5", O_WRONLY);
    set_cloexec(touch_fd);
    if (touch_fd >= 0) {
        uint32_t pkt[5] = { 0x00010014u, 0u, dev, code, value };
        (void)write(touch_fd, pkt, 20);
    }
}

/* WHEEL's rtf5 packet is a distinct 16-byte/4-field shape (device fixed at 0x0d, no
 * separate value field) - not expressible through send_rtf5_event()'s 5-field layout. */
static void send_rtf5_wheel(uint32_t field3)
{
    if (touch_fd < 0)
        touch_fd = open("/dev/rtf5", O_WRONLY);
    set_cloexec(touch_fd);
    if (touch_fd >= 0) {
        uint32_t pkt[4] = { 0x00010010u, 0u, 0x0000000du, field3 };
        (void)write(touch_fd, pkt, 16);
    }
}

/* Called once, the moment nks4_inject.ko is permanently given up on for this boot (see
 * try_load_nks4_inject()'s -1 return and the main loop's retry give-up paths).  Flips on
 * the rtf5 fallback and makes the degradation impossible to miss: logged to stderr AND to
 * an FTP-visible, append-across-boots log file, since a silently-degraded unit (buttons
 * appear to work but sequencer transport/tempo quietly don't - see header comment) is far
 * worse than a loud one. */
static void nks4_give_up(const char *reason)
{
    time_t t = time(NULL);
    char ts[32];
    struct tm *ti = localtime(&t);
    FILE *f;

    if (g_rtf5_fallback_active) return;   /* already logged/active - don't double-log */
    g_rtf5_fallback_active = 1;

    strftime(ts, sizeof(ts), "%Y-%m-%d %H:%M:%S", ti);
    f = fopen(RTF5_FALLBACK_LOG, "a");
    if (f) {
        fprintf(f, "%s  nks4_inject unavailable (%s) - DEGRADED: falling back to rtf5 "
                "front-panel injection for this boot (BUTTON/CHORD/TOUCH/WHEEL/SLIDER/"
                "VSLIDER only; sequencer transport, TAP_TEMPO, and sampling triggers will "
                "silently no-op - see screenremote.c header comment)\n", ts, reason);
        fclose(f);
    }
    fprintf(stderr, "screenremote: nks4_inject unavailable (%s) - DEGRADED: falling back "
            "to rtf5 injection for this boot (see %s)\n", reason, RTF5_FALLBACK_LOG);
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
                /* A second pen-down landing on a different pad before the
                 * matching pen-up for the first one (no TOUCH_UP in between)
                 * used to overwrite g_active_pad here, stranding the first
                 * pad's chord with no Note-Off - an audible stuck chord
                 * recoverable only by a panic/power-cycle (found 2026-07-16).
                 * Release whatever's already held before starting the new one. */
                if (g_active_pad != -1 && g_active_pad != pad)
                    send_padchord(g_active_pad, 0);
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
    if (g_nks4_loaded)
        nks4_write("TOUCH %d %u\n", type, v_adc | (h_adc << 8u));
    else if (g_rtf5_fallback_active)
        send_rtf5_event(0x11u, (uint32_t)type, v_adc | (h_adc << 8u));
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

    /* Mode - forced to the safe 0/0 "unknown"/"none" sentinels during boot,
     * same reasoning as STATE/MODE_DETAIL's own boot handling. */
    {
        int _mode, _editctx; const char *_src;
        get_mode_state(&_mode, &_editctx, &_src);
        if (g_boot_active) { _mode = 0; _editctx = 0; }
        SI_APPEND("MODE=%u\n", (unsigned)_mode);
        SI_APPEND("EDITCTX=%d\n", _editctx);
    }

    /* Boot gate - see update_boot_state() */
    SI_APPEND("BOOT=%d\n", g_boot_active);

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

/* Persists across wait_for_oa_live() calls so a multi-second retry loop (nks4_inject's
 * deferred retry - see main()) mounts this AT MOST ONCE per boot, not once per poll.
 * wait_for_oa_live() used to mount+umount unconditionally on every single call; that was
 * harmless when it was only ever called once (the old fixed 5 s wait), but became up to
 * ~120 mount()+umount()+mkdir()+rmdir() cycles - real syscall/mount-table churn during the
 * most fragile part of boot - once the deferred retry started calling it every second for
 * up to NKS4_LOAD_DEADLINE_S.  See oa_sys_priv_cleanup(). */
static int g_oa_sys_priv_mounted = 0;

/* Unmount/remove the private sysfs mount, if one of ours is active.  Called from
 * wait_for_oa_live() itself the moment the real /sys shows up (self-heal - typically once
 * /sbin/init finishes its own mounts) and once more from main()'s shutdown path as a
 * backstop.  Never called from inside the retry hot path. */
static void oa_sys_priv_cleanup(void)
{
    if (!g_oa_sys_priv_mounted) return;
    umount(OA_SYS_PRIV);
    rmdir(OA_SYS_PRIV);
    g_oa_sys_priv_mounted = 0;
}

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
 * collides with the system /sys mount) - mounted at most once per boot and left
 * mounted across calls (see g_oa_sys_priv_mounted above), not remounted per poll.
 * Returns 1 once OA is live, 0 on timeout.  Polls in 100 ms steps. */
static int wait_for_oa_live(int max_deciseconds)
{
    const char *path = "/sys/module/OA/initstate";
    char privpath[256];
    int i, result = 0;

    /* Prefer the system /sys the moment it's available - self-heals off the private
     * mount (cheap access() check every call; the mount/unmount itself only ever runs
     * once, on the actual transition). Otherwise reuse our own if already mounted, or
     * mount it once if this is the first call to ever need it. */
    if (access("/sys/module", F_OK) == 0) {
        oa_sys_priv_cleanup();
    } else if (!g_oa_sys_priv_mounted) {
        mkdir(OA_SYS_PRIV, 0700);
        if (mount("sysfs", OA_SYS_PRIV, "sysfs", 0, NULL) == 0)
            g_oa_sys_priv_mounted = 1;
    }
    if (g_oa_sys_priv_mounted) {
        snprintf(privpath, sizeof(privpath), "%s/module/OA/initstate", OA_SYS_PRIV);
        path = privpath;
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

    return result;
}


static int hex_decode(const char *hex, uint8_t *out, int maxlen)
{
    int len = 0;
    while (*hex && len < maxlen) {
        while (*hex == ' ') hex++;
        if (!*hex) break;
        unsigned int b;
        int consumed = 0;
        /* %n records how many chars %2x actually consumed (1 for a lone final
         * hex nibble, 2 normally) - advancing by a hardcoded 2 regardless steps
         * past the string's NUL on an odd-length tail, reading whatever
         * garbage/stale bytes follow in the caller's buffer (network-reachable
         * via MIDI_SEND/SYSEX) and feeding them into the decoded MIDI output;
         * at a buffer's exact end this is a one-past-the-end OOB read
         * (found 2026-07-16). */
        if (sscanf(hex, "%2x%n", &b, &consumed) != 1 || consumed <= 0)
            return -1;
        out[len++] = (uint8_t)b;
        hex += consumed;
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
            set_cloexec(fd);
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

/* The midi_tcp helper (our MIDI-capture side) has gone away - EOF or a hard
 * error on midi_cap_fd.  Tear the capture side down cleanly:
 *   - a permanently-readable dead fd left in the select set spins the main loop
 *     at 100% CPU (select() returns immediately on EOF every iteration), so the
 *     fd MUST leave the set - close it and drop midi_cap_fd to -1;
 *   - any in-flight SysEx capture can never complete now, so finish it so the
 *     waiting client gets its captured-so-far bytes or ERR TIMEOUT instead of
 *     hanging until its own socket timeout;
 *   - reap the child so it doesn't linger as a zombie.
 * MIDI *injection* (midi_in_fd -> /proc/.midi_in) is a wholly separate path and
 * keeps working; SYSEX/capture just report unavailable (ERR SYSEX_FAIL, and
 * MIDI_STATUS's MIDI_CAPTURE=0) until the daemon is restarted. */
static void midi_capture_down(void)
{
    if (sysex_pending)
        sysex_finish();
    if (midi_cap_fd >= 0) { close(midi_cap_fd); midi_cap_fd = -1; }
    if (midi_cap_pid > 0) {
        kill(midi_cap_pid, SIGTERM);
        waitpid(midi_cap_pid, NULL, 0);
        midi_cap_pid = -1;
    }
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
        for (;;) {
            n = recv(midi_cap_fd, tmp, sizeof(tmp), MSG_DONTWAIT);
            if (n == 0) {
                /* midi_tcp closed the socket - the helper died.  Without this,
                 * the dead fd stays select()-readable forever and spins the
                 * main loop at 100% CPU.  Tear capture down and stop here. */
                midi_capture_down();
                return;
            }
            if (n < 0) {
                if (errno == EAGAIN || errno == EWOULDBLOCK)
                    break;                 /* nothing left to read this pass */
                if (errno == EINTR)
                    continue;              /* transient - retry the read */
                midi_capture_down();       /* real error - treat as helper gone */
                return;
            }
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

    /* Hard read-only enforcement during boot (see update_boot_state()'s block
     * comment) - every mutating command is rejected here, uniformly, whether
     * it arrived through the one-shot accept path or an already-established
     * CTRL_PERSIST session. The read-only allowlist itself still answers
     * normally (STATE/MODE_DETAIL/SYSINFO in particular are how a client
     * finds out BOOT= cleared). */
    if (g_boot_active && !is_readonly_cmd(line)) {
        REPLY("ERR BOOTING\n", 12);
        return;
    }

    /* The rest of the read-only allowlist doesn't need to stay answerable
     * during boot the way STATE/MODE_DETAIL/SYSINFO do (those three are
     * how a client polls for BOOT to clear, and null out their synth-state
     * fields individually instead - see their own handlers below).
     * LASTTOUCH/PADMAP_LIST/PADMAP_STATE/PIXEL/REGION all report on touch/
     * pad/pixel activity that's either meaningless (no legitimate touch
     * input exists while BOOT=1, since that's mutating and already
     * rejected above) or, for PIXEL/REGION, could otherwise be read as a
     * claim about live UI content that isn't trustworthy yet. PALETTE and
     * VERSION are deliberately exempt - PALETTE is a fixed lookup table a
     * client needs just to decode the video stream at all (boot splash
     * included), and VERSION is static build metadata, neither is synth
     * state. */
    if (g_boot_active &&
            (strcmp(line, "LASTTOUCH") == 0 ||
             strcmp(line, "PADMAP_LIST") == 0 ||
             strcmp(line, "PADMAP_STATE") == 0 ||
             strncmp(line, "PIXEL ", 6) == 0 ||
             strncmp(line, "REGION ", 7) == 0)) {
        REPLY("ERR BOOTING\n", 12);
        return;
    }

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
        if (!g_nks4_loaded && !g_rtf5_fallback_active) { REPLY("ERR NKS4_NOT_LOADED\n", 20); return; }
        if (sscanf(line + 6, "%d %d", &x, &y) == 2) {
            inject_touch(1, x, y);  /* pen-down */
            inject_touch(2, x, y);  /* pen-up */
            REPLY("OK\n", 3);
        } else {
            REPLY("ERR\n", 4);
        }

    } else if (strncmp(line, "TOUCH_DOWN ", 11) == 0) {
        int x = 0, y = 0;
        if (!g_nks4_loaded && !g_rtf5_fallback_active) { REPLY("ERR NKS4_NOT_LOADED\n", 20); return; }
        if (sscanf(line + 11, "%d %d", &x, &y) == 2) {
            inject_touch(1, x, y);
            REPLY("OK\n", 3);
        } else {
            REPLY("ERR\n", 4);
        }

    } else if (strncmp(line, "TOUCH_MOVE ", 11) == 0) {
        int x = 0, y = 0;
        if (!g_nks4_loaded && !g_rtf5_fallback_active) { REPLY("ERR NKS4_NOT_LOADED\n", 20); return; }
        if (sscanf(line + 11, "%d %d", &x, &y) == 2) {
            inject_touch(3, x, y);
            REPLY("OK\n", 3);
        } else {
            REPLY("ERR\n", 4);
        }

    } else if (strncmp(line, "TOUCH_UP ", 9) == 0) {
        int x = 0, y = 0;
        if (!g_nks4_loaded && !g_rtf5_fallback_active) { REPLY("ERR NKS4_NOT_LOADED\n", 20); return; }
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
        if (!g_nks4_loaded) { REPLY("ERR NKS4_NOT_LOADED\n", 20); return; }
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
        /* Release a held chord before dropping it, not after - clearing
         * g_active_pad here without a Note-Off used to strand a sounding
         * chord with no way to recover it (found 2026-07-16). */
        if (g_active_pad != -1) {
            send_padchord(g_active_pad, 0);
            g_active_pad = -1;
        }
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
        if (!g_nks4_loaded && !g_rtf5_fallback_active) { REPLY("ERR NKS4_NOT_LOADED\n", 20); return; }
        for (b = btn_table; b->name; b++) {
            if (strcmp(bname, b->name) == 0) break;
        }
        /* Unknown button name: nothing sensible to snap to, so reject outright
         * rather than silently pick some other button. */
        if (!b->name) {
            REPLY("ERR\n", 4);
        } else if (g_nks4_loaded) {
            nks4_write("BTN %u\n", b->code);
            mode_from_btn(b->code);
            REPLY("OK\n", 3);
        } else {
            /* rtf5 fallback: the name exists in btn_table[] (current NKS4 code
             * space) but may not exist in rtf5_btn_table[] (older, smaller code
             * space) - see the header comment and rtf5_btn_table's own comment. */
            const struct rtf5_btn_def *rb = rtf5_find_btn(bname);
            if (!rb) {
                REPLY("ERR RTF5_UNSUPPORTED\n", 21);
            } else {
                send_rtf5_event(rb->dev, rb->code, 0x7fu);
                send_rtf5_event(rb->dev, rb->code, 0x00u);
                mode_from_btn(b->code);
                REPLY("OK\n", 3);
            }
        }

    } else if (strncmp(line, "CHORD ", 6) == 0) {
        char names[8][16];
        const struct btn_def *btns[8];
        const struct rtf5_btn_def *rbtns[8];
        int count = 0, hold_ms = 0;
        const char *p = line + 6;
        if (!g_nks4_loaded && !g_rtf5_fallback_active) { REPLY("ERR NKS4_NOT_LOADED\n", 20); return; }
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
            rbtns[count] = rtf5_find_btn(names[count]);
            count++;
        }
        if (count >= 2) {
            int ok = 1;
            for (int i = 0; i < count; i++) {
                if (g_nks4_loaded ? !btns[i] : !rbtns[i]) { ok = 0; break; }
            }
            if (ok) {
                if (g_nks4_loaded) {
                    for (int i = 0; i < count; i++)
                        nks4_write("BTN_DOWN %u\n", btns[i]->code);
                    if (hold_ms > 0) usleep(hold_ms * 1000);
                    for (int i = count - 1; i >= 0; i--)
                        nks4_write("BTN_UP %u\n", btns[i]->code);
                } else {
                    for (int i = 0; i < count; i++)
                        send_rtf5_event(rbtns[i]->dev, rbtns[i]->code, 0x7fu);
                    if (hold_ms > 0) usleep(hold_ms * 1000);
                    for (int i = count - 1; i >= 0; i--)
                        send_rtf5_event(rbtns[i]->dev, rbtns[i]->code, 0x00u);
                }
                REPLY("OK\n", 3);
            } else {
                REPLY("ERR\n", 4);   /* one or more unknown/unsupported button names */
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
        if (!g_nks4_loaded && !g_rtf5_fallback_active) { REPLY("ERR NKS4_NOT_LOADED\n", 20); return; }
        if (strcmp(dir, "CW") == 0)       delta = 0x00000100;
        else if (strcmp(dir, "CCW") == 0) delta = 0x0000FF00;
        else { REPLY("ERR\n", 4); return; }
        if (g_nks4_loaded)
            nks4_write("ROT %d\n", delta);
        else
            send_rtf5_wheel((uint32_t)delta);   /* rtf5's field3 uses the identical encoding */
        REPLY("OK\n", 3);

    } else if (strncmp(line, "SLIDER ", 7) == 0) {
        /* Physical Slider n, device code 16 + (n-1).  idx and val are each
         * snapped into their valid range rather than rejected - only a
         * genuinely malformed line (sscanf fails to find two integers at
         * all) is treated as an error. */
        int idx = 0, val = 0;
        if (!g_nks4_loaded && !g_rtf5_fallback_active) { REPLY("ERR NKS4_NOT_LOADED\n", 20); return; }
        if (sscanf(line + 7, "%d %d", &idx, &val) == 2) {
            idx = clampi(idx, 1, 8);
            val = clampi(val, 0, 127);
            if (g_nks4_loaded)
                nks4_analog_write(16 + (idx - 1), val);
            else
                /* rtf5's SLIDER packet takes the raw 0-127 value directly - unlike
                 * nks4_analog_write()'s byte0=val*2 transform, which is specific to
                 * ShortInvertNkS4AnalogValue on the nks4_inject path. */
                send_rtf5_event(0x0eu, (uint32_t)(idx - 1), (uint32_t)val);
            REPLY("OK\n", 3);
        } else {
            REPLY("ERR\n", 4);
        }

    } else if (strncmp(line, "KNOB ", 5) == 0) {
        /* Physical RT Knob n, device code 8 + (n-1). Same snap-not-reject
         * policy as SLIDER. */
        int idx = 0, val = 0;
        if (!g_nks4_loaded) { REPLY("ERR NKS4_NOT_LOADED\n", 20); return; }
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
        if (!g_nks4_loaded && !g_rtf5_fallback_active) { REPLY("ERR NKS4_NOT_LOADED\n", 20); return; }
        if (sscanf(line + 8, "%d", &val) == 1) {
            val = clampi(val, 0, 127);
            if (g_nks4_loaded)
                nks4_analog_write(25, val);
            else
                /* rtf5's VSLIDER packet, like SLIDER, takes the raw 0-127 value directly. */
                send_rtf5_event(0x0fu, 0x09u, (uint32_t)val);
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
        if (!g_nks4_loaded) { REPLY("ERR NKS4_NOT_LOADED\n", 20); return; }
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
        if (!g_nks4_loaded) { REPLY("ERR NKS4_NOT_LOADED\n", 20); return; }
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
        if (!g_nks4_loaded) { REPLY("ERR NKS4_NOT_LOADED\n", 20); return; }
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
        if (!g_nks4_loaded) { REPLY("ERR NKS4_NOT_LOADED\n", 20); return; }
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
        if (!g_nks4_loaded) { REPLY("ERR NKS4_NOT_LOADED\n", 20); return; }
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
        if (!g_nks4_loaded) { REPLY("ERR NKS4_NOT_LOADED\n", 20); return; }
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
        if (!g_nks4_loaded) { REPLY("ERR NKS4_NOT_LOADED\n", 20); return; }
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
        if (!g_nks4_loaded) { REPLY("ERR NKS4_NOT_LOADED\n", 20); return; }
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
        char resp[48];
        int  rlen, mode, editctx; const char *src;
        get_mode_state(&mode, &editctx, &src);
        /* Don't report synth state we don't trust yet - see update_boot_state()'s
         * block comment for exactly why a raw eva_mode.ko/pixel reading can look
         * confidently wrong during this window (this is the false MODE=3
         * EDITCTX=1 "Program edit while in Combi" a client would otherwise see
         * on every boot). MODE=0/EDITCTX=0 reuse this wire format's own
         * pre-existing "unknown"/"none" sentinels - not new values, just forced
         * regardless of what was actually read. BOOT itself always stays
         * accurate - it's how a client knows to stop trusting 0/0 as a real
         * reading and start polling again. */
        if (g_boot_active) { mode = 0; editctx = 0; }
        rlen = snprintf(resp, sizeof(resp), "MODE=%u EDITCTX=%d BOOT=%d\n",
                         (unsigned)mode, editctx, g_boot_active);
        REPLY(resp, (size_t)rlen);

    } else if (strcmp(line, "MODE_DETAIL") == 0) {
        /* Richer counterpart to STATE, same spirit as PADMAP_STATE alongside
         * PADMAP_* - exposes which source answered (eva_mode.ko's direct
         * memory read vs. framebuffer pixel fallback) and the edit-context
         * timbre/track slot, for callers that want to know more than just
         * the two STATE fields, or want to confirm eva_mode.ko is actually
         * in play rather than the pixel fallback. BOOT is the same
         * server-side boot gate STATE reports - see update_boot_state(). */
        char resp[128];
        int  rlen, mode, editctx, eva_mode2 = 0, eva_editctx2 = 0, eva_slot = -1;
        const char *src;
        int eva_resolved = eva_mode_read(&eva_mode2, &eva_editctx2, &eva_slot, NULL);
        get_mode_state(&mode, &editctx, &src);
        /* Same reasoning as STATE above - MODE/EDITCTX/EDITSLOT/SOURCE are
         * the fields that describe synth state a client could act on, so
         * they're forced to safe values during boot. EVA_LOADED/EVA_RESOLVED
         * are left as real live values on purpose - they describe the boot
         * gate's OWN progress (is eva_mode.ko loaded, has it resolved a
         * reading at all), which is exactly what this command exists to let
         * a caller watch, not synth state the client could mistakenly act on. */
        if (g_boot_active) { mode = 0; editctx = 0; src = "none"; eva_slot = -1; }
        rlen = snprintf(resp, sizeof(resp),
                         "SOURCE=%s MODE=%u EDITCTX=%d EDITSLOT=%d EVA_LOADED=%d EVA_RESOLVED=%d BOOT=%d\n",
                         src, (unsigned)mode, editctx,
                         eva_resolved ? eva_slot : -1, g_eva_mode_loaded, eva_resolved, g_boot_active);
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
                if (c == '\n') {
                    if (ctrl_lb_overflow) {
                        /* Line exceeded ctrl_lb - reject explicitly rather than
                         * dispatch a silently-truncated (corrupt) command. */
                        write_all(ctrl_fd, "ERR LINE_TOO_LONG\n", 18);
                    } else if (ctrl_lb_n > 0) {
                        ctrl_lb[ctrl_lb_n] = '\0';
                        if (strncmp(ctrl_lb, "TOUCH_MOVE ", 11) == 0 &&
                            ctrl_lb_n < (int)sizeof(ctrl_pending_move)) {
                            memcpy(ctrl_pending_move, ctrl_lb, (size_t)ctrl_lb_n + 1);
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
                    ctrl_lb_overflow = 0;
                } else if (ctrl_lb_n < (int)sizeof(ctrl_lb) - 1) {
                    ctrl_lb[ctrl_lb_n++] = c;
                } else {
                    ctrl_lb_overflow = 1;   /* consume to newline, then reject */
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
    put_le32(hdr,     payload);
    put_le16(hdr + 4, (uint16_t)first_row);
    put_le16(hdr + 6, (uint16_t)row_count);
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
    apply_boot_splash(staging);   /* no-op once BOOT= clears or nothing loaded */
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
        set_cloexec(midi_in_fd);
        g_midi_loaded = (midi_in_fd >= 0);
        start_midi_capture();
        fprintf(stderr, "screenremote: midi_in=%d capture=%d\n",
                midi_in_fd >= 0 ? 1 : 0, midi_cap_fd >= 0 ? 1 : 0);
    } else {
        fprintf(stderr, "screenremote: midi_bridge failed (%ld)\n", ret);
    }
}

/* Deferred-retry tuning for nks4_inject (see try_load_nks4_inject / main()).
 * The early attempt polls OA-Live only briefly so it fails fast and defers on
 * non-rooted units where OA.ko loads after screenremote starts; the main-loop
 * retry then keeps trying until OA comes up or the give-up deadline elapses. */
#define NKS4_EARLY_WAIT_DS      1   /* early attempt: 1 poll (~100 ms) then defer */
#define NKS4_LOAD_DEADLINE_S  120   /* give up retrying this many s after retries begin */

/* Attempt to load nks4_inject.ko once - see its own header comment for what it
 * does and why.  Unlike midi_bridge this makes pure additive calls into OA's
 * own CSTGFrontPanel::Handle* methods; it never claims a reader slot on a live
 * queue or touches anything EVA's USB-codec init could still be mutating, so it
 * only needs OA to be Live (kallsyms resolvable, .text relocated) - NOT EVA's
 * UI to be up.  Tried early (right after vkbd, see main()), well before
 * midi_bridge's EVA-ready gate.
 *
 * live_wait_ds bounds how long THIS attempt polls for OA-Live (deciseconds,
 * 100 ms each).  Returns a tri-state so the caller can retry - on a non-rooted
 * Kronos OA.ko is often not yet Live at the early attempt, and screenremote's
 * old fixed 5 s wait then disabled front-panel injection for the whole boot:
 *    1  loaded (or already loaded) and /proc/.nks4inject open   - done
 *    0  OA not Live yet                                         - retry later
 *   -1  OA Live but the load can't succeed (symbols missing, init_module
 *       error, or the proc node never opened)                   - permanent
 * Idempotent: a prior load returns EEXIST, which we treat as success and just
 * (re)open the fd.  The not-Live path is silent by design - the caller logs. */
static int try_load_nks4_inject(int live_wait_ds)
{
    unsigned long fn_switch = 0, fn_touch = 0, fn_rotary = 0, fn_analog = 0, fn_invert = 0;
    unsigned long fn_chord = 0;
    char params[320];
    long ret;

    if (g_nks4_loaded)
        return 1;                            /* already loaded on an earlier try */
    if (!wait_for_oa_live(live_wait_ds))
        return 0;                            /* OA not Live yet - caller may retry */

    resolve_nks4_kallsyms(&fn_switch, &fn_touch, &fn_rotary, &fn_analog, &fn_invert, &fn_chord);
    if (!fn_switch || !fn_touch || !fn_rotary || !fn_analog || !fn_invert) {
        fprintf(stderr, "screenremote: missing NKS4 symbols in kallsyms "
                "(switch=%s touch=%s rotary=%s analog=%s invert=%s) - "
                "front-panel injection disabled\n",
                fn_switch ? "ok" : "none", fn_touch ? "ok" : "none",
                fn_rotary ? "ok" : "none", fn_analog ? "ok" : "none",
                fn_invert ? "ok" : "none");
        return -1;   /* OA is Live so its kallsyms are populated - retrying won't heal this */
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
        set_cloexec(nks4_fd);
        g_nks4_loaded = (nks4_fd >= 0);
        fprintf(stderr, "screenremote: nks4_inject %s (fd=%s)\n",
                ret == 0 ? "loaded" : "already loaded",
                nks4_fd >= 0 ? "ok" : "open failed");
        return g_nks4_loaded ? 1 : -1;   /* node is created in module_init; open-fail is permanent */
    }
    fprintf(stderr, "screenremote: nks4_inject failed (%ld)\n", ret);
    return -1;
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

/* Unload every kernel module this daemon owns.  Single source of truth for
 * "which modules are ours" - called both from the normal end-of-main shutdown
 * and from graceful_shutdown() below, so an early exit can't skip a module an
 * on-time exit would have unloaded (or vice versa).  Safe to call unconditionally:
 * g_midi_loaded/g_nks4_loaded guard the conditionally-loaded ones, and
 * delete_module on a not-currently-loaded "vkbd" just fails silently (ENOENT). */
static void unload_our_modules(void)
{
    /* Unconditional by name, like vkbd always was - NOT gated on
     * g_midi_loaded/g_nks4_loaded. Those flags track "the /proc node opened",
     * not "init_module succeeded"; under RTAI workqueue starvation the proc
     * node can fail to appear in the retry window even though the module IS
     * resident, which left the flag at 0 and this function skipping the
     * delete_module call entirely - orphaning a module with a live OA-unload
     * notifier still registered, the exact hazard this function exists to
     * prevent (found on review 2026-07-16). delete_module on an unloaded name
     * just fails silently (ENOENT), same as vkbd's call already relied on. */
    syscall(__NR_delete_module, "midi_bridge", O_NONBLOCK);
    g_midi_loaded = 0;
    syscall(__NR_delete_module, "nks4_inject", O_NONBLOCK);
    g_nks4_loaded = 0;
    syscall(__NR_delete_module, "vkbd", O_NONBLOCK);
}

/* Reap the forked helper children (kmsg drainer + midi_tcp capture) AND unload
 * our kernel modules, before an EARLY exit from main().  The normal shutdown
 * path at the bottom of main does the same two things; the early "return 0/1"
 * paths (SIGTERM during the fb1/network wait, fb1 never appears, malloc
 * failure, bind failure) run after modules are already loaded (vkbd/nks4_inject
 * load early, well before this point) and would otherwise orphan both the
 * helper children AND the modules: the drainer rewrites boot_kmsg.log every
 * 150 ms forever, a stray midi_tcp keeps port 9875 and a /proc/.midi_ring
 * reader open (a second reader would steal the first's MIDI bytes), and an
 * orphaned nks4_inject/vkbd stays resident with a live OA-unload notifier
 * registered through whatever comes next (e.g. a Korg OS update's OA teardown).
 * kmsg_pid is main-local so it is passed in; midi_cap_pid and the module-load
 * flags are file-scope globals. */
static void graceful_shutdown(pid_t kmsg_pid)
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
    unload_our_modules();
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
    time_t last_mirror_chk = 0, last_net_chk = 0, last_eva_chk = 0, last_boot_chk = 0;
    time_t last_nks4_chk = 0;          /* throttle deferred nks4_inject retry (1/s) */
    time_t nks4_retry_t0 = 0;          /* when deferred retry began - give-up anchor */
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
        set_cloexec(vkbd_fd);

        /* Eva mode-state module: safe to load this early, unlike nks4_inject/
         * midi_bridge below it - it has no dependency on OA.ko being Live and
         * no dependency on Eva having started either. It resolves lazily on
         * every /proc/.eva_mode read (find_eva_mm() re-scans the process list
         * each time - see eva_mode.c), so loading it before Eva exists just
         * means early STATE/SYSINFO/MODE_DETAIL queries see EVA_RESOLVED=0
         * and fall back to pixel detection (get_mode_state()) until Eva comes
         * up - no retry/deferred-load plumbing needed here at all. */
        {
            extract_ko(EVA_MODE_KO, eva_mode_ko, eva_mode_ko_len);
            long _emret = syscall(SYS_init_module, (void *)eva_mode_ko,
                                   (unsigned long)eva_mode_ko_len, "");
            g_eva_mode_loaded = (_emret == 0 || errno == EEXIST);
            fprintf(stderr, "screenremote: eva_mode %s\n",
                    g_eva_mode_loaded ? "loaded" : "load failed - falling back to pixel mode detection");
        }

        /* Front-panel injection module: safe to load this early too (see
         * try_load_nks4_inject()'s own comment for why it doesn't need to wait
         * for EVA like midi_bridge does) - BUTTON/TOUCH/WHEEL/SLIDER/KNOB/
         * VSLIDER are unavailable (ERR NKS4_NOT_LOADED) until it succeeds.
         *
         * On a non-rooted Kronos OA.ko is loaded after screenremote starts, so
         * this early attempt frequently finds OA not-yet-Live.  We poll only
         * briefly (NKS4_EARLY_WAIT_DS) and, on a not-Live result, arm a deferred
         * retry in the main loop rather than disabling injection for the whole
         * boot the way the old fixed 5 s wait did.  On a rooted unit OA is
         * already Live, so this succeeds on the first poll and nothing defers. */
        {
            int _nks4_early = try_load_nks4_inject(NKS4_EARLY_WAIT_DS);
            if (_nks4_early == 0) {
                g_nks4_load_pending = 1;
                fprintf(stderr, "screenremote: OA not Live yet at early nks4_inject "
                        "load - will retry from main loop\n");
            } else if (_nks4_early < 0) {
                nks4_give_up("permanent load failure at early boot");
            }
        }
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
            /* Honor a shutdown request during this up-to-30 s wait - otherwise a
             * SIGTERM early in boot is ignored until /dev/fb1 appears. */
            if (g_exit) { graceful_shutdown(kmsg_pid); return 0; }
            _probe = open(FB_SRC, O_RDONLY);
            if (_probe >= 0) { close(_probe); break; }
            usleep(100000);
        }
    }
    if (fb1_open() < 0) { graceful_shutdown(kmsg_pid); return 1; }
    load_boot_splash();   /* optional - missing/invalid file just leaves compositing off */

    shadow = malloc(frame_bytes);
    if (!shadow) { perror("malloc shadow"); graceful_shutdown(kmsg_pid); return 1; }
    staging = malloc(frame_bytes);
    if (!staging) { perror("malloc staging"); graceful_shutdown(kmsg_pid); return 1; }
    rle_buf = malloc(frame_bytes * 2);
    if (!rle_buf) { perror("malloc rle_buf"); graceful_shutdown(kmsg_pid); return 1; }

    check_mirror_flag();

    /* Wait for usable network.  This wait is unbounded (a unit with no link can
     * sit here indefinitely), so it MUST honor a shutdown request - otherwise
     * SIGTERM can't stop the daemon until the network comes up. */
    while (!network_ok()) {
        if (g_exit) { graceful_shutdown(kmsg_pid); return 0; }
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
            graceful_shutdown(kmsg_pid);
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
     * knows this one succeeded and loads MIDI normally.
     *
     * EXCEPT: if midi_bridge's load is still pending (deferred until EVA is
     * up - see g_midi_load_pending below), clearing the flag here leaves the
     * boot-loop recovery net NOT actually covering that load - the
     * historically brick-causing one. A freeze during/right after it would
     * then have no .boot flag left to catch on the next boot (found
     * 2026-07-16). Defer the clear until right after that load actually
     * happens, in the main loop. If midi_bridge isn't going to load at all
     * this boot (calibration mode, kill-switch, or already-skipped), there's
     * no load event to wait for, so clear immediately as before. */
    if (!g_midi_load_pending)
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

        /* Boot-state gate - see update_boot_state()'s block comment. Throttled
         * to 1/s (plenty for its 600ms debounce window - the streak clock is
         * wall-clock-driven via clock_gettime(), not tied to poll cadence) and
         * self-disables once latched (g_boot_active goes 0 and this whole
         * block stops firing). */
        if (g_boot_active && now - last_boot_chk >= 1) {
            update_boot_state();
            last_boot_chk = now;
        }

        /* Periodic mirror flag check */
        if (now - last_mirror_chk >= 1) {
            check_mirror_flag();
            last_mirror_chk = now;

            /* Stuck-chord watchdog: overlapping TOUCH_DOWNs and PADMAP_OFF
             * already release a held pad by hand, but a client that simply
             * disconnects mid-hold with no further command has nothing to
             * trigger either path - force it after PADCHORD_MAX_HOLD_S. */
            if (g_active_pad != -1) {
                struct timespec pnow;
                clock_gettime(CLOCK_MONOTONIC, &pnow);
                if (pnow.tv_sec - g_chord_down_time.tv_sec >= PADCHORD_MAX_HOLD_S) {
                    fprintf(stderr, "screenremote: pad %d held past %ds with no "
                            "release - forcing note-off\n", g_active_pad,
                            PADCHORD_MAX_HOLD_S);
                    send_padchord(g_active_pad, 0);
                    g_active_pad = -1;
                }
            }
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
                    unlink(BOOT_FLAG);   /* deferred from startup - see the comment there */
                }
            } else {
                eva_ready_streak = 0;
            }
        }

        /* Deferred nks4_inject retry - its OWN block, deliberately not folded
         * into the FBCURVE/midi else-if chain above (that chain would starve it
         * in calibration mode or while a midi load is pending).  On non-rooted
         * units OA.ko becomes Live after the early attempt, so keep retrying
         * with a cheap ~100 ms OA-Live probe once a second until it loads or we
         * give up gracefully.  The give-up clock (NKS4_LOAD_DEADLINE_S) is
         * anchored to the FIRST retry here, NOT to boot: the network wait before
         * this loop is unbounded, so a boot-anchored deadline could expire
         * before we ever retry against a live OA - the exact failure we're
         * fixing.  g_nks4_load_pending is only ever set inside the brick-safe
         * load_mods && !boot_flag_found guard, so this inherits that gating. */
        if (g_nks4_load_pending && !g_nks4_loaded && now - last_nks4_chk >= 1) {
            last_nks4_chk = now;
            if (nks4_retry_t0 == 0)
                nks4_retry_t0 = now;
            int st = try_load_nks4_inject(1);   /* quick probe: ~100 ms if not Live */
            if (st == 1) {
                fprintf(stderr, "screenremote: nks4_inject loaded on retry "
                        "(%lds after retries began)\n", (long)(now - nks4_retry_t0));
                g_nks4_load_pending = 0;
            } else if (st < 0) {
                g_nks4_load_pending = 0;   /* permanent failure - already logged the cause */
                nks4_give_up("permanent load failure on retry");
            } else if (now - nks4_retry_t0 >= NKS4_LOAD_DEADLINE_S) {
                fprintf(stderr, "screenremote: OA never went Live within %ds of "
                        "retrying - front-panel injection disabled\n",
                        NKS4_LOAD_DEADLINE_S);
                g_nks4_load_pending = 0;   /* give up gracefully */
                nks4_give_up("OA never went Live within the retry deadline");
            }
        }

        /* Periodic network check - re-bind if IP changed (DHCP, link down/up) */
        if (now - last_net_chk >= 10) {
            last_net_chk = now;
            uint32_t cur_ip = find_lan_ip();
            if (cur_ip != g_bound_ip) {
                if (client_fd >= 0) { close(client_fd); client_fd = -1; }
                if (ctrl_fd  >= 0) { close(ctrl_fd);  ctrl_fd = -1; ctrl_lb_n = 0; ctrl_lb_overflow = 0; }
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
            /* do_handshake guarantees fps in [1,FPS_MAX], but guard the divisor
             * anyway so this can never divide by zero (matches the select-timeout
             * path below, which already guards it). */
            long frame_ns = 1000000000L / (client_fps ? client_fps : FPS_MAX);
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
                set_cloexec(new_fd);
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
                char firstline[CTRL_LINE_MAX]; int fl = 0, fl_overflow = 0;
                struct timeval rto = {0, 200000};  /* 200 ms read timeout */
                struct timeval sto = {2, 0};        /* 2 s send timeout - unauth replies must not block forever either */
                struct timespec deadline;
                set_cloexec(cfd);
                setsockopt(cfd, SOL_SOCKET, SO_RCVTIMEO, &rto, sizeof(rto));
                setsockopt(cfd, SOL_SOCKET, SO_SNDTIMEO, &sto, sizeof(sto));
                /* A per-byte 200 ms recv timeout alone doesn't bound the whole
                 * read: a client trickling one byte every <200ms with no
                 * newline keeps every individual recv() succeeding forever,
                 * freezing this single-threaded daemon's entire main loop
                 * indefinitely - reachable pre-auth, from any LAN host, no
                 * credentials (found 2026-07-16). Cap the WHOLE line read to a
                 * 1 s wall-clock deadline too, independent of per-byte timing. */
                clock_gettime(CLOCK_MONOTONIC, &deadline);
                deadline.tv_sec += 1;
                for (;;) {
                    char c;
                    struct timespec now;
                    clock_gettime(CLOCK_MONOTONIC, &now);
                    if (now.tv_sec > deadline.tv_sec ||
                        (now.tv_sec == deadline.tv_sec && now.tv_nsec > deadline.tv_nsec)) {
                        close(cfd); cfd = -1; break;
                    }
                    if (recv(cfd, &c, 1, 0) != 1) { close(cfd); cfd = -1; break; }
                    if (c == '\n') break;
                    if (fl < (int)sizeof(firstline) - 1)
                        firstline[fl++] = c;
                    else
                        fl_overflow = 1;   /* consume to newline, then reject */
                }
                if (cfd >= 0) {
                    firstline[fl] = '\0';
                    struct timeval zero = {0, 0};
                    setsockopt(cfd, SOL_SOCKET, SO_RCVTIMEO, &zero, sizeof(zero));

                    int is_readonly = is_readonly_cmd(firstline);
                    int owned = (g_ctrl_allowed_ip != 0 &&
                                 cpeer.sin_addr.s_addr == g_ctrl_allowed_ip);

                    if (fl_overflow) {
                        /* Over-length first line - reject explicitly instead of
                         * acting on a silently-truncated command. */
                        write_all(cfd, "ERR LINE_TOO_LONG\n", 18);
                        close(cfd);
                    } else if (is_readonly) {
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
                            g_ctrl_persist_ip = cpeer.sin_addr.s_addr;
                            ctrl_lb_n = 0;
                            ctrl_lb_overflow = 0;
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

        /* Ownership may have changed since this persistent session was
         * established (a new stream client took over, the owner disconnected,
         * or the bound IP was rebound) - see g_ctrl_persist_ip's comment. */
        if (ctrl_fd >= 0 && g_ctrl_persist_ip != g_ctrl_allowed_ip) {
            fprintf(stderr, "screenremote: persistent ctrl ownership revoked, closing\n");
            close(ctrl_fd); ctrl_fd = -1; ctrl_lb_n = 0; ctrl_lb_overflow = 0;
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
                ctrl_lb_overflow = 0;
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
    if (touch_fd >= 0) { close(touch_fd); touch_fd = -1; }
    oa_sys_priv_cleanup();
    if (vkbd_fd  >= 0) { close(vkbd_fd);  vkbd_fd  = -1; }
    if (kbd_fd   >= 0) { ioctl(kbd_fd, UI_DEV_DESTROY); close(kbd_fd); kbd_fd = -1; }
    if (midi_in_fd  >= 0) { close(midi_in_fd);  midi_in_fd = -1; }
    if (midi_cap_fd >= 0) { close(midi_cap_fd); midi_cap_fd = -1; }
    if (midi_cap_pid > 0) { kill(midi_cap_pid, SIGTERM); waitpid(midi_cap_pid, NULL, 0); }
    unload_our_modules();
    fb0_close();
    fprintf(stderr, "screenremote: exited cleanly\n");
    return 0;
}
