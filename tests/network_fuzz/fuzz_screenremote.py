#!/usr/bin/env python3
"""
Network fuzz / malformed-input regression suite for screenremote.

Targets the three network-facing surfaces of the daemon over real TCP/UDP:
  - stream port (7373): the KSCR handshake protocol (do_handshake())
  - ctrl port   (7374): the line-based text command protocol (process_ctrl_cmd())
  - discovery   (7372/UDP): the "KSCR?" LAN discovery probe

Goal: find inputs that crash the daemon process or oops the kernel, not just
inputs that get rejected cleanly (ERR/connection-close is a PASS - that's the
protocol working as designed). A "crash" is either the screenremote process
disappearing (pidof comes back empty when it didn't before) or new dmesg
content containing an oops/BUG signature.

Usage:
    python3 fuzz_screenremote.py --host 192.168.100.15 --phase 1
    python3 fuzz_screenremote.py --host 192.168.100.15 --phase 2
    python3 fuzz_screenremote.py --host 192.168.100.15 --phase 1 --only HS,CT01-CT10
    python3 fuzz_screenremote.py --host 192.168.100.15 --phase 1 --bisect CT07

Requires: sshpass, ssh (for liveness checks + daemon restart between phases).
Set KRONOS_SSH_PASS env var, or edit SSH_PASS below.
"""
import argparse
import json
import os
import socket
import struct
import subprocess
import sys
import threading
import time

HOST = "192.168.100.15"
STREAM_PORT = 7373
CTRL_PORT = 7374
DISC_PORT = 7372
SSH_USER = "root"
SSH_PASS = os.environ.get("KRONOS_SSH_PASS", "kronos")
MAGIC = b"KSCR"

RESULTS_DIR = os.path.join(os.path.dirname(__file__), "results")
os.makedirs(RESULTS_DIR, exist_ok=True)


# ---------------------------------------------------------------------------
# SSH helpers (liveness checks + daemon control) - the only place this script
# talks to the Kronos outside of the raw sockets under test.
# ---------------------------------------------------------------------------

def ssh(cmd, timeout=10):
    full = [
        "sshpass", "-p", SSH_PASS, "ssh",
        "-o", "ConnectTimeout=6", "-o", "StrictHostKeyChecking=no",
        f"{SSH_USER}@{HOST}", cmd,
    ]
    try:
        r = subprocess.run(full, capture_output=True, text=True, timeout=timeout)
        return r.stdout, r.stderr, r.returncode
    except subprocess.TimeoutExpired:
        return "", "SSH TIMEOUT", -1


def check_liveness():
    """Returns (pid_or_None, new_dmesg_text). Clears dmesg as it reads (dmesg -c)."""
    out, err, rc = ssh("pidof screenremote; echo ---SPLIT---; dmesg -c 2>&1")
    if "---SPLIT---" not in out:
        return ("SSH_UNREACHABLE", err)
    pid_part, dmesg_part = out.split("---SPLIT---", 1)
    pid = pid_part.strip() or None
    return (pid, dmesg_part.strip())


def restart_daemon():
    ssh("kill $(pidof screenremote) 2>/dev/null; sleep 2", timeout=15)
    ssh("cd /korg/rw/screenremote && nohup ./screenremote > /korg/rw/screenremote_fuzz.log 2>&1 &", timeout=10)
    time.sleep(3)
    pid, _ = check_liveness()
    return pid


def get_pubid_creds():
    """check_auth() tries KronosNet.conf FIRST, and only falls back to the
    PublicID (/proc/id) if that file doesn't contain a 'kronos' entry at all -
    a kr==1 (username matched, wrong password) short-circuits before the
    PublicID branch is ever reached. Prefer KronosNet.conf's stored creds
    when present; fall back to PublicID only if that file is empty/missing."""
    out, _, _ = ssh("cat /korg/rw/Startup/KronosNet.conf 2>/dev/null")
    lines = out.splitlines()
    if len(lines) >= 2 and lines[0].strip():
        return lines[0].strip(), lines[1].strip()
    out, _, _ = ssh("cat /proc/id")
    pubid = out.strip().replace("-", "")
    return "kronos", pubid


# ---------------------------------------------------------------------------
# Raw protocol helpers
# ---------------------------------------------------------------------------

def raw_connect(port, timeout=3.0):
    s = socket.socket(socket.AF_INET, socket.SOCK_STREAM)
    s.settimeout(timeout)
    s.connect((HOST, port))
    return s


def send_and_maybe_recv(port, payload, recv_timeout=1.0, recv_len=4096, then=None, rst=False):
    """Connect, send payload (bytes or list of (bytes, delay) chunks), try one
    recv, close. `then` is an optional callable(sock) for follow-up actions.
    `rst` sends TCP RST on close instead of a clean FIN (SO_LINGER trick)."""
    s = raw_connect(port)
    try:
        if isinstance(payload, list):
            for chunk, delay in payload:
                if chunk:
                    s.sendall(chunk)
                if delay:
                    time.sleep(delay)
        elif payload:
            s.sendall(payload)
        resp = b""
        if recv_timeout:
            s.settimeout(recv_timeout)
            try:
                resp = s.recv(recv_len)
            except socket.timeout:
                pass
            except (ConnectionResetError, OSError):
                pass
        if then:
            then(s)
        return resp
    finally:
        if rst:
            s.setsockopt(socket.SOL_SOCKET, socket.SO_LINGER, struct.pack("ii", 1, 0))
        try:
            s.close()
        except OSError:
            pass


def udp_send(payload, recv_timeout=0.5):
    s = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
    s.settimeout(recv_timeout)
    try:
        s.sendto(payload, (HOST, DISC_PORT))
        try:
            resp, _ = s.recvfrom(4096)
            return resp
        except socket.timeout:
            return None
    finally:
        s.close()


def hs_header(magic=MAGIC, ver=0x02, mode=1, fps=30, ulen=6, plen=16):
    return magic + bytes([ver, mode, fps, ulen & 0xFF, plen & 0xFF])


def ctrl_line(cmd):
    if isinstance(cmd, str):
        cmd = cmd.encode()
    if not cmd.endswith(b"\n"):
        cmd += b"\n"
    return cmd


# ---------------------------------------------------------------------------
# Test case registry
# ---------------------------------------------------------------------------

TESTS = []  # list of (id, category, desc, fn)


def test(tid, category, desc):
    def deco(fn):
        TESTS.append((tid, category, desc, fn))
        return fn
    return deco


# ---- HS: stream-port handshake -------------------------------------------

@test("HS01", "HS", "connect + immediate close, no data")
def _(ctx):
    send_and_maybe_recv(STREAM_PORT, b"", recv_timeout=0.5)


@test("HS02", "HS", "1 byte then close")
def _(ctx):
    send_and_maybe_recv(STREAM_PORT, b"\x00", recv_timeout=0.5)


@test("HS03", "HS", "wrong magic, right length")
def _(ctx):
    send_and_maybe_recv(STREAM_PORT, b"XXXX\x02\x01\x1e\x06\x10")


@test("HS04", "HS", "right magic, wrong version byte")
def _(ctx):
    send_and_maybe_recv(STREAM_PORT, hs_header(ver=0xFF))


@test("HS05", "HS", "ulen=0 (below min 1)")
def _(ctx):
    send_and_maybe_recv(STREAM_PORT, hs_header(ulen=0))


@test("HS06", "HS", "ulen=65 (above max 64)")
def _(ctx):
    send_and_maybe_recv(STREAM_PORT, hs_header(ulen=65) + b"A" * 65)


@test("HS07", "HS", "ulen=255")
def _(ctx):
    send_and_maybe_recv(STREAM_PORT, hs_header(ulen=255) + b"A" * 255)


@test("HS08", "HS", "plen=129 (above max 128)")
def _(ctx):
    send_and_maybe_recv(STREAM_PORT, hs_header(ulen=6, plen=129) + b"kronos" + b"A" * 129)


@test("HS09", "HS", "plen=255")
def _(ctx):
    send_and_maybe_recv(STREAM_PORT, hs_header(ulen=6, plen=255) + b"kronos" + b"A" * 255)


@test("HS10", "HS", "header claims ulen=64/plen=128, sends far less, holds open")
def _(ctx):
    send_and_maybe_recv(STREAM_PORT, hs_header(ulen=64, plen=128) + b"short", recv_timeout=1.0)


@test("HS11", "HS", "credentials with embedded NUL / high-bit / format-string bytes")
def _(ctx):
    user = b"kro\x00nos"
    payload = hs_header(ulen=len(user), plen=8) + user + b"%n%n%n\xff\xfe"
    send_and_maybe_recv(STREAM_PORT, payload)


@test("HS12", "HS", "header sent 1 byte at a time with delays (fragmentation)")
def _(ctx):
    hdr = hs_header(ulen=6, plen=16)
    chunks = [(bytes([b]), 0.02) for b in hdr]
    send_and_maybe_recv(STREAM_PORT, chunks, recv_timeout=1.0)


@test("HS13", "HS", "header + immediate large trailing garbage before any reply")
def _(ctx):
    send_and_maybe_recv(STREAM_PORT, hs_header(ulen=6, plen=16) + b"kronos" + b"X" * 16 + os.urandom(4096))


@test("HS14", "HS", "valid creds, invalid mode byte")
def _(ctx):
    user, pw = get_pubid_creds()
    ub = user.encode(); pb = pw.encode()
    payload = MAGIC + bytes([0x02, 0xEE, 30, len(ub), len(pb)]) + ub + pb
    send_and_maybe_recv(STREAM_PORT, payload)


@test("HS15", "HS", "valid creds, fps=0 and fps=255 (two sub-cases)")
def _(ctx):
    user, pw = get_pubid_creds()
    ub = user.encode(); pb = pw.encode()
    for fps in (0, 255):
        payload = MAGIC + bytes([0x02, 1, fps, len(ub), len(pb)]) + ub + pb
        send_and_maybe_recv(STREAM_PORT, payload)


@test("HS16", "HS", "50x rapid connect + RST churn")
def _(ctx):
    for _ in range(50):
        try:
            send_and_maybe_recv(STREAM_PORT, hs_header()[:3], recv_timeout=0.05, rst=True)
        except OSError:
            pass


@test("HS17", "HS", "connect, send nothing, idle (short of the 5s server timeout)")
def _(ctx):
    send_and_maybe_recv(STREAM_PORT, b"", recv_timeout=2.0)


@test("HS18", "HS", "valid handshake success, then garbage as post-handshake stream")
def _(ctx):
    user, pw = get_pubid_creds()
    ub = user.encode(); pb = pw.encode()
    payload = MAGIC + bytes([0x02, 1, 30, len(ub), len(pb)]) + ub + pb
    def followup(s):
        s.sendall(os.urandom(256))
    send_and_maybe_recv(STREAM_PORT, payload, recv_timeout=1.0, then=followup)


@test("HS19", "HS", "all-zero 9-byte header")
def _(ctx):
    send_and_maybe_recv(STREAM_PORT, b"\x00" * 9)


@test("HS20", "HS", "all-0xFF 9-byte header")
def _(ctx):
    send_and_maybe_recv(STREAM_PORT, b"\xff" * 9)


@test("HS21", "HS", "partial header (5/9 bytes) then close")
def _(ctx):
    send_and_maybe_recv(STREAM_PORT, hs_header()[:5], recv_timeout=0.3)


@test("HS22", "HS", "partial header (5/9 bytes), hold open without closing")
def _(ctx):
    send_and_maybe_recv(STREAM_PORT, hs_header()[:5], recv_timeout=2.0)


@test("HS23", "HS", "200 concurrent raw connections, no data, held 1s then closed")
def _(ctx):
    socks = []
    try:
        for _ in range(200):
            try:
                s = socket.socket(socket.AF_INET, socket.SOCK_STREAM)
                s.settimeout(2.0)
                s.connect((HOST, STREAM_PORT))
                socks.append(s)
            except OSError:
                break
        time.sleep(1.0)
    finally:
        for s in socks:
            try:
                s.close()
            except OSError:
                pass


# ---- CT: ctrl-port command protocol ---------------------------------------

@test("CT01", "CT", "connect, no data, close")
def _(ctx):
    send_and_maybe_recv(CTRL_PORT, b"", recv_timeout=0.5)


@test("CT02", "CT", "empty line (just newline)")
def _(ctx):
    send_and_maybe_recv(CTRL_PORT, b"\n")


@test("CT03", "CT", "one line, no newline, exactly at/over CTRL_LINE_MAX (8320)")
def _(ctx):
    send_and_maybe_recv(CTRL_PORT, b"A" * 9000, recv_timeout=1.5)


@test("CT04", "CT", "trickle 1 byte/150ms, no newline, for 2s (deadline-fix stress)")
def _(ctx):
    chunks = [(b"A", 0.15) for _ in range(14)]
    t0 = time.time()
    send_and_maybe_recv(CTRL_PORT, chunks, recv_timeout=1.0)
    ctx.setdefault("timing", {})["CT04_elapsed"] = time.time() - t0


@test("CT05", "CT", "random binary garbage, newline-terminated")
def _(ctx):
    send_and_maybe_recv(CTRL_PORT, os.urandom(512) + b"\n")


CMDS_NEED_ARGS = [
    "TOUCH", "TOUCH_DOWN", "TOUCH_MOVE", "TOUCH_UP", "PADCHORD", "PADMAP",
    "REGION", "PIXEL", "BUTTON", "CHORD", "WHEEL", "SLIDER", "KNOB",
    "VSLIDER", "JOYSTICK", "VECTOR", "RIBBON", "AFTERTOUCH", "PEDAL",
    "FOOTSWITCH", "DAMPER", "TEMPO", "KEY", "SS_TIMEOUT", "MIDI_SEND", "SYSEX",
]


@test("CT06", "CT", "each arg-requiring command sent with NO arguments")
def _(ctx):
    for cmd in CMDS_NEED_ARGS:
        send_and_maybe_recv(CTRL_PORT, ctrl_line(cmd + " "), recv_timeout=0.3)


NUMERIC_CMDS = [
    ("TOUCH", 2), ("TOUCH_DOWN", 2), ("TOUCH_MOVE", 2), ("TOUCH_UP", 2),
    ("PADCHORD", 2), ("SLIDER", 2), ("KNOB", 2), ("VSLIDER", 1),
    ("AFTERTOUCH", 1), ("PEDAL", 1), ("DAMPER", 1), ("TEMPO", 1), ("KEY", 2),
    ("SS_TIMEOUT", 1), ("REGION", 4), ("PIXEL", 2),
]
BAD_NUMS = [
    "-999999999", "999999999999999999999999", "-1", "0", "abc",
    "1.5", "-2147483649", "2147483648", "-2147483648", "2147483647",
    "0x41", "1e10", "+", "-", "", "NaN", "%d%d%d",
]


@test("CT07", "CT", "numeric commands with boundary/garbage args (matrix)")
def _(ctx):
    for cmd, nargs in NUMERIC_CMDS:
        for bad in BAD_NUMS:
            args = " ".join([bad] * nargs)
            send_and_maybe_recv(CTRL_PORT, ctrl_line(f"{cmd} {args}"), recv_timeout=0.2)


@test("CT08", "CT", "MIDI_SEND/SYSEX malformed hex payloads")
def _(ctx):
    bad_hex = [
        "", "F", "41F", "ZZ", "41 ZZ", "41" * 5000, "  ", "41,42,43",
        "41" + " " * 100 + "42", "\x00\x01\x02", "-41",
    ]
    for h in bad_hex:
        send_and_maybe_recv(CTRL_PORT, ctrl_line(f"MIDI_SEND {h}"), recv_timeout=0.2)
        send_and_maybe_recv(CTRL_PORT, ctrl_line(f"SYSEX {h}"), recv_timeout=0.2)


@test("CT09", "CT", "PADCHORD out-of-range pad/vel")
def _(ctx):
    for pad, vel in [(-1, 0), (999, 999), (-999999, -999999), (7, 128), (8, 0)]:
        send_and_maybe_recv(CTRL_PORT, ctrl_line(f"PADCHORD {pad} {vel}"), recv_timeout=0.2)


@test("CT10", "CT", "PADMAP wildly out-of-range / inverted rects")
def _(ctx):
    for args in ["-1 -1 -1 999999 999999", "0 999999 999999 0 0", "0 0 0 -1 -1", "255 0 0 800 600"]:
        send_and_maybe_recv(CTRL_PORT, ctrl_line(f"PADMAP {args}"), recv_timeout=0.2)


@test("CT11", "CT", "JOYSTICK/VECTOR/RIBBON invalid axis char")
def _(ctx):
    for cmd in ("JOYSTICK", "VECTOR", "RIBBON"):
        for axis in ("Z", "1", "%", "\x00", ""):
            send_and_maybe_recv(CTRL_PORT, ctrl_line(f"{cmd} {axis} 64"), recv_timeout=0.2)


@test("CT12", "CT", "REGION/PIXEL negative/huge/inverted coords")
def _(ctx):
    for args in ["-1 -1 -1 -1", "999999 999999 999999 999999", "500 500 0 0"]:
        send_and_maybe_recv(CTRL_PORT, ctrl_line(f"REGION {args}"), recv_timeout=0.3)
    for args in ["-1 -1", "999999 999999"]:
        send_and_maybe_recv(CTRL_PORT, ctrl_line(f"PIXEL {args}"), recv_timeout=0.2)


@test("CT13", "CT", "CHORD: >8 tokens, >15-char token, bad hold_ms prefix")
def _(ctx):
    send_and_maybe_recv(CTRL_PORT, ctrl_line("CHORD " + " ".join(["BTN%d" % i for i in range(20)])))
    send_and_maybe_recv(CTRL_PORT, ctrl_line("CHORD " + "X" * 200 + " Y"))
    send_and_maybe_recv(CTRL_PORT, ctrl_line("CHORD -99999 A B"))
    send_and_maybe_recv(CTRL_PORT, ctrl_line("CHORD abc A B"))


@test("CT14", "CT", "unknown/garbage command names")
def _(ctx):
    for cmd in ["FROBNICATE", "TOUCH2", "touch 1 2", "  TOUCH 1 2", "\x01\x02\x03", "A" * 20]:
        send_and_maybe_recv(CTRL_PORT, ctrl_line(cmd), recv_timeout=0.2)


@test("CT15", "CT", "embedded NUL bytes mid-line")
def _(ctx):
    send_and_maybe_recv(CTRL_PORT, b"TOUCH\x00 1 2\n", recv_timeout=0.3)
    send_and_maybe_recv(CTRL_PORT, b"STATE\x00GARBAGE\n", recv_timeout=0.3)


@test("CT16", "CT", "20x rapid unauthenticated CTRL_PERSIST attempts")
def _(ctx):
    for _ in range(20):
        send_and_maybe_recv(CTRL_PORT, ctrl_line("CTRL_PERSIST"), recv_timeout=0.1)


@test("CT17", "CT", "burst of 100 valid-looking short commands back to back, one connection")
def _(ctx):
    payload = b"".join(ctrl_line("VERSION") for _ in range(100))
    send_and_maybe_recv(CTRL_PORT, payload, recv_timeout=2.0)


@test("CT18", "CT", "format-string-like payloads as command args")
def _(ctx):
    for p in ["%n", "%s%s%s%s", "%x%x%x%x%x%x", "%99999999s", "%.999999f"]:
        send_and_maybe_recv(CTRL_PORT, ctrl_line(f"TOUCH {p} {p}"), recv_timeout=0.2)
        send_and_maybe_recv(CTRL_PORT, ctrl_line(f"CHORD {p} {p}"), recv_timeout=0.2)


@test("CT19", "CT", "SS_TIMEOUT negative/huge")
def _(ctx):
    for v in ["-1", "-999999", "999999999999"]:
        send_and_maybe_recv(CTRL_PORT, ctrl_line(f"SS_TIMEOUT {v}"), recv_timeout=0.2)


@test("CT20", "CT", "partial line then TCP RST at several byte offsets")
def _(ctx):
    for n in (0, 1, 5, 50, 500):
        send_and_maybe_recv(CTRL_PORT, b"TOUCH " + b"1" * n, recv_timeout=0.1, rst=True)


@test("CT21", "CT", "200 concurrent connections sending STATE, held then closed")
def _(ctx):
    socks = []
    try:
        for _ in range(200):
            try:
                s = socket.socket(socket.AF_INET, socket.SOCK_STREAM)
                s.settimeout(2.0)
                s.connect((HOST, CTRL_PORT))
                s.sendall(b"STATE\n")
                socks.append(s)
            except OSError:
                break
        time.sleep(1.0)
    finally:
        for s in socks:
            try:
                s.close()
            except OSError:
                pass


@test("CT22", "CT", "8000-char garbage command name, no newline")
def _(ctx):
    send_and_maybe_recv(CTRL_PORT, b"A" * 8000, recv_timeout=1.2)


@test("CT23", "CT", "TOUCH with exact int32 boundary values")
def _(ctx):
    for v in ["-2147483648", "2147483647", "-2147483649", "2147483648"]:
        send_and_maybe_recv(CTRL_PORT, ctrl_line(f"TOUCH {v} {v}"), recv_timeout=0.2)


@test("CT24", "CT", "KEY with negative/huge code/val")
def _(ctx):
    for args in ["-1 -1", "99999 99999", "-99999 1"]:
        send_and_maybe_recv(CTRL_PORT, ctrl_line(f"KEY {args}"), recv_timeout=0.2)


@test("CT25", "CT", "50x rapid unauthenticated STATE/SYSINFO/VERSION from fresh connections")
def _(ctx):
    for i in range(50):
        cmd = ["STATE", "SYSINFO", "VERSION"][i % 3]
        send_and_maybe_recv(CTRL_PORT, ctrl_line(cmd), recv_timeout=0.1)


# ---- DS: UDP discovery -----------------------------------------------------

@test("DS01", "DS", "empty UDP packet")
def _(ctx):
    udp_send(b"")


@test("DS02", "DS", "valid trigger KSCR?")
def _(ctx):
    udp_send(b"KSCR?")


@test("DS03", "DS", "KSCR without ? (4 bytes, below n>=5 threshold)")
def _(ctx):
    udp_send(b"KSCR")


@test("DS04", "DS", "KSCR? plus large trailing garbage")
def _(ctx):
    udp_send(b"KSCR?" + os.urandom(2000))


@test("DS05", "DS", "oversized packet (> buf[16])")
def _(ctx):
    udp_send(os.urandom(4096))


@test("DS06", "DS", "exactly 16 bytes (recvfrom bound is sizeof(buf)-1=15)")
def _(ctx):
    udp_send(b"KSCR?" + b"X" * 11)


@test("DS07", "DS", "random garbage, various short lengths")
def _(ctx):
    for n in (1, 2, 5, 10, 15):
        udp_send(os.urandom(n))


@test("DS08", "DS", "UDP flood: 500 packets rapid-fire")
def _(ctx):
    s = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
    try:
        for _ in range(500):
            s.sendto(os.urandom(16), (HOST, DISC_PORT))
    finally:
        s.close()


@test("DS09", "DS", "near-magic mismatches (case/char variants)")
def _(ctx):
    for p in (b"kscr?", b"KSCR!", b"KSCr?", b"\x00SCR?"):
        udp_send(p)


# ---- CC: true concurrency chaos (not sequential) --------------------------
# These require phase 2 (an authenticated session) to be meaningful for the
# ownership-related races; run standalone in phase 1 they just hammer the
# unauthenticated surface concurrently instead.

def _authed_stream_socket(mode=0x02):
    user, pw = get_pubid_creds()
    ub, pb = user.encode(), pw.encode()
    s = raw_connect(STREAM_PORT, timeout=5)
    s.sendall(MAGIC + bytes([0x02, mode, 30, len(ub), len(pb)]) + ub + pb)
    s.settimeout(3)
    s.recv(4096)
    return s


def _authed_persist_socket():
    s = raw_connect(CTRL_PORT, timeout=5)
    s.sendall(b"CTRL_PERSIST\n")
    return s


@test("CC01", "CC", "50 concurrent threads, each hammering ctrl port with random garbage")
def _(ctx):
    def worker():
        for _ in range(20):
            try:
                send_and_maybe_recv(CTRL_PORT, os.urandom(64) + b"\n", recv_timeout=0.1)
            except OSError:
                pass
    threads = [threading.Thread(target=worker) for _ in range(50)]
    for t in threads: t.start()
    for t in threads: t.join(timeout=10)


@test("CC02", "CC", "20 concurrent threads racing CTRL_PERSIST establishment")
def _(ctx):
    socks = []
    lock = threading.Lock()
    def worker():
        try:
            s = raw_connect(CTRL_PORT, timeout=3)
            s.sendall(b"CTRL_PERSIST\n")
            with lock:
                socks.append(s)
        except OSError:
            pass
    threads = [threading.Thread(target=worker) for _ in range(20)]
    for t in threads: t.start()
    for t in threads: t.join(timeout=10)
    time.sleep(0.5)
    for s in socks:
        try: s.close()
        except OSError: pass


@test("CC03", "CC", "1000 simultaneous raw connections to ctrl port (fd exhaustion stress)")
def _(ctx):
    socks = []
    try:
        for _ in range(1000):
            try:
                s = socket.socket(socket.AF_INET, socket.SOCK_STREAM)
                s.settimeout(1.0)
                s.connect((HOST, CTRL_PORT))
                socks.append(s)
            except OSError:
                break
        time.sleep(0.5)
    finally:
        for s in socks:
            try: s.close()
            except OSError: pass


@test("CC04", "CC", "concurrent: legit CTRL_PERSIST session + 30 threads spamming numeric-arg garbage")
def _(ctx):
    try:
        persist = _authed_persist_socket()
    except OSError:
        persist = None
    def worker():
        for cmd, nargs in NUMERIC_CMDS[:6]:
            for bad in BAD_NUMS[:6]:
                args = " ".join([bad] * nargs)
                try:
                    send_and_maybe_recv(CTRL_PORT, ctrl_line(f"{cmd} {args}"), recv_timeout=0.05)
                except OSError:
                    pass
    threads = [threading.Thread(target=worker) for _ in range(30)]
    for t in threads: t.start()
    for t in threads: t.join(timeout=15)
    if persist:
        try: persist.close()
        except OSError: pass


# ---- ST: stateful sequence attacks - target the code paths patched today --

@test("ST01", "ST", "TOUCH_DOWN pad A, then TOUCH_DOWN pad B with no UP, then abrupt RST")
def _(ctx):
    try:
        s = _authed_persist_socket()
    except OSError:
        return
    try:
        s.sendall(b"PADMAP_ON\n")
        time.sleep(0.1)
        # Two different screen coords, hoping to land on two different pads if
        # PADMAP regions are configured; if not, this degrades to same-pad
        # repeat which the fix also needs to handle sanely.
        s.sendall(b"TOUCH_DOWN 50 50\n")
        time.sleep(0.05)
        s.sendall(b"TOUCH_DOWN 400 400\n")
        time.sleep(0.05)
    finally:
        s.setsockopt(socket.SOL_SOCKET, socket.SO_LINGER, struct.pack("ii", 1, 0))
        s.close()


@test("ST02", "ST", "CTRL_PERSIST holds a pad, then a second client supersedes ownership mid-hold")
def _(ctx):
    try:
        persist = _authed_persist_socket()
        persist.sendall(b"PADMAP_ON\nTOUCH_DOWN 50 50\n")
        time.sleep(0.1)
    except OSError:
        return
    try:
        # Second stream client authenticates from the SAME source IP (only IP
        # this host has) - still exercises the takeover code path even though
        # it can't test a genuinely different IP without another host.
        s2 = _authed_stream_socket(mode=0x02)
        time.sleep(0.2)
        s2.close()
    except OSError:
        pass
    finally:
        try:
            persist.sendall(b"TOUCH_UP 50 50\n")
        except OSError:
            pass
        time.sleep(0.1)
        try: persist.close()
        except OSError: pass


@test("ST03", "ST", "rapid repeated SYSEX open/abort cycles (async state churn)")
def _(ctx):
    for i in range(15):
        s = raw_connect(CTRL_PORT, timeout=2)
        try:
            s.sendall(b"SYSEX F04100\n")
            time.sleep(0.02)
        except OSError:
            pass
        finally:
            s.setsockopt(socket.SOL_SOCKET, socket.SO_LINGER, struct.pack("ii", 1, 0))
            s.close()


@test("ST04", "ST", "concurrent PADMAP_OFF spam vs TOUCH_DOWN spam (race on g_active_pad)")
def _(ctx):
    try:
        persist = _authed_persist_socket()
    except OSError:
        return
    persist.sendall(b"PADMAP_ON\n")
    stop = threading.Event()
    def toucher():
        while not stop.is_set():
            try:
                persist.sendall(b"TOUCH_DOWN 50 50\n")
            except OSError:
                break
            time.sleep(0.01)
    def offer():
        while not stop.is_set():
            try:
                send_and_maybe_recv(CTRL_PORT, b"PADMAP_OFF\n", recv_timeout=0.05)
            except OSError:
                pass
            time.sleep(0.01)
    t1 = threading.Thread(target=toucher)
    t2 = threading.Thread(target=offer)
    t1.start(); t2.start()
    time.sleep(2.0)
    stop.set()
    t1.join(timeout=3); t2.join(timeout=3)
    try: persist.close()
    except OSError: pass


# ---------------------------------------------------------------------------
# Runner
# ---------------------------------------------------------------------------

def run_phase(phase, only_ids=None, bisect_id=None):
    print(f"=== Fuzz run: phase {phase} against {HOST} ===")
    print("Baseline liveness check...")
    pid0, dmesg0 = check_liveness()
    print(f"  screenremote pid={pid0}")
    if dmesg0:
        print(f"  (drained {len(dmesg0)} bytes of pre-existing dmesg backlog)")
    if pid0 in (None, "SSH_UNREACHABLE"):
        print("!! Daemon not running / unreachable at baseline - restarting.")
        pid0 = restart_daemon()
        print(f"  restarted, pid={pid0}")

    ctx = {}
    client_thread_sock = None
    ctrl_persist_sock = None
    if phase == 2:
        user, pw = get_pubid_creds()
        ub, pb = user.encode(), pw.encode()
        hdr = MAGIC + bytes([0x02, 2, 30, len(ub), len(pb)]) + ub + pb  # mode=2 MODE_CHANGE
        client_thread_sock = raw_connect(STREAM_PORT, timeout=5)
        client_thread_sock.sendall(hdr)
        client_thread_sock.settimeout(3)
        resp = client_thread_sock.recv(4096)
        print(f"  phase 2: legit stream client handshake response: {resp[:16]!r}...")
        ctrl_persist_sock = raw_connect(CTRL_PORT, timeout=5)
        ctrl_persist_sock.sendall(b"CTRL_PERSIST\n")
        ctrl_persist_sock.settimeout(2)
        try:
            r2 = ctrl_persist_sock.recv(256)
            print(f"  phase 2: legit CTRL_PERSIST response: {r2!r}")
        except socket.timeout:
            print("  phase 2: CTRL_PERSIST sent no immediate reply (expected - it's a data channel)")

    results = []
    tests_to_run = TESTS
    if bisect_id:
        tests_to_run = [t for t in TESTS if t[0] == bisect_id]
    elif only_ids:
        prefixes = only_ids.split(",")
        tests_to_run = [t for t in TESTS if any(t[0] == p or t[0].startswith(p) for p in prefixes)]

    crash_found = False
    for i, (tid, cat, desc, fn) in enumerate(tests_to_run):
        print(f"[{i+1}/{len(tests_to_run)}] {tid} ({cat}): {desc}")
        err = None
        try:
            fn(ctx)
        except Exception as e:
            err = str(e)
            print(f"    (client-side exception, not necessarily a server bug: {err})")

        pid, dmesg = check_liveness()
        status = "PASS"
        if pid == "SSH_UNREACHABLE":
            status = "SSH_UNREACHABLE"
        elif pid is None:
            status = "CRASH_PROCESS_DIED"
        elif any(sig in dmesg for sig in ("Oops", "BUG:", "general protection", "Kernel panic", "RIP:", "EIP:")):
            status = "CRASH_KERNEL_OOPS"

        rec = {"id": tid, "category": cat, "desc": desc, "phase": phase,
               "status": status, "client_error": err,
               "dmesg_excerpt": dmesg[:4000] if dmesg else None,
               "timestamp": time.time()}
        results.append(rec)

        if status != "PASS":
            print(f"  !!! {status} after {tid} !!!")
            if dmesg:
                print("----- dmesg -----")
                print(dmesg[:4000])
                print("-----------------")
            crash_found = True
            break

    if client_thread_sock:
        try: client_thread_sock.close()
        except OSError: pass
    if ctrl_persist_sock:
        try: ctrl_persist_sock.close()
        except OSError: pass

    out_path = os.path.join(RESULTS_DIR, f"phase{phase}_{int(time.time())}.json")
    with open(out_path, "w") as f:
        json.dump(results, f, indent=2)
    print(f"\nResults written to {out_path}")
    print(f"Ran {len(results)}/{len(tests_to_run)} cases. Crash found: {crash_found}")
    if crash_found:
        print(f"Suspect test: {results[-1]['id']} - {results[-1]['desc']}")
        print("Daemon may need a restart before continuing:")
        print("  python3 fuzz_screenremote.py --restart-only")
    return results


if __name__ == "__main__":
    ap = argparse.ArgumentParser()
    ap.add_argument("--host", default=HOST)
    ap.add_argument("--phase", type=int, choices=[1, 2], default=1)
    ap.add_argument("--only", help="comma-separated test id / category prefixes, e.g. HS,CT07")
    ap.add_argument("--bisect", help="run exactly one test id")
    ap.add_argument("--restart-only", action="store_true")
    ap.add_argument("--list", action="store_true")
    args = ap.parse_args()

    HOST = args.host

    if args.list:
        for tid, cat, desc, _ in TESTS:
            print(f"{tid:6s} [{cat}] {desc}")
        sys.exit(0)

    if args.restart_only:
        pid = restart_daemon()
        print(f"Restarted, pid={pid}")
        sys.exit(0)

    run_phase(args.phase, only_ids=args.only, bisect_id=args.bisect)
