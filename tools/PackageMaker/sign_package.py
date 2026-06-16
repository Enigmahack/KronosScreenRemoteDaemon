#!/usr/bin/env python3
"""
Kronos package signature generator.

Computes SHA1(pretar.sh || posttar.sh || UpdaterScriptsKey) for a
Kronos USB update package.  Useful for re-signing manually edited scripts
or verifying an existing package's SIGNATURE field.

Usage:
    python sign_package.py <package_dir>
    python sign_package.py <pretar.sh> <posttar.sh>

    Add --update to write the computed signature back into install.info.
    Add --verify to compare against the existing SIGNATURE field.
"""

import hashlib
import sys
from pathlib import Path

UPDATER_SCRIPTS_KEY = bytes.fromhex("13d0afefe03c9b92162faeff775355e1")


def compute_signature(pretar: bytes, posttar: bytes) -> str:
    return hashlib.sha1(pretar + posttar + UPDATER_SCRIPTS_KEY).hexdigest()


def _resolve_paths(args: list) -> tuple:
    """Return (pretar_path, posttar_path, install_info_path) from args."""
    if len(args) == 1:
        d = Path(args[0])
        if not d.is_dir():
            sys.exit(f"Error: '{d}' is not a directory.")
        return d / "pretar.sh", d / "posttar.sh", d / "install.info"
    if len(args) == 2:
        pre, post = Path(args[0]), Path(args[1])
        return pre, post, pre.parent / "install.info"
    print(__doc__)
    sys.exit(1)


def main() -> None:
    if "--help" in sys.argv or "-h" in sys.argv or len(sys.argv) == 1:
        print(__doc__)
        sys.exit(0)

    positional = [a for a in sys.argv[1:] if not a.startswith("--") and a != "-h"]
    do_update  = "--update" in sys.argv
    do_verify  = "--verify" in sys.argv

    pre_path, post_path, info_path = _resolve_paths(positional)

    for p in (pre_path, post_path):
        if not p.is_file():
            sys.exit(f"Error: '{p}' not found.")

    sig = compute_signature(pre_path.read_bytes(), post_path.read_bytes())
    print(sig)

    if do_verify:
        if not info_path.is_file():
            sys.exit(f"Error: '{info_path}' not found — cannot verify.")
        existing = ""
        for line in info_path.read_text(encoding="ascii").splitlines():
            if line.startswith("SIGNATURE="):
                existing = line[len("SIGNATURE="):]
                break
        if not existing:
            print("VERIFY: no SIGNATURE field found in install.info")
        elif existing == sig:
            print("VERIFY: OK")
        else:
            print(f"VERIFY: MISMATCH\n  expected: {sig}\n  found:    {existing}")

    if do_update:
        if not info_path.is_file():
            sys.exit(f"Error: '{info_path}' not found — cannot update.")
        lines = info_path.read_text(encoding="ascii").splitlines(keepends=True)
        updated, replaced = [], False
        for line in lines:
            if line.startswith("SIGNATURE="):
                updated.append(f"SIGNATURE={sig}\n")
                replaced = True
            else:
                updated.append(line)
        if not replaced:
            updated.append(f"SIGNATURE={sig}\n")
        info_path.write_text("".join(updated), encoding="ascii", newline="\n")
        print(f"Updated: {info_path}")


if __name__ == "__main__":
    main()
