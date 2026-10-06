# PETdisk MAX Agent Channel — Product Requirements

**Status:** Draft for community review
**Applies to:** PETdisk MAX v2 (ESP32) and v3 (ESP32-S2), the `fixes` branch and later
**License:** GPL-3.0, like the rest of PETdisk MAX

## 1. Summary

Let a Commodore PET talk to a language model or AI agent through its PETdisk
MAX, using nothing but standard BASIC disk commands.

The PET sends a message with `PRINT#`, checks progress on the command
channel, and reads the reply with `GET#`. A small **bridge** service on the
home network passes the message to whatever model the owner chooses: a local
model (Ollama, LM Studio, llama.cpp), a cloud API (Claude, OpenAI), an n8n
workflow, or a coding agent such as Claude Code. The bridge returns text that
is ready for a PET screen.

The flagship use case is **"vibe coding" on a real PET**: you type a request
on the PET keyboard, an agent writes and tests a BASIC program in an
emulator, saves it to the network drive, and you `LOAD` and `RUN` it on the
real machine.

## 2. Background

- The PET (1977) has no networking. Its IEEE-488 port talks to disk drives
  and printers.
- The PETdisk MAX by bitfixer plugs into that port and emulates a disk drive.
  It has an ESP32 with Wi-Fi and can already serve files from a web server
  as up to four **network drives**, through a PHP script (`petdisk.php`).
- That makes the PETdisk the natural network gateway for a PET. No PET
  hardware modification is needed beyond what the PETdisk already requires.

**What works today (no firmware changes):** a PET program can save a
message to a network drive and repeatedly load a status file until a reply
appears ("mailbox" mode). It works, but each poll is a full file round trip,
it needs a separate file watcher on the server, and every program has to
implement the protocol itself.

**What blocks reliable use today:** the network-drive code had memory-safety
and state bugs (responses overwriting each other's buffers, a stack overflow
on every network save, DNS reused across hosts, missing files reported with
random sizes). These are fixed on the `fixes` branch and must be validated on
hardware before building on them (milestone M0).

## 3. Goals

1. A PET BASIC program can send a message to an AI and get the reply in a
   few lines of code.
2. Long-running agents (minutes) work: the PET is never blocked and can show
   progress.
3. **Model-agnostic.** The firmware knows nothing about any AI vendor. The
   bridge is where models are plugged in.
4. Replies arrive ready for the PET: PET character set, wrapped to 40 or 80
   columns, no Markdown, no emoji.
5. Safe on a home network: API keys never on the SD card, agent access
   protected by a token, nothing exposed to the internet by default.
6. Works on both PETdisk chips, including the single-core ESP32-S2 (v3).
7. Fully backwards compatible: PETdisks without the feature, and PETs that
   never use it, behave exactly as before.

## 4. Non-goals (for version 1)

- Calling cloud APIs directly from the PETdisk (HTTPS, JSON and API keys on
  the device). The bridge does this.
- Streaming tokens to the screen as they are generated. Replies are read
  when complete; progress messages cover the wait.
- Voice, images, or anything beyond text.
- Running a model on the PETdisk itself.
- Support for BASIC 1 PETs (the PETdisk doesn't support them either).

## 5. Users and use cases

| User | Use case |
|---|---|
| Hobbyist with a PET | Ask a chat assistant questions from the PET keyboard |
| Retro programmer | "Vibe code": describe a program, get a tested BASIC listing on the network drive, `LOAD` and `RUN` it |
| Game author | Write BASIC games that call a model (text adventures with generated descriptions, a chatty opponent) using a small subroutine library |
| Workflow builder | Route PET messages into n8n (or similar) and build anything there: home automation, lookups, notifications |
| Museums and shows | A working 1977 computer talking to a modern AI is a great demo |
| Educators | Teach BASIC with an assistant that explains errors on the same screen |

## 6. Solution overview

Three parts, each usable and testable on its own:

```
  PET keyboard / BASIC program
        │  OPEN, PRINT#, INPUT#15, GET#   (standard IEEE-488 disk commands)
        ▼
  PETdisk MAX firmware  ── "agent channel" device (e.g. device 12)
        │  plain HTTP on the home network, shared token
        ▼
  Bridge service  ── sessions, jobs, PET text formatting
        │
        ├── local model (Ollama / LM Studio / llama.cpp, OpenAI-style API)
        ├── cloud API (Claude, OpenAI, ...)
        ├── n8n webhook
        └── command (e.g. Claude Code headless, with an emulator for testing)
```

1. **Firmware agent channel**: a new device type in `PETDISK.CFG`. It turns
   disk commands into requests to the bridge.
2. **Bridge**: a small service with one simple HTTP protocol (section 8) and
   pluggable back ends. A reference implementation ships with PETdisk MAX;
   anyone can write their own.
3. **PET software**: a ready-to-run chat program (`AICHAT`) and a subroutine
   library for other programs.

## 7. Requirements — firmware

### 7.1 Configuration

- **FW-1.** A new `PETDISK.CFG` line type creates an agent device:
  `12,AGENT,192.168.1.20/petagent/bridge.php`
  (device number, the word `AGENT`, then the bridge URL without `http://`,
  same host/port/path rules as network drive URLs).
- **FW-2.** An optional line `token,SECRET` sets a shared token sent with
  every agent request. Without it no token is sent.
- **FW-3.** Optional `cols,40` or `cols,80` tells the bridge the screen
  width. Default 40.
- **FW-4.** One agent device per PETdisk is enough for version 1.

### 7.2 Sending a message

- **FW-5.** `OPEN n,12,ch,"NAME"` (any secondary address 2–14) starts a
  message. `NAME` is a session name chosen by the program (for example
  `CHAT`, `GAME1`); the bridge keeps a separate conversation per session.
- **FW-6.** Bytes written with `PRINT#` are collected, unchanged PETSCII, up
  to at least 2,048 bytes per message. Longer messages are cut off and
  reported as an error on the status channel.
- **FW-7.** `CLOSE` sends the message to the bridge (one HTTP request) and
  returns quickly. The device must not hold the IEEE bus while the bridge
  works.
- **FW-8.** Special messages: `NEW` starts a fresh conversation for the
  session; `CANCEL` stops the current job.

### 7.3 Status (command channel 15)

- **FW-9.** `INPUT#15,S,S$` on the agent device returns one status line in
  the familiar `number,text` style:

  | S | Meaning | S$ |
  |---|---|---|
  | 0 | Reply ready (or idle) | `READY` |
  | 1 | Working | progress text from the bridge, e.g. `TESTING PASS 3` |
  | 2 | Error | short reason, e.g. `BRIDGE UNREACHABLE`, `BAD TOKEN`, `TOO LONG` |

- **FW-10.** Each status read makes at most one short request to the bridge
  (target under 100 ms on a home network). The device does **no** background
  network work, so it works on the single-core ESP32-S2, whose core is busy
  timing the IEEE bus.

### 7.4 Reading the reply

- **FW-11.** Once status is `0`, `OPEN n,12,ch,"NAME"` followed by `GET#`
  (or `INPUT#`) returns the reply; `ST` is 64 after the last byte.
- **FW-12.** The reply is read block by block from the bridge, the same way
  network files are read (512-byte ranges), so replies are not limited by
  PETdisk memory.
- **FW-13.** Reading before the reply is ready returns an empty reply and an
  error status, never stale data from a previous reply.

### 7.5 Robustness and compatibility

- **FW-14.** Every network operation times out (5 s); a dead bridge produces
  status `2`, never a hung device or a hung PET.
- **FW-15.** Uses the per-device buffers and bounded receive code from the
  `fixes` branch. No new state in the shared 1K buffer.
- **FW-16.** No effect on SD card or network drive behaviour. A
  `PETDISK.CFG` without an `AGENT` line behaves exactly as before.
- **FW-17.** Fits the existing flash layout (firmware slot 0x230000 bytes)
  and RAM budget of the ESP32-WROOM (no PSRAM).

## 8. Bridge protocol (the contract)

Plain HTTP/1.0, so the PETdisk can use its existing client. All requests
carry `t=TOKEN` when a token is configured; a wrong token gets `403`.
Bodies from the PETdisk are raw PETSCII, base64-encoded like network saves.

| Request | Purpose | Response |
|---|---|---|
| `PUT ?a=send&s=SESSION&cols=40&b64=1` (body: message) | Start a job | `200`, body `JOBID` |
| `GET ?a=status&j=JOBID` | Progress | `0,READY` · `1,WORKING,<text>` · `2,ERROR,<text>` |
| `GET ?a=reply&j=JOBID&l=1` | Reply size | size, then `\r\n` (0 if none) |
| `GET ?a=reply&j=JOBID&s=S&e=E` | Reply bytes S..E-1 | PETSCII bytes |
| `GET ?a=new&s=SESSION` | Forget the conversation | `0,READY` |
| `GET ?a=cancel&j=JOBID` | Stop a job | `0,READY` |

- **BR-1.** Status and reply-size requests answer immediately; the model
  call runs in the background.
- **BR-2.** Replies are converted for the PET before they are stored:
  - ASCII to PETSCII (letters mapped so they display correctly in the PET's
    default uppercase/graphics mode; curly quotes, dashes and accents
    simplified; unsupported characters dropped).
  - Markdown removed (headings, bold, bullets become plain lines; code
    blocks kept verbatim).
  - Word-wrapped to `cols`, with paragraph breaks preserved.
  - Size limit (configurable, default 8 KB) with a note when cut.
- **BR-3.** Incoming PETSCII is converted to ASCII before it goes to the
  model.
- **BR-4.** Each session keeps its conversation history (configurable
  length). `NEW` clears it.
- **BR-5.** Pluggable back ends, chosen in the bridge's config file:
  - `openai`: any OpenAI-compatible chat endpoint (Ollama, LM Studio,
    llama.cpp server, OpenAI, and other services offering that API).
  - `anthropic`: the Claude Messages API.
  - `webhook`: POST the message to a URL (for example an n8n Webhook node
    with "Respond to Webhook") and use the response body as the reply.
  - `command`: run a program with the message on standard input, for
    example Claude Code in headless mode, and use its output. Progress lines
    the program prints (a configurable prefix) become status text.
- **BR-6.** A system prompt per back end, with a sensible default that tells
  the model it is answering on a 40-column Commodore PET (short, plain text).
- **BR-7.** Optional: write generated programs into the PETdisk network
  folder, so `LOAD"NAME",9` picks them up. The `command` back end can do this
  itself.

### 8.1 Mailbox mode (for unmodified PETdisks)

- **BR-8.** The bridge can also watch a network-drive folder for the
  file-based protocol, so PETdisks with stock firmware can use it: the PET
  saves `PROMPT.SEQ` then `GO.SEQ`; the bridge writes `STATUS.SEQ` and
  `REPLY.SEQ`. This is slower but works today, and it is the first thing to
  ship (milestone M1).

## 9. Requirements — PET software

- **PS-1.** `AICHAT.PRG`: a chat program. You type a message (longer than
  BASIC's `INPUT` allows, commas and colons included), see a spinner with
  progress while you wait, and read the reply a page at a time. It has
  commands for a new conversation and quitting. Works on BASIC 2 and 4,
  40 and 80 columns.
- **PS-2.** A subroutine library to merge into other programs: send `M$`
  to session `S$`, wait (calling a user routine while waiting), and return
  the reply in an array of lines.
- **PS-3.** A mailbox-mode version of both for stock-firmware PETdisks.
- **PS-4.** Tested in VICE (`xpet`) with folder-backed drives before
  release, and on hardware.

## 10. Security and privacy

- **SEC-1.** The bridge listens on the home network only by default. The
  docs warn against port forwarding or exposing it to the internet.
- **SEC-2.** API keys live in the bridge's config, never on the SD card or
  the PETdisk.
- **SEC-3.** The shared token protects the bridge from other devices on the
  network. This matters most with the `command` back end, which can run
  code.
- **SEC-4.** The `command` back end runs agents in a dedicated folder with
  an explicit list of allowed tools, and never with administrator rights.
- **SEC-5.** Conversation logs are off by default.
- **SEC-6.** Rate limit per session (configurable) to cap cloud costs.
- **SEC-7.** The token travels in plain HTTP. This is acceptable on a home
  network and documented as such.

## 11. Non-functional requirements

| Area | Requirement |
|---|---|
| Latency | Status poll under 100 ms on a home network; reply read speed similar to network file loads |
| Reliability | No hangs: every network step times out; the PET always gets a status |
| Portability | Bridge runs on Linux, macOS, Windows, a Raspberry Pi, or a NAS, and in Docker. Reference implementation in Python with no required third-party packages |
| Footprint | Firmware addition fits the existing flash slot and RAM |
| Compatibility | Old firmware + new bridge: mailbox mode works. New firmware + no bridge: everything else unchanged |
| Documentation | Setup guide for each back end, a protocol reference, and the BASIC library reference |

## 12. Milestones

| | Milestone | Needs hardware? | Done when |
|---|---|---|---|
| **M0** | Validate the `fixes` branch on a PETdisk (`test/pet/PDTEST`) | Yes | All hardware tests pass on ESP32; ideally also ESP32-S2 |
| **M1** | Bridge with mailbox mode and all four back ends; `AICHAT` mailbox version | No (VICE) | Chat works end to end in VICE with folder drives, then on hardware with stock firmware |
| **M2** | Firmware agent channel (FW-1 to FW-17) | Yes | `AICHAT` channel version works; PDTEST still passes |
| **M3** | BASIC subroutine library and an example game | No (VICE), then yes | Example game plays on hardware |
| **M4** | Vibe-coding setup: Claude Code + VICE in Docker as a `command` back end | No | A request typed on the PET produces a tested program on the network drive |
| **M5** | Community release: docs, prebuilt firmware, offer changes upstream to bitfixer | — | Tagged release on the fork; pull request opened upstream |

## 13. Success measures

- A new user goes from a working PETdisk to chatting from the PET in under
  30 minutes with the setup guide.
- Zero hangs in a 100-message soak test against each back end.
- At least one person outside this project builds something with the
  subroutine library.
- The firmware changes are accepted upstream, or the fork is the version
  the community points people to.

## 14. Risks

| Risk | Mitigation |
|---|---|
| The `fixes` firmware has an unknown problem on real hardware | M0 comes first; recovery steps in `test/pet/TESTING.md` (serial/USB reflash) |
| PET IEEE timeouts if the device is slow to answer | The device never waits on the model during a bus transfer; status and reply reads are short requests, like network file reads today |
| ESP32-S2 is single-core | No background tasks (FW-10) |
| Model replies are long, chatty or full of Markdown | System prompt (BR-6), formatting pipeline (BR-2), size limit |
| Cloud costs | Local models are a first-class back end; rate limits (SEC-6) |
| Someone exposes the bridge to the internet | Token, LAN-only default, clear warnings |
| Upstream is unresponsive | Work stays on a public fork under the same license; small, separate commits make it easy to merge later |

## 15. Open questions

1. **Device number and name.** Is `AGENT` the right config keyword, or a
   more general `TEXT` / `HTTP` channel (useful for BBS-style chat and
   lookups as well)?
2. **Session naming.** Should the session be the `OPEN` filename (as above),
   or fixed per device with `NEW` as the only control?
3. **Reference bridge language.** Python fits background jobs best. Should
   there also be a minimal PHP version that runs next to `petdisk.php` on
   any shared web host, for synchronous back ends only?
4. **Character set.** Should the bridge offer lowercase/uppercase mode
   (the PET's alternate character set) for programs that switch to it?
5. **Upstream.** Propose the agent channel to bitfixer before or after the
   bug fixes are merged?

## Appendix A: example BASIC session (agent channel)

```
10 OPEN 15,12,15
20 INPUT "YOU";M$
30 OPEN 2,12,2,"CHAT":PRINT#2,M$;:CLOSE 2
40 INPUT#15,S,S$:IF S=1 THEN PRINT S$:GOTO 40
50 IF S=2 THEN PRINT "ERROR: ";S$:GOTO 20
60 OPEN 2,12,2,"CHAT"
70 GET#2,A$:PRINT A$;:IF ST=0 THEN 70
80 CLOSE 2:PRINT:GOTO 20
```

(Real programs should wait a moment between status reads and handle long
input; `AICHAT` and the library do this.)

## Appendix B: example `PETDISK.CFG`

```
8,SD0
9,192.168.1.20/petdisk/petdisk.php
12,AGENT,192.168.1.20/petagent/bridge.php
token,CHANGE-ME
cols,40
ssid,YourNetwork
password,YourPassword
```
