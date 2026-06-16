# Deployment plan: from scaffold to a flashed, working device

This is the end-to-end route from the current state (reverse-engineering done,
port scaffolded) to **OpenWrt running on a physical DSP-W215, switching the
relay and reporting power into Home Assistant.**

Each phase has an **exit gate** — don't proceed until it's met. The ordering is
deliberately *non-destructive first*: everything that can go wrong is shaken out
in RAM, over TFTP, before a single byte is written to flash.

```
Phase 0  RE + scaffold ............................. DONE
Phase 1  Pull hardware facts over UART ............. needs the device  ← you are here
Phase 2  Fill the port from those facts ............ desk work
Phase 3  Build images on a Linux host .............. desk work
Phase 4  Boot in RAM over TFTP (no flash writes) ... validation gate
Phase 5  Meter bring-up (can trail Phase 6) ........ the hard part
Phase 6  Flash to NOR ............................... the irreversible step
Phase 7  Home Assistant + hardening ................ make it useful
Phase 8  factory image / upstream .................. optional
```

A useful milestone sits **after Phase 6 even without Phase 5**: a fully working
OpenWrt plug (Wi-Fi + relay + LEDs + LuCI/SSH), just without energy metering.
Treat the meter as a follow-on so it can't block first boot.

---

## Phase 0 — RE + scaffold  ✅ done

Established: AR9330 SoC, `ath9k` Wi-Fi, `console=ttyS0,115200`, LEDs GPIO 26/27
(+19), `/dev/gpio` access model, PL8331 meter on `ttyS0`, button kernel drivers,
relay safety cut-outs. Scaffolded the DTS, image recipe, board.d, and the
`dspw215-meterd` package. Remaining unknowns are tagged `CONFIRM:` and listed in
[HARDWARE.md](HARDWARE.md).

---

## Phase 1 — Pull the hardware facts over UART

**Goal:** turn every `CONFIRM:` into a real value, and get a verified full-flash
backup before anything is at risk.

**Do:** follow [UART-EXTRACTION.md](UART-EXTRACTION.md) end to end (solder/attach
the 3.3 V adapter, get the boot log + root shell, run
[`tools/stock-hw-dump.sh`](tools/stock-hw-dump.sh)).

**Outputs to capture:**
- Full **U-Boot + kernel boot log** (RAM size, flash size + JEDEC id, board init).
- `/proc/mtd` → exact partition offsets/sizes.
- Relay GPIO number (via the `gpio_out` diff), LED polarity, gpio19 role.
- Reset/relay **button GPIO numbers** (from the boot log / trace).
- **Per-partition flash backup** (`mtd*.bin`), the `art` partition especially.
- A `/var/tmp/MeterWatt` + `/var/tmp/MeterStatus` sample under a known load.

**Exit gate:**
- [ ] `mtd*.bin` for **every** partition is copied off the device and the `art`
      image is set aside, read-verified (non-zero, correct size).
- [ ] RAM size, flash size, and the full partition map are written down.
- [ ] Relay + reset-button GPIO numbers known; LED polarity known.

> If you get **nothing else**, get the flash backup + `/proc/mtd`. Those two make
> the rest recoverable and unbrickable.

---

## Phase 2 — Fill the port from those facts

Desk work in this repo, no device needed.

**Do — edit these files with the Phase 1 values:**

| Value | File | What to set |
|-------|------|-------------|
| Flash size | `image/dsp-w215.mk` | `IMAGE_SIZE` = firmware-region size; subtarget `tiny` (≤8 MB) vs `generic` |
| Partition map | `dts/ar9330_dlink_dsp-w215.dts` | `partitions` node offsets/sizes; keep `u-boot`(0) + `art` |
| ART offset | `dts/...dts` | `mtd-cal-data = <&art 0x….>` (cal data offset inside `art`) |
| Relay GPIO + polarity | `dts/...dts` | `gpio-export` `relay` node |
| Button GPIO(s) | `dts/...dts` | `gpio-keys` `reset` (and `wps` if separate) |
| LED polarity | `dts/...dts` | `ACTIVE_LOW`/`HIGH` on the 26/27 nodes; enable gpio19 led if identified |
| RAM (if non-default) | DTS `memory` node | size |

**Decision points to settle here:**
- **Subtarget:** `tiny` if flash ≤ 8 MB / RAM ≤ 32 MB, else `generic` (move the
  recipe to `generic.mk`). Drives which features fit.
- **Meter UART strategy:** if the meter and console are the *same* UART, decide
  whether OpenWrt keeps a serial console (default) and `meterd` shares it, or you
  give the port to `meterd` and rely on SSH. Record in the DTS `chosen`/`bootargs`.

**Exit gate:**
- [ ] No `CONFIRM:` markers remain in `dts/` or `image/` (grep is clean).
- [ ] DTS compiles in-tree (Phase 3 will confirm).

---

## Phase 3 — Build the images

On a Linux build host. Full mechanics in [BUILD.md](BUILD.md); the essentials:

1. Clone OpenWrt, drop in the DTS, append the recipe, copy `board.d` + the
   `dspw215-meterd` package.
2. `make menuconfig`: ATH79 → the chosen subtarget → **Target Profile: D-Link
   DSP-W215**; select `kmod-ath9k`, `wpad-basic-mbedtls`, `dspw215-meterd`,
   `libmosquitto-nossl`. **Also enable the RAM image:** Global build settings →
   *ramdisk* (`CONFIG_TARGET_ROOTFS_INITRAMFS=y`) so you get an
   `…-initramfs-kernel.bin` for Phase 4.
3. `make -j$(nproc)`.

**Outputs:**
- `…-dlink_dsp-w215-initramfs-kernel.bin`  ← boots entirely in RAM (Phase 4)
- `…-dlink_dsp-w215-squashfs-sysupgrade.bin` ← the on-flash image (Phase 6)

**Exit gate:**
- [ ] Both images build without error.
- [ ] `sysupgrade.bin` is **≤** your flash firmware-region size.

---

## Phase 4 — Boot in RAM over TFTP (the validation gate)

**This writes nothing to flash.** It proves the DTS/driver bring-up before you
risk the device.

1. TFTP-serve `…-initramfs-kernel.bin` from your PC.
2. At the U-Boot prompt (from UART):
   ```text
   setenv ipaddr 192.168.1.1
   setenv serverip 192.168.1.2
   tftpboot 0x80060000 openwrt-...-dlink_dsp-w215-initramfs-kernel.bin
   bootm 0x80060000
   ```
3. OpenWrt comes up in RAM. Verify on the console / over SSH (`192.168.1.1`):

| Check | How |
|-------|-----|
| Boots cleanly | console reaches OpenWrt login; no DTS panics |
| Wi-Fi | `iw dev` lists the radio; ART cal applied (no "eeprom" errors in `dmesg`) |
| LEDs | `echo 1 > /sys/class/leds/<green>/brightness` lights the right colour |
| Relay | toggle the `relay` gpio-export; **hear the click**, socket switches |
| Buttons | `evtest`/`logread` shows reset/wps presses |
| Meter port | `dspw215-meterd -r /dev/ttyS0` sees PL8331 bytes (if console freed) |

**Exit gate:**
- [ ] Boots in RAM with **relay + LEDs + Wi-Fi** all correct.
- [ ] If anything's wrong, fix the **DTS** and rebuild — *still no flash writes*.
      Loop Phase 3→4 until green.

> If U-Boot can't TFTP (some D-Link AR9330 U-Boots are locked-down), the
> fallback is a built-in bootloader recovery (often a fixed IP + TFTP filename —
> check the boot log) or, worst case, flash directly in Phase 6 with the backup
> as your safety net. RAM-boot is strongly preferred if available.

---

## Phase 5 — Meter bring-up (the hard part; can trail Phase 6)

The exact PL8331 on-wire framing is the one thing static RE couldn't fully
recover (it shares the console UART). Finish it from a capture:

1. On the stock firmware (or an OpenWrt RAM boot), capture the PL8331
   command/response bytes — see [UART-EXTRACTION.md](UART-EXTRACTION.md) §Meter
   (`killall prolific` then tap, or `meterd -r`).
2. Drive a **known resistive load** (e.g. a 60 W bulb); correlate raw frames with
   the decoded `/var/tmp/MeterWatt`.
3. Implement `poll_meter()` in `package/dspw215-meterd/files/src/meterd.c`
   (parse offsets for V/I/W/energy/temp; nail the raw→watts scaling).
4. Rebuild; verify `meterd` publishes watts matching the bulb.

**Exit gate:**
- [ ] `meterd` reports power within a few % of a known load, and 0 W when off.

Until this passes, ship relay + LEDs + Wi-Fi (Phase 6) and treat metering as the
next iteration.

---

## Phase 6 — Flash to NOR (the irreversible step)

Only after Phase 4 is green and the Phase 1 backups are verified.

- **Preferred:** from the OpenWrt RAM boot, `sysupgrade -n` the
  `…-squashfs-sysupgrade.bin`.
- **Or** from U-Boot: `tftpboot` then `erase`/`cp.b` into the **firmware** region
  only (offsets from `/proc/mtd` — *never* touch `u-boot`@0 or `art`). See
  [BUILD.md](BUILD.md) §4.

**Exit gate:**
- [ ] Reboots **from flash** into OpenWrt unaided.
- [ ] Survives a power-cycle; config persists (overlay mounts).
- [ ] Wi-Fi/relay/LEDs still correct from the flashed image.

**Rollback:** UART + U-Boot TFTP re-flashes either OpenWrt or your **stock**
`mtd*.bin` backup. This is why Phase 1 is non-negotiable.

---

## Phase 7 — Home Assistant + hardening

1. Configure the bridge (UCI), per [BUILD.md](BUILD.md) §7:
   ```sh
   uci set dspw215.@meter[0].broker='<HA-broker-ip>'
   uci set dspw215.@meter[0].username='…'; uci set dspw215.@meter[0].password='…'
   uci commit dspw215 && /etc/init.d/dspw215-meterd restart
   ```
2. Entities appear in HA via **MQTT Discovery** (switch + power/energy/temp).
3. **Harden — make it local-only** (the whole point vs. stock):
   - no WAN/cloud; block outbound (the stock `wrpd.dlink.com` phone-home is gone).
   - set a real root password / SSH keys; disable unused services.
   - put it on your IoT VLAN.

**Exit gate:**
- [ ] Switch + power entities live in HA; survive a device reboot.

---

## Phase 8 — Optional: flashable factory image & upstreaming

- **`factory.bin`:** wrap the image in the stock "HORNET" header (checksum only,
  no signature) so it installs via the stock recovery/HNAP without UART. Nice for
  a second unit; not needed once you have UART.
- **Upstream:** clean up the DTS/recipe and submit to OpenWrt so the device is
  supported in mainline (others stop needing this repo).

---

## Risk register (one glance)

| Risk | Mitigation |
|------|------------|
| Brick on flash | Phase 4 RAM-boot first; verified backups; UART recovery |
| Wrong flash offsets | Take them from `/proc/mtd`, not assumption |
| Lost Wi-Fi calibration | Preserve `art`; never erase it; back it up separately |
| Locked U-Boot (no TFTP) | Check for bootloader recovery; else direct flash w/ backup |
| Meter never decoded | Ship without metering; it's isolated to `meterd` |
| Mains hazard | Only ever probe/solder **unplugged from mains** |
```
