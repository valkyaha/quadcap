# quadcap

**Elgato game capture for Linux.** quadcap records 4K60 console gameplay from an Elgato PCIe HDMI
capture card — the 4K60 Pro MK.2, 4K Pro or Cam Link Pro — on a platform Elgato never shipped
software for. Hardware HDMI passthrough to your monitor, instant replay, separate game and
microphone tracks, and a virtual camera for OBS Studio.

[![Release](https://img.shields.io/github/v/release/valkyaha/quadcap?sort=semver)](https://github.com/valkyaha/quadcap/releases)
[![Downloads](https://img.shields.io/github/downloads/valkyaha/quadcap/total?label=downloads)](https://github.com/valkyaha/quadcap/releases)
[![Latest downloads](https://img.shields.io/github/downloads/valkyaha/quadcap/latest/total?label=downloads%40latest)](https://github.com/valkyaha/quadcap/releases/latest)
[![Stars](https://img.shields.io/github/stars/valkyaha/quadcap?style=flat)](https://github.com/valkyaha/quadcap/stargazers)
[![Licence](https://img.shields.io/github/license/valkyaha/quadcap)](LICENSE)
[![Issues](https://img.shields.io/github/issues/valkyaha/quadcap)](https://github.com/valkyaha/quadcap/issues)

Plug a console into the card, play on the passthrough display, and record what you played.

---

## Features

- **Up to 4K60 capture**, hardware-encoded to HEVC or H.264 on an NVIDIA GPU
- **Instant replay** — a continuous keyframe-aligned ring buffer, so you can save the last N minutes
  after something happens. Saving is a remux, so it completes in well under a second regardless of
  buffer length
- **Live audio mix and monitoring** — hear and meter console audio, balance or mute the mix, and
  retain raw independent tracks for post-production, drift-corrected against the capture clock
- **Live preview** with a GPU-side path — the video never makes a round trip through system memory
- **Output follows the source** — a 1080p console produces genuine 1080p files at a proportional
  bitrate, not upscaled 4K
- **Guided first run** — the app diagnoses a missing driver, an unenrolled Secure Boot key or a
  permissions problem and shows the exact command to fix it
- **Survives reboots and kernel upgrades** — DKMS driver, signed for Secure Boot, loaded at boot

## Supported hardware

| Card | PCI ID | Status |
|---|---|---|
| Elgato Game Capture 4K60 Pro MK.2 | `12ab:0710` / `1cfa:000e` | Developed and tested against |
| Elgato Game Capture 4K Pro | `12ab:0710` / `1cfa:0012` | Driver supports it; untested here |
| Elgato Cam Link Pro | `12ab:0710` / `1cfa:0011` | Driver support is experimental |

Capture itself is handled by the out-of-tree [`sc0710`](https://github.com/Nakildias/sc0710) kernel
driver, which quadcap installs and configures for you.

## Streaming to OBS

OBS cannot open the capture card while quadcap holds it, so quadcap hands it the signal instead.
Switch on **Send to OBS** and three sources appear:

| In OBS | Add as | Carries |
| --- | --- | --- |
| `quadcap` | Video Capture Device (V4L2) | The picture |
| Monitor of `quadcap game audio` | Audio Output Capture | Console sound |
| Monitor of `quadcap microphone` | Audio Output Capture | Your microphone |

The two audio sources are deliberately separate rather than pre-mixed, so the game can be ducked
under your voice, and the microphone gated or compressed, without either touching the other. The
faders in quadcap shape only what it records; what OBS receives is unprocessed.

`packaging/install.sh` sets this up: `v4l2loopback` for the virtual camera and two PipeWire null
sinks for the audio, both configured to survive a reboot. Recording and the preview keep priority —
the branches feeding OBS drop frames rather than let a slow consumer stall capture.

### Getting the scene into OBS

Rather than adding three sources by hand, press **Create OBS scene**. It writes a scene collection
wired to the devices on this machine, which OBS then offers under **Scene Collection → quadcap**.
Close OBS first: it keeps its collections in memory and writes them out when it exits, so anything
added underneath a running instance is discarded. quadcap checks and will tell you.

Headless, the same two things:

```bash
quadcapd --write-obs-scene       # writes the collection to OBS's config directory
quadcapd --record out.mkv --obs  # record and feed OBS at the same time
```

The scene names only the device and its input, leaving OBS to detect format, resolution and frame
rate, so it keeps working when the console changes mode.

## Requirements

- Linux, kernel 6.12 or newer
- A PCIe slot wired **x4 or wider** — a narrower slot cannot carry 4K60
- NVIDIA GPU for hardware encoding (software encoding works, but not at 4K60)
- Qt 6.4+, GStreamer 1.22+

## Install

Every [release](https://github.com/valkyaha/quadcap/releases/latest) ships three downloads. All of
them still need the `sc0710` kernel driver, which only `packaging/install.sh` sets up — it handles
DKMS so the module survives a kernel upgrade, and signs it so it loads under Secure Boot.

**AppImage** — the portable option, with Qt and GStreamer bundled:

```bash
chmod +x quadcap-*-x86_64.AppImage
./quadcap-*-x86_64.AppImage
```

**Debian package** — for the Ubuntu release it was built against. quadcap links Qt's QML private
ABI, so the package pins the exact Qt patch version and will refuse to install against another one.
Use the AppImage or build from source on other releases:

```bash
sudo apt install ./quadcap_*_amd64-ubuntu24.04.deb
```

**Source bundle** — carries the source, the pinned driver, and prebuilt binaries:

```bash
tar -xf quadcap-*-linux-x86_64.tar.xz
cd quadcap-*-linux-x86_64
sudo ./packaging/install.sh
```

Each download has a `.sha256` beside it. Verify before installing:

```bash
sha256sum -c quadcap-*.sha256
```

To build from source instead:

```bash
git clone https://github.com/valkyaha/quadcap.git
cd quadcap
cmake -S . -B build -G Ninja -DCMAKE_BUILD_TYPE=Release
cmake --build build
sudo ./packaging/install.sh
```

Build dependencies on Debian and Ubuntu:

```bash
sudo apt install cmake ninja-build qt6-base-dev qt6-declarative-dev \
  qml6-module-qtqml qml6-module-qtqml-models qml6-module-qtqml-workerscript \
  qml6-module-qtquick qml6-module-qtquick-controls qml6-module-qtquick-layouts \
  qml6-module-qtquick-templates qml6-module-qtquick-window \
  libgstreamer1.0-dev libgstreamer-plugins-base1.0-dev \
  gstreamer1.0-alsa gstreamer1.0-gl gstreamer1.0-plugins-base \
  gstreamer1.0-plugins-good gstreamer1.0-plugins-bad gstreamer1.0-qt6 \
  gstreamer1.0-pipewire v4l-utils
```

**Secure Boot is supported and does not need to be disabled.** The driver is installed through DKMS,
which signs it with your machine-owner key. If that key has never been enrolled, the installer
enrols it and tells you to confirm at the blue MOK Manager screen on the next boot.

Full details in [INSTALL.md](INSTALL.md).

## Usage

Launch **quadcap** from your application menu, or:

```bash
quadcap
```

There is also a headless companion for scripting, diagnostics and soak testing:

```bash
quadcapd --setup                      # what, if anything, is blocking capture
quadcapd --status                     # card, driver and current signal
quadcapd --watch                      # follow signal changes
quadcapd --record out.mkv --seconds 60
quadcapd --get-edid backup.edid       # back up the card's EDID
quadcapd --set-edid image.edid        # change what modes the console is offered
```

Recordings are written to `~/Videos/quadcap/`.

## Troubleshooting

### The passthrough display shows "unsupported input"

The card advertises an EDID offering 4K60 with HDR, so a console always picks that. If the display
on HDMI OUT cannot sync to 4K60 HDR it stays black.

Write a narrower EDID:

```bash
quadcapd --set-edid /usr/local/share/quadcap/edid/1080p60-sdr.edid
```

The console renegotiates in a second or two. On the MK.2 this is volatile, so
`quadcap-edid.service` reapplies it at boot — point it at your chosen image in
`/etc/quadcap/edid.conf`. See [packaging/edid/README.md](packaging/edid/README.md).

### Capture runs at a fraction of the expected frame rate

The driver can tonemap HDR to SDR on the CPU, inside the DMA completion path, which throttles frame
delivery — measured at 8.07 fps against 59.92 fps for the same signal. `hw_tonemap=1` moves it onto
the card's MCU and is set by the installer.

### The picture is sheared or unrecognisable

Some UHD modes are misidentified as DCI 4K by the driver's static timing table, which makes it
report a 4096-pixel stride for 3840-wide frames. `procedural_timings=1` takes the geometry from the
card's registers instead, and is set by the installer.

### Nothing starts, and the error mentions GStreamer

The card's ALSA node is exclusive, so only one capturing process at a time. The desktop app refuses
to start a second copy and raises the existing window instead; check for a stray `quadcapd` too.

## Frequently asked questions

### Does the Elgato 4K60 Pro work on Linux?

Yes. Elgato ships no Linux software and no Linux driver, but the card is driven by the open-source
[`sc0710`](https://github.com/Nakildias/sc0710) kernel module, and quadcap is the application on top
of it. `packaging/install.sh` installs both. Development and testing were done on a 4K60 Pro MK.2.

### Is there an Elgato Game Capture HD alternative for Linux?

quadcap is that application for the supported PCIe cards: preview, recording, instant replay and an
audio mixer. It does not support Elgato's USB devices — see below.

### Which Elgato capture cards are supported?

The three PCIe cards in [Supported hardware](#supported-hardware): 4K60 Pro MK.2, 4K Pro and
Cam Link Pro, all of which are `12ab:0710` boards. USB devices — HD60 S, HD60 X, Cam Link 4K and the
original Game Capture HD — use entirely different hardware and are **not** supported by this project.

### How do I use an Elgato capture card with OBS Studio on Linux?

OBS cannot open the card while quadcap is using it, so quadcap passes the signal on instead: the
picture through a virtual camera and each audio source through its own sink. See
[Streaming to OBS](#streaming-to-obs). quadcap can also write the OBS scene for you.

### Does HDMI passthrough add latency to the game?

No. Passthrough is done in the card's hardware and does not pass through the PC, so the display
attached to the card's HDMI output is unaffected by what the software is doing — or by whether the
PC is even running quadcap.

### Does it work with Secure Boot enabled?

Yes. The driver is installed through DKMS and signed with the machine's own MOK key, so it rebuilds
on kernel upgrades and still loads with Secure Boot on. The installer walks through enrolling the
key, which needs a reboot and a password you set. See [INSTALL.md](INSTALL.md).

### What about HDR?

The card tone-maps HDR to SDR for capture on its own MCU, which is what keeps 4K60 at full frame
rate. If a console picks a mode the rest of your chain cannot carry, quadcap can change the EDID the
card advertises so it negotiates something else — see [Troubleshooting](#troubleshooting).

### Do I need an NVIDIA GPU?

For 4K60, in practice yes: recording uses NVENC. Software encoding works and is selectable with
`--software`, but will not keep up at 4K60 on most machines.

## Contributing

Contributions are welcome. Start with [CONTRIBUTING.md](CONTRIBUTING.md) for the development
workflow, coding standards and test requirements. All participation is covered by our
[Code of Conduct](CODE_OF_CONDUCT.md).

## Licence

Apache License 2.0 — see [LICENSE](LICENSE).

The bundled `sc0710` kernel driver is a separate project with its own licence, fetched at build time
rather than vendored into this repository.

Not affiliated with, endorsed by, or sponsored by Corsair or Elgato. Product names are trademarks of
their respective owners and are used only to identify compatible hardware.
