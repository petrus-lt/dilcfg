# dilcfg — DSD TECH diLogger configuration tool for Unix

Configure a DSD TECH **diLogger** serial data logger from **Linux, FreeBSD or
OpenBSD**. One C file, libc + POSIX termios only — no curses, no libraries,
no .NET, no Wine.

## Why this exists

The official configuration tool, **diConfig**, is Windows-only
(Microsoft Store `9N2HWNKZRMT6`, publisher zip at
https://www.deshide.com/News-detail_diLogger.html). It is a thin .NET
WinForms frontend over a trivial text protocol. `dilcfg` speaks that
protocol directly; it was derived by disassembling diConfig v1.3.0.0
(`DSDParserTester.English.DiLogger`, `System.UtilityEx.Serial.DSDSerailPort`).

## Build

    make            # cc -O2 -Wall -Wextra
Tested: gcc/Linux. The code uses only POSIX termios, select(), nanosleep()
and hand-rolled raw-mode setup (no cfmakeraw, no TIOCSSERIAL extensions),
so FreeBSD and OpenBSD need no changes.

## Connect

The diLogger's **config port** (USB-C) enumerates as a USB serial device
and is **fixed at 115200 baud, 8 data bits, 1 stop bit, no parity**:

    Linux   /dev/ttyUSB0        FreeBSD /dev/ttyU0
    OpenBSD /dev/ttyU0

## Use

Interactive:

    $ ./dilcfg /dev/ttyUSB0
    dil> version
    dil> baudrate            # query
    dil> baudrate 9600       # set
    dil> parity odd
    dil> sync                # device clock <- host clock
    dil> dump                # whole device config, parsed and pretty-printed
    dil> save backup.conf    # write it to a file loadable with -f
    dil> verbose on          # also echo the outgoing command (off by default)
    dil> raw AT              # verbatim line (AT-terminal style)

Batch from a config file (see `logger.conf`):

    ./dilcfg -f logger.conf /dev/ttyUSB0

One-shot (scriptable):

    ./dilcfg -c 'baudrate 19200' /dev/ttyUSB0
    ./dilcfg -c version /dev/ttyUSB0
    ./dilcfg -c dump /dev/ttyUSB0
    ./dilcfg -c 'save backup.conf' /dev/ttyUSB0   # backup current device config

`save` reads every setting from the device and writes a file ready for
`-f` (or for git, diffing, restoring). The device clock line is written
commented-out so an old timestamp is never pushed back accidentally;
use `sync` to set the host clock instead.

Verbosity: by default only the device's replies are printed. `-v` (CLI) or
`verbose on|off` (TUI, bare `verbose` toggles) additionally echo the
outgoing wire commands. `-q` still works for compatibility. Boolean
settings (`timestamp`, `hexlog`, `filenew`) are shown as `on`/`off`
instead of the raw `1`/`0` — display only, the wire format is unchanged.

## Protocol (reverse-engineered, exact wire format)

ASCII line + `\r\n`, then the tool waits for the reply until the line
goes idle (~300 ms), 10 ms inter-command delay:

| Action | Wire command |
|---|---|
| query/set recording-port baud | `param baudrate` / `param baudrate {N}` |
| query/set stop bits | `param stopbit` / `param stopbit {1\|2\|3}`  (device codes: 1=1, 2=2, 3=1.5) |
| query/set parity | `param parity` / `param parity {0\|1\|2}` (0=none 1=odd 2=even) |
| query/set max log file MB | `param filemb` / `param filemb {1..100}` |
| query/set timestamp | `param usetime` / `param usetime {0\|1}` |
| query/set hex recording | `param hexlog` / `param hexlog {0\|1}` |
| query/set new-file-on-boot | `param filenew` / `param filenew {0\|1}` |
| read/set clock | `time` / `time set {YYYY} {M} {D} {H} {m} {s}` (space separated, no zero padding) |
| firmware version | `system version` |
| reboot | `system reset` |
| factory reset | `param setdefault` |

Reply text is whatever the firmware prints; `dilcfg` displays it verbatim
(the Windows tool never parses replies either — it just shows the raw
stream, so reply formats are firmware-dependent). Confirmed reply tokens
on firmware `0203-240411`: `> baudrate:115200`, `> stop_bit:1`,
`> parity:0`, `> file_mb:30`, `> log_use_time:0`, `> hex_log:0`,
`> filenew:0`, `> 2026-11-15 19:08:56,week:0`, `> system version:0203-240411`,
`> WorkParam Saved.` after a set; the device also echoes the sent line.

A plain `AT` test only applies to DSD TECH's WiFi modules (SH-W08A tab);
diLogger configuration uses the commands above.

## Using the device over the network (socat)

On the host physically wired to the diLogger ("devhost"):

    socat /dev/ttyUSB0,b115200,raw,echo=0 tcp-listen:4000,reuseaddr

On your workstation, expose it as a local PTY and point dilcfg at it:

    socat pty,link=/tmp/dilogger,raw,echo=0 tcp:devhost:4000
    dilcfg /tmp/dilogger

(Through SSH instead: `ssh -N -L 4000:localhost:4000 devhost` after starting
the socat listener, then connect to `tcp:localhost:4000`.)
Alternatively just copy the single binary to devhost — it has no runtime deps.

License: 0BSD.
