# Chiaki-lg Fork

A native port of [chiaki-ng](https://github.com/streetpea/chiaki-ng) for LG webOS smart TVs.
Streams PS4/PS5 Remote Play directly to your LG webOS TV.

> **Primary target:** PS5 + webOS 5+. PS4 and webOS 4.x paths are retained but have not been directly validated; a backend that does not expose native Opus is rejected cleanly instead of starting a broken A/V pipeline.

> **AI disclosure:** This project was developed with assistance from [Claude.ai](https://claude.ai), [ChatGPT](https://chat.openai.com), and [Google Gemini](https://gemini.google.com). All generated code was reviewed, tested, and integrated by the project author.

---

## Tested on

| TV | webOS / firmware | Platform | Kernel | Verified paths |
|---|---|---|---|---|
| LG OLED G1 (`OLED65G13LA`) | webOS `6.5.3`, firmware `03.53.45` | `O20N_DVB_EU`, ARM64 `lge,lg1212` | `4.4.84-229.kcl4tv.6` | Development-IPK installation and launcher startup; rooted Bluetooth signature correction and compatibility-module loading |

The row above records the exact hardware used for regression checks on
2026-08-22. Streaming and controller feedback still depend on the console,
controller transport, and installed build, so hardware-specific reports should
include the TV model as well as the webOS and firmware versions.

---

## Features

- **PS4 and PS5 Remote Play** over local network
- **1080p60 streaming** with H.265, H.265 HDR, or H.264 video codecs
- **Balanced PS5 defaults** — H.265 at 1080p60 and 15 Mbps, with backend-aware codec fallback and bitrate clamping
- **Hardware video decode** via webOS NDL (direct media pipeline) — video is decoded on a dedicated hardware plane below the app surface, not software-rendered
- **Native Opus audio passthrough** — raw Opus packets fed directly to webOS NDL hardware decoder (no software decode step)
- **Minimal built-in GUI** — a lightweight launcher screen on every launch lets you enter your PS5's IP, import your chiaki-ng config, adjust settings, and connect
- **Full gamepad support** — DualSense, DualShock 4, Xbox Wireless Controller, and other Bluetooth/USB gamepads through SDL's standardized controller mapping
- **Controller feedback** — rumble for supported pads, PS5 haptic-to-rumble feedback, and DualSense adaptive triggers, lightbar, and player LEDs
- **Magic Remote friendly** — d-pad navigation, pointer hover/click, number-pad IP entry, and Red as a Back/disconnect substitute
- **chiaki-ng settings import** — drop a `chiaki-ng-Default.ini` export file onto the TV to import registration credentials and the matching manual-host IP
- **Wake-on-LAN** — wakes PS5 from rest mode before connecting (UDP broadcast + unicast)
- **PSN cloud wakeup** — optional Sony push-notification wakeup via PSN session API when local UDP wakeup is unreliable (requires PSN refresh token)
- **Sleep-on-exit** — sends PS5 to rest mode when you quit the app
- **Tiered stream recovery** — requests a clean frame on stalls, rebuilds the decoder on persistent failure, gives watchdog reconnects time to release the prior console session, and retries network errors with cancelable backoff
- **Expanded stats overlay** — bitrate, FPS, codec, decoder latency, frame age, packet loss/FEC, A/V feed outcomes, IDR requests, and reconnect count
- **webOS version auto-detection** — automatically selects the correct NDL backend (`ndl-webos5` or `ndl-webos4`) at runtime
- **Two-tier logging** — critical diagnostics always written; verbose chatter gated by `log_level`

---

## How this fork differs from the original Chiaki-lg build

This fork keeps the original Chiaki-lg hardware-accelerated streaming pipeline,
but improves stream correctness and recovery, controller handling, TV
navigation, and package management. The comparison below refers to the original
`org.homebrew.chiaki` version from which this repository was forked.

| Area | Original Chiaki-lg | This fork |
|---|---|---|
| PlayStation controls | Direct evdev mapping; button and axis aliases can vary by TV kernel | Bundled SDL GameController mappings with corrected Square/Triangle positions, analog L2/R2, sticks, and hotplug |
| Controller feedback | Controller input only | Rumble for supported pads, PS5 haptic-to-rumble conversion, and DualSense adaptive triggers, lightbar, and player LEDs |
| Older webOS DualSense support | No feedback path for TVs without LG's `hid-playstation` driver | Optional rooted compatibility driver and a signature-checked, memory-only correction for LG's Bluetooth output-report bug on supported LG1212/O20 TVs |
| Streaming dependencies | Moving/unversioned dependency inputs | chiaki-ng 1.10.0 and SS4S are pinned to exact revisions; builds verify those inputs and reject modified dependency source trees |
| Video callback contract | Loss/recovery metadata was treated as codec/keyframe metadata | Uses chiaki-ng's exact `frames_lost` / `frame_recovered` contract and detects real H.264/H.265 keyframes from NAL units |
| Congestion and FEC recovery | Packet-loss reporting was implicitly clamped to zero; no configured FEC-to-IDR path | Reports up to 5% loss by default, enables IDR on FEC failure, and allows chiaki-ng to downgrade an unsupported profile |
| Decoder lifecycle | One SS4S player/decoder survives reconnect attempts | Every attempt owns a fresh player, video decoder, and audio decoder; callbacks are joined before teardown |
| Stream stalls and network failures | Fixed four-second reconnect after a subset of failures | Requests IDR frames for short stalls, reconnects after persistent startup/feed/latency failures, waits 10 seconds for the console to release a watchdog-stopped session, and temporarily retries session-in-use collisions with cancelable capped backoff |
| A/V backend outcomes | SS4S feed results were handled as a generic success/failure value; audio results were ignored | Classifies video not-ready/keyframe/error and audio not-ready/overflow/error outcomes, suppresses duplicate audio opens, and supplies Opus frame size |
| Stream diagnostics | Bitrate, FPS, codec, latency, and a combined video error count | Adds two-second loss rate, lost/recovered/FEC totals, last-frame age, per-result A/V counters, IDR requests, reconnects, and a final attempt summary in the log |
| Resolution choices | UI offered 1440p and 2160p even though libchiaki exposes presets only through 1080p | UI and config validation enforce the supported 1080p ceiling; legacy oversized values safely normalize to 1080p |
| PS5 connection failures | An initial session failure can terminate the app | Nonrecoverable registration/protocol/capability errors return to the launcher; transient connection failures retry until canceled |
| Config import | Imports registration credentials; the console IP must be entered separately | Also imports the matching manual-host address, correctly decodes Qt named escapes, and accepts canonical base64 or compatible decimal PSN account IDs |
| Magic Remote | Basic remote handling | D-pad navigation, pointer hover/click, number-pad PIN/IP entry, and Red as Back/disconnect |
| Installation identity | `org.homebrew.chiaki` | Release ID `org.homebrew.chiaki.fork` and development ID `org.homebrew.chiaki.fork.dev`, allowing all variants to coexist |
| Development builds | No source revision in the installed identity | `DEV`-badged icon plus the exact commit hash in the launcher title, description, artifact name, and IPK filename |

The rooted compatibility path is deliberately conservative. It does not edit
LG firmware files, rejects unknown Bluetooth-library signatures and unsupported
kernels, and can be removed with the bundled uninstaller. Root is not required
for normal streaming, controller input, Magic Remote support, or TVs that
already provide a suitable PlayStation driver.

The core limitations remain the same: console registration is performed with
desktop chiaki-ng, and actual codec/resolution support still depends on the
TV's webOS NDL implementation.

---

## Quick start

### Step 1 — Download and install

Download the latest `.ipk` from the [Releases](../../releases/latest) page, then install it on your TV:

```bash
ares-setup-device   # one-time TV setup — TV must be on and in Developer Mode
ares-install --device myTV org.homebrew.chiaki.fork_*.ipk
```

Or use [WebOS Dev Manager](https://github.com/webosbrew/dev-manager-desktop) if you prefer a GUI — it can install IPKs directly via drag and drop.

**TV requirements:** LG webOS 4.x or 5+ TV with [Homebrew Channel](https://github.com/webosbrew/webos-homebrew-channel) installed and Developer Mode enabled.

### Step 2 — Export your chiaki-ng registration

Chiaki-lg does not handle PS5 registration itself. You must have already registered with your PS5 using [chiaki-ng](https://github.com/streetpea/chiaki-ng) on Windows, Linux, MacOS. This is a one-time step.

**On Computer (Win/Linux/MacOS):**
1. Open chiaki-ng
2. Go to **Settings → Config → Export Settings To File**
3. Copy/use that file and rename it `chiaki-ng-Default.ini` if needed


### Step 3 — Copy the file to the TV

```bash
adb push chiaki-ng-Default.ini \
  /media/developer/apps/usr/palm/applications/org.homebrew.chiaki.fork/chiaki-ng-Default.ini
```

Or transfer via [WebOS Dev Manager](https://github.com/webosbrew/dev-manager-desktop)'s file browser.

### Step 4 — Launch and connect

1. Launch **Chiaki-lg Fork** on the TV
2. Click **Import Config**. The matching local PS5 address is imported when it is present in the export; otherwise, enter it manually. The `.ini` file is renamed to `.imported` after a successful import so it won't be reprocessed
3. Open **Settings** and change anything as needed
4. Select **Connect** — streaming starts immediately

> If the export contains several consoles, verify the imported address before connecting. The app currently imports the first registered console and its matching manual host.

---

## Configuration

Config file location on the TV:
```
/media/developer/apps/usr/palm/applications/org.homebrew.chiaki.fork/config.json
```

Most settings are managed through the in-app Settings screen. You can also edit the file directly via SSH or WebOS Dev Manager.

### Example config

```json
{
    "host": "192.168.1.100",
    "ps5": true,
    "psn_account_id": "",
    "registered_key": "",
    "rp_key": "",
    "rp_key_type": 2,
    "video_width": 1920,
    "video_height": 1080,
    "video_fps": 60,
    "video_bitrate": 15000,
    "video_codec": "h265",
    "packet_loss_max": 0.05,
    "idr_on_fec_failure": true,
    "audio_volume": 100,
    "wakeup": true,
    "ps5_mac": "AA:BB:CC:DD:EE:FF",
    "wakeup_delay_ms": 60000,
    "sleep_on_exit": true,
    "log_level": "warning",
    "psn_refresh_token": ""
}
```

### Config reference

| Field | Type | Default | Description |
|---|---|---|---|
| `host` | string | `""` | PS5/PS4 local IP address |
| `ps5` | bool | `true` | `true` for PS5, `false` for PS4 |
| `psn_account_id` | string | `""` | PSN account ID. The canonical chiaki-ng eight-byte base64 form is preferred; a validated unsigned decimal ID is also accepted for compatibility |
| `registered_key` | string | `""` | Registration key (base64) — written by import |
| `rp_key` | string | `""` | Remote Play key (base64) — written by import |
| `rp_key_type` | int | `0` | RP key type — written by import |
| `video_width` | int | `1920` | Stream width; values above 1920 normalize to 1080p |
| `video_height` | int | `1080` | Stream height; values above 1080 normalize to 1080p |
| `video_fps` | int | `60` | Stream frame rate |
| `video_bitrate` | int | `15000` | Target bitrate in kbps |
| `video_codec` | string | `"h265"` | `"h265"`, `"h265_hdr"`, or `"h264"` (PS4 always uses H.264) |
| `packet_loss_max` | number | `0.05` | Advanced: maximum packet-loss fraction reported to chiaki-ng congestion control, from `0.0` to `1.0`. Lower values react more aggressively; invalid values fall back to `0.05` |
| `idr_on_fec_failure` | bool | `true` | Advanced: ask chiaki-ng to request an IDR frame when FEC cannot reconstruct a video frame |
| `audio_volume` | int | `100` | Audio volume (0–100) |
| `wakeup` | bool | `true` | Send wakeup packets before connecting |
| `ps5_mac` | string | `""` | PS5 MAC address (used for Wake-on-LAN) |
| `wakeup_delay_ms` | int | `60000` | Max time (ms) to wait for PS5 to wake |
| `sleep_on_exit` | bool | `true` | Send PS5 to rest mode on app exit |
| `ss4s_module` | string | `"auto"` | SS4S backend. `"auto"` detects webOS before connecting and selects `ndl-webos5` (webOS 5+) or `ndl-webos4` (webOS 4.x) automatically. Override only if auto-detection fails. |
| `log_level` | string | `"warning"` | `"off"`, `"error"`, `"warning"`, `"info"`, `"verbose"`, `"debug"` |
| `psn_refresh_token` | string | `""` | PSN OAuth2 refresh token for cloud wakeup (optional — see below) |

The two advanced recovery fields are intentionally JSON-only and are not shown
as rows in the TV Settings screen. Saving ordinary settings preserves them.
chiaki-ng exports using `packet_loss_reported_max` (or its legacy
`packet_loss_max` name) and `idr_on_fec_failure` are imported when present.
Profile auto-downgrade is always enabled. The recommended balanced values are
`0.05` and `true`; setting the loss cap to `0` disables useful congestion
feedback, while a very large cap can make the console tolerate visible loss for
too long.

### PSN cloud wakeup (optional)

If your PS5 ignores local UDP wakeup packets, you can enable PSN cloud wakeup. Copy the `psn_refresh_token` value from your chiaki-ng config:

- **Windows:** `%APPDATA%\Roaming\Chiaki\Chiaki.conf` → field `psn_refresh_token` under `[General]`
- **Linux:** `~/.config/Chiaki/Chiaki.conf` → same field

Paste the value (beginning with `v3.`) into `psn_refresh_token` in `config.json`. The app uses Sony's push notification service as the primary wakeup method with local UDP as fallback. The token is valid for approximately 60 days. Also confirm that **PS5 Settings → System → Power Saving → Features Available in Rest Mode → Enable Turning On PS5 from Network** is enabled.

---

## Gamepad support

Connect a gamepad via Bluetooth or USB. Supported controllers include DualSense, DualShock 4, Xbox Wireless Controller, and most HID-compliant gamepads.

The app bundles the webosbrew SDL 2.30.12 webOS.6 backport and uses its
standardized GameController mapping. Its kernel-uevent device monitor detects
hotplug/reconnect transitions and input event indices above 31. This keeps the
PlayStation face-button positions and trigger axes consistent across DualSense,
DualShock, Xbox, and other pads.

On webOS, Bluetooth DualSense and DualSense Edge devices are excluded from
SDL's HIDAPI backend and routed through their kernel `/dev/input/event*` node.
The TV app jail can enumerate `/dev/hidraw*` but does not reliably deliver
Bluetooth input reports there. The exception is limited to these two Bluetooth
device IDs; USB DualSense and all other controller backends remain unchanged.

Rumble is sent through SDL's evdev force-feedback path. For PS5 sessions, the
haptic audio stream is translated to the controller motors. A Bluetooth
DualSense additionally receives adaptive-trigger effects, lightbar colour, and
player-LED state through webOS's Bluetooth HID service.

Newer TVs include LG's `hid-playstation` driver and work without setup. The IPK
also bundles a minimal compatibility driver for rooted ARM64 LG1212/O20 TVs
using LG's 4.4.84 kernel (webOS 5/6 generation). It is built against LG's
published `lg1k` kernel source and configuration so its in-kernel structure ABI
matches these TVs. On first launch, the app asks Homebrew Channel's elevated
service to install it under
`/var/lib/webosbrew/chiaki-dualsense` and adds the reversible
`90-chiaki-dualsense` boot hook. Unsupported kernels and non-rooted TVs are left
unchanged. On app launch only, a connected compatible pad is handed from
`hid-generic` to `playstation` before SDL initializes. The boot hook never
rebinds pads, and the app never rebinds one during an active input session. If a
pad is first connected after the app has launched, restart the app once to get
feedback; input remains available through `hid-generic` in the meantime.

These older releases also contain an LG Bluetooth-stack bug that labels HID
output as a feature report. The root component includes a small runtime helper
that scans the running Bluetooth daemon for one exact instruction signature and
changes only the report-type immediate from `3` to `2`. The correction is made
in process memory: the firmware library on disk is never replaced or edited,
ambiguous/unknown builds fail closed, app launch reapplies it after daemon
restarts, and uninstall restores the live byte when applicable. The compatibility
driver is not loaded and Bluetooth pads are not rebound unless that correction
is confirmed active. A failed driver probe or missing input node restores
`hid-generic`, preserving basic controller input instead of leaving the pad
unusable.

The compatibility module is selected by CPU architecture, kernel release,
device-tree platform, and module vermagic. It preserves the descriptor-derived
input mapping while adding native `EV_FF` rumble and the DualSense initialization
needed by Bluetooth trigger/light reports. The rumble mode follows Sony's
firmware feature version when available and defaults to the current vibration-v2
protocol. Installation diagnostics are written to
`/tmp/chiaki-hid-playstation-install.log`; runtime driver messages go to
`/tmp/chiaki-hid-playstation.log`.

To remove the root component and return connected pads to `hid-generic`:

```sh
/var/lib/webosbrew/chiaki-dualsense/uninstall.sh
```

The TV remote is not forwarded to the PS5 as controller input. During streaming it serves only:

| TV Remote Button | Action |
|---|---|
| **Up** | Toggle stats overlay |
| **Other navigation buttons** | Reserved by the app during a stream |
| **Back / Red** | Disconnect and exit the app |
| **Home** | Exit the app |

In the launcher and Settings screens, the Magic Remote supports d-pad/OK,
pointer hover and click, and direct number-pad entry for the console IP.

---

## Logs

Logs are written to `/tmp/chiaki.log` on the TV (cleared on each launch).

```bash
# Stream logs live
ares-shell --device myTV -- "tail -f /tmp/chiaki.log"

# Download the log
ares-shell --device myTV -- "cat /tmp/chiaki.log" > chiaki.log
```

Set `"log_level": "info"` or `"debug"` in config.json for more detail. Critical events (session lifecycle, wakeup, errors) are always logged regardless of level.

---

## Troubleshooting

**Stream doesn't start / connection timeout**
Ensure Remote Play is enabled on your PS5 (Settings → System → Remote Play → Enable Remote Play). Verify the IP in `config.json` and that the TV and PS5 are on the same subnet.
When wakeup is enabled, the app first checks whether the console is already
ready and skips PSN/UDP wakeup in that case. Imported configurations wait up to
60 seconds. If the console remains unreachable, the app returns to the launcher
with a specific wakeup error instead of starting a session that cannot succeed.

**Black screen with audio**
Likely a codec or NDL issue. Check logs. Try `"video_codec": "h264"` as a fallback — H.264 has broader compatibility across NDL versions.

**Stutter, rising latency, or repeated reconnects**
Press **Up** during a stream and watch `Loss (2s)`, `Frame age`, `FEC`, the
decoder/audio result counters, IDR requests, and reconnects. A rising loss/FEC
count points to the network; repeated decoder errors with low loss points to the
TV media backend. The app requests an IDR after a short stall and rebuilds the
entire SS4S player after a persistent stall or five consecutive one-second
latency samples above 750 ms. Each completed attempt is summarized in
`/tmp/chiaki.log` under `[STREAM]`.

**Black screen on webOS 4**
Check `/tmp/chiaki.log` for the `[AUTO]` line confirming which SS4S module was selected. If auto-detection chose `ndl-webos5` incorrectly, set `"ss4s_module": "ndl-webos4"` in `config.json` manually.

**PS5 won't wake from rest mode**
Add your `psn_refresh_token` to `config.json` to enable PSN cloud wakeup (see above).

**Controller input works but rumble/triggers/lightbar do not**
Check `/tmp/chiaki.log` for the `[DUALSENSE]` driver message. Newer TVs provide
the kernel `hid-playstation` driver directly. Rooted LG1212/O20 TVs using LG's
4.4.84 kernel can install the bundled compatibility module automatically; other
older platforms continue to work as input-only through `hid-generic`. Keep the
controller connected while launching the app so the compatibility driver can
claim it before SDL starts. If it is connected later, restart the app once. The
app deliberately avoids rebinding or sending advanced feedback through the
wrong driver during an active controller session.

**Controller is paired but the app detects no buttons**
Set `log_level` to `"info"`, launch the app with the controller connected, and
inspect the `[INPUT]` lines in `/tmp/chiaki.log`. They include SDL's device index,
name, `/dev/input/event*` path, GUID, vendor/product IDs, and mapping decision.
For rooted compatibility installs, also inspect
`/tmp/chiaki-hid-playstation-install.log` and
`/tmp/chiaki-hid-playstation.log`. Do not publish a complete verbose Chiaki log
or `config.json`, because authentication material may be present.

**Import not working / file not found**
Ensure the file is named exactly `chiaki-ng-Default.ini` and placed in:
`/media/developer/apps/usr/palm/applications/org.homebrew.chiaki.fork/`
After upgrading from a build with the old Qt escape parser, press **Import
Config** again. The app reuses the saved `.imported` export; an already written
`config.json` cannot repair previously corrupted registration bytes by itself.

**App crashes on launch**
Usually a malformed `config.json`. Check `/tmp/chiaki.log`. Delete the config file to let the app recreate defaults, then repeat the import.

**App returns to the launcher after “Starting stream”**
The PS5 rejected the initial session request or could not be reached. Verify the
local PS5 address and ensure Remote Play is enabled. The launcher displays the
connection error instead of terminating the app.

---

## Architecture

### Video pipeline

H.264/H.265 NAL units from the chiaki-ng callback are fed directly to `SS4S_PlayerVideoFeed()`, which routes them to webOS NDL's hardware video decoder. The callback's loss and recovery metadata feeds diagnostics, while actual NAL types provide the SS4S keyframe flag. NDL renders on a hardware plane *below* the app's OpenGL surface. The EGL surface is configured with an 8-bit alpha channel and cleared to transparent each frame so the video plane shows through. The SDL/GL layer is only used for UI overlays.

### Audio pipeline

Raw Opus packets from the chiaki-ng audio callback are fed directly to `SS4S_PlayerAudioFeed()` with codec `SS4S_AUDIO_OPUS`. webOS NDL has native Opus hardware decoding — there is no software decode step.

### Session lifecycle and recovery

SS4S is initialized once for a launcher connection cycle, but each connection
attempt creates its own player plus video/audio contexts. Chiaki callbacks are
stopped and joined before those contexts are closed, preventing a reconnect from
feeding an already-unloaded NDL pipeline. chiaki-ng handles congestion feedback,
FEC, and audio jitter buffering. The app adds a watchdog around decoder progress:
IDR requests are rate-limited to one every two seconds, startup is rebuilt after
15 seconds without an accepted frame, an active feed is rebuilt after a
five-second stall, and sustained decoder latency also triggers a rebuild.

### Input pipeline

Gamepad input uses the bundled webosbrew SDL GameController layer. SDL's
standard A/B/X/Y and trigger axes are translated to `ChiakiControllerState`,
with controller hotplug handled by the main event loop. Ordinary rumble uses
SDL/evdev force feedback. DualSense trigger/light state is coalesced and sent
from a rate-limited worker through the public webOS Bluetooth service, keeping
process creation off the render loop.

### webOS version auto-detection

Before connecting, the app reads `/var/run/nyx/os_info.json` to determine the major webOS version, then selects `ndl-webos5` (webOS 5+) or `ndl-webos4` (webOS 4.x). If that file is unreadable it falls back to probing for `libndl-directmedia2.so` on disk. The result is logged as `[AUTO]` in `/tmp/chiaki.log`.

---

## Building from source

Most users should download the IPK from the [Releases](../../releases/latest) page. Build from source only if you want to make code changes or the pre-built IPK doesn't work for your TV.

### Requirements

- Linux or WSL2 build machine
- GCC, CMake ≥ 3.16, make, pkg-config, wget, git, Python 3 + pip
- [webOS native SDK toolchain](https://github.com/webosbrew/native-toolchain) — extract and run `relocate-sdk.sh`
- [ares-cli](https://github.com/webosbrew/ares-cli-rs) in PATH

```bash
pip3 install nanopb protobuf --break-system-packages
```

> **WSL users:** keep source files on a Linux filesystem (`~/`) rather than `/mnt/c/`. Always build via `./build-webos.sh` — not `cmake --build` directly.

### Build

```bash
git clone https://github.com/streetpea/chiaki-ng.git
git -C chiaki-ng checkout 0c4a45df0cae2af2ba2daef84e881850b07038a3
git clone <this-repo-url> chiaki-lg
cd chiaki-lg

export TOOLCHAIN_DIR=~/webos-sdk/arm-webos-linux-gnueabi_sdk-buildroot
bash ./build-webos.sh ../chiaki-ng 2>&1 | tee build.log
```

The script verifies the exact chiaki-ng revision, checks out the exact SS4S
revision, cross-compiles all dependencies, installs the pinned SDL-webOS
backport, applies and then reverses the webOS glibc compatibility patch,
pre-generates nanopb protobuf sources, builds the binary, and packages an `.ipk`
via `ares-package`. Dependencies are cached in `/tmp/webos-staging` and skipped
on subsequent runs.

The IPK is output to `build-webos/*.ipk`.

### CI artifacts and releases

Branch pushes, pull requests, and manual workflow runs produce a `Debug` IPK
under the workflow run's **Artifacts** section. The packaged executable retains
its debug information so it can be used for testing and diagnostics. CI keeps
these artifacts for 14 days. Development builds use the distinct application ID
`org.homebrew.chiaki.fork.dev` and a `DEV`-badged icon, so they can be installed
alongside release builds. Their launcher title and description identify the
exact source commit, and the short commit hash is included in the IPK filename,
for example `org.homebrew.chiaki.fork.dev_0.1.1+a1b2c3d4_arm.ipk`.

Pushing a version tag in the exact `vMAJOR.MINOR.PATCH` format (for example,
`v0.2.0`) builds a stripped `Release` IPK, sets the application version from the
tag, creates the corresponding GitHub Release, and attaches the IPK to it.
Release builds use the application ID `org.homebrew.chiaki.fork` and keep their
configuration separate from development builds.

### Common build errors

**"cannot find -lchiaki"** or **"Syntax error: ( unexpected"** — You ran `cmake --build` directly. Always use `./build-webos.sh`.

**"nanopb_generator.py not found"** — Run `pip3 install nanopb --break-system-packages`.

---

## Source files

| File | Purpose |
|---|---|
| `main.c` | Entry point, session lifecycle, wakeup (UDP + PSN cloud), webOS version detection, SDL event loop |
| `config.c` / `config.h` | JSON config loader, default config generation |
| `config_import.c` / `config_import.h` | chiaki-ng INI settings import |
| `video.c` / `video.h` | Video callback → SS4S/NDL feed, NAL classification, stats counters |
| `audio.c` / `audio.h` | Audio callback → SS4S/NDL Opus feed |
| `input.c` / `input.h` | SDL gamepad mapping, hotplug, rumble, and PS5 haptics |
| `dualsense.c` / `dualsense.h` | Rate-limited DualSense Bluetooth HID feedback |
| `ui.c` / `ui.h` | Launcher UI, settings screen, loading screen, stats overlay renderer |
| `stats.c` / `stats.h` | Thread-safe stream statistics and overlay state |
| `stream_health.c` / `stream_health.h` | Deterministic IDR, stall, latency, and reconnect-backoff policy |
| `app_log.h` | Shared logging macros |
| `NDL_directmedia.h` | webOS NDL API declarations |
| `CMakeLists.txt` | Build system |
| `build-webos.sh` | One-shot cross-compile + IPK packaging script |
| `appinfo.json` | webOS app metadata |
| `config.json` | Default config (deployed to TV, populated on first run) |
| `config.json.example` | Example config template |

---

## Dependencies

| Library | Version | Link | Purpose |
|---|---|---|---|
| chiaki-ng | 1.10.0 (`0c4a45df0cae2af2ba2daef84e881850b07038a3`) | static | PS Remote Play protocol, congestion control, FEC/IDR recovery, audio jitter buffer |
| SS4S | `dfba721b85420ccabf91dac65be73984bf1865f9` | dynamic | webOS NDL video/audio abstraction (ndl-webos4 + ndl-webos5) |
| OpenSSL | 3.2.1 | static | TLS for PSN API, crypto for chiaki |
| Opus | 1.4 | static | Audio codec (chiaki internal use) |
| FFmpeg | 6.1.1 | static | H.264/H.265 codec support |
| json-c | 0.17 | static | Config file parsing |
| cURL | 8.7.1 | static | PSN API HTTP calls |
| miniupnpc | 2.2.7 | static | UPnP (chiaki dependency) |
| libevent | 2.1.12-stable | static | Event loop for chiaki-ng remote hole punching |
| GF-Complete | master | static | Erasure coding (chiaki dependency) |
| Jerasure | 2.0 | static | FEC (chiaki dependency) |
| SDL-webOS | 2.30.12 webOS.6 | bundled dynamic | Window/GL surface, uevent-based controller hotplug, mapping/rumble, TV remote input |
| nanopb | 0.4.x | static | Protobuf (chiaki submodule) |

---

## License

AGPL-3.0 — same as [chiaki-ng](https://github.com/streetpea/chiaki-ng), on which this project is based.
