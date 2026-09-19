# EDID images for the capture card

The card presents an EDID to the console, and the console picks its output mode from it. The factory
EDID advertises 3840x2160p60 with PQ HDR, so a console always chooses 4K60 HDR — and a passthrough
display that cannot sync to that shows "unsupported input" and stays black.

Writing a different EDID is the only reliable way to change that: the `edid_source` control and
`color_deep` parameter were both measured to leave the console on 4K60 HDR.

| File | Advertises |
|---|---|
| `factory-4K60ProMK2.edid` | The card's original: 2160p60 preferred, 2160p50/30/25/24, 1080p120, BT.2020, PQ HDR, deep colour, 300 MHz TMDS |
| `1080p60-sdr.edid` | 1920x1080p60 preferred, nothing above it, no HDR, no BT.2020, no deep colour, 150 MHz TMDS. LPCM 2ch 32/44.1/48 kHz audio preserved. |

## Applying one

```bash
quadcapd --set-edid packaging/edid/1080p60-sdr.edid
quadcapd --get-edid /tmp/readback.edid    # verify
```

The console renegotiates within a second or two — no reboot, no console-side settings.

## It does NOT persist across a power cycle

On the 4K60 Pro MK.2 the write goes to the frontend MCU (fn 0x59 UpdateEDID), and that is **volatile**
— after a reboot the card is back to advertising 4K60 HDR and the passthrough display goes black
again. Measured: written and verified, then factory again after the next boot.

`quadcap-edid.service` reapplies it at every boot. Point it at the image you want:

```bash
sudo sed -i 's|^QUADCAP_EDID_IMAGE=.*|QUADCAP_EDID_IMAGE=/usr/local/share/quadcap/edid/1080p60-sdr.edid|' \
  /etc/quadcap/edid.conf
sudo systemctl restart quadcap-edid.service
```

Leaving `QUADCAP_EDID_IMAGE` empty keeps the factory EDID. Restore the original by hand at any
time:

```bash
quadcapd --set-edid packaging/edid/factory-4K60ProMK2.edid
```

## Before writing your own

Take a backup first — this is the card's own EEPROM:

```bash
quadcapd --get-edid my-factory-backup.edid
```

The driver validates the image before writing (128–512 bytes, a multiple of 128, valid header,
correct extension count, and every block must checksum), and verifies the readback afterwards. On
the MK.2 the write goes through the MCU's fn 0x59 protocol on its safe port rather than raw EEPROM
paging.
