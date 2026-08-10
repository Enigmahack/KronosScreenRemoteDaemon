#!/usr/bin/env bash
set -euo pipefail

# build_via_home_build.sh - mirrors this repo onto /home/build (local ZFS,
# symlink-capable) and runs release.sh there, then syncs the generated
# artifacts back into this working tree.
#
# Why this exists: /home/share is a CIFS mount, which can't hold the
# symlinks kernel out-of-tree module builds create in their own M= build
# directory. /home/build/linux-kronos (the ABI-matched KDIR every module
# here must build against - see CLAUDE.md's "Development Environment"
# section) already lives on ZFS, but that alone doesn't help if the MODULE
# source directory being built (M=...) is itself still on /home/share.
# kronosology/ and KronosExtract/ already work around this by living on
# /home/build directly; this script gets the same result for this repo
# without moving its canonical, git-tracked location off /home/share -
# it stays the single source of truth, /home/build/KronosScreenRemoteDaemon
# is a disposable mirror that only exists to build in.
#
# Usage: ./build_via_home_build.sh [--debug]
#   (arguments are passed straight through to release.sh)

REPO_NAME="KronosScreenRemoteDaemon"
SRC_ROOT="$(cd "$(dirname "$0")" && pwd)"
BUILD_ROOT="/home/build/${REPO_NAME}"

if [ "$(basename "$SRC_ROOT")" != "$REPO_NAME" ]; then
    echo "ERROR: expected to run from a directory named ${REPO_NAME}, got ${SRC_ROOT}" >&2
    exit 1
fi

echo "=== Syncing ${SRC_ROOT} -> ${BUILD_ROOT} ==="
mkdir -p "$BUILD_ROOT"
rsync -a --delete \
    --exclude '/build/' \
    --exclude '/tools/PackageMaker/dist/' \
    --exclude '/tools/PackageMaker/payload/mnt/korg/rw/' \
    "$SRC_ROOT"/ "$BUILD_ROOT"/

echo ""
echo "=== Building in ${BUILD_ROOT} ==="
( cd "$BUILD_ROOT" && ./release.sh "$@" )

echo ""
echo "=== Syncing build artifacts back to ${SRC_ROOT} ==="

# Generated embedded-module headers (xxd -i output consumed by screenremote.c)
rsync -a "$BUILD_ROOT"/source/*_ko.h "$BUILD_ROOT"/source/midi_tcp_bin.h "$SRC_ROOT"/source/ 2>/dev/null || true

# Kernel modules themselves + the final daemon binary
for mod in vkbd_module midi_module nks4_inject_module eva_mode_module eva_mode_peek_module; do
    [ -d "$BUILD_ROOT/$mod" ] || continue
    rsync -a --include='*.ko' --include='*.o' --include='*.mod.c' --include='*.mod.o' \
        --exclude='*' "$BUILD_ROOT/$mod"/ "$SRC_ROOT/$mod"/
done
mkdir -p "$SRC_ROOT/build"
rsync -a --delete "$BUILD_ROOT"/build/ "$SRC_ROOT"/build/

# Release zip + installer/uninstaller package folders
mkdir -p "$SRC_ROOT/tools/PackageMaker/dist"
rsync -a --delete "$BUILD_ROOT"/tools/PackageMaker/dist/ "$SRC_ROOT"/tools/PackageMaker/dist/

echo ""
echo "=== Done - artifacts are back under ${SRC_ROOT} ==="
