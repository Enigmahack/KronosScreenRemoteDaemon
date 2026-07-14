#!/usr/bin/env python3
"""
kscr_client.py — Python client library for KronosScreenRemoteDaemon (KSCR protocol)

Protocol reference: docs/api.md
Daemon source:      source/screenremote.c

Usage examples at the bottom of this file.
"""
import socket
import struct
import time
import sys
from typing import Optional, Tuple

# ── Constants ─────────────────────────────────────────────────────────────────

KSCR_MAGIC    = b'KSCR'
VERSION       = 0x02
MODE_PULL     = 0x01   # client polls with 0xFF
MODE_CHANGE   = 0x02   # server pushes on framebuffer change
FPS_DEFAULT   = 15
FPS_MAX       = 15
DISC_PORT     = 7372   # UDP discovery (fixed)
STREAM_PORT   = 7373   # TCP stream (default; may differ per device config)
CTRL_PORT     = 7374   # TCP ctrl text (default)
MIDI_PORT     = 9875   # MIDI bridge (fixed, no auth)

STATUS_OK     = 0x00
STATUS_FAIL   = 0x01   # wrong password
STATUS_NOUSER = 0x02   # user not found

AUTH_RESP_LEN = 4 + 1 + 2 + 2 + 256 * 3  # = 777 bytes


# ── Helpers ───────────────────────────────────────────────────────────────────

def _recv_all(sock: socket.socket, n: int, timeout: float = 10.0) -> bytes:
    """Receive exactly n bytes, blocking until available or timeout."""
    sock.settimeout(timeout)
    buf = b''
    while len(buf) < n:
        chunk = sock.recv(n - len(buf))
        if not chunk:
            raise EOFError(f'connection closed after {len(buf)}/{n} bytes')
        buf += chunk
    return buf


def _make_auth_pkt(user: str, pwd: str, mode: int, fps: int) -> bytes:
    u = user.encode('ascii')
    p = pwd.encode('ascii')
    return KSCR_MAGIC + bytes([VERSION, mode, fps, len(u), len(p)]) + u + p


def packbits_decode(data: bytes, out_size: int) -> bytes:
    """Decode PackBits RLE (Apple/TIFF variant) as used by screenremote.
    - Header 0x00–0x7F: n+1 literal bytes follow
    - Header 0x81–0xFF: repeat next byte 257-n times
    - Header 0x80:      NOP
    """
    out = bytearray(out_size)
    si = di = 0
    while si < len(data) and di < out_size:
        h = data[si]; si += 1
        if 0x00 <= h <= 0x7f:
            n = h + 1
            out[di:di + n] = data[si:si + n]; si += n; di += n
        elif h >= 0x81:
            n = 257 - h
            b = data[si]; si += 1
            out[di:di + n] = bytes([b]) * n; di += n
        # 0x80 = NOP
    return bytes(out)


def frame_to_rgb(pixels: bytes, palette: bytes) -> bytes:
    """Convert 8bpp indexed-color framebuffer to packed RGB24.
    palette: 768 bytes (256 × 3, RGB8).
    """
    rgb = bytearray(len(pixels) * 3)
    for i, idx in enumerate(pixels):
        o = idx * 3
        rgb[i * 3]     = palette[o]
        rgb[i * 3 + 1] = palette[o + 1]
        rgb[i * 3 + 2] = palette[o + 2]
    return bytes(rgb)


# ── UDP Discovery ─────────────────────────────────────────────────────────────

def discover(host: str, port: int = DISC_PORT, timeout: float = 2.0) -> Optional[dict]:
    """Send KSCR? and return parsed discovery response dict, or None on timeout.

    Response fields: stream_port (SP), ctrl_port (CP), midi_loaded (MIDI).
    """
    s = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
    s.settimeout(timeout)
    try:
        s.sendto(b'KSCR?HELLO', (host, port))
        resp, _ = s.recvfrom(256)
        text = resp.decode('ascii', errors='replace').strip()
        # "KSCR SP=7373 CP=7374 MIDI=0"
        if not text.startswith('KSCR '):
            return None
        parts = {}
        for tok in text[5:].split():
            k, _, v = tok.partition('=')
            parts[k] = int(v) if v.isdigit() else v
        return parts
    except socket.timeout:
        return None
    finally:
        s.close()


# ── Auth response parser ──────────────────────────────────────────────────────

class AuthResponse:
    __slots__ = ('width', 'height', 'palette', 'frame_bytes')

    def __init__(self, raw: bytes):
        if raw[:4] != KSCR_MAGIC:
            raise ValueError(f'bad magic: {raw[:4]!r}')
        status = raw[4]
        if status != STATUS_OK:
            msgs = {STATUS_FAIL: 'wrong password', STATUS_NOUSER: 'user not found'}
            raise PermissionError(f'auth failed (status=0x{status:02x}): {msgs.get(status, "unknown")}')
        self.width   = struct.unpack_from('<H', raw, 5)[0]
        self.height  = struct.unpack_from('<H', raw, 7)[0]
        self.palette = raw[9:9 + 256 * 3]
        self.frame_bytes = self.width * self.height

    def __repr__(self):
        return f'<AuthResponse {self.width}x{self.height}>'


# ── Pull mode client ──────────────────────────────────────────────────────────

class KSCRPullClient:
    """MODE_PULL (0x01): client requests frames on-demand with 0xFF.

    Example:
        with KSCRPullClient('10.0.2.15') as c:
            frame = c.next_frame()
    """

    def __init__(self, host: str, port: int = STREAM_PORT,
                 user: str = 'kronos', pwd: str = 'kronos', fps: int = FPS_DEFAULT):
        self._sock = socket.socket(socket.AF_INET, socket.SOCK_STREAM)
        self._sock.connect((host, port))
        self._sock.sendall(_make_auth_pkt(user, pwd, MODE_PULL, fps))
        self._auth = AuthResponse(_recv_all(self._sock, AUTH_RESP_LEN))

    @property
    def width(self)       -> int:   return self._auth.width
    @property
    def height(self)      -> int:   return self._auth.height
    @property
    def palette(self)     -> bytes: return self._auth.palette
    @property
    def frame_bytes(self) -> int:   return self._auth.frame_bytes

    def next_frame(self, timeout: float = 10.0) -> bytes:
        """Send 0xFF, return raw 8bpp framebuffer (width × height bytes)."""
        self._sock.send(b'\xff')
        hdr = _recv_all(self._sock, 4, timeout)
        n = struct.unpack('<I', hdr)[0]
        if n != self.frame_bytes:
            raise ValueError(f'unexpected frame size {n}, expected {self.frame_bytes}')
        return _recv_all(self._sock, n, timeout)

    def close(self):
        self._sock.close()

    def __enter__(self):  return self
    def __exit__(self, *_): self.close()


# ── Change mode client ────────────────────────────────────────────────────────

class KSCRChangeClient:
    """MODE_CHANGE (0x02): server pushes frames on framebuffer change (up to fps).

    First frame arrives immediately after auth (no poll needed).
    Subsequent frames are full or dirty-rect PackBits delta packets.

    Example:
        with KSCRChangeClient('10.0.2.15') as c:
            while True:
                is_full, _ = c.recv_frame()
                # c.frame holds the current framebuffer state
    """

    def __init__(self, host: str, port: int = STREAM_PORT,
                 user: str = 'kronos', pwd: str = 'kronos', fps: int = FPS_DEFAULT):
        self._sock = socket.socket(socket.AF_INET, socket.SOCK_STREAM)
        self._sock.connect((host, port))
        self._sock.sendall(_make_auth_pkt(user, pwd, MODE_CHANGE, fps))
        self._auth = AuthResponse(_recv_all(self._sock, AUTH_RESP_LEN))
        self.frame = bytearray(self.frame_bytes)

    @property
    def width(self)       -> int:   return self._auth.width
    @property
    def height(self)      -> int:   return self._auth.height
    @property
    def palette(self)     -> bytes: return self._auth.palette
    @property
    def frame_bytes(self) -> int:   return self._auth.frame_bytes

    def recv_frame(self, timeout: float = 60.0) -> Tuple[bool, Optional[Tuple[int, int]]]:
        """Receive one frame packet and update self.frame.

        Returns:
            (is_full_frame, changed_rows)
            is_full_frame: True if full frame, False if dirty-rect delta
            changed_rows:  None for full frame, (first_row, row_count) for delta
        """
        hdr = _recv_all(self._sock, 4, timeout)
        payload_len = struct.unpack('<I', hdr)[0]

        if payload_len == self.frame_bytes:
            # Full frame
            pixels = _recv_all(self._sock, self.frame_bytes, timeout)
            self.frame[:] = pixels
            return True, None
        else:
            # Dirty-rect delta: 4B row coords + PackBits RLE
            row_hdr = _recv_all(self._sock, 4, timeout)
            first_row = struct.unpack_from('<H', row_hdr, 0)[0]
            row_count = struct.unpack_from('<H', row_hdr, 2)[0]
            rle_size  = payload_len - 4
            rle_data  = _recv_all(self._sock, rle_size, timeout)
            raw_size  = row_count * self.width
            pixels    = packbits_decode(rle_data, raw_size)
            start = first_row * self.width
            self.frame[start:start + raw_size] = pixels
            return False, (first_row, row_count)

    def close(self):
        self._sock.close()

    def __enter__(self):  return self
    def __exit__(self, *_): self.close()


# ── Ctrl text client ──────────────────────────────────────────────────────────

class KSCRCtrlClient:
    """Persistent control-text client on TCP 7374.

    MUST connect from the SAME IP as the active stream client.
    The server rejects all ctrl connections if no stream client is connected.

    Example:
        with KSCRPullClient(HOST) as stream:
            with KSCRCtrlClient(HOST) as ctrl:
                ctrl.button('ENTER')
                ctrl.touch(400, 300)
    """

    def __init__(self, host: str, port: int = CTRL_PORT):
        self._sock = socket.socket(socket.AF_INET, socket.SOCK_STREAM)
        self._sock.connect((host, port))
        self._sock.sendall(b'CTRL_PERSIST\n')
        # No response to auth

    def send_cmd(self, cmd: str, timeout: float = 3.0) -> str:
        """Send one command, return full response text."""
        self._sock.sendall((cmd + '\n').encode('ascii'))
        self._sock.settimeout(timeout)
        resp = b''
        try:
            while not resp.endswith(b'OK\n') and not resp.endswith(b'ERR\n'):
                chunk = self._sock.recv(1024)
                if not chunk:
                    break
                resp += chunk
        except socket.timeout:
            pass
        return resp.decode('ascii', errors='replace').strip()

    def button(self, name: str) -> str:
        return self.send_cmd(f'BUTTON {name}')

    def chord(self, *names: str, hold_ms: Optional[int] = None) -> str:
        """Press 2–8 buttons as a chord. Optional hold_ms pauses between press and release."""
        if hold_ms is not None:
            return self.send_cmd(f'CHORD {hold_ms} ' + ' '.join(names))
        return self.send_cmd('CHORD ' + ' '.join(names))

    def touch(self, x: int, y: int) -> str:
        return self.send_cmd(f'TOUCH {x} {y}')

    def touch_down(self, x: int, y: int) -> str:
        return self.send_cmd(f'TOUCH_DOWN {x} {y}')

    def touch_move(self, x: int, y: int) -> str:
        return self.send_cmd(f'TOUCH_MOVE {x} {y}')

    def touch_up(self, x: int, y: int) -> str:
        return self.send_cmd(f'TOUCH_UP {x} {y}')

    def padchord(self, pad_index: int, velocity: int) -> str:
        """Play (velocity 1-127) or release (velocity 0) one of the 8
        "Pads (touch to play)" chords. pad_index is 0-indexed (0 = on-screen
        "Pad 1"). See docs/api.md's PADCHORD section for why this bypasses
        TOUCH entirely."""
        return self.send_cmd(f'PADCHORD {pad_index} {velocity}')

    def padmap(self, pad_index: int, x0: int, y0: int, x1: int, y1: int) -> str:
        """Live-set pad_index's rectangular hit region in framebuffer pixel
        space (uncalibrated placeholders by default - see docs/api.md's
        PADMAP section). Takes effect immediately, no daemon restart."""
        return self.send_cmd(f'PADMAP {pad_index} {x0} {y0} {x1} {y1}')

    def padmap_list(self) -> str:
        """Return all 8 pads' current hit regions, one 'idx x0 y0 x1 y1' per line."""
        return self.send_cmd('PADMAP_LIST')

    def padmap_on(self) -> str:
        """Enable touch->PADCHORD auto-detection using the current PADMAP regions."""
        return self.send_cmd('PADMAP_ON')

    def padmap_off(self) -> str:
        """Disable touch->PADCHORD auto-detection (default state)."""
        return self.send_cmd('PADMAP_OFF')

    def last_touch(self) -> str:
        """Return the most recent touch's raw framebuffer pixel coordinates
        ('X=<x> Y=<y>') - use this to find real on-screen pad box positions
        during PADMAP calibration."""
        return self.send_cmd('LASTTOUCH')

    def wheel(self, direction: str) -> str:
        """direction: 'CW' or 'CCW'"""
        return self.send_cmd(f'WHEEL {direction}')

    def slider(self, n: int, value: int) -> str:
        """n: 1–8, value: 0–127"""
        return self.send_cmd(f'SLIDER {n} {value}')

    def vslider(self, value: int) -> str:
        """value: 0–127"""
        return self.send_cmd(f'VSLIDER {value}')

    def key(self, code: int, val: int) -> str:
        """code: 1–511, val: 0=release 1=press"""
        return self.send_cmd(f'KEY {code} {val}')

    def mirror(self, on: bool) -> str:
        return self.send_cmd('MIRROR_ON' if on else 'MIRROR_OFF')

    def refresh(self) -> str:
        """Force full-frame resend on next Change tick."""
        return self.send_cmd('REFRESH')

    def state(self) -> str:
        return self.send_cmd('STATE')

    def version(self) -> str:
        return self.send_cmd('VERSION')

    def sysinfo(self) -> str:
        return self.send_cmd('SYSINFO', timeout=5.0)

    def midi_status(self) -> str:
        return self.send_cmd('MIDI_STATUS')

    def midi_send(self, hex_bytes: str) -> str:
        """hex_bytes: space-separated or continuous hex, e.g. 'B0 07 64'"""
        return self.send_cmd(f'MIDI_SEND {hex_bytes}')

    def sysex(self, hex_bytes: str, timeout: float = 6.0) -> str:
        """Send SysEx, wait for response. hex_bytes must start with F0."""
        return self.send_cmd(f'SYSEX {hex_bytes}', timeout=timeout)

    def ss_timeout(self, seconds: int) -> str:
        return self.send_cmd(f'SS_TIMEOUT {seconds}')

    def close(self):
        self._sock.close()

    def __enter__(self):  return self
    def __exit__(self, *_): self.close()


# ── MIDI bridge client (port 9875) ───────────────────────────────────────────

class KSCRMidiBridge:
    """Raw MIDI hub on TCP port 9875. No authentication required.

    Up to 8 clients may connect simultaneously.
    - Kronos MIDI output is streamed as raw MIDI bytes (no framing).
    - Writing raw MIDI bytes to the socket injects them into the Kronos.

    The stream has no framing; parse as standard MIDI (status byte bit 7 set).
    Multiple output ports being active on the Kronos can cause duplicate events.

    Example:
        with KSCRMidiBridge('192.168.100.15') as m:
            m.send(bytes([0x90, 0x3C, 0x7F]))  # Note On: middle C, vel 127
            data = m.recv(timeout=0.5)
    """

    def __init__(self, host: str, port: int = MIDI_PORT):
        self._sock = socket.socket(socket.AF_INET, socket.SOCK_STREAM)
        self._sock.setsockopt(socket.IPPROTO_TCP, socket.TCP_NODELAY, 1)
        self._sock.connect((host, port))
        self._buf = b''

    def send(self, midi_bytes: bytes):
        """Inject raw MIDI bytes into the Kronos. Max 4096 bytes per call."""
        self._sock.sendall(midi_bytes)

    def recv(self, timeout: float = 1.0) -> bytes:
        """Read available MIDI output bytes from the Kronos (raw, unframed)."""
        self._sock.settimeout(timeout)
        data = b''
        try:
            while True:
                chunk = self._sock.recv(4096)
                if not chunk:
                    break
                data += chunk
        except socket.timeout:
            pass
        return data

    def recv_sysex(self, timeout: float = 5.0) -> Optional[bytes]:
        """Read MIDI stream until a complete F0…F7 SysEx message is captured.
        Returns the SysEx bytes including F0/F7, or None on timeout.
        Skips non-SysEx bytes before F0.
        """
        deadline = time.time() + timeout
        self._sock.settimeout(0.2)
        in_sysex = False
        msg = bytearray()
        while time.time() < deadline:
            try:
                chunk = self._sock.recv(4096)
            except socket.timeout:
                continue
            if not chunk:
                break
            for b in chunk:
                # Real-time bytes (0xF8-0xFF: clock, active sensing, etc.) may
                # appear anywhere in the stream, including mid-SysEx. midi_tcp
                # forwards them inline (spec-correct), so skip them here or they
                # corrupt the reassembled dump. Mirrors screenremote.c's parser.
                if b >= 0xF8:
                    continue
                if not in_sysex:
                    if b == 0xF0:
                        in_sysex = True
                        msg = bytearray([b])
                else:
                    msg.append(b)
                    if b == 0xF7:
                        return bytes(msg)
        return None

    def close(self):
        self._sock.close()

    def __enter__(self):  return self
    def __exit__(self, *_): self.close()


# ── One-shot ctrl (no CTRL_PERSIST) ──────────────────────────────────────────

def ctrl_oneshot(host: str, cmd: str, port: int = CTRL_PORT, timeout: float = 3.0) -> str:
    """Send a single command on a fresh connection (server closes after response).
    Works from any IP that matches the current stream client IP.
    """
    s = socket.socket(socket.AF_INET, socket.SOCK_STREAM)
    s.connect((host, port))
    s.sendall((cmd + '\n').encode('ascii'))
    s.settimeout(timeout)
    resp = b''
    try:
        while True:
            chunk = s.recv(1024)
            if not chunk:
                break
            resp += chunk
    except socket.timeout:
        pass
    s.close()
    return resp.decode('ascii', errors='replace').strip()


# ── Save frame as PPM ─────────────────────────────────────────────────────────

def save_ppm(path: str, pixels: bytes, width: int, height: int, palette: bytes):
    """Write an 8bpp frame to a PPM file for quick inspection."""
    with open(path, 'wb') as f:
        f.write(f'P6\n{width} {height}\n255\n'.encode())
        f.write(frame_to_rgb(pixels, palette))


# ── CLI test ──────────────────────────────────────────────────────────────────

if __name__ == '__main__':
    import argparse, os

    ap = argparse.ArgumentParser(description='KSCR protocol test client')
    ap.add_argument('host',              help='Kronos IP address')
    ap.add_argument('--port', '-p', type=int, default=STREAM_PORT)
    ap.add_argument('--ctrl-port',  type=int, default=CTRL_PORT)
    ap.add_argument('--mode',       choices=['pull', 'change'], default='pull')
    ap.add_argument('--fps',        type=int, default=FPS_DEFAULT)
    ap.add_argument('--save-ppm',   metavar='FILE', help='save frame as PPM')
    ap.add_argument('--change-frames', type=int, default=5,
                    help='number of change-mode frames to receive (default 5)')
    ap.add_argument('--button',     metavar='NAME', help='press a button via ctrl')
    ap.add_argument('--version',    action='store_true', help='query daemon version')
    ap.add_argument('--sysinfo',    action='store_true', help='query sysinfo')
    ap.add_argument('--discover',   action='store_true', help='UDP discovery')
    args = ap.parse_args()

    if args.discover:
        print(f'[UDP] Discovering {args.host}:{DISC_PORT}...')
        r = discover(args.host)
        print(f'[UDP] {r}' if r else '[UDP] no response')
        sys.exit(0)

    if args.mode == 'pull':
        print(f'[PULL] Connecting to {args.host}:{args.port}...')
        with KSCRPullClient(args.host, args.port, fps=args.fps) as c:
            print(f'[PULL] Auth OK: {c.width}x{c.height}, {c.frame_bytes} bytes/frame')
            print(f'[PULL] Palette[0]: R={c.palette[0]} G={c.palette[1]} B={c.palette[2]}')
            t0 = time.time()
            frame = c.next_frame()
            dt = time.time() - t0
            unique = len(set(frame))
            print(f'[PULL] Frame: {len(frame)} bytes in {dt:.3f}s, {unique} unique pixel values')
            if args.save_ppm:
                save_ppm(args.save_ppm, frame, c.width, c.height, c.palette)
                print(f'[PULL] Saved {args.save_ppm}')

    elif args.mode == 'change':
        print(f'[CHANGE] Connecting to {args.host}:{args.port}...')
        with KSCRChangeClient(args.host, args.port, fps=args.fps) as c:
            print(f'[CHANGE] Auth OK: {c.width}x{c.height}')
            for i in range(args.change_frames):
                t0 = time.time()
                is_full, rows = c.recv_frame(timeout=10.0)
                dt = time.time() - t0
                if is_full:
                    print(f'[CHANGE] Frame {i+1}: FULL {c.frame_bytes}B in {dt:.3f}s')
                else:
                    fr, rc = rows
                    print(f'[CHANGE] Frame {i+1}: DELTA rows {fr}–{fr+rc-1} in {dt:.3f}s')
            if args.save_ppm:
                save_ppm(args.save_ppm, bytes(c.frame), c.width, c.height, c.palette)
                print(f'[CHANGE] Saved {args.save_ppm}')

    if args.button or args.version or args.sysinfo:
        print(f'[CTRL] Connecting to {args.host}:{args.ctrl_port}...')
        # Need stream open first for IP check — open a background pull client
        bg = KSCRPullClient(args.host, args.port, fps=1)
        try:
            with KSCRCtrlClient(args.host, args.ctrl_port) as ctrl:
                if args.version:
                    print(f'[CTRL] VERSION: {ctrl.version()}')
                if args.sysinfo:
                    print(f'[CTRL] SYSINFO:\n{ctrl.sysinfo()}')
                if args.button:
                    resp = ctrl.button(args.button)
                    print(f'[CTRL] BUTTON {args.button}: {resp}')
        finally:
            bg.close()
