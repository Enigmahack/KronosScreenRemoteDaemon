# Network fuzz / stability regression suite

`fuzz_screenremote.py` throws malformed, boundary, and adversarial data at
screenremote's three network surfaces — stream port (7373), ctrl port
(7374), and UDP discovery (7372) — over real TCP/UDP against a live Kronos,
and checks after every test case whether the daemon process is still alive
and whether the kernel oopsed. A crash immediately stops the run and prints
the exact test that caused it, plus the dmesg excerpt.

## Usage

```
export KRONOS_SSH_PASS=kronos   # or edit SSH_PASS in the script
python3 fuzz_screenremote.py --host 192.168.100.15 --phase 1   # no client connected
python3 fuzz_screenremote.py --host 192.168.100.15 --phase 2   # with an authenticated stream client + CTRL_PERSIST session held open
python3 fuzz_screenremote.py --host 192.168.100.15 --phase 2 --only CC,ST   # concurrency/stateful attacks only
python3 fuzz_screenremote.py --list                            # list every test id
python3 fuzz_screenremote.py --bisect CT07                      # rerun exactly one case (after narrowing down a failure)
python3 fuzz_screenremote.py --restart-only                     # just restart the daemon on the device
```

Requires `sshpass` (for the liveness/dmesg checks and daemon restart-on-crash
between runs — the raw fuzz traffic itself is plain Python sockets, no SSH).

Phase 2 needs valid credentials from `/korg/rw/Startup/KronosNet.conf` on the
target — the script reads them itself via SSH. Note `check_auth()`'s
priority: KronosNet.conf is tried FIRST and short-circuits (even on a wrong
password) before the `/proc/id`-derived PublicID fallback is ever reached —
don't assume the PublicID always works if KronosNet.conf has a `kronos` user
configured with a different password.

## Test categories

- `HS` — stream-port KSCR handshake protocol (`do_handshake()`): magic/version/length
  boundary values, credential edge cases, fragmentation, connection churn.
- `CT` — ctrl-port line command protocol (`process_ctrl_cmd()`/
  `handle_ctrl_persistent_data()`): every command's numeric-argument boundary
  matrix, malformed hex (MIDI_SEND/SYSEX), embedded NULs, format-string-like
  args, connection churn, backlog/fd stress.
- `DS` — UDP discovery probe: magic boundary, oversized/truncated packets, flood.
- `CC` — true concurrency chaos (threaded, not sequential): races on shared
  global state (`g_ctrl_allowed_ip`, `ctrl_fd`, fd table) under load.
- `ST` — stateful sequence attacks targeting the specific code paths patched
  2026-07-16 (stuck-chord-note fix, CTRL_PERSIST ownership revocation, SYSEX
  async state) rather than single malformed packets.

## Results

`results/*.json` are timestamped run logs (git-ignored — see `.gitignore`
here). Baseline clean runs from 2026-07-16 (v1.11.2, post real-hardware
update-crash fix) are kept as reference: `phase1_1784241549.json` (57/57
pass, no client), `phase2_1784241729.json` (57/57 pass, authenticated stream
client + CTRL_PERSIST session held open throughout), and
`phase2_1784241839.json` (8/8 CC+ST concurrency/stateful attacks pass).
Zero crashes found in ~130 test-case executions (each representing many
individual malformed network operations) across both phases plus the
concurrency round. FD count on the daemon after the heaviest stress case
(1000 simultaneous connections) returned to baseline (12) with no leak.

Re-run this suite after any change that touches network input handling
before considering it safe to deploy.
