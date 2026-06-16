# PackageMaker

Builds USB-installable update packages for the Korg Kronos using the same
`UpdateOS` mechanism Korg uses for official firmware updates. No root access
required on the target hardware.

---

## Requirements

Python 3.6 or later. No third-party packages — standard library only.

## Files

| File | Purpose |
|---|---|
| `build_package.py` | Interactive package builder. Run this to produce a USB package. |
| `sign_package.py` | Standalone signature generator. Re-sign or verify any package's scripts. |
| `payload/` | Your files, organised under the `mnt/` prefix (see below). Not committed — project-specific. |
| `dist/` | Output packages. Each build produces an installer + auto-generated uninstaller here. Not committed. |

---

## Quick Start

### 1. Organise your payload

Files must be placed under a `mnt/` prefix that maps to the Kronos filesystem root
This example uses the KSCR daemon (Kronos Screen Remote)

```
payload/
  mnt/
    korg/
      rw/
        screenremote/
          screenremote        ← your binary
        mymodule/
          mymodule.ko         ← kernel module
```

**Only `/korg/rw/` survives Korg OS updates.** Do not install to `/etc/`, `/usr/`, or
anywhere else — those paths are wiped on every official Korg firmware update.

### 2. Build the package

```
python build_package.py
```

Answer the prompts:

| Prompt | Notes |
|---|---|
| Package name | Shown on the Kronos display during install, e.g. `ScreenRemote` |
| Version | Shown on the Kronos display, e.g. `1.1.0` |
| Payload directory | Path to your `payload/` folder |
| Add boot hook? | `Y` to auto-start at boot; `N` for file-delivery only |
| Enable FTP boot logging? | `Y` to write daemon stdout/stderr to `/korg/rw/HD/` (FTP-accessible); defaults to `N` |

Auto-detection runs on the payload and shows what boot and uninstall commands will
be generated before you confirm.

The `md5sum` binary is read from `<payload_dir>/md5sum/md5sum` — place an i386 Linux
`md5sum` ELF there before building. If absent, the builder warns and `pretar.sh`
will abort the install.

### 3. Copy the output to USB

Format a USB drive as **FAT32**. Copy the entire contents of
`dist/<Name>_<Version>/` to the **root** of the drive:

```
install.info
pretar.sh
posttar.sh
<Name>_<Version>.tar.gz
md5sum
mnt/            ← empty directory, must exist
```

A matching uninstaller is also generated at `dist/<Name>_<Version>_Uninstall/`
and is a separate USB package.

### 4. Apply on the Kronos

1. Power on and wait for the Kronos to fully boot.
2. Insert the USB drive.
3. Go to **Global > Basic Setup > Load/Save** (or the OS update screen for your firmware).

4. The Kronos detects the package and displays `"<Name> <Version> update found"`.
5. Confirm and wait for the progress bar.
6. **Reboot** — the boot hook only takes effect after a full restart.

---

## How Auto-Detection Works

`build_package.py` scans files under `mnt/korg/rw/` and derives commands automatically:

| File pattern | Generated boot command | Generated uninstall command |
|---|---|---|
| `*.ko` | `insmod /korg/rw/... 2>/dev/null \|\| true` | `rmmod <name> 2>/dev/null \|\| true` |
| No extension (any path) | `/korg/rw/... &` | `kill $(pidof <name>) 2>/dev/null \|\| true` |

Files with a dot in their name (`.cfg`, `.txt`, `.so`, etc.) are treated as data — no
boot command is generated for them.

**Kernel modules — rooted vs non-rooted:** `insmod` and `rmmod` work correctly on a
rooted Kronos (Scenario 2). On a non-rooted Kronos (Scenarios 1 and 3) they are not
available, so the generated commands will silently fail. If your payload includes a
`.ko` and you need it to load on non-rooted units, your daemon must load it at startup
using the `init_module(2)` syscall directly (as `screenremote` does for `vkbd.ko`).

---

## Boot Hook — Three Scenarios

`posttar.sh` auto-detects which Kronos variant it is running on and installs the
appropriate hook. You do not need to choose manually.

### Scenario 1 — Factory (non-rooted) Kronos, standard grub.conf

**Detected by:** single `title` entry, `default 0`, no existing `init=` line.

`posttar.sh` appends `init=/korg/kronos_init` to the kernel line in `grub.conf`.
`kronos_init` lives at `/korg/kronos_init` on the root filesystem (sda2) — it cannot
live on `/korg/rw` (sda6) because that partition is not yet mounted when the kernel
execs the `init=` target. On boot, `kronos_init` mounts `/korg/rw`, runs
`kronosmods_init` (which insmods modules and starts daemons), then `exec`s `/sbin/init`
so normal Kronos startup continues unmodified.

**Gotcha:** `grub.conf` is reset on every official Korg OS update. Re-run the USB
package after any firmware update to restore the hook.

### Scenario 2 — Root-hacked Kronos (OA.clonos.rc present)

**Detected by:** both `/etc/OA.clonos.rc` **and** `/etc/clontab` exist.
`OA.clonos.rc` alone is not sufficient — it can be a ghost left behind after a
Korg firmware reinstall wipes `clontab`. Without `clontab`, `OA.clonos.rc` is
never invoked at boot, so injecting into it would have no effect.

`posttar.sh` injects a call to `/korg/rw/kronosmods_init` into `OA.clonos.rc`
immediately after the `STATUS=$?` line (i.e. after `loadoa` returns).

**Why this specific injection point:** Running anything concurrently with
`OA.clonos.rc` via `clontab` on the RTAI kernel disrupts `loadoa`. Injecting
after `STATUS=$?` is the only safe hook point that guarantees `loadoa` has
finished before any custom code runs.

`OA.clonos.rc` is not in Korg's update checksum list, so this injection
**survives Korg OS updates** — no re-deployment needed after firmware updates.

**Note:** Even when Scenario 2 fires, `posttar.sh` still mounts sda1 and checks
`grub.conf`. This handles the ghost-rooted case where a Korg firmware update has
reset `grub.conf` to factory state (no `init=/bin/init`), leaving the root-hack
files present but inactive via GRUB. In that case a GRUB patch is applied on top
of the OA.clonos.rc injection.

### Scenario 3 — Non-rooted Kronos with customised grub.conf

**Detected by:** anything that doesn't match scenario 1 (multiple entries,
non-zero default, or an existing `init=` parameter).

`posttar.sh` copies the current default boot entry, adds `init=/korg/kronos_init`
to the kernel line of the copy, appends it as a new `KronosMods Boot` entry, and
updates `default` to point to it. The original default index is saved to
`/korg/rw/.grub_orig_default` for the uninstaller.

**Gotcha:** Same as scenario 1 — `grub.conf` is reset by Korg OS updates.

### No hook found

If neither `OA.clonos.rc` nor `grub.conf` is present, `posttar.sh` emits a warning
to the kernel log (`/dev/kmsg`) and exits cleanly. Files are still installed.

---

## sign_package.py

A standalone tool for recomputing and verifying package signatures. Useful after
manually editing `pretar.sh` or `posttar.sh`.

```sh
# Print signature only
python sign_package.py dist/ScreenRemote_1_1_0/

# Verify against the SIGNATURE field in install.info
python sign_package.py dist/ScreenRemote_1_1_0/ --verify

# Recompute and write back to install.info
python sign_package.py dist/ScreenRemote_1_1_0/ --update

# Explicit script paths
python sign_package.py path/to/pretar.sh path/to/posttar.sh --verify
```

**--update and --verify can be combined.**

The signature algorithm is `SHA1(pretar.sh || posttar.sh || UpdaterScriptsKey)`.
`install.info` is not covered by the signature, so `VERSION`, `SOURCE`, and other
fields can be changed freely without re-signing.

---

## Installing on the Korg Kronos

************ WARNING *****************
I am not responsible if your Kronos stops booting. 
This is currently BETA, and comes with inherent risk. 
Use this software at your own risk. 
************ WARNING *****************

To install the Kronos ScreenRemote daemon (the thing that runs on the Kronos): 
Extract the zip file. There will be two folders: 
	ScreenRemoteBETA_version 
	ScreenRemoteBETA_version_Uninstall	

***Installing and Uninstalling follow the same process.***

Copy the contents of the folders to the root of a USB stick. 
You should see: 
	
	mnt/
	install.info
	md5sum
	posttar.sh
	pretar.sh
	ScreenRemoteBETA_version.tar.gz

Once those are on the root of the USB stick, put that into the Kronos. (Should be up and running already. If not, boot it up.)

Then, on the Kronos, go to: 
GLOBAL MODE
Under the Basic > Basic tab, tap the top-right drop down menu, and select "Update System Software"
Tap "OK"


It will scan for the updater files.
Tap OK or "Install" to install the update to the version number mentioned. This does not change your operating system. This installs the ScreenRemote software daemon. 


You will be asked to reboot. Do that once it is complete. 


Ensure your Kronos starts as normal. (It should)
If not, reboot and try again. If still not, you may need to re-run the DVD1 installer to re-load the OS. 
This should *not* be the case, however Korg has the right to alter their updating at any time, which can 
mitigate/break this process. 


---

## Gotchas and Failure Points

### USB drive not FAT32
UpdateOS will not detect the package. The Kronos does not mount NTFS or exFAT.
Format as FAT32 before copying files.

### Missing `mnt/` directory on the USB root
UpdateOS requires this empty directory to exist before it begins extraction. The
builder creates it in the output folder — do not omit it when copying to USB.

### Missing `md5sum` binary
`pretar.sh` uses an i386 Linux `md5sum` binary to verify the tarball before
extraction. The builder reads it from `<payload_dir>/md5sum/md5sum` and copies it
to the USB root automatically. If it is absent, the builder warns and `pretar.sh`
will fail the checksum check, calling `kill -9` on UpdateOS and aborting the
install. Place a Kronos-compatible i386 `md5sum` ELF at that path before building.

### Payload files outside `mnt/korg/rw/`
Files installed to `/etc/`, `/usr/`, `/bin/`, or anywhere else on the root
filesystem **will be wiped by the next Korg OS update.** Only `/korg/rw/` is
on a separate partition that Korg does not touch. The builder does not
enforce this — it is your responsibility to keep payload files under
`mnt/korg/rw/`.

### File named with a dot won't be auto-detected as executable
`_mode_for` uses `"." not in fname` to decide whether to set mode `0755`.
A binary named `my.tool` will be stored as `0644` and not auto-detected as a
daemon. Rename executables to have no extension, or they will not start at
boot and may not be runnable on the Kronos.

### Two binaries with the same name in different subdirectories
`kill $(pidof <name>)` in the uninstaller matches by binary name only. If two
of your daemons share a filename (e.g. `mnt/korg/rw/a/server` and
`mnt/korg/rw/b/server`), both will be killed on uninstall and only one will be
in `kronosmods_init`. Use distinct names.

### Boot hook installed but Kronos doesn't start the daemon after reboot
Most likely cause: the package was applied but the Kronos was not rebooted. The
boot hook (grub.conf patch or OA.clonos.rc injection) only takes effect on the
next full boot. A power-cycle is required, not just a soft restart of the Kronos
application.

### Scenario 1/3: hook lost after a Korg firmware update
`grub.conf` is assumed to be overwritten by every official Korg OS update. Re-apply the USB
package after updating firmware. Scenario 2 (root-hacked, OA.clonos.rc) does
not have this problem.

### Manually editing pretar.sh or posttar.sh
The SIGNATURE in `install.info` covers both script files. If you edit either
after the build, UpdateOS will reject the package with a signature error.
Re-sign with:

```sh
python sign_package.py dist/<package>/ --update
```

### `install.info` fields
`VERSION` is a display-only string — UpdateOS does not compare it against the
running firmware. It can be anything. `SOURCE` must match the tarball filename
exactly (case-sensitive). If you rename the tarball, update `SOURCE` accordingly;
the signature does not cover `install.info` so no re-signing is needed.

### Korg proprietary modules in payload
The builder checks for known Korg module filenames (`OA_real.ko`, `loadmod.ko`,
etc.) and warns before proceeding. Do not include these — redistributing them
likely violates Korg's copyright. Only ship code you own.

### Kronos shell is ash, not bash
`kronosmods_init`, `kronos_init`, `pretar.sh`, and `posttar.sh` all run under
busybox ash. Avoid bash-specific syntax (`[[ ]]`, arrays, `$'...'` strings,
`source`, etc.). The device also has no `nice`, `python`, `pip`, `modinfo`,
`journalctl`, or `systemctl`.

### Kernel module load order
The auto-detected `insmod` commands in `kronosmods_init` are ordered alphabetically
before daemon commands. On a rooted Kronos (Scenario 2) this works as expected. On a
non-rooted Kronos (Scenarios 1 and 3) `insmod` is not available and the commands
silently fail — if your daemon depends on a module, it must load it via `init_module(2)`
at startup rather than relying on the shell-level `insmod`.

### RTAI kernel timing
The Kronos runs Linux 2.6.32 with RTAI real-time patches. `kronosmods_init` runs
after `loadoa` returns (scenario 2) or as `init=` before `/sbin/init` (scenarios
1 & 3). In either case `/korg/rw` is already mounted. Do not assume network
availability, mounted USB drives, or other late-init resources when your daemon
starts.
