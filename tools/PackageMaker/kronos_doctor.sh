#!/bin/sh
# kronos_doctor.sh -- KronosDoctor: all-in-one boot diagnosis, repair, fsck and
# (un)root for the Korg Kronos, driven from the KronosRescue Live USB.
#
# Detects which of four scenarios a unit is in (from filesystem + grub markers
# that survive a corrupted grub.conf), reports boot-preventing problems, and lets
# you pick what to do:
#     1 factory   2 rooted   3 factory+ScreenRemote   4 rooted+ScreenRemote
#
# Modes:
#   (no args, interactive tty) -> the menu.   Over SSH you get a clean terminal;
#                                 the local Kronos console is swamped by the front
#                                 panel's phantom keystrokes, so DRIVE IT OVER SSH.
#   --report      print the report and exit (what the local console auto-shows)
#   --menu        force the interactive menu
#   --repair      non-interactive: apply every recommended repair, then exit
#   --fsck        non-interactive: unmount + fsck the SSD, then exit
#   --on-device   operate on / and /boot instead of the rescue mounts
#   --root/--boot PATH   override the rootfs / grub mount points
#
# It fixes GRUB + simple rootfs perms/markers; to actually (un)install DevRoot it
# calls apply_devroot.sh (rooting) sitting next to it, or drops the DevRoot grub
# entry (safe "boot factory" normalise).

# ---- configuration --------------------------------------------------------
ROOT=/mnt/root
BOOT=/mnt/boot
MODE=""
HERE=$(dirname "$0")
DEV_SDA1=/dev/sda1; DEV_SDA2=/dev/sda2; DEV_SDA5=/dev/sda5; DEV_SDA6=/dev/sda6

CANON_ROOT='	root (hd0,0)'
CANON_KERNEL='	kernel /bzImage root=/dev/sda2 max_loop=16 fbcon=map:0 memmap=384m vga=0x0303 loglevel=0 fastboot Single raid=noautodetect elevator=noop'
DEVROOT_TITLE='Kronos DevRoot (SSH + busybox)'

while [ $# -gt 0 ]; do
    case "$1" in
        --on-device) ROOT=/ ; BOOT=/boot ;;
        --root) ROOT="$2"; shift ;;
        --boot) BOOT="$2"; shift ;;
        --report) MODE=report ;;
        --menu)   MODE=menu ;;
        --repair) MODE=repair ;;
        --fsck)   MODE=fsck ;;
        -h|--help) sed -n '2,40p' "$0"; exit 0 ;;
        *) echo "unknown arg: $1" >&2; exit 2 ;;
    esac
    shift
done
[ "$ROOT" = "/" ] && ROOT=""
GRUB="$BOOT/grub/grub.conf"

say()  { echo "$*"; }
have() { [ -e "$1" ]; }
execu(){ [ -x "$1" ]; }
ask()  { printf "%s " "$1"; read _a; echo "$_a"; }
yes()  { case "$1" in y|Y|yes|YES) return 0;; *) return 1;; esac; }

# ---- detection ------------------------------------------------------------
detect() {
    ROOTED=0; SR=0; DEVROOT=0
    [ -f "$GRUB" ] && grep -q 'init=/bin/init' "$GRUB" 2>/dev/null && ROOTED=1
    { [ -f "$ROOT/etc/inittab.busybox" ] && have "$ROOT/sbin/busybox"; } && ROOTED=1
    have "$ROOT/korg/rw/devroot" && DEVROOT=1
    have "$ROOT/korg/kronos_init"                  && SR=1
    have "$ROOT/korg/rw/screenremote/screenremote" && SR=1
    have "$ROOT/korg/rw/kronosmods_init"           && SR=1
    [ -f "$GRUB" ] && grep -q 'init=/korg/kronos_init' "$GRUB" 2>/dev/null && SR=1

    if   [ $ROOTED = 1 ] && [ $SR = 1 ]; then SCEN=4; NAME="rooted + ScreenRemote"
    elif [ $ROOTED = 1 ];                then SCEN=2; NAME="rooted"
    elif [ $SR = 1 ];                    then SCEN=3; NAME="factory + ScreenRemote"
    else                                      SCEN=1; NAME="factory (unrooted)"
    fi
    ROOTKIND=""
    [ $ROOTED = 1 ] && { [ $DEVROOT = 1 ] && ROOTKIND="DevRoot" || ROOTKIND="legacy KronosRootHack"; }

    ROOT_OK=0; { have "$ROOT/bin/init" && have "$ROOT/sbin/busybox"; } && ROOT_OK=1
    SR_OK=0;   have "$ROOT/korg/kronos_init" && SR_OK=1
    KERNEL_OK=0; have "$BOOT/bzImage" && KERNEL_OK=1
}

# ---- diagnostics: sets EN_<flag> for repairable issues + PROB_* text -------
diagnose() {
    # reset flags
    EN_crlf=0 EN_perms=0 EN_reconstruct=0 EN_collapse=0 EN_dedup=0 EN_strip_ki=0
    EN_drop_bi=0 EN_rebuild_bi=0 EN_add_sr=0 EN_menufix=0 EN_default=0
    PROBLEMS=0; FATAL=0; ISSUES=""; WARNONLY=""; FATALS=""
    # gate-able (user-selectable) repairs; collapse/default/menufix always ride
    # along with any grub rewrite (you can't safely rewrite entries and leave the
    # header pointing at the wrong one), so they are not separately deselectable.
    GATEABLE=" perms reconstruct crlf strip_ki drop_bi rebuild_bi add_sr dedup "
    add() { PROBLEMS=$((PROBLEMS+1)); ISSUES="$ISSUES $1"; eval "EN_$1=1; DESC_$1=\"\$2\""; }
    warn(){ PROBLEMS=$((PROBLEMS+1)); WARNONLY="$WARNONLY|$1"; }
    fat(){  PROBLEMS=$((PROBLEMS+1)); FATAL=$((FATAL+1)); FATALS="$FATALS|$1"; }

    # rootfs-level
    if [ ! -f "$ROOT/etc/inittab.busybox" ] && [ $ROOTED = 1 ] && have "$ROOT/sbin/busybox"; then
        warn "rooted but /etc/inittab.busybox is MISSING -> no SSH/loadoa (re-run apply_devroot; not grub-repairable)"
    fi
    for t in "$ROOT/bin/init" "$ROOT/sbin/busybox" "$ROOT/korg/kronos_init"; do
        have "$t" && ! execu "$t" && add perms "an init/busybox target is not executable -> chmod 755 it"
    done

    if [ ! -f "$GRUB" ] || ! grep -q '^[[:space:]]*title ' "$GRUB" 2>/dev/null; then
        add reconstruct "grub.conf missing/empty/no entries -> rebuild from .bak or template"
        FATAL=$((FATAL+1))
        # a reconstructed grub on a rooted unit must also get its DevRoot entry back
        if [ "$ROOT_OK" = 1 ] && [ -f "$ROOT/etc/inittab.busybox" ]; then
            add rebuild_bi "rooted unit with no grub -> also rebuild its init=/bin/init entry"
        fi
    else
        NENT=$(grep -c '^[[:space:]]*title ' "$GRUB")
        DEF=$(awk -F= '/^[[:space:]]*default=/{print $2+0; exit}' "$GRUB"); DEF=${DEF:-0}
        TMO=$(awk -F= '/^[[:space:]]*timeout=/{print $2+0; exit}' "$GRUB"); TMO=${TMO:-0}
        HID=$(grep -q '^hiddenmenu' "$GRUB" 2>/dev/null && echo 1 || echo 0)
        grep -q "$(printf '\r')" "$GRUB" 2>/dev/null && add crlf "grub.conf has DOS/CRLF line endings -> normalise to LF"
        [ "$DEF" -ge "$NENT" ] 2>/dev/null && add default "default=$DEF is out of range ($NENT entries) -> point at a bootable entry"
        grep -qE 'kernel .*init=[^ ]+ .*init=' "$GRUB" 2>/dev/null && add collapse "a kernel line has TWO init= parameters -> collapse to one"
        [ "$(grep -c 'init=/bin/init' "$GRUB" 2>/dev/null)" -gt 1 ] 2>/dev/null && add dedup "more than one init=/bin/init entry -> keep one, drop duplicates"
        if grep -q 'init=/bin/init' "$GRUB" 2>/dev/null && [ "$ROOT_OK" = 0 ]; then
            add drop_bi "grub boots init=/bin/init but busybox is MISSING -> drop that entry (unit boots factory)"
        fi
        if grep -q 'init=/korg/kronos_init' "$GRUB" 2>/dev/null && ! have "$ROOT/korg/kronos_init"; then
            add strip_ki "init=/korg/kronos_init target MISSING -> strip it (entry boots factory /sbin/init)"
        fi
        if [ "$ROOT_OK" = 1 ] && [ -f "$ROOT/etc/inittab.busybox" ] && ! grep -q 'init=/bin/init' "$GRUB" 2>/dev/null; then
            add rebuild_bi "rooted rootfs but grub has NO init=/bin/init entry -> rebuild it"
        fi
        if [ $SR = 1 ] && [ "$SR_OK" = 1 ] && ! grep -q 'init=/korg/kronos_init' "$GRUB" 2>/dev/null; then
            add add_sr "ScreenRemote installed but no init=/korg/kronos_init hook -> restore it on the factory entry"
        fi
        if grep -q 'init=/bin/init' "$GRUB" 2>/dev/null && { [ "$TMO" -lt 3 ] 2>/dev/null || [ "$HID" = 1 ]; }; then
            add menufix "rooted menu is hard to recover (timeout=$TMO,hidden=$HID) -> timeout>=3, show menu"
        fi
        if [ "$KERNEL_OK" = 0 ] && grep -qE '^[[:space:]]*kernel +/bzImage' "$GRUB" 2>/dev/null; then
            fat "grub loads /bzImage but $BOOT/bzImage is MISSING (use Restore DVD / autofix; not grub-repairable)"
        fi
    fi
}

# ---- report ---------------------------------------------------------------
print_report() {
    say "================ KronosDoctor ================"
    say "  root fs (sda2): ${ROOT:-/}      grub (sda1): $GRUB"
    [ -f "$GRUB" ] || say "  !! grub.conf not found -- is sda1 mounted at $BOOT ?"
    say ""
    say "  SCENARIO $SCEN: $NAME${ROOTKIND:+  [$ROOTKIND]}"
    say "    ROOTED=$ROOTED  SCREENREMOTE=$SR   (busybox:$(have "$ROOT/sbin/busybox" && echo y||echo n) inittab.busybox:$(have "$ROOT/etc/inittab.busybox" && echo y||echo n) kronos_init:$(have "$ROOT/korg/kronos_init" && echo y||echo n) bzImage:$([ "$KERNEL_OK" = 1 ] && echo y||echo n))"
    say ""
    if [ "$PROBLEMS" = 0 ]; then
        say "  [ok] no boot-preventing problems detected."
    else
        say "  problems detected ($PROBLEMS):"
        n=0
        for f in $ISSUES; do n=$((n+1)); eval "d=\$DESC_$f"; say "    $n) [$f] $d"; done
        [ -n "$WARNONLY" ] && printf '%s' "$WARNONLY" | tr '|' '\n' | while read w; do [ -n "$w" ] && say "    [!] $w (not auto-repairable)"; done
        [ -n "$FATALS" ]   && printf '%s' "$FATALS"   | tr '|' '\n' | while read w; do [ -n "$w" ] && say "    [XX] $w"; done
    fi
    say ""
    case $SCEN in
      1) say "  recommendation: factory. To root: apply DevRoot (menu option R)." ;;
      2) say "  recommendation: rooted ($ROOTKIND). Boots init=/bin/init." ;;
      3) say "  recommendation: factory + ScreenRemote. To root: apply DevRoot (menu option R)." ;;
      4) say "  recommendation: rooted + ScreenRemote." ;;
    esac
    say "=============================================="
}

# ---- repair engine (acts on whichever EN_<flag> are set) -------------------
do_repair() {
    say "  applying repairs:$(for f in crlf perms reconstruct collapse dedup strip_ki drop_bi rebuild_bi add_sr menufix default; do eval "v=\$EN_$f"; [ "$v" = 1 ] && printf ' %s' "$f"; done)"
    mkdir -p "$BOOT/grub" 2>/dev/null || true
    TMP="$BOOT/grub/.grub_tmp"

    # perms
    if [ "$EN_perms" = 1 ]; then
        for t in "$ROOT/bin/init" "$ROOT/sbin/busybox" "$ROOT/korg/kronos_init"; do
            have "$t" && ! execu "$t" && chmod 755 "$t" 2>/dev/null && say "    chmod +x $t"
        done
    fi
    # reconstruct missing grub
    if [ "$EN_reconstruct" = 1 ]; then
        SEED=""
        for b in "$BOOT/grub/grub.conf.bootfix.bak" "$BOOT/grub/grub.conf.bak" "$BOOT/grub/menu.lst"; do
            [ -f "$b" ] && grep -q '^[[:space:]]*title ' "$b" 2>/dev/null && { SEED="$b"; break; }
        done
        if [ -n "$SEED" ]; then say "    restoring grub.conf from $SEED"; cp "$SEED" "$GRUB"
        else say "    writing template factory grub.conf"; { echo "title Kronos (factory)"; echo "$CANON_ROOT"; echo "$CANON_KERNEL"; } > "$GRUB"; fi
    fi
    [ -f "$GRUB" ] || { say "    no grub.conf to edit"; return 1; }
    # crlf (always safe to run if requested)
    [ "$EN_crlf" = 1 ] && { tr -d '\r' < "$GRUB" > "$TMP.lf" && mv "$TMP.lf" "$GRUB"; say "    normalised CRLF -> LF"; }

    # per-entry awk pass
    awk -v strip_ki="$EN_strip_ki" -v drop_bi="$EN_drop_bi" -v add_sr="$EN_add_sr" -v dedup="$EN_dedup" '
        function flush(){
            if(!intitle) return
            isdev = (buf ~ /init=\/bin\/init/)
            if(drop_bi && isdev) return
            if(dedup && isdev && seendev) return
            if(isdev) seendev=1
            printf "%s", buf
        }
        BEGIN{ intitle=0; seendev=0 }
        /^[[:space:]]*title /{ flush(); buf=$0 ORS; intitle=1; next }
        {
            if(intitle){
                line=$0
                if(line ~ /^[[:space:]]*kernel /){
                    if(strip_ki) gsub(/ +init=\/korg\/kronos_init/,"",line)
                    if(line ~ /init=\/bin\/init/){ gsub(/ +init=[^ ]+/,"",line); line=line" init=/bin/init" }
                    else if(add_sr && line !~ /init=\/korg\/kronos_init/){ line=line" init=/korg/kronos_init" }
                }
                buf=buf line ORS; next
            }
            next
        }
        END{ flush() }
    ' "$GRUB" > "$TMP.entries" 2>/dev/null

    # rebuild missing DevRoot entry
    if [ "$EN_rebuild_bi" = 1 ] && ! grep -q 'init=/bin/init' "$TMP.entries" 2>/dev/null; then
        B_ROOT=$(awk '/^[[:space:]]*root /{print; exit}' "$TMP.entries"); [ -z "$B_ROOT" ] && B_ROOT="$CANON_ROOT"
        B_KERN=$(awk '/^[[:space:]]*kernel /{gsub(/ +init=[^ ]+/,""); print; exit}' "$TMP.entries"); [ -z "$B_KERN" ] && B_KERN="$CANON_KERNEL"
        { echo "title $DEVROOT_TITLE"; echo "$B_ROOT"; echo "$B_KERN init=/bin/init"; } >> "$TMP.entries"
        say "    rebuilt the init=/bin/init (DevRoot) entry"
    fi

    # header: default -> bootable (prefer DevRoot when rooted), recoverable menu if rooted
    NKEEP=$(grep -c '^[[:space:]]*title ' "$TMP.entries")
    if [ "$ROOT_OK" = 1 ] && grep -q 'init=/bin/init' "$TMP.entries" 2>/dev/null; then
        DEFIDX=$(awk '/^[[:space:]]*title /{i++} /init=\/bin\/init/{print i-1; exit}' "$TMP.entries"); WANT_TMO=3; WANT_HID=0
    else
        DEFIDX=0; WANT_TMO=0; WANT_HID=1
    fi
    { [ -z "$DEFIDX" ] || [ "$DEFIDX" -ge "$NKEEP" ] 2>/dev/null; } && DEFIDX=0
    {
        grep -E '^[[:space:]]*#' "$GRUB" 2>/dev/null | head -20
        echo "default=$DEFIDX"; echo "timeout=$WANT_TMO"; [ "$WANT_HID" = 1 ] && echo "hiddenmenu"
        cat "$TMP.entries"
    } > "$TMP"
    rm -f "$TMP.entries" "$TMP.lf"

    if grep -qE '^[[:space:]]*kernel .*bzImage' "$TMP" && [ "$NKEEP" -ge 1 ]; then
        mv "$TMP" "$GRUB"; sync 2>/dev/null
        say "    wrote grub.conf: $NKEEP entries, default=$DEFIDX, timeout=$WANT_TMO, hiddenmenu=$WANT_HID"
    else
        rm -f "$TMP"; say "    ABORTED: rewritten grub failed sanity check; original untouched." >&2; return 1
    fi
}

# ---- fsck (unmount nested SSD mounts, check, remount) ----------------------
remount_ssd() {
    mount -o rw "$DEV_SDA2" "$ROOT" 2>/dev/null
    mount -o rw "$DEV_SDA1" "$BOOT" 2>/dev/null
    [ -d "$ROOT/boot" ] && mount --bind "$BOOT" "$ROOT/boot" 2>/dev/null
    [ -d "$ROOT/korg" ] && { mkdir -p "$ROOT/korg/ro" "$ROOT/korg/rw" 2>/dev/null
        mount -o ro "$DEV_SDA5" "$ROOT/korg/ro" 2>/dev/null
        mount -o rw "$DEV_SDA6" "$ROOT/korg/rw" 2>/dev/null; }
}
# Conservative per-partition check: READ-ONLY first (changes nothing), report the
# result, recommend the exact write-repair command, and only run it if the user
# confirms (interactive) -- never auto-writes.
fsck_one() {
    dev=$1; typ=$2; iact=$3
    [ -b "$dev" ] || { say "    $dev: not present -- skip"; return 0; }
    say ""
    say "  --- $dev ($typ) ---"
    if [ "$typ" = ext ]; then
        command -v e2fsck >/dev/null 2>&1 || { say "    no e2fsck available -- skip"; return 0; }
        e2fsck -fn "$dev"; rc=$?
        FIXCMD="e2fsck -fy $dev"
    else
        command -v dosfsck >/dev/null 2>&1 || { say "    no vfat checker (dosfsck) available -- skip"; return 0; }
        dosfsck -n "$dev"; rc=$?
        FIXCMD="dosfsck -a -w $dev"
    fi
    if [ "$rc" = 0 ]; then say "    CLEAN -- read-only check found no errors (nothing changed)."; return 0; fi
    say "    *** ERRORS found (read-only check -- nothing was changed). ***"
    say "    RECOMMENDED FIX (writes to the partition):"
    say "        $FIXCMD"
    if [ "$iact" = 1 ]; then
        printf "    run this repair now? [y/N] "; read yn
        if yes "$yn"; then
            say "    repairing ..."
            if [ "$typ" = ext ]; then e2fsck -fy "$dev"; else dosfsck -a -w "$dev"; fi
            if [ "$typ" = ext ]; then e2fsck -fn "$dev" >/dev/null 2>&1 \
                && say "    re-check: now CLEAN." \
                || say "    re-check: STILL has errors -- back up /korg/rw, then a manual '$FIXCMD' or reformat may be needed."; fi
        else
            say "    skipped. The partition is unmounted now, so you can run it yourself:"
            say "        $FIXCMD"
        fi
    else
        say "    (non-interactive mode: not writing) To fix, with the SSD unmounted, run:"
        say "        $FIXCMD"
        say "    or use the KronosDoctor menu -> F (it will prompt you)."
    fi
}

do_fsck() {
    iact=${1:-0}
    [ "${ROOT:-/}" = "/" ] && { say "  fsck: refusing on a live (--on-device) system."; say "        Boot the KronosRescue USB and run fsck from there (partitions must be unmounted)."; return 1; }
    say "  fsck: unmounts the SSD, runs a READ-ONLY check on each partition, reports"
    say "        problems, and only writes if you confirm -- one partition at a time."
    cd /
    say "  unmounting SSD partitions ..."
    for m in "$ROOT/korg/rw2" "$ROOT/korg/rw" "$ROOT/korg/ro" "$ROOT/boot" "$BOOT" "$ROOT"; do
        umount "$m" 2>/dev/null && say "    unmounted $m"
    done
    fsck_one "$DEV_SDA1" vfat "$iact"
    fsck_one "$DEV_SDA2" ext  "$iact"
    fsck_one "$DEV_SDA5" ext  "$iact"
    fsck_one "$DEV_SDA6" ext  "$iact"
    say ""
    say "  remounting SSD ..."; remount_ssd
    say "  fsck: done."
    say "  NOTE: if a partition still has errors after repair, back up /korg/rw (over SSH/FTP)"
    say "        before any reformat -- repeated errors can also mean a failing SSD."
}

# ---- (un)root dispatch ----------------------------------------------------
do_root() {
    if [ -x "$HERE/apply_devroot.sh" ]; then
        say "  running apply_devroot.sh ..."; sh "$HERE/apply_devroot.sh"
    else
        say "  apply_devroot.sh not found next to KronosDoctor ($HERE) -- can't root here." >&2
    fi
}
do_boot_factory() {
    # Safe "un-root the boot" = drop the DevRoot grub entry + factory-quiet header.
    # Leaves /bin/init etc. on disk (reversible); does NOT delete binaries.
    say "  normalising grub to boot FACTORY (DevRoot entry dropped; files kept) ..."
    EN_crlf=0 EN_perms=0 EN_reconstruct=0 EN_collapse=0 EN_dedup=0 EN_strip_ki=0
    EN_drop_bi=1 EN_rebuild_bi=0 EN_add_sr=0 EN_menufix=0 EN_default=0
    ROOT_OK=0   # force header to factory-quiet + drop init=/bin/init entries
    do_repair
    detect   # re-detect after
}

# ---- interactive menu -----------------------------------------------------
menu() {
    while :; do
        detect; diagnose; print_report
        say ""
        say "  actions:"
        [ "$PROBLEMS" -gt 0 ] && say "    A) apply ALL recommended repairs"
        [ "$PROBLEMS" -gt 0 ] && say "    S) select repairs individually"
        say "    F) filesystem check (fsck all SSD partitions)"
        [ $ROOTED = 0 ] && [ -x "$HERE/apply_devroot.sh" ] && say "    R) ROOT this unit now (apply DevRoot)"
        [ $ROOTED = 1 ] && say "    B) make it BOOT FACTORY (drop DevRoot entry; keep files)"
        say "    V) view grub.conf     G) re-scan     H) shell     X) reboot     P) power off     Q) quit"
        say "  (local screen showing random characters? plug in a USB keyboard, or connect"
        say "   over SSH from another PC on the network -- same menu, clean terminal.)"
        printf "  choose: "
        # EOF-safe: a closed/absent console returns non-zero -> fall through to a shell
        # instead of spinning.  A real console tty blocks here for input.
        if ! read c; then say ""; say "  (no console input) -- dropping to a shell; run 'kronos_doctor' to return."; return 0; fi
        case "$c" in
            A|a) [ "$PROBLEMS" -gt 0 ] && { do_repair; say ""; say "  press Enter"; read _; } ;;
            S|s) [ "$PROBLEMS" -gt 0 ] && {
                    # prompt only for gate-able repairs; header/collapse always ride along
                    for f in $ISSUES; do
                        case "$GATEABLE" in *" $f "*)
                            eval "d=\$DESC_$f"
                            printf "    apply [%s] %s ? [Y/n] " "$f" "$d"; read yn
                            yes "${yn:-y}" || eval "EN_$f=0" ;;
                        *) say "    [$f] applied automatically (safe grub normalisation)" ;;
                        esac
                    done
                    do_repair; say ""; say "  press Enter"; read _; } ;;
            F|f) printf "  fsck unmounts the SSD and read-only checks it first. proceed? [y/N] "; read yn; yes "$yn" && do_fsck 1; say "  press Enter"; read _ ;;
            R|r) [ $ROOTED = 0 ] && { do_root; say "  press Enter"; read _; } ;;
            B|b) [ $ROOTED = 1 ] && { printf "  drop the DevRoot boot entry (files kept)? [y/N] "; read yn; yes "$yn" && do_boot_factory; say "  press Enter"; read _; } ;;
            V|v) [ -f "$GRUB" ] && sed 's/^/    /' "$GRUB" | ${PAGER:-cat}; say "  press Enter"; read _ ;;
            G|g) : ;;
            H|h) say "  dropping to shell -- type 'exit' or run 'kronos_doctor' to return."; ${SHELL:-/bin/sh} ;;
            X|x) printf "  reboot now? [y/N] "; read yn; yes "$yn" && { sync; reboot -f 2>/dev/null || reboot; } ;;
            P|p) printf "  power off now? [y/N] "; read yn; yes "$yn" && { sync; poweroff -f 2>/dev/null || poweroff; } ;;
            Q|q) return 0 ;;
            *) : ;;
        esac
    done
}

# ---- main -----------------------------------------------------------------
detect; diagnose
case "$MODE" in
    report) print_report ;;
    repair) print_report; [ "$PROBLEMS" -gt 0 ] && do_repair || say "  nothing to repair." ;;
    fsck)   do_fsck ;;
    menu)   menu ;;
    "")     if [ -t 0 ]; then menu; else print_report; fi ;;   # tty -> menu, else report
esac
