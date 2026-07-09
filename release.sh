#!/usr/bin/env bash
set -euo pipefail

REPO_ROOT="$(cd "$(dirname "$0")" && pwd)"
SOURCE_DIR="$REPO_ROOT/source"
BUILD_DIR="$REPO_ROOT/build"
PAYLOAD_BIN="$REPO_ROOT/tools/PackageMaker/payload/mnt/korg/rw/screenremote/screenremote"
PACKAGER_DIR="$REPO_ROOT/tools/PackageMaker"
DIST_DIR="$PACKAGER_DIR/dist"

# --------------------------------------------------------------------------
# Build mode: release (default) or debug (--debug).  A debug build produces a
# visible GRUB menu + FTP-readable on-device diagnostics (via build_auto.py
# --debug) and labels its zip/title/output _debug so it is never mistaken for a
# release.  release.sh takes no other arguments (VERSION comes from source).
# --------------------------------------------------------------------------
DEBUG=0
for _arg in "$@"; do
    case "$_arg" in
        --debug)   DEBUG=1 ;;
        -h|--help) echo "Usage: $0 [--debug]"; exit 0 ;;
        *) echo "ERROR: unknown argument '$_arg' (expected --debug)" >&2
           echo "Usage: $0 [--debug]" >&2; exit 1 ;;
    esac
done
if [ "$DEBUG" = 1 ]; then
    MODE_LC="debug";   MODE_TC="Debug";   ZIP_SUFFIX="_debug"; TITLE_SUFFIX="_Debug"; DEBUG_FLAG="--debug"
else
    MODE_LC="release"; MODE_TC="Release"; ZIP_SUFFIX="";       TITLE_SUFFIX="";       DEBUG_FLAG=""
fi

# --------------------------------------------------------------------------
# 0. Extract version from screenremote.c
# --------------------------------------------------------------------------
VERSION=$(sed -n 's/.*SCREENREMOTE_VERSION "\([^"]*\)".*/\1/p' "$SOURCE_DIR/screenremote.c")
if [ -z "$VERSION" ]; then
    echo "ERROR: could not extract SCREENREMOTE_VERSION from screenremote.c" >&2
    exit 1
fi
echo "=== ScreenRemoteDaemon ${MODE_LC} v${VERSION} ==="

PKG_ID="ScreenRemote_${VERSION//./_}"
UNINST_ID="${PKG_ID}_Uninstall"
ZIP_NAME="ScreenRemoteDaemon_${VERSION}${ZIP_SUFFIX}.zip"

# --------------------------------------------------------------------------
# 1. Build screenremote binary (delegates to source/Makefile which builds
#    vkbd.ko, midi_bridge.ko, midi_tcp, and the final screenremote binary)
# --------------------------------------------------------------------------
echo ""
echo "--- Step 1: Building screenremote ---"

make -C "$SOURCE_DIR" clean
make -C "$SOURCE_DIR" all

file "$BUILD_DIR/screenremote"
echo "  Build complete: $BUILD_DIR/screenremote"

# --------------------------------------------------------------------------
# 1b. Boot-safety gate: the kernel modules ship EMBEDDED in the daemon, so a
#     stale or mis-patched .ko silently bricks a non-rooted unit at boot (the
#     kernel calls a wrong pointer at init_module time and the RTAI box
#     freezes).  `make clean` above now force-rebuilds the modules; verify the
#     freshly built .ko relocations land init_module at 0xd4 (the Kronos
#     2.6.32.11-korg struct module layout) and abort the release if not.
# --------------------------------------------------------------------------
echo ""
echo "--- Step 1b: Verifying embedded kernel modules ---"
python3 - "$REPO_ROOT/vkbd_module/vkbd.ko" "$REPO_ROOT/midi_module/midi_bridge.ko" <<'PYEOF'
import struct, sys
REQUIRED = 0xd4
def init_offset(path):
    d = open(path, "rb").read()
    e_shoff = struct.unpack_from("<I", d, 0x20)[0]
    ess     = struct.unpack_from("<H", d, 0x2e)[0]
    n       = struct.unpack_from("<H", d, 0x30)[0]
    sx      = struct.unpack_from("<H", d, 0x32)[0]
    secs = [list(struct.unpack_from("<IIIIIIIIII", d, e_shoff + i*ess)) for i in range(n)]
    shstr = secs[sx][4]
    name = lambda i: d[shstr+i:d.index(0, shstr+i)].decode()
    rel = next((s for s in secs if name(s[0]) == ".rel.gnu.linkonce.this_module"), None)
    if not rel:
        raise SystemExit("  FAIL: %s has no .rel.gnu.linkonce.this_module" % path)
    st = secs[rel[6]]; so = st[4]; se = st[9]; strt = secs[st[6]][4]
    def sname(i):
        ni = struct.unpack_from("<I", d, so + i*se)[0]
        return d[strt+ni:d.index(0, strt+ni)].decode()
    for j in range(rel[5] // 8):
        r_off, r_info = struct.unpack_from("<II", d, rel[4] + j*8)
        if sname(r_info >> 8) == "init_module":
            return r_off
    raise SystemExit("  FAIL: %s has no init_module relocation" % path)

ok = True
for path in sys.argv[1:]:
    off = init_offset(path)
    print("  %-42s init_module @ 0x%x  [%s]" % (path, off, "ok" if off == REQUIRED else "WRONG"))
    ok = ok and (off == REQUIRED)
if not ok:
    raise SystemExit("ERROR: a shipped module is not patched to init offset 0x%x "
                     "— would freeze a non-rooted Kronos at boot" % REQUIRED)
print("  All embedded modules verified.")
PYEOF

# --------------------------------------------------------------------------
# 2. Copy built binary into PackageMaker payload and run build_auto.py
#    (adds --debug in debug mode: visible GRUB menu + on-device diagnostics)
# --------------------------------------------------------------------------
echo ""
echo "--- Step 2: Building deployment package ---"

mkdir -p "$(dirname "$PAYLOAD_BIN")"
cp "$BUILD_DIR/screenremote" "$PAYLOAD_BIN"
echo "  Payload updated"

rm -rf "${DIST_DIR:?}/${PKG_ID}" "${DIST_DIR:?}/${UNINST_ID}"
python3 "$PACKAGER_DIR/build_auto.py" "$VERSION" ${DEBUG_FLAG}

if [ ! -d "$DIST_DIR/$PKG_ID" ] || [ ! -d "$DIST_DIR/$UNINST_ID" ]; then
    echo "ERROR: PackageMaker did not produce expected output" >&2
    ls "$DIST_DIR/" >&2
    exit 1
fi

# --------------------------------------------------------------------------
# 3. Zip installer + uninstaller into release archive
# --------------------------------------------------------------------------
echo ""
echo "--- Step 3: Creating release zip ---"

rm -f "$DIST_DIR/$ZIP_NAME"

python3 - "$DIST_DIR" "$PKG_ID" "$UNINST_ID" "$ZIP_NAME" <<'PYEOF'
import os, sys, zipfile
from pathlib import Path

dist_dir  = Path(sys.argv[1])
pkg_id    = sys.argv[2]
uninst_id = sys.argv[3]
zip_name  = sys.argv[4]

zip_path = dist_dir / zip_name
with zipfile.ZipFile(zip_path, "w", zipfile.ZIP_DEFLATED) as zf:
    for folder in (pkg_id, uninst_id):
        folder_path = dist_dir / folder
        for root, dirs, files in os.walk(folder_path):
            rel_root = Path(root).relative_to(dist_dir)
            zf.write(root, str(rel_root))
            for fname in sorted(files):
                fpath = Path(root) / fname
                zf.write(str(fpath), str(rel_root / fname))

size = zip_path.stat().st_size
print(f"  {zip_path.name}: {size:,} bytes")
PYEOF

# --------------------------------------------------------------------------
# 4. Create GitHub release (draft)
# --------------------------------------------------------------------------
echo ""
echo "--- Step 4: Creating GitHub release ---"

TAG="v${VERSION}"
REPO="Enigmahack/KronosScreenRemoteDaemon"

if ! command -v gh &>/dev/null; then
    echo "  Installing GitHub CLI ..."
    curl -fsSL https://cli.github.com/packages/githubcli-archive-keyring.gpg \
        | dd of=/usr/share/keyrings/githubcli-archive-keyring.gpg 2>/dev/null
    echo "deb [arch=$(dpkg --print-architecture) signed-by=/usr/share/keyrings/githubcli-archive-keyring.gpg] https://cli.github.com/packages stable main" \
        | tee /etc/apt/sources.list.d/github-cli.list > /dev/null
    apt-get update -qq && apt-get install -y -qq gh > /dev/null 2>&1
fi

if ! gh auth status &>/dev/null; then
    echo ""
    echo "  GitHub CLI is not authenticated."
    echo "  Authenticate with:  gh auth login"
    echo "  Then create the release manually:"
    echo ""
    echo "    gh release create $TAG '${DIST_DIR}/${ZIP_NAME}' \\"
    echo "      --repo $REPO --title 'ScreenRemoteDaemon${TITLE_SUFFIX} $TAG' \\"
    echo "      --generate-notes --draft"
    echo ""
    echo "=== Build complete (zip ready, GitHub release skipped) ==="
    exit 0
fi

if gh release view "$TAG" --repo "$REPO" &>/dev/null 2>&1; then
    echo "  Release $TAG exists — uploading asset ..."
    gh release upload "$TAG" "$DIST_DIR/$ZIP_NAME" --repo "$REPO" --clobber
else
    gh release create "$TAG" "$DIST_DIR/$ZIP_NAME" \
        --repo "$REPO" \
        --title "ScreenRemoteDaemon${TITLE_SUFFIX} ${TAG}" \
        --generate-notes \
        --draft
    echo "  Draft release created: $TAG"
fi

echo ""
echo "=== ${MODE_TC} v${VERSION} complete ==="
