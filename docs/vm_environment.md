# KronosScreenRemoteDaemon — VM Test Environment

This document records the complete state of the QEMU virtual machine used for
testing `screenremote`. It covers the host, the QEMU invocation, the guest
kernel and modules, the network stack, the screenremote daemon, and every
finding about what works and what does not. Use this as a reference when
comparing against other environments or when setting the VM up fresh.

---

## 1. Host environment

| Field | Value |
|---|---|
| Host OS | Linux 7.0.12-1-pve (Proxmox VE kernel, x86-64) |
| QEMU binary | `/usr/libexec/qemu-system-i386` version **7.2.22** |
| Acceleration | TCG only — KVM not available (`/dev/kvm` absent) |
| TUN/TAP | Not available (`/dev/net/tun` absent) |
| Loop devices | Blocked (`losetup` returns "Operation not permitted") |
| Working dir | QEMU launched from directory containing `kronos.img` |

### 1.1 QEMU command line (exact)

```
/usr/libexec/qemu-system-i386
  -M pc
  -cpu n270
  -m 1024M
  -smp 2
  -accel tcg
  -drive file=kronos.img,format=raw,if=ide,index=0,media=disk
  -display none
  -vga none
  -serial file:/root/.claude/jobs/840a85cf/tmp/serial6.log
  -serial tcp::4444,server,nowait
  -monitor unix:/root/.claude/jobs/840a85cf/tmp/qemu_monitor6.sock,server,nowait
  -net nic,model=rtl8139
  -net user,hostfwd=tcp::7373-:7373,hostfwd=tcp::7374-:7374
  -rtc base=utc
```

Key points:
- CPU model `n270` (Intel Atom N270) running under TCG software emulation
- 1 GiB RAM, 2 vCPUs configured (but guest disables SMP — see §3.2)
- Disk: raw IDE image `kronos.img`
- No display or VGA
- **ttyS0** → log file (boot messages, kernel console)
- **ttyS1** → TCP port 4444, `server,nowait` (interactive shell)
- QEMU monitor → Unix socket
- NIC: `rtl8139` model via SLIRP userspace networking
- Port-forwards: host 7373→guest 7373 (stream), host 7374→guest 7374 (ctrl)

### 1.2 Available QEMU NIC models

```
e1000  e1000-82544gc  e1000-82545em  e1000e  i82550  i82551
i82557a  i82557b  i82557c  i82558a  i82558b  i82559a  i82559b
i82559c  i82559er  i82562  i82801  ne2k_pci  pcnet  rtl8139
```

Note: `virtio-net-pci` is **not** available in this build.

---

## 2. Disk image (`kronos.img`)

- Format: raw
- Interface: IDE (`/dev/hda` inside guest)
- Partition layout (from kernel messages at boot):

| Device | Mount | FS | Mount options |
|---|---|---|---|
| `/dev/hda2` (= `/dev/root`) | `/` | ext2 | rw |
| `/dev/sda5` | `/korg/ro` | ext2 | ro |
| `/dev/sda6` | `/korg/rw` | ext3 | rw,noatime,commit=1,data=writeback |

> The kernel IDE driver registers the drive as `hda`; the kernel cmdline uses
> `root=/dev/hda2`. The `/proc/mounts` entries for `/korg/{ro,rw}` show `sda5`
> and `sda6` — these refer to the same physical image via the libata path.

---

## 3. Guest kernel

### 3.1 Kernel version and build

```
Linux version 2.6.32.11-korg (root@kronos) (gcc version 4.5.0 (GCC))
#31 SMP PREEMPT Thu Oct 10 08:41:36 JST 2024
```

- Architecture: i686 (32-bit x86), PAGE_OFFSET=0x40000000
- RTAI real-time patch: **I-pipe 2.6-03** (`I-pipe 2.6-03: pipeline enabled`)
- Preemption: PREEMPT (voluntary + RTAI)

### 3.2 Kernel command line (exact)

```
BOOT_IMAGE=/bzImage
root=/dev/hda2
max_loop=16
elevator=noop
loglevel=8
console=uart8250,io,0x3f8,115200n8
console=ttyS0,115200
8250.nr_uarts=4
nosmp
nmi_watchdog=0
vga=0x0103
video=vesafb
```

Key parameters:
- `nosmp` — SMP disabled; kernel boots with 1 CPU despite `-smp 2`
- `nmi_watchdog=0` — NMI watchdog off
- `console=ttyS0,115200` — primary console on ttyS0 (→ log file)
- `loglevel=8` — all kernel messages logged

### 3.3 Memory

```
Memory: 1033448k/1048448k available
  (3293k kernel code, 14248k reserved, 1227k data, 324k init, 0k highmem)
```

- Total RAM: 1024 MiB (1 GiB, from `-m 1024M`)
- Available to OS: ~1009 MiB
- No HIGHMEM

### 3.4 CPU

```
Detected 2294.055 MHz processor.
4588.11 BogoMIPS (lpj=2294055)
CPU: L1 I cache: 32K, L1 D cache: 32K
CPU: L2 cache: 4096K
CPU: L3 cache: 16384K
```

TCG emulates `n270` (Intel Atom N270). CPUID reported to guest reflects the
host CPU's capabilities, not actual N270 specs.

---

## 4. Boot process (loadoa / OA.si stub)

Boot uses a VM-specific stub (`/sbin/loadoa`) instead of the production OA.si.
The stub runs as `/etc/vm_init.sh`. Sequence on each clean boot:

```
INIT: Entering runlevel: 3

[loadoa] VM stub loadoa starting (no-RTAI mode)

[loadoa] fakefb: loading...
fakefb: init called
fakefb: fb_mem=8093f000
fakefb: registered as /dev/fb0 (800x600 8bpp)
[loadoa] fakefb: loaded OK

[loadoa] 8139cp: loading...
8139cp: 10/100 PCI Ethernet driver v1.3 (Mar 22, 2004)
eth0: RTL-8139C+ at 0x809ca000, 52:54:00:12:34:56, IRQ 10
8139cp 0000:00:02.0: setting latency timer to 64
eth0: link up, 100Mbps, full-duplex, lpa 0x05E1
[loadoa] 8139cp: insmod done
[loadoa] eth0: 10.0.2.15/24 up

GetPubIdMod: Unknown symbol stgNV2AC_sync_cmd
GetPubIdMod: Unknown symbol stgNV2AC_sync_read_cmd
[loadoa] GetPubIdMod: FAILED — /proc/id absent

[loadoa] fb1 -> fb0 symlink created

[loadoa] screenremote: launched PID=1120
/sbin/loadoa: line 100: head: command not found
[loadoa] VM stub loadoa done — exiting 0
```

Then OA.rc runs the standard Debian-style rc3.d (NIFPD, messagebus, avahi,
vsftpd — most fail with "Not a directory" because initscripts are absent).

### 4.1 Kill-switch flag

`/korg/rw/HD/_nomod` is present. This prevents screenremote from loading
optional kernel modules (`vkbd.ko`, `midi_inject.ko`). The flag does not affect
the screenremote binary itself; it just skips the module insmod calls.

### 4.2 GetPubIdMod failure

`GetPubIdMod.ko` fails to load because it imports
`stgNV2AC_sync_cmd`/`stgNV2AC_sync_read_cmd` which are only exported by
`OmapNKS4Module.ko` (the AT88 chip driver from real hardware). In the VM, these
symbols are absent and `/proc/id` is never created.

**Effect on screenremote auth:** When `KronosNet.conf` credentials match, auth
succeeds normally. If they don't match, the PublicID fallback fails (no
`/proc/id`). Tested credentials: user=`kronos` pass=`kronos` (from
`/korg/rw/Startup/KronosNet.conf`).

---

## 5. Loaded kernel modules (clean boot)

From `/proc/modules` on a clean boot:

```
8139cp   14125  0  - Live 0x809bf000
fakefb    1518  2  - Live 0x80939000
smsc7500 58207  0  - Live 0x8085a000
asix     12867  0  - Live 0x8083a000
usbnet   13225  1 asix,  Live 0x80829000
r8169    33679  0  - Live 0x80812000
mii       3768  4 8139cp,asix,usbnet,r8169,  Live 0x807ff000
```

- `8139cp` — RTL8139C+ NIC driver (PIO + C+ DMA mode). **TX broken** (see §7).
- `fakefb` — Fake framebuffer module; provides `/dev/fb0` and `/dev/fb1`
  (as a symlink) at 800×600 8bpp. Loaded from `/korg/rw/screenremote/fakefb.ko`.
- `smsc7500`, `asix`, `usbnet` — USB Ethernet drivers; preloaded from
  `/korg/rw/screenremote/` (not used in this VM — no USB Ethernet devices).
- `r8169` — Realtek Gigabit Ethernet driver; preloaded, no matching PCI device.
- `mii` — MII library, dependency of the above NIC drivers.

### 5.1 Module files in `/korg/rw/screenremote/`

```
-rwxr-xr-x  root  20568  Jun 30  8139cp.ko
-rw-r--r--  root  26612  Jun 30  8139too.ko
-rwxr-xr-x  root   4060  Jun 30  fakefb.ko
-rwxr-xr-x  root   9996  Jun 29  midi_inject.ko   (not loaded: _nomod flag)
-rwxr-xr-x  root   3848  Jun 30  vkbd.ko          (not loaded: _nomod flag)
```

### 5.2 8139too.ko — load failure

`8139too.ko` (PIO-mode RTL8139 driver, intended as TX-working alternative) is
present but **cannot be loaded** in this environment:

- `insmod /korg/rw/screenremote/8139too.ko` causes a kernel oops in
  `sysfs_add_file_mode` within `sys_init_module`
- Root cause: RTAI's I-pipe interrupt pipeline interferes with the sysfs
  registration path during module init for this driver
- Symptom: insmod process receives `Killed` signal; subsequent dmesg shows an
  oops backtrace including `__ipipe_handle_exception` and `sysfs_add_file_mode`
- The 8139cp driver does NOT exhibit this issue (it loads cleanly)

---

## 6. Network configuration (clean boot)

### 6.1 SLIRP topology

```
Host 127.0.0.1 ─── QEMU SLIRP ─── Guest 10.0.2.15/24
Gateway: 10.0.2.2   DNS: 10.0.2.3
```

### 6.2 Guest eth0 (after loadoa)

```
eth0      Link encap:Ethernet  HWaddr 52:54:00:12:34:56
          inet addr:10.0.2.15  Bcast:10.0.2.255  Mask:255.255.255.0
          UP BROADCAST RUNNING MULTICAST  MTU:1500  Metric:1
          Interrupt:10  Base address:0xa000
```

IP is assigned statically by loadoa (`ifconfig eth0 10.0.2.15 netmask 255.255.255.0 up`).

### 6.3 ARP state

```
IP address       HW type  Flags  HW address         Device
10.0.2.2         0x1      0x0    00:00:00:00:00:00  eth0
```

The gateway ARP entry (10.0.2.2) is permanently incomplete — no ARP request
can be sent because TX is broken (see §7).

---

## 7. NIC TX failure — root cause and evidence

### 7.1 Symptom

On every boot, eth0 TX remains permanently zero:

```
eth0: RX bytes=1288 (1.2 KiB)  TX bytes=0 (0.0 b)
      RX packets=28             TX packets=0
```

Any attempt to send data via eth0 produces (in dmesg):

```
8139cp: eth0: BUG! Tx Ring full when queue awake!
eth0: Transmit timeout, status  d   2b    0    0
```

### 7.2 Root cause

The `8139cp` Linux driver uses the RTL8139C+ **C+ DMA TX ring** (not the
legacy 4-descriptor PIO path used by `8139too`). QEMU 7.2.22's RTL8139
emulation under TCG does not properly clear the OWN bit in C+ TX descriptors
after "transmitting" a packet. As a result:

1. Guest writes packet descriptors to C+ TX ring (OWN=1)
2. QEMU "transmits" via SLIRP internally — but does not clear OWN
3. `rtl8139cp_tx_interrupt()` fires in guest, iterates all descriptors,
   finds all OWN=1 → declares ring full
4. TX queue is stopped permanently

### 7.3 What this means for host↔guest communication

| Direction | Path | Works? |
|---|---|---|
| Host → Guest (inbound) | SLIRP injects to NIC RX buffer | **YES** — NIC RX is unaffected |
| Guest → Host (outbound) | NIC TX → SLIRP → host socket | **NO** — C+ TX never completes |
| Guest loopback (10.0.2.15→10.0.2.15) | kernel loopback only | **YES** — bypasses NIC entirely |
| Serial port (ttyS1, port 4444) | QEMU serial device, not NIC | **YES** — independent of NIC |

**Practical consequence:** A TCP connection from host to guest port 7373
completes its handshake (SLIRP manages the SYN-ACK on behalf of the guest),
and the host can send data. However, responses from the screenremote daemon
(auth response, frames, ctrl replies) are written to the TCP socket, go to the
NIC TX ring, and are silently dropped. The host socket receives nothing.

### 7.4 Workarounds attempted (all failed)

| Attempt | Outcome |
|---|---|
| `rmmod 8139cp; insmod 8139too.ko use_io=1` | 8139too causes RTAI oops on insmod; shell process killed |
| Change QEMU NIC model to e1000/pcnet | No e1000 or pcnet32 driver in Kronos kernel |
| Enable KVM acceleration | `/dev/kvm` not available in host container |
| TUN/TAP networking | `/dev/net/tun` not available |
| Loop-mount disk image to inject NIC driver | `losetup` blocked by container security |
| QEMU monitor `device_del`/`device_add` hotplug | No alternate NIC driver to receive a new PCI device |

### 7.5 Recommended fix (requires host privileges or different environment)

- **Option A**: Launch QEMU with KVM: replace `-accel tcg` with `-accel kvm`. KVM's hardware interrupt delivery handles C+ TX correctly.
- **Option B**: Launch QEMU with a different NIC model **and** supply the matching driver module. The Kronos kernel has no e1000/pcnet/virtio drivers compiled in or available as `.ko` files.
- **Option C**: Use a QEMU build or patch that fixes the RTL8139 C+ TX descriptor OWN-bit handling in TCG mode.

---

## 8. Serial console access

### 8.1 ttyS0 — boot console (log file)

QEMU maps ttyS0 to a log file. All kernel boot messages and printk output go
here. The log is **not** interactive and is only readable from the host.

### 8.2 ttyS1 — interactive shell (port 4444)

QEMU maps ttyS1 to TCP port 4444 (`server,nowait`). The loadoa init script
spawns `/bin/sh -i` on ttyS1. This is the **only bidirectional communication
path** between host and guest (not affected by NIC TX).

**Shell environment:**
- `/bin/sh` → bash 3.2 (`GNU bash, version 3.2.57(1)-release (i486-pc-linux-gnu)`)
- `uname -a`: `Linux kronos 2.6.32.11-korg #31 SMP PREEMPT ... i686 i686 i386 GNU/Linux`
- Busybox not present; individual binaries in `/bin`, `/sbin`, `/usr/bin`
- **Available**: `bash`, `dd`, `ls`, `printf`, `echo`, `cat`, `find`, `grep`, `ps`, `ifconfig`, `route`, `netstat`, `insmod`, `rmmod`, `chmod`, `cp`, `sync`, `/dev/tcp`, `/dev/udp`
- **NOT available**: `head`, `tail`, `tee`, `awk`, `wc`, `od`, `hexdump`, `xxd`, `seq`, `nc`, `socat`, `python`, `base64`, `uudecode`, `lsmod`, `modinfo`

**Shell behaviour quirks:**
- `bash 3.2` does not support `read -N` (read exact N chars) — use `read -r -n 1`
- `dd if=/dev/fd/N` silently returns 0 bytes — use `<&N` redirect instead
- Variable substitution for `$'\r'` requires quoting care in this shell version

**Shell stability:** The shell (PID 1144 on a typical boot) dies if the kernel
oopses or if RTAI's scheduler kills the foreground process during module
operations. After a shell death, the only recovery is `system_reset` via the
QEMU monitor (port: Unix socket at `qemu_monitor6.sock`).

---

## 9. screenremote daemon

### 9.1 Binary

```
/korg/rw/screenremote/screenremote
  Size:    1,771,688 bytes
  Type:    ELF 32-bit LSB executable, Intel 80386, statically linked
  BuildID: 8852d5be014f8d31e4031c27181e174d866e32e9
  Source:  KronosScreenRemoteDaemon/source/screenremote.c v1.7.9b (2323 lines)
```

The binary in the VM is the current production build from the repository.
`build/screenremote` on the host has the same BuildID.

### 9.2 Configuration (`/korg/rw/screenremote/screenremote.cfg`)

```
stream_port=7373
ctrl_port=7374
```

No `bind_ip` option exists — the daemon always binds to the first non-loopback
LAN IPv4 address it finds via `getifaddrs()`. In the VM this is `10.0.2.15`.

### 9.3 Runtime state (clean boot)

- PID: 1120 (consistent across boots; may vary)
- State: `S (sleeping)` in `select()`
- Bound sockets:
  - fd 4 → socket [inode 128] = `10.0.2.15:7373` (TCP LISTEN, stream port)
  - fd 5 → socket [inode 129] = `10.0.2.15:7374` (TCP LISTEN, ctrl port)
  - fd 6 → socket [inode 130] = `0.0.0.0:7372` (UDP, discovery)
- fd 3 → `/dev/fb0` (open for framebuffer reads)
- stderr → `/dev/ttyS1` (log lines visible on serial shell)

### 9.4 Access log (`/korg/rw/screenremote/access.log`)

Empty on a clean boot. An entry is written for every authentication attempt
(success or failure) once a client connects on port 7373. Because host→guest
TCP produces no responses (NIC TX broken), no auth completes and the log stays
empty in normal VM operation.

### 9.5 Kill-switch behaviour

With `/korg/rw/HD/_nomod` present:
```
screenremote: kill-switch /korg/rw/HD/_nomod present — not loading any kernel modules (vkbd, midi_inject)
```
vkbd.ko and midi_inject.ko are not loaded. The `BUTTON`, `TOUCH`, `WHEEL`
commands still function via `/dev/rtf5` if it exists; if absent, the event
write silently fails but the daemon continues. The `KEY` command falls back to
uinput if `/proc/.vkbd` is absent.

### 9.6 MIDI bridge

`midi_inject.ko` is not loaded (kill-switch). The MIDI bridge subprocess
(`midi_tcp`) is not started. TCP port 9875 is not bound. `MIDI_STATUS` returns
`MIDI_LOADED=0`. `MIDI_SEND` and `SYSEX` return `ERR MIDI_NOT_LOADED`.

### 9.7 VGA mirror

`/korg/rw/screenremote/.mirror_enable` is absent on clean boot. `MIRROR_ON` /
`MIRROR_OFF` commands will fail to open `/dev/fb0_real` (only `/dev/fb0` exists
as a fakefb alias to fb1). Mirror functionality is effectively a no-op in the VM.

---

## 10. Framebuffer

| Property | Value |
|---|---|
| Device | `/dev/fb1` (required by screenremote) → symlinked from `/dev/fb0` |
| Driver | `fakefb.ko` (custom, from `/korg/rw/screenremote/fakefb.ko`) |
| Resolution | 800 × 600 pixels |
| Colour depth | 8 bpp (indexed, palette-based) |
| Stride | 800 bytes/line |
| Total frame | 480,000 bytes |
| Palette | 256-entry RGB8 table from `palette_data.h` (Kronos hardware palette) |
| fb_mem | 0x8093f000 (kernel virtual address) |

The fakefb module allocates a static 480,000-byte buffer and registers it as
both `/dev/fb0` and `/dev/fb1`. The init script creates the `/dev/fb0 → fb1`
symlink that screenremote requires.

Content of the framebuffer is initially zero (black). The Kronos UI would
normally write here; in the VM there is no UI process, so the framebuffer is
black unless manually written.

---

## 11. Protocol verification results

All findings in this section were obtained by running in-VM shell scripts that
connected to screenremote on the loopback address (10.0.2.15:7373), since
host→guest TCP produces no response data.

### 11.1 UDP discovery (port 7372)

| Field | Value |
|---|---|
| Request | `KSCR?` (5 bytes, any suffix accepted) |
| Response | `KSCR SP=7373 CP=7374 MIDI=0\n` |
| Source IP filter | None (binds INADDR_ANY) |

Verified from source: screenremote binds UDP to `INADDR_ANY:7372`.

### 11.2 TCP stream auth (port 7373)

| Field | Value |
|---|---|
| Client hello | `KSCR` + 0x02 + mode + fps + ulen + plen + user + pass |
| Auth response (success) | 777 bytes: `KSCR` + 0x00 + W_LE16 + H_LE16 + 768B palette |
| Width encoding | Little-endian 16-bit: 800 = `0x20 0x03` |
| Height encoding | Little-endian 16-bit: 600 = `0x58 0x02` |
| Auth failure | 5 bytes: `KSCR` + status (0x01=wrong pw, 0x02=no user) |
| Handshake timeout | 5 seconds |

Verified empirically from in-VM probes (`boundary_probe.sh`, `probe.sh`):
- MAGIC bytes 0–3: `4b 53 43 52` = "KSCR" ✓
- STATUS byte 4: `00` ✓
- WIDTH bytes 5–6: `20 03` = 800 LE ✓
- HEIGHT bytes 7–8: `58 02` = 600 LE ✓
- Palette bytes 9–776: 768 bytes ✓

### 11.3 TCP stream MODE_PULL (0x01)

| Field | Value |
|---|---|
| Frame request | Send `0xFF` |
| Frame response | `[len LE32][480000 bytes pixels]` |
| Frame size prefix | `00 53 07 00` = 480,000 = 0x75300 LE ✓ |
| Invalid byte | Any byte ≠ 0xFF disconnects client |

Verified: `ff_stream.bin` = 5,760,825 bytes = 777 (auth) + 12 × 480,004 (frames)

### 11.4 TCP stream MODE_CHANGE (0x02)

| Field | Value |
|---|---|
| First frame | Full frame sent immediately after auth |
| Full frame format | `[F LE32][F bytes]` where F=width×height |
| Delta frame format | `[len LE32][first_row LE16][row_count LE16][rle_bytes]` |
| Delta vs full | `len < F` → delta; `len == F` → full |
| PackBits encoding | Header 0x00–0x7F: literal run (n+1 bytes); 0x81–0xFF: repeat run (257-n times); 0x80: NOP |
| Max literal run | 128 bytes |
| Max repeat run | 128 repeats |
| FPS | Clamped to 15; 0 in client hello → uses 15 |

### 11.5 TCP ctrl (port 7374)

| Property | Value |
|---|---|
| Access control | Client IP must match stream client IP; rejected if no stream client |
| Command format | ASCII line, `\n` terminated, case-sensitive, UPPERCASE |
| One-shot | Default: send command, read response, server closes |
| Persistent | Send `CTRL_PERSIST\n` first, then commands inline |
| Commands verified | BUTTON, TOUCH, TOUCH_DOWN/MOVE/UP, WHEEL, SLIDER, VSLIDER, KEY, CHORD, REFRESH, MIRROR_ON/OFF, SS_TIMEOUT, STATE, VERSION, SYSINFO, MIDI_STATUS |

All commands return `OK\n` on success, `ERR\n` on bad args.
`SYSINFO` returns a multi-line block ending with `OK\n`.
`STATE` returns `MODE=<n>\n` (0=init, 1=Setlist…7=Disk).
`VERSION` returns `VER=<version> BUILD=<build_id>\n`.

---

## 12. Python tools (host-side)

### 12.1 `kscr_client.py`

Location: `KronosScreenRemoteDaemon/tools/kscr_client.py`

Full Python 3 client library implementing:
- `KSCRPullClient` — MODE_PULL stream client
- `KSCRChangeClient` — MODE_CHANGE stream client with delta frame reconstruction
- `KSCRCtrlClient` — CTRL_PERSIST control session (all commands)
- `KSCRMidiBridge` — TCP 9875 raw MIDI hub client
- `ctrl_oneshot()` — single one-shot ctrl command
- `discover()` — UDP discovery
- `packbits_decode()` — PackBits RLE decoder (source-verified correct)
- `save_ppm()` / `frame_to_rgb()` — frame output utilities

Protocol reference: `docs/api.md`

### 12.2 `mock_kscr_server.py`

Location: `KronosScreenRemoteDaemon/tools/mock_kscr_server.py`

Full mock KSCR server for host-side testing (not affected by NIC TX issue):
- Implements full auth + MODE_PULL + MODE_CHANGE + ctrl + UDP discovery
- Greyscale test pattern; delta frames with 5-row dirty regions
- Default listen: `127.0.0.1:7373` (stream), `127.0.0.1:7374` (ctrl)
- Credentials: `kronos` / `kronos`

Usage:
```sh
python3 mock_kscr_server.py &
python3 kscr_client.py 127.0.0.1 --mode pull --save-ppm /tmp/frame.ppm
python3 kscr_client.py 127.0.0.1 --button ENTER --version --sysinfo
```

### 12.3 End-to-end test results (against mock server)

All of the following verified passing:

| Test | Result |
|---|---|
| UDP discovery | `{'SP': 7373, 'CP': 7374, 'MIDI': 0}` ✓ |
| MODE_PULL auth (800×600, 777B response) | ✓ |
| Pull frame (480,000 bytes, LE32 header) | ✓ |
| Pull second frame (tick incremented) | ✓ |
| PPM save (1,440,000 byte RGB24 output) | ✓ |
| MODE_CHANGE first frame (full) | `(True, None)` ✓ |
| MODE_CHANGE delta frame (5 rows) | `(False, (0, 5))` ✓ |
| PackBits decode (all header ranges) | ✓ |
| CTRL one-shot VERSION | `VER=1.7.9b BUILD=...` ✓ |
| CTRL one-shot STATE / BUTTON mode change | ✓ |
| CTRL_PERSIST TOUCH/WHEEL/SLIDER/VSLIDER | ✓ |
| CTRL_PERSIST CHORD (2 buttons, hold_ms) | ✓ |
| CTRL_PERSIST SYSINFO (multi-line OK) | ✓ |
| CTRL_PERSIST MIDI_STATUS | ✓ |

---

## 13. Known divergences from real Kronos hardware

| Area | VM behaviour | Real Kronos behaviour |
|---|---|---|
| `/proc/id` | Absent (GetPubIdMod fails) | Present (AT88 PublicID) |
| PublicID auth fallback | Always fails | Works with correct PublicID |
| vkbd.ko / midi_inject.ko | Not loaded (`_nomod` flag) | Loaded normally |
| MIDI bridge port 9875 | Not bound | Bound, up to 8 clients |
| VGA mirror (fb0) | No-op (fakefb only) | Works (real VGA hardware) |
| Framebuffer content | Black (no UI process) | Live Kronos display |
| RTAI real-time domain | Present but no RT tasks | Full RTAI with RT tasks |
| NIC | TX completely broken | Full TX/RX |
| SMP | Disabled (`nosmp`) | 2 cores + HT (Kronos 1/X/2) |
| Audio | No audio hardware | USB audio, `/proc/KorgUsbAudio` |
| Touch FIFO (`/dev/rtf5`) | Absent | Present (RTAI FIFO) |
| AT88 chip | Absent | Present, accessed via OmapNKS4 |

---

## 14. Appendix: QEMU monitor access

The QEMU monitor is accessible via a Unix domain socket:

```
/root/.claude/jobs/840a85cf/tmp/qemu_monitor6.sock
```

Useful commands (note: monitor uses readline with ANSI echoing; strip escape
sequences from responses):

```
system_reset          # Hard-reset the guest (reboots)
info status           # "VM status: running" or "paused"
info network          # Show NIC and SLIRP configuration
sendkey alt-sysrq-b   # SysRq-B in guest (emergency reboot)
```

Python example:
```python
import socket, re
s = socket.socket(socket.AF_UNIX, socket.SOCK_STREAM)
s.connect('/path/to/qemu_monitor6.sock')
s.settimeout(2)
s.recv(4096)  # drain banner
s.sendall(b'system_reset\n')
time.sleep(0.5)
raw = s.recv(4096)
clean = re.sub(b'\x1b\\[[^a-zA-Z]*[a-zA-Z]', b'', raw).decode()
```
