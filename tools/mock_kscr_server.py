#!/usr/bin/env python3
"""
mock_kscr_server.py — Minimal KSCR mock server for testing kscr_client.py

Implements:
  - TCP 7373: auth handshake, MODE_PULL frames, MODE_CHANGE frames
  - TCP 7374: ctrl commands (CTRL_PERSIST + one-shot)
  - UDP 7372: discovery

Frames contain a simple test pattern (palette index = row number mod 256).
Palette: greyscale (R=G=B=i for index i).

Usage:
    python3 mock_kscr_server.py [--host H] [--port P] [--ctrl-port CP] [--fps F]
"""
import socket
import struct
import threading
import time
import sys
import argparse
import hashlib

WIDTH  = 800
HEIGHT = 600
FRAME_BYTES = WIDTH * HEIGHT

# Greyscale palette: palette[i] = (i, i, i)
PALETTE = bytes([i for i in range(256) for _ in range(3)])  # 768 bytes

VALID_USER = 'kronos'
VALID_PASS = 'kronos'

# --- RLE / PackBits -------------------------------------------------------

def packbits_encode(data: bytes) -> bytes:
    """PackBits encoder (Apple/TIFF variant)."""
    out = bytearray()
    i = 0
    n = len(data)
    while i < n:
        # Try run
        run = 1
        while run < 128 and i + run < n and data[i + run] == data[i]:
            run += 1
        if run >= 2:
            out.append(257 - run)
            out.append(data[i])
            i += run
            continue
        # Literal run
        j = i + 1
        while j < n and j - i < 128:
            if j + 1 < n and data[j] == data[j + 1]:
                break
            j += 1
        lit = j - i
        out.append(lit - 1)
        out.extend(data[i:j])
        i = j
    return bytes(out)

# --- Frame generator -------------------------------------------------------

def make_frame(tick: int = 0) -> bytes:
    """Return an 800×600 8bpp test frame. tick changes one row color."""
    frame = bytearray(FRAME_BYTES)
    for row in range(HEIGHT):
        color = (row + tick) % 256
        frame[row * WIDTH:(row + 1) * WIDTH] = bytes([color]) * WIDTH
    return bytes(frame)


def make_dirty_rect(tick: int, prev_tick: int) -> tuple:
    """Return (first_row, row_count, rle_bytes) for changed rows between ticks."""
    first_row = prev_tick % HEIGHT
    row_count = min(5, HEIGHT - first_row)
    rows_data = bytearray()
    for row in range(first_row, first_row + row_count):
        color = (row + tick) % 256
        rows_data.extend(bytes([color]) * WIDTH)
    rle = packbits_encode(bytes(rows_data))
    return first_row, row_count, rle

# --- Authentication --------------------------------------------------------

def do_auth(conn: socket.socket) -> tuple:
    """Read and validate client hello. Returns (mode, fps) or raises."""
    conn.settimeout(5.0)

    def recv_exact(n):
        buf = b''
        while len(buf) < n:
            c = conn.recv(n - len(buf))
            if not c:
                raise ConnectionError('client disconnected')
            buf += c
        return buf

    hdr = recv_exact(9)
    magic   = hdr[:4]
    version = hdr[4]
    mode    = hdr[5]
    fps     = hdr[6]
    ulen    = hdr[7]
    plen    = hdr[8]

    if magic != b'KSCR':
        raise ValueError(f'bad magic {magic!r}')
    if version != 0x02:
        raise ValueError(f'unknown version {version}')

    user = recv_exact(ulen).decode('ascii', errors='replace')
    pwd  = recv_exact(plen).decode('ascii', errors='replace')

    if user == VALID_USER and pwd == VALID_PASS:
        status = 0x00
    else:
        status = 0x01  # auth failed
        rsp = b'KSCR' + bytes([status])
        conn.sendall(rsp)
        raise PermissionError(f'bad credentials user={user!r}')

    # Success response: KSCR + 0x00 + W_LE16 + H_LE16 + 768B palette
    rsp = (b'KSCR' +
           bytes([0x00]) +
           struct.pack('<H', WIDTH) +
           struct.pack('<H', HEIGHT) +
           PALETTE)
    conn.sendall(rsp)
    conn.settimeout(None)
    fps = min(fps, 15) if fps > 0 else 15
    return mode, fps

# --- Stream handler --------------------------------------------------------

def send_full_frame(conn: socket.socket, tick: int):
    frame = make_frame(tick)
    hdr = struct.pack('<I', FRAME_BYTES)
    conn.sendall(hdr + frame)


def send_delta(conn: socket.socket, tick: int, prev_tick: int):
    first_row, row_count, rle = make_dirty_rect(tick, prev_tick)
    payload_len = 4 + len(rle)  # first_row(2) + row_count(2) + rle
    hdr = struct.pack('<I', payload_len)
    rect = struct.pack('<HH', first_row, row_count)
    conn.sendall(hdr + rect + rle)


def handle_pull(conn: socket.socket, fps: int):
    tick = 0
    print(f'[stream] MODE_PULL connected')
    conn.settimeout(30.0)
    while True:
        try:
            b = conn.recv(1)
            if not b:
                break
            if b[0] != 0xFF:
                print(f'[stream] bad pull byte {b[0]:#x}, disconnecting')
                break
            send_full_frame(conn, tick)
            tick += 1
        except (ConnectionError, BrokenPipeError, socket.timeout):
            break
    print(f'[stream] MODE_PULL disconnected')


def handle_change(conn: socket.socket, fps: int):
    print(f'[stream] MODE_CHANGE connected at {fps} fps')
    interval = 1.0 / fps
    tick = 0
    # Send first full frame immediately
    send_full_frame(conn, tick)
    prev_tick = tick
    tick += 1
    while True:
        time.sleep(interval)
        try:
            # Send delta if small, else full
            first_row, row_count, rle = make_dirty_rect(tick, prev_tick)
            payload_len = 4 + len(rle)
            if payload_len < FRAME_BYTES:
                send_delta(conn, tick, prev_tick)
            else:
                send_full_frame(conn, tick)
            prev_tick = tick
            tick += 1
        except (ConnectionError, BrokenPipeError, OSError):
            break
    print(f'[stream] MODE_CHANGE disconnected')


def stream_worker(conn: socket.socket, addr):
    try:
        mode, fps = do_auth(conn)
        print(f'[stream] auth OK from {addr}, mode={mode}, fps={fps}')
        # Register allowed ctrl IP
        _ctrl_allowed_ip[0] = addr[0]
        if mode == 0x01:
            handle_pull(conn, fps)
        elif mode == 0x02:
            handle_change(conn, fps)
        else:
            print(f'[stream] unknown mode {mode}')
    except Exception as e:
        print(f'[stream] error from {addr}: {e}')
    finally:
        _ctrl_allowed_ip[0] = None
        conn.close()

# --- Ctrl handler ----------------------------------------------------------

_ctrl_allowed_ip = [None]  # mutable so stream_worker can update it
_version = '1.7.9b'
_mode = 0  # current Kronos mode

CTRL_COMMANDS = {
    'REFRESH', 'MIRROR_ON', 'MIRROR_OFF', 'STATE', 'VERSION', 'SYSINFO',
    'MIDI_STATUS', 'BUTTON', 'CHORD', 'TOUCH', 'TOUCH_DOWN', 'TOUCH_MOVE',
    'TOUCH_UP', 'WHEEL', 'SLIDER', 'VSLIDER', 'KEY', 'SS_TIMEOUT',
    'MIDI_SEND', 'SYSEX', 'CTRL_PERSIST',
}

MODE_BUTTONS = {'SETLIST': 1, 'COMBI': 2, 'PROGRAM': 3, 'SEQUENCE': 4,
                'SAMPLING': 5, 'GLOBAL': 6, 'DISK': 7}


def handle_ctrl_cmd(line: str) -> str:
    """Process one control command line, return response string."""
    global _mode
    line = line.strip()
    if not line:
        return ''
    parts = line.split()
    cmd = parts[0]

    if cmd == 'STATE':
        return f'MODE={_mode}\n'
    if cmd == 'VERSION':
        return f'VER={_version} BUILD=20260630-{_version}\n'
    if cmd == 'SYSINFO':
        return (
            'UPTIME=12345\n'
            'LOAD=0.05 0.03 0.01\n'
            'MEM_TOTAL_KB=1048576\n'
            'MEM_FREE_KB=512000\n'
            'MEM_AVAIL_KB=600000\n'
            'CPU_PCT=5\n'
            'CPU0_PCT=3\n'
            'CPU1_PCT=7\n'
            'DISK_FREE_MB=500\n'
            'DISK_TOTAL_MB=1000\n'
            'MODE=0\n'
            'OK\n'
        )
    if cmd == 'MIDI_STATUS':
        return 'MIDI_LOADED=0\nMIDI_IN=0\nMIDI_CAPTURE=0\nOK\n'
    if cmd == 'BUTTON' and len(parts) >= 2:
        name = parts[1]
        if name in MODE_BUTTONS:
            _mode = MODE_BUTTONS[name]
        return 'OK\n'
    if cmd in ('TOUCH', 'TOUCH_DOWN', 'TOUCH_MOVE', 'TOUCH_UP'):
        if len(parts) >= 3:
            return 'OK\n'
        return 'ERR\n'
    if cmd == 'CHORD' and len(parts) >= 3:
        return 'OK\n'
    if cmd == 'WHEEL':
        if len(parts) >= 2 and parts[1] in ('CW', 'CCW'):
            return 'OK\n'
        return 'ERR\n'
    if cmd == 'SLIDER':
        if len(parts) >= 3:
            try:
                n, v = int(parts[1]), int(parts[2])
                if 1 <= n <= 8 and 0 <= v <= 127:
                    return 'OK\n'
            except ValueError:
                pass
        return 'ERR\n'
    if cmd == 'VSLIDER':
        if len(parts) >= 2:
            try:
                v = int(parts[1])
                if 0 <= v <= 127:
                    return 'OK\n'
            except ValueError:
                pass
        return 'ERR\n'
    if cmd == 'KEY':
        if len(parts) >= 3:
            try:
                code, val = int(parts[1]), int(parts[2])
                if 1 <= code <= 511 and val in (0, 1):
                    return 'OK\n'
            except ValueError:
                pass
        return 'ERR\n'
    if cmd in ('MIRROR_ON', 'MIRROR_OFF', 'REFRESH'):
        return 'OK\n'
    if cmd == 'SS_TIMEOUT':
        if len(parts) >= 2:
            try:
                int(parts[1])
                return 'OK\n'
            except ValueError:
                pass
        return 'ERR\n'
    if cmd == 'MIDI_SEND':
        if len(parts) >= 2:
            return 'OK\n'
        return 'ERR\n'
    if cmd == 'SYSEX':
        if len(parts) >= 2:
            return 'ERR MIDI_NOT_LOADED\n'
        return 'ERR BAD_SYSEX\n'
    # Unknown command: no response, close in one-shot mode
    return ''


def ctrl_worker(conn: socket.socket, addr):
    ip = addr[0]
    allowed = _ctrl_allowed_ip[0]
    if allowed is not None and ip != allowed:
        print(f'[ctrl] rejected {ip} (allowed: {allowed})')
        conn.close()
        return

    conn.settimeout(1.0)
    buf = b''
    try:
        # Peek at first line
        while b'\n' not in buf:
            try:
                chunk = conn.recv(256)
                if not chunk:
                    return
                buf += chunk
            except socket.timeout:
                continue

        line_end = buf.index(b'\n')
        first_line = buf[:line_end].decode('ascii', errors='replace').strip()
        rest = buf[line_end + 1:]

        if first_line == 'CTRL_PERSIST':
            print(f'[ctrl] CTRL_PERSIST from {ip}')
            conn.settimeout(None)
            buf = rest
            while True:
                while b'\n' not in buf:
                    try:
                        chunk = conn.recv(2048)
                        if not chunk:
                            return
                        buf += chunk
                    except socket.timeout:
                        continue
                line_end = buf.index(b'\n')
                line = buf[:line_end].decode('ascii', errors='replace')
                buf = buf[line_end + 1:]
                resp = handle_ctrl_cmd(line)
                if resp:
                    print(f'[ctrl] {line.strip()!r} → {resp.strip()!r}')
                    conn.sendall(resp.encode())
        else:
            # One-shot
            resp = handle_ctrl_cmd(first_line)
            if resp:
                print(f'[ctrl] one-shot {first_line!r} → {resp.strip()!r}')
                conn.sendall(resp.encode())
    except Exception as e:
        print(f'[ctrl] error from {ip}: {e}')
    finally:
        conn.close()


# --- UDP discovery ---------------------------------------------------------

def udp_worker(host: str, stream_port: int, ctrl_port: int):
    s = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
    s.setsockopt(socket.SOL_SOCKET, socket.SO_REUSEADDR, 1)
    s.bind((host, 7372))
    print(f'[udp] discovery listening on {host}:7372')
    while True:
        try:
            data, addr = s.recvfrom(256)
            if data[:5] == b'KSCR?':
                resp = f'KSCR SP={stream_port} CP={ctrl_port} MIDI=0\n'
                s.sendto(resp.encode(), addr)
        except Exception as e:
            print(f'[udp] error: {e}')

# --- Main ------------------------------------------------------------------

def main():
    ap = argparse.ArgumentParser(description='Mock KSCR server for testing')
    ap.add_argument('--host',      default='127.0.0.1')
    ap.add_argument('--port',      type=int, default=7373)
    ap.add_argument('--ctrl-port', type=int, default=7374)
    ap.add_argument('--fps',       type=int, default=5)
    args = ap.parse_args()

    # UDP discovery thread
    t = threading.Thread(target=udp_worker, args=(args.host, args.port, args.ctrl_port), daemon=True)
    t.start()

    # TCP ctrl listener
    ctrl_sock = socket.socket(socket.AF_INET, socket.SOCK_STREAM)
    ctrl_sock.setsockopt(socket.SOL_SOCKET, socket.SO_REUSEADDR, 1)
    ctrl_sock.bind((args.host, args.ctrl_port))
    ctrl_sock.listen(8)
    print(f'[ctrl] listening on {args.host}:{args.ctrl_port}')

    def ctrl_accept():
        while True:
            try:
                conn, addr = ctrl_sock.accept()
                threading.Thread(target=ctrl_worker, args=(conn, addr), daemon=True).start()
            except Exception as e:
                print(f'[ctrl] accept error: {e}')

    threading.Thread(target=ctrl_accept, daemon=True).start()

    # TCP stream listener
    stream_sock = socket.socket(socket.AF_INET, socket.SOCK_STREAM)
    stream_sock.setsockopt(socket.SOL_SOCKET, socket.SO_REUSEADDR, 1)
    stream_sock.bind((args.host, args.port))
    stream_sock.listen(4)
    print(f'[stream] listening on {args.host}:{args.port}')
    print(f'[mock] Credentials: {VALID_USER}/{VALID_PASS}')
    print(f'[mock] Press Ctrl+C to stop')

    while True:
        try:
            conn, addr = stream_sock.accept()
            threading.Thread(target=stream_worker, args=(conn, addr), daemon=True).start()
        except KeyboardInterrupt:
            break
        except Exception as e:
            print(f'[stream] accept error: {e}')

    stream_sock.close()
    ctrl_sock.close()


if __name__ == '__main__':
    main()
