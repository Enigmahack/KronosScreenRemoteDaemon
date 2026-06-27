#!/usr/bin/env bash
set -euo pipefail

REPO_ROOT="$(cd "$(dirname "$0")" && pwd)"
SOURCE_DIR="$REPO_ROOT/source"
BUILD_DIR="$REPO_ROOT/build"
PAYLOAD_BIN="$REPO_ROOT/tools/PackageMaker/payload/mnt/korg/rw/screenremote/screenremote"
PACKAGER_DIR="$REPO_ROOT/tools/PackageMaker"
DIST_DIR="$PACKAGER_DIR/dist"

# --------------------------------------------------------------------------
# 0. Extract version from screenremote.c
# --------------------------------------------------------------------------
VERSION=$(sed -n 's/.*SCREENREMOTE_VERSION "\([^"]*\)".*/\1/p' "$SOURCE_DIR/screenremote.c")
if [ -z "$VERSION" ]; then
    echo "ERROR: could not extract SCREENREMOTE_VERSION from screenremote.c" >&2
    exit 1
fi
echo "=== ScreenRemoteDaemon debug v${VERSION} ==="

PKG_ID="ScreenRemoteDaemon_${VERSION//_debug./_}"
UNINST_ID="${PKG_ID}Daemon_Uninstall"
ZIP_NAME="ScreenRemoteDaemon_${VERSION}_debug.zip"

# --------------------------------------------------------------------------
# 1. Build screenremote binary (delegates to source/Makefile which builds
#    vkbd.ko, midi_inject.ko, midi_tcp, and the final screenremote binary)
# --------------------------------------------------------------------------
echo ""
echo "--- Step 1: Building screenremote ---"

make -C "$SOURCE_DIR" clean
make -C "$SOURCE_DIR" all

file "$BUILD_DIR/screenremote"
echo "  Build complete: $BUILD_DIR/screenremote"

# --------------------------------------------------------------------------
# 2. Copy built binary into PackageMaker payload and run build_auto.py
# --------------------------------------------------------------------------
echo ""
echo "--- Step 2: Building deployment package ---"

mkdir -p "$(dirname "$PAYLOAD_BIN")"
cp "$BUILD_DIR/screenremote" "$PAYLOAD_BIN"
echo "  Payload updated"

rm -rf "${DIST_DIR:?}/${PKG_ID}" "${DIST_DIR:?}/${UNINST_ID}"
python3 "$PACKAGER_DIR/build_auto.py" "$VERSION"

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
    echo "      --repo $REPO --title 'ScreenRemoteDaemon $TAG' \\"
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
        --title "ScreenRemoteDaemon_Debug ${TAG}" \
        --generate-notes \
        --draft
    echo "  Draft release created: $TAG"
fi

echo ""
echo "=== Debug v${VERSION} complete ==="
