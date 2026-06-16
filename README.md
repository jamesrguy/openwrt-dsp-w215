# OpenWrt for the D-Link DSP-W215

A work-in-progress OpenWrt port for the D-Link **DSP-W215** Wi-Fi smart plug.
The goal: replace the end-of-life, insecure stock firmware (see
`../dsp-w215-security-findings.md`) with a modern, maintained, **local-only**
firmware that exposes the relay + power meter to Home Assistant over MQTT.

## Why this is feasible

The DSP-W215 is built on the **Atheros AR9330 "Hornet"** SoC — one of the
best-supported chips in OpenWrt (`ath79` target). Everything we need is either
already supported or solvable in userspace (facts established by
reverse-engineering the stock firmware — see `../dsp-w215-meter-protocol.md`
and `../CLAUDE.md` history):

| Subsystem | Plan | Status |
|-----------|------|--------|
| SoC / Wi-Fi | AR9330 + `ath9k` (mainline) | supported by OpenWrt |
| Flash | SPI-NOR, `spi-nor` + fixed-partitions | supported |
| Relay | GPIO (`RELAY_SWITCH_GPIO`) via sysfs/`gpio-export` | DTS + `dspw215-meterd` |
| Status LED | bi-colour GPIO 26 / 27 | DTS `gpio-leds` |
| Buttons | Atheros `simple_config` (reset/WPS) | DTS `gpio-keys` |
| Power meter | **Prolific PL8331** on UART `ttyS0` @115200, userspace daemon | `dspw215-meterd` |
| HA integration | MQTT + MQTT Discovery | `dspw215-meterd` (or the `dspw215` bridge) |

## Status: **Phase 0 — scaffold + hardware bring-up**

This directory contains the *source files* for the port and the procedure to
finish it. **It is not yet a flashable image** — a working build needs a few
values that can only be read off the physical board (flash size, exact MTD
offsets, the relay/button GPIO numbers, and how the meter UART is wired). Those
are marked `CONFIRM:` throughout and enumerated in [HARDWARE.md](HARDWARE.md).

**Start here:** [PLAN.md](PLAN.md) is the end-to-end route from this scaffold to
a flashed, working device (with non-destructive RAM-boot validation before any
flash write). [UART-EXTRACTION.md](UART-EXTRACTION.md) is the step-by-step for
pulling the needed values + a flash backup off the device over UART.

Roadmap:

- [x] Identify SoC, Wi-Fi, console baud, rootfs partition, meter chip/transport
- [x] Scaffold DTS, image recipe, board config, meter daemon package
- [ ] **Bring-up:** open device, attach UART, dump flash, read `/proc/mtd`,
      identify relay/button GPIOs ([HARDWARE.md](HARDWARE.md))
- [ ] Fill `CONFIRM:` values in the DTS / image recipe
- [ ] Capture the PL8331 serial frames; implement `poll_meter()` in `meterd`
- [ ] Build in an OpenWrt tree and flash via U-Boot TFTP ([BUILD.md](BUILD.md))
- [ ] Verify relay + meter + Wi-Fi, then submit upstream

## Layout

```
openwrt/
├── README.md                 # this file
├── PLAN.md                   # end-to-end roadmap: scaffold -> flashed device
├── UART-EXTRACTION.md        # pull values + flash backup off the device via UART
├── HARDWARE.md               # teardown + flash + GPIO bring-up checklist
├── BUILD.md                  # add to OpenWrt buildroot, build, flash, recover
├── tools/
│   └── stock-hw-dump.sh      # one-shot stock-firmware data collector
├── dts/
│   └── ar9330_dlink_dsp-w215.dts
├── image/
│   └── dsp-w215.mk           # Device/ recipe to merge into ath79 image Makefile
├── base-files/etc/board.d/
│   ├── 01_leds
│   └── 02_network
└── package/dspw215-meterd/   # PL8331 meter reader + MQTT bridge (procd service)
    ├── Makefile
    └── files/
        ├── src/meterd.c
        ├── dspw215-meterd.init
        └── dspw215-meterd.config
```

## ⚠️ Safety

This device switches **mains voltage**. Only open/probe it unplugged, never
work on it live, and preserve the **`art`** (radio calibration) partition or the
Wi-Fi will be out of spec. Flashing carries a brick risk — a soldered UART for
U-Boot recovery is strongly recommended (see HARDWARE.md / BUILD.md).
