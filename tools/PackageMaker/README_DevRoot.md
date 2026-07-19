# DevRoot — robust Kronos rooting + on-device developer environment

`build_root.py` is the **root builder** in this PackageMaker. It is the sibling of
`build_package.py` (ScreenRemote daemon), `build_unroot.py`, and `build_cleaner.py`,
and it produces a signed Korg USB update package that performs the full *clonos
substitution* — the same thing the legacy `KronosRootHack/` does — but reproducibly,
idempotently, and without the `timeout=0` brick risk.

It is the modern replacement for `KronosRootHack/`.

## What it installs

| Component | Location | Notes |
|---|---|---|
| busybox 1.23.2 | `/sbin/busybox` | root fs (needed at `init=` time, before `/korg/rw` mounts) |
| PID-1 + login | `/bin/init`, `/bin/login` → busybox | boots `init=/bin/init` |
| **applet farm** (349 links) | `/usr/bin/<applet>` → `../../sbin/busybox` | `tail head dmesg less find top watch xargs strings …` as bare commands |
| SSH | `/bin/dropbear` (`-F -R`), `scp`, `dropbearkey` | respawned by inittab |
| persistent host keys | `/etc/dropbear` → `/korg/rw/devroot/dropbear` | survive reboots **and** Korg OS updates |
| Tier-2 dev tools | `/korg/rw/devroot/bin/*` + `/usr/bin` symlinks | seeded with static `e2fsck`; add more (see below) |
| editor | `/bin/nano` | |
| boot scripts | `/etc/inittab.busybox`, `/etc/OA.clonos.si`, `/etc/OA.clonos.rc`, `/etc/profile` | **never** `/etc/inittab` |

The synth is untouched: `OA.clonos.rc` runs `/sbin/loadoa` exactly as the factory does.

### Why the applet farm is in `/usr/bin`, not `/bin`

A factory unit already has a real **GNU** userland in `/bin` (bash, sed, grep, gawk,
tar, gzip, …) but `/usr/bin` is **empty** and `tail`/`head`/`dmesg`/`less`/`find`/`top`
are missing entirely. DevRoot fills the gaps: the farm lives in `/usr/bin`, and
`/etc/profile` keeps `/bin` **first** on `PATH`, so the GNU tools are never shadowed —
you get busybox only for names the factory doesn't provide.

## Boot / brick safety (enforced, see CLAUDE.md + `kronos_factory_rootfs` memory)

- Boots `init=/bin/init` **directly** (never `/korg/kronos_init`, which is the
  ScreenRemote factory hook and must exec `/sbin/init`).
- Ships **only** `inittab.busybox`, never `/etc/inittab` (busybox init dies parsing
  the sysvinit inittab).
- **grub is patched in place** by mounting sda1 in `posttar` — never shipped as a
  wholesale `grub.conf`. The factory entry is kept as a **selectable fallback**, one
  new `init=/bin/init` entry is appended, `default` points at it, **`timeout=3`** and
  `hiddenmenu` is removed so a bad boot is recoverable from the menu (the legacy
  hack's `timeout=0` left no window).
- All grub / `OA.clonos.*` rewrites use a **same-directory** temp file
  (`/boot/grub/.grub_tmp`), never `/tmp` (cross-fs `mv` is not atomic; `/boot` is a
  separate vfat partition).
- The uninstaller removes the boot-critical `/bin/init` + `/sbin/busybox` **only**
  once `GRUB_OK` confirms `grub.conf` no longer execs `/bin/init`.

The install→uninstall grub round-trip is validated to return `grub.conf`
**byte-identical** to the factory file.

## Usage

```bash
cd KronosScreenRemoteDaemon/tools/PackageMaker
python3 build_root.py 1.0.0                       # release build (factory-quiet boot)
python3 build_root.py 1.0.0 --verbose-boot        # NOISY: loglevel=7, no fastboot, AND
                                                  #   syslog -> /korg/rw/HD/DevRoot/messages
                                                  #   (FTP-retrievable even if SSH is down)
python3 build_root.py 1.0.0 --authorized-keys id_ed25519.pub   # key-based SSH (embeds pubkey)
python3 build_root.py 1.0.0 --default-factory     # grub auto-boots FACTORY; DevRoot is
                                                  #   opt-in from the 3s menu (safer first test)
```

Each build also emits a `dist/<pkg>/rescue/` bundle (payload + `apply_devroot.sh`)
— the updater-independent rooting path, see below.

## Updater-independent rooting (when Global > Update System Software fails)

If the Korg updater path won't take the package, root the SSD directly from a
**KronosRescue Live USB**, which mounts the SSD read-write at `/mnt/root`,
`/mnt/boot`, `/mnt/root/korg/rw`:

- **Self-contained image**: build a rescue USB with the bundle baked in at
  `/opt/devroot`:
  ```bash
  cd KronosRescue
  DEVROOT_BUNDLE=../KronosScreenRemoteDaemon/tools/PackageMaker/dist/<pkg>/rescue \
    OUT=$PWD/kronos_devroot_rescue.img bash build_rescue_usb.sh
  ```
  Boot it → choose **Live Rescue** → `sh /opt/devroot/apply_devroot.sh` → reboot.
  (`DEVROOT_BUNDLE` is opt-in; unset, the build is the byte-for-byte plain rescue
  image. `OUT` keeps it from clobbering the committed `kronos_rescue.img`.)
- **Or with the existing rescue USB**: boot Live Rescue, copy the `rescue/` folder
  over its SSH from 192.168.100.10, and run `apply_devroot.sh` (see the bundle's
  `README.txt`).

`apply_devroot.sh` verifies the payload md5, refuses to run unless the SSD is
mounted correctly, extracts the **same** tarball the USB installer uses, and
applies the **identical** grub 2-entry transform to `/mnt/boot/grub/grub.conf`.

## KronosDoctor — all-in-one detect / repair / fsck / (un)root

`kronos_doctor.sh` (in every `rescue/` bundle and baked into the rooting USB at
`/opt/devroot/`, launcher `kronos_doctor`) is the interactive front-end. On the
rooting USB it **auto-starts the menu both locally and over SSH** — no remote
access required. The front panel's phantom keystrokes are a GRUB/BIOS-phase issue
(this initramfs never loads the panel driver), so once Linux is up the **local
console (with a USB keyboard) is clean** and fully usable; the menu is also
EOF-safe (no keyboard → it drops to a plain shell). SSH from another PC gives the
same menu on a guaranteed-clean terminal.

Menu actions: apply all recommended repairs, **select repairs individually**, run
**fsck** (conservative — see below), **root** the unit (runs `apply_devroot.sh`),
make it **boot factory** (drop the DevRoot grub entry, keep files), view grub,
shell, reboot, power off.

**fsck is conservative:** it unmounts the SSD, runs a **read-only** check
(`e2fsck -fn` on the ext partitions, `dosfsck -n` on sda1) that changes nothing,
reports any errors, prints the exact write-repair command, and only runs it if you
**confirm per-partition** — then re-checks. `--fsck` non-interactive never writes;
it reports and prints the command to run.

Non-interactive modes for scripting: `--report`, `--repair`, `--fsck`,
`--on-device`. `kronos_boot_doctor.sh` remains as the non-interactive engine.

It detects, from filesystem + grub markers, which of four states a unit is in and
cross-checks grub for brick-class inconsistencies:

| # | scenario | ROOTED | SCREENREMOTE |
|---|---|---|---|
| 1 | factory (unrooted) | no | no |
| 2 | rooted (DevRoot or legacy hack) | yes | no |
| 3 | factory + ScreenRemote | no | yes |
| 4 | rooted + ScreenRemote | yes | yes |

```sh
sh kronos_boot_doctor.sh            # report scenario + problems (rescue mounts)
sh kronos_boot_doctor.sh --repair   # safely normalise grub
sh kronos_boot_doctor.sh --on-device   # run against / and /boot on a booted unit
```

Report-only by default. `--repair` makes **atomic** fixes for the boot-preventing
problems it can:

- grub.conf **missing/empty** → rebuild from a `.bak`, else a factory template
- **CRLF** line endings (edited on Windows) → normalise to LF
- a **doubled** `init=A init=B` → collapse to the one that should win
- `init=/korg/kronos_init` whose **target is gone** → strip it (→ factory init)
- `init=/bin/init` whose **busybox is gone** → drop that entry (→ factory boot)
- rooted rootfs but grub **lost its `init=/bin/init` entry** → **rebuild** it
- ScreenRemote installed but the **hook is gone** → **restore** `init=/korg/kronos_init`
- an init target present but **not executable** → `chmod +x` (EACCES would fail PID 1)
- `default=` out of range / on a dead entry → point it at a bootable entry
- rooted **blind menu** (`timeout=0`+`hiddenmenu`) → `timeout>=3`, no `hiddenmenu`

It **reports but cannot fix** (needs a reinstall/restore): `/boot/bzImage` missing,
or `/etc/inittab.busybox` missing on a rooted unit. It never installs software —
use `apply_devroot.sh` / the Uninstaller for that; the doctor only makes the
existing install boot correctly.
```

Outputs two ready-to-copy packages under `dist/`:

- `Kronos_DevRoot_1_0_0/` — the installer
- `Kronos_DevRoot_1_0_0_Uninstall/` — the matching uninstaller

Copy a package's contents to the **root of a FAT32 USB stick** (`install.info`,
`*.tar.gz`, `pretar.sh`, `posttar.sh`, `md5sum`, `DisplayUpdaterMessage`, `mnt/`),
apply it from the Kronos OS-update screen, then reboot. Set a root password
(`passwd`) or use `--authorized-keys` on first login.

## Payload staging

Real binaries live under `payload_root/mnt/` (regular files; symlinks and config
files are generated at build time so this works on the CIFS-mounted repo):

```
payload_root/mnt/sbin/busybox                 busybox 1.23.2 (ABI 2.6.32)
payload_root/mnt/bin/{dropbear,dropbearkey,scp,nano}
payload_root/mnt/korg/rw/devroot/bin/<tool>   Tier-2 tools — drop new ones here
```

**To add a Tier-2 tool** (gdb, gdbserver, strace, git, tmux, rsync, full e2fsprogs):
build it as a **static i386** binary (see the busybox note below re. the kernel
gate), drop it in `payload_root/mnt/korg/rw/devroot/bin/`, and rebuild — the builder
auto-discovers it and creates the `/usr/bin` symlink. The applet farm is regenerated
from the staged busybox's own `--list`, so swapping the busybox binary updates the
farm automatically.

## busybox: why 1.23.2 (and the newer-busybox constraint)

The default is the in-repo **busybox 1.23.2** (ABI tag `2.6.32`) — proven on this
hardware, and already carrying every applet requested. A **newer** busybox is
possible but has one hard constraint:

> A modern busybox built with the host's Debian **glibc** gets ABI tag **3.2.0**
> and aborts *"FATAL: kernel too old"* on the Kronos's 2.6.32 kernel. It **must** be
> linked against **musl** (or an old glibc) instead — musl imposes no minimum-kernel
> gate.

An i386 musl toolchain has been built for exactly this at
`/home/build/devroot/musl-i386/bin/musl-gcc` (wrapper forces `-m32 -Wl,-melf_i386`).
A musl-static busybox 1.36.1 builds with it after disabling a few networking applets
that hit a musl↔kernel-header clash (`net/if.h` vs `linux/if.h`) — e.g. drop
`ETHER_WAKE` and the `libiproute`/`ip` family, or supply sanitized UAPI headers. Any
new busybox must be **boot-tested on the VM** before hardware regardless, so 1.23.2
remains the shipping default until that validation is done.

## Next step: VM boot test

Before any real hardware, boot-test the installer on the sandbox VM
(`kronosvm` @ .87): apply the package, confirm it boots `init=/bin/init`, SSH in,
spot-check farm commands (`dmesg | tail`, `find`, `top`), confirm `loadoa`/audio
comes up, then confirm the factory entry (entry 0) still boots as a fallback and the
uninstaller restores the factory grub.
