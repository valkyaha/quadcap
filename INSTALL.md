# Installing quadcap

Works on any machine with a supported card, including with Secure Boot enabled.

Download and extract the Linux bundle from the latest GitHub release, then run:

```bash
sudo ./packaging/install.sh
```

The release bundle contains prebuilt Ubuntu 24.04+ x86-64 binaries and the exact pinned driver
source. A source checkout can be built and installed with:

```bash
cmake -S . -B build -G Ninja -DCMAKE_BUILD_TYPE=Release
cmake --build build
sudo ./packaging/install.sh
```

If the installer says an action is needed, **reboot and complete MOK enrollment** — see
[Secure Boot](#secure-boot) below. Otherwise you are done: run `quadcap`.

---

## What the installer does

| Step | Why |
|---|---|
| Installs `dkms`, `build-essential`, kernel headers | Needed to compile an out-of-tree module |
| Registers the driver with **DKMS** | Rebuilds it automatically on every kernel update, and signs it |
| Writes `/etc/modprobe.d/quadcap-sc0710.conf` | Keeps the tuned module options across reboots |
| Writes `/etc/modules-load.d/quadcap-sc0710.conf` | Loads the driver at boot |
| Installs a udev rule | Gives the `video` group access to the capture node |
| Adds you to the `video` group | So the app can open the device without root |
| Installs and enables `quadcap-edid.service` | Reapplies the EDID at boot, since the card forgets it |
| Installs `quadcap`, `quadcapd`, desktop entry, icon, EDID images | Launchable from your application menu |

It is safe to re-run, and `packaging/uninstall.sh` reverses all of it.

### Why DKMS rather than a hand-built module

This card has no in-tree driver, so a module has to be compiled for your running kernel. Building it
by hand works exactly once: the next kernel update leaves you with a module that no longer matches,
and a `modprobe` that fails. DKMS rebuilds it as part of the kernel upgrade, and on Debian and
Ubuntu it also signs the result, which is what makes Secure Boot work at all.

### Module options that are set, and why

```
options sc0710 hw_tonemap=1 procedural_timings=1
```

- **`hw_tonemap=1`** moves HDR→SDR tonemapping onto the card's MCU. The host path runs per-pixel in
  the DMA completion handler and costs roughly two thirds of the frame rate — measured 8.07 fps
  against 59.92 fps on the same 4K signal.
- **`procedural_timings=1`** makes the driver derive capture geometry from its own registers instead
  of a static timing table that misidentifies some UHD modes as DCI 4K, which yields a sheared,
  unrecognisable picture.

Both are explained under Troubleshooting in the [README](README.md).

---

## Secure Boot

Secure Boot will not load a module unless it is signed by a key the firmware trusts. Nothing here
requires turning it off.

DKMS signs with the machine-owner key at `/var/lib/shim-signed/mok/`. If another DKMS module
(NVIDIA, VirtualBox, ZFS) already loads on your machine, that key is enrolled and **everything just
works** — the installer detects this and says so.

If it is not yet enrolled, the installer runs `update-secureboot-policy --enroll-key`, which asks
you to choose a one-time password. Then:

1. **Reboot.**
2. A blue **MOK Manager** screen appears before the desktop. Choose **Enroll MOK** → **Continue**.
3. Enter the password you just chose.
4. The machine boots normally, and the driver loads by itself from then on.

That screen only appears once. If you miss it, re-run the installer.

> Choosing not to reboot leaves the driver installed but unloadable. The app detects that state and
> says so rather than showing an empty window.

---

## Checking what is wrong

```bash
quadcapd --setup
```

Reports the first thing blocking capture and the exact command to fix it. Exit status is 0 when
ready, 1 otherwise, so it is usable in a script. The app shows the same guidance on screen when it
cannot capture, with the command available to copy.

Example on a machine where the driver was never installed:

```
issue: driver-not-installed
card: present
module: not loaded
dkms: not registered
secure boot: enabled

The sc0710 driver is not installed
  1. This card has no in-tree driver, so one has to be built for your kernel...
  2. Run the installer from the project directory.
  3. Secure Boot is on, so the module must be signed by a key your firmware trusts...

  $ sudo ./packaging/install.sh
```

---

## Requirements

- Linux with kernel 6.12 or newer (tested on 6.12 through 7.0)
- A supported card: Elgato 4K60 Pro MK.2, 4K Pro, or Cam Link Pro (PCI `12ab:0710`)
- A PCIe slot wired **x4 or wider** — a narrower slot cannot carry 4K60
- Qt 6.4+, GStreamer 1.22+, and `gstreamer1.0-qt6` for the preview
- An NVIDIA GPU for hardware encoding; without one, pass `--software` to `quadcapd`

Build dependencies on Debian/Ubuntu:

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

---

## Passthrough display

The card splits HDMI in hardware, so the display on HDMI OUT has to be able to sync to whatever mode
the console chooses — and the console chooses it from the EDID the card advertises.

### What the console is offered decides everything

The card advertises an EDID naming itself `4K60ProMK2VRR` and offering 3840x2160p60 at 594 MHz with
PQ HDR, so a console picks 4K60 HDR and a display that cannot sync to that stays black.

The driver's `edid_source` control (internal, display, merged) has no observable effect on the
MK.2 — all three present a byte-identical EDID — so it is left at the driver's default. Constraining
the console means replacing the EDID image itself.

### Making the passthrough display work

Write an EDID the display can actually satisfy. Two are included:

```bash
quadcapd --set-edid packaging/edid/1080p60-sdr.edid     # 1080p60 SDR, works on essentially anything
quadcapd --set-edid packaging/edid/factory-4K60ProMK2.edid   # back to the card's original
```

The console renegotiates within a second or two, with nothing to change on the console.

**The write is volatile on the MK.2** — it goes to the frontend MCU, and a reboot brings the factory
4K60 HDR EDID back. `quadcap-edid.service` reapplies it at boot; point it at the image you want:

```bash
sudo sed -i 's|^QUADCAP_EDID_IMAGE=.*|QUADCAP_EDID_IMAGE=/usr/local/share/quadcap/edid/1080p60-sdr.edid|' \
  /etc/quadcap/edid.conf
sudo systemctl restart quadcap-edid.service
```

See [packaging/edid/README.md](packaging/edid/README.md).

Measured going from the factory EDID to `1080p60-sdr.edid`:

```
before:  3840x2160p60   BT_2020  HDR-PQ
after:   1920x1080p60   BT_709   SDR      (held steady, capture 20.3 Mbps, all 3 audio tracks)
```

Capture follows whatever the console outputs, so a 1080p console means genuine 1080p recordings —
the pipeline sizes itself to the incoming signal rather than upscaling.


### A 4K60 passthrough display is the clean answer

If the display on HDMI OUT can sync to 4K60 HDR, none of the above matters: the console outputs 4K60,
the display shows it, and capture runs at 4K60.

---

## Uninstalling

```bash
sudo ./packaging/uninstall.sh
```

Removes the driver, its DKMS registration, all configuration and both binaries. Recordings and the
flashback ring are left alone. The Secure Boot key stays enrolled, since other DKMS modules may rely
on it.
