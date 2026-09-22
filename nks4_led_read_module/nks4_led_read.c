/*
 * nks4_led_read.c - ONE-SHOT diagnostic: issues a real "read LED row" NKS4
 * wire command to the Nautilus panel MCU and reports the raw response.
 *
 * Background: led_hook.c (this repo, 2026-09-22 session) confirmed via
 * live inline hooks that none of CSTGFrontPanel::SetLED / SetLED16Bits /
 * SetLEDBlinking ever fire an "off" event for the MODE/PAGE popup buttons -
 * whatever turns their LED back off isn't driven from the host at all. That
 * pointed at the panel MCU itself tracking LED state. Its own firmware
 * (NAUTILUS_V01R10.VSB) has the same hidden factory self-test strings as
 * the Kronos firmware ("LED : %s", "Switch On/Off : %s"), and the
 * kronosology K1_V06R06 reconstruction (built from the Kronos-family
 * sibling firmware, same wire protocol lineage) documents exactly how to
 * ask for it over the wire:
 *
 *   wire_dispatch_command() (kronosology/reconstructed/K1_V06R06/
 *   wire_dispatch.c) - opcode 0 (cmd[3]==0), reg 0x51 (cmd[2]) calls
 *   cpsoc_read_led_row(ctx, cmd[1]) - reg 0x50/0x52 are the analogous
 *   switch-row reads.
 *
 * Host-side encoding (confirmed two independent ways against the real
 * Nautilus OA.ko/OmapNKS4Module.ko, decrypted copy, 2026-09-22):
 *   1. GetRawDipSwitches's own machine code loads EAX = 0x01F10000 before
 *      calling SubmitNKS4CommandWrite() - byte-for-byte opcode=cmd[3]=0x01,
 *      reg=cmd[2]=0xF1, matching the wire_dispatch_command() cmd[] indexing
 *      exactly (byte3=opcode is the LOW byte of a big-endian-looking but
 *      actually just-byte-addressed 32-bit word: value = (opcode<<24) |
 *      (reg<<16) | (param1<<8) | param0).
 *   2. CommunicationCheck()'s own error printk literally prints the sent
 *      value: 0x00EE0000 - decodes to opcode=0, reg=0xEE, which is exactly
 *      wire_dispatch_command()'s own documented reg==0xee branch
 *      ("CommunicationCheck"). Independent confirmation of the same
 *      encoding from a completely different call site.
 *
 * So a "read LED row `index`" command is: (0<<24)|(0x51<<16)|(index<<8)|0
 *                                        = 0x00510000 | (index << 8)
 *
 * Calling convention (confirmed from GetRawDipSwitches's own disassembly,
 * OmapNKS4Module.ko offset 0x12e70): both SubmitNKS4CommandWrite and
 * WaitForNKS4ReadEvent are regparm(3) with their single argument in EAX -
 * GCC's regparm(3) function-pointer-typedef trick (same one nks4_inject.c
 * already uses for OA.ko's HandleSwitchEvent/SetLED-family calls)
 * reproduces this exactly, no hand-written inline asm needed.
 * SubmitNKS4CommandWrite(cmd) submits the write, returns 0 on success.
 * WaitForNKS4ReadEvent(&response_buf) blocks (bounded, ~1000 ticks) until
 * the panel's reply lands in *response_buf, returns 0 on success or
 * 0xffffffff on timeout - GetRawDipSwitches reads its own two output bytes
 * directly back out of that same buffer afterward, confirming the
 * completion path writes the real response there.
 *
 * *** SAFETY: THIS IS NOT A PASSIVE READ. *** Both functions operate
 * through a SINGLE GLOBAL wait-pointer (sWaitReadPtr, OmapNKS4Module.ko's
 * own .bss - address derived below from WaitForNKS4ReadEvent's own `mov
 * [sWaitReadPtr],eax` instruction, same "read the live module's own
 * machine code" technique this session already used repeatedly, not a
 * guessed offset) that OA.ko's own live driver code ALSO uses continuously
 * for real panel traffic (GetVersion, progress bar, DIP switches, etc.).
 * There is no lock this module can safely take from outside the driver, so
 * a genuine race exists: if a legitimate request starts in the gap between
 * this module's busy-check and its own SubmitNKS4CommandWrite() call, one
 * side's response could be dropped/garbled. This is bounded, not
 * catastrophic - WaitForNKS4ReadEvent's own timeout path unconditionally
 * clears sWaitReadPtr back to 0 either way, so a collision cannot wedge the
 * driver permanently, only cost one lost exchange somewhere. The busy-check
 * below (poll sWaitReadPtr==0, bounded retries, abort with NO command sent
 * if never idle) narrows the window; it does not eliminate it. Discussed
 * and accepted explicitly before this module was written - see the
 * conversation this file was built in.
 *
 * Usage:
 *   insmod nks4_led_read.ko submit_addr=0x<VA> wait_addr=0x<VA> \
 *          waitptr_addr=0x<VA> [led_reg=0x51] [led_index=0]
 *     submit_addr  = grep SubmitNKS4CommandWrite /proc/kallsyms
 *     wait_addr    = grep WaitForNKS4ReadEvent /proc/kallsyms
 *     waitptr_addr = live OmapNKS4 module base + (sWaitReadPtr's Ghidra
 *                    address - the module's own Ghidra image base) - see
 *                    this session's derivation, offset 0xE438 from
 *                    WaitForNKS4ReadEvent's own 0x2AA0 (i.e. waitptr_addr =
 *                    wait_addr + 0xB998) on this firmware build.
 *   cat /proc/.nks4_led_read     - fires exactly ONE read attempt per read,
 *                                  so the operator controls timing
 *   led_reg/led_index are live-writable (0644) to probe other rows without
 *   a reload - echo not supported by this proc style, use insmod params or
 *   rmmod/reinsmod with new values.
 *   rmmod nks4_led_read           - no patched bytes anywhere to restore;
 *                                    this module never modifies code, only
 *                                    calls two pre-existing functions
 */

#include <linux/module.h>
#include <linux/kernel.h>
#include <linux/init.h>
#include <linux/proc_fs.h>
#include <linux/workqueue.h>
#include <linux/delay.h>

MODULE_LICENSE("GPL");
MODULE_DESCRIPTION("ONE-SHOT diagnostic: issue a real NKS4 'read LED row' command to the Nautilus panel MCU");

static unsigned long submit_addr = 0;
module_param(submit_addr, ulong, 0444);
MODULE_PARM_DESC(submit_addr, "Live VA of SubmitNKS4CommandWrite (grep /proc/kallsyms)");

static unsigned long wait_addr = 0;
module_param(wait_addr, ulong, 0444);
MODULE_PARM_DESC(wait_addr, "Live VA of WaitForNKS4ReadEvent (grep /proc/kallsyms)");

static unsigned long waitptr_addr = 0;
module_param(waitptr_addr, ulong, 0444);
MODULE_PARM_DESC(waitptr_addr, "Live VA of sWaitReadPtr (OmapNKS4Module.ko's own shared wait-pointer global) - see this file's header comment for how to derive it");

static unsigned int led_reg = 0x51;
module_param(led_reg, uint, 0644);
MODULE_PARM_DESC(led_reg, "Wire register to read - 0x51=LED row, 0x50=switch row, 0x52=switch row+clear (default 0x51)");

static unsigned int led_index = 0;
module_param(led_index, uint, 0644);
MODULE_PARM_DESC(led_index, "Row/index byte (cmd[1]) - which LED/switch group to read (default 0)");

#define BUSY_RETRIES   50
#define BUSY_SLEEP_MS  2

typedef int (*submit_cmd_t)(unsigned int cmd) __attribute__((regparm(3)));
typedef int (*wait_read_t)(void *buf) __attribute__((regparm(3)));

static struct proc_dir_entry *proc_entry;
static struct work_struct setup_work;

static volatile unsigned int last_cmd_sent;
static volatile int last_rc_submit = -2;   /* -2 = never attempted */
static volatile int last_rc_wait = -2;
static volatile unsigned int last_response;
static volatile unsigned int last_busy_retries;
static volatile int last_aborted_busy;

static int nks4_led_read_proc(char *page, char **start, off_t off,
                               int count, int *eof, void *data)
{
    submit_cmd_t submit_fn = (submit_cmd_t)submit_addr;
    wait_read_t  wait_fn   = (wait_read_t)wait_addr;
    volatile unsigned int *waitptr = (volatile unsigned int *)waitptr_addr;
    unsigned int cmd;
    unsigned int response = 0;
    unsigned int retries = 0;
    int rc_submit, rc_wait;
    int len;

    cmd = ((unsigned int)0 << 24) | ((led_reg & 0xffU) << 16) |
          ((led_index & 0xffU) << 8) | 0U;

    /* Best-effort collision guard - see header comment. Narrows the race,
     * does not eliminate it: aborts with NO command sent if the shared
     * wait-pointer never goes idle within the retry budget, rather than
     * firing into a request we can see is still in flight. */
    while (*waitptr != 0 && retries < BUSY_RETRIES) {
        retries++;
        msleep(BUSY_SLEEP_MS);
    }
    if (*waitptr != 0) {
        last_aborted_busy = 1;
        last_busy_retries = retries;
        len = snprintf(page, count,
            "ABORTED=busy CMD=0x%08x BUSY_RETRIES=%u (sWaitReadPtr never went idle - "
            "no command sent)\nOK\n", cmd, retries);
        *eof = 1;
        return len;
    }
    last_aborted_busy = 0;
    last_busy_retries = retries;
    last_cmd_sent = cmd;

    rc_submit = submit_fn(cmd);
    last_rc_submit = rc_submit;
    if (rc_submit != 0) {
        len = snprintf(page, count,
            "CMD=0x%08x RC_SUBMIT=%d (submit failed - no free URB, nothing sent)\nOK\n",
            cmd, rc_submit);
        *eof = 1;
        return len;
    }

    rc_wait = wait_fn((void *)&response);
    last_rc_wait = rc_wait;
    last_response = response;

    len = snprintf(page, count,
        "CMD=0x%08x RC_SUBMIT=%d RC_WAIT=%d RESPONSE=0x%08x "
        "TAG=0x%04x BYTE1=0x%02x BYTE0=0x%02x BUSY_RETRIES=%u\nOK\n",
        cmd, rc_submit, rc_wait, response,
        (response >> 16) & 0xffffU, (response >> 8) & 0xffU, response & 0xffU,
        retries);

    *eof = 1;
    return len;
}

static void nks4_led_read_setup(struct work_struct *work)
{
    /* create_proc_entry() needs deferring out of init_module context on
     * this RTAI kernel - see CLAUDE.md's "Critical RTAI constraints" table. */
    proc_entry = create_proc_entry(".nks4_led_read", 0444, NULL);
    if (proc_entry)
        proc_entry->read_proc = nks4_led_read_proc;
    printk(KERN_INFO "nks4_led_read: ready - cat /proc/.nks4_led_read "
           "(led_reg=0x%02x led_index=%u)\n", led_reg, led_index);
}

static int __init nks4_led_read_init(void)
{
    if (!submit_addr || !wait_addr || !waitptr_addr) {
        printk(KERN_ERR "nks4_led_read: submit_addr=, wait_addr=, and "
               "waitptr_addr= are all required - see this file's header "
               "comment for how to derive each one\n");
        return -EINVAL;
    }

    INIT_WORK(&setup_work, nks4_led_read_setup);
    schedule_work(&setup_work);
    return 0;
}

static void __exit nks4_led_read_exit(void)
{
    flush_scheduled_work();
    if (proc_entry)
        remove_proc_entry(".nks4_led_read", NULL);
    /* Nothing else to undo - this module never patches code, only calls
     * two pre-existing functions through normal function pointers. */
}

module_init(nks4_led_read_init);
module_exit(nks4_led_read_exit);
