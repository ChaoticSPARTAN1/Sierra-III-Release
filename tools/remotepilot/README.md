# RemotePilot for Sierra-III (optional)

[RemotePilot](https://github.com/ChaoticSPARTAN1/RemotePilot) is a local QA
tool: a viewer that runs on your PC, and a small companion on the device,
talking over your own Wi-Fi. Nothing here is needed to build or play
Sierra-III; it is what we used to test the B41 fix (`docs/sleep_b41.md`), and
it can save you time on a PSP. Its PSP section (`psp/` there) has the
companion's source, the viewer and the tests.

This folder holds the viewer's preset for Sierra-III, `psp-hce/`
(`manifest.json`, `adapter.py`): copy it into RemotePilot's
`psp/pc/presets/`. It holds no address, pairing or personal setting: each
viewer makes its own pairing (below). The two files are MIT, as RemotePilot's
viewer is.

## What it gives you on a PSP

**RemotePilot PSP**, the companion (an EBOOT of its own, started from the
XMB), with guided screens on the console:

- **Settings backup** (△): the PSP's system settings, flash1 (registry,
  wallpaper, an installed custom theme), copied into
  `ms0:/BACKUP/flash1-<date>-<time>`, each copy read back and checked, with a
  manifest of every file's size and SHA-256. Each start compares the settings
  with the newest backup and says whether they still match: a quick check
  after any test that could touch them (a sleep test, a crash).
- **Settings restore** (□): a backup put back after a reset. It checks the
  copies against their manifest and the firmware first, shows what differs,
  needs ✕ held for two seconds on the console, writes only what differs
  (through a small kernel helper that writes nowhere but flash1 and reads
  from nowhere but the backup folder), reads each file back, and restarts the
  PSP. Nothing on the network can start a restore.
- **Wi-Fi check:** the WLAN switch read live (with a strip that shows a worn
  switch's flicker), the chip, the saved network, a join test with every
  step's time and the firmware's error codes, and a report on the stick.

**The viewer** (on your PC, `python server.py`):

- **Live view and screenshots** of the PSP's screen.
- **Remote input:** the PSP's buttons and nub from a drawn PSP, the mouse or
  a game controller.
- **The Memory Stick's folders**, read only: `halo/log/` (the game's log and
  its numbered copies), `halo/crash.txt` and other crash files, pictures;
  each file copied to the PC on request.
- **Device log and report:** the log's last 64 KB, firmware, battery, free
  memory and stick space.
- **Settings backups copied to the PC** and checked file by file.
- **Bug reports, local only:** a report (what happened, steps, expected) saved
  on the PC with the device's screen and log. Nothing is filed or sent
  anywhere.

## Setting it up

1. Get RemotePilot (its README), and copy `psp-hce/` into
   `psp/pc/presets/` (RemotePilot already has it in its PSP section).
2. Build RemotePilot PSP (`psp/tools/build_psp.py`) and copy its `EBOOT.PBP`
   and `rp_flash.prx` to `ms0:/PSP/GAME/REMOTEPILOT/`.
3. Start the viewer, choose **PSP · Halo CE (Sierra-III)**, and follow its
   guide: **Prepare pairing**, then copy the pairing file it writes to the
   stick as `halo/remotepilot.cfg`. The file is a private 256-bit key: keep it
   out of bug reports and repositories.
4. On the PSP: WLAN switch on, start RemotePilot PSP, ✕ Connect. It joins
   the first network saved in Settings > Network Settings and shows its
   address; the viewer finds it on your network by itself.

## The API, for scripts and AI assistants

The viewer is a local HTTP API (loopback only; every call carries the header
`X-Remote-Pilot-Viewer: 1`). What the page does, a script can:

| Call | Does |
| --- | --- |
| `GET /api/state` | Connection, device state, the running backup or bug report, recent requests |
| `POST /api/action` `{"kind": "shot"}` | A screenshot (also `report`, `log`, `perf`) |
| `POST /api/action` `{"kind": "files", "payload": "ms0:/halo/log"}` | A folder listing; `"file"` copies one file |
| `POST /api/capture` `{"diagnostic": "log"}` | A capture: screen and log, saved together |
| `POST /api/control` `{"kind": "gamepad", "payload": {...}}` | Buttons and sticks, held briefly |
| `POST /api/backup` `{}` | A settings backup, made on the PSP, copied here and checked |
| `POST /api/bug-report` | A local bug report with the screen and log |

The device side speaks RP2 (RemotePilot's `psp/pc/docs/PROTOCOL.md`): one
request per connection, HMAC-SHA256 both ways with the pairing key and a fresh
nonce, private networks only, bounded sizes and times. It authenticates; it
doesn't encrypt: use a network you trust.

**AI-driven QA.** Because every step is an API call, an AI coding assistant
can run a test loop by itself: take a screenshot, press buttons, read the log
tail, list `halo/log/` and fetch a crash file, back up and compare the
settings, and write a local bug report with what it saw. Parts of the B41
work were checked that way: settings backups made on the PSP were copied to
the PC over Wi-Fi and checked file by file, and the Wi-Fi check's report
found that the companion was asking for a saved network that no longer
existed.

**What isn't here yet:** RemotePilot inside Sierra-III itself (live view,
remote input and console commands during play, which RP2's `console` kind is
for). On a PSP-1000 the campaign build has no network modules loaded and
little memory to spare for them; that needs measuring on hardware first.
