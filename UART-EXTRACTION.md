# Pulling what we need off the device over UART

Everything OpenWrt still needs lives on the running stock firmware. This guide
takes you from "soldered UART" to "all the `CONFIRM:` values + a full flash
backup in hand." It folds in what reverse-engineering the stock firmware already
told us, so you know what each command *should* show and what it feeds.

> ⚠️ **Mains.** This plug switches mains voltage. Do all of this with the device
> **unplugged from mains.** The board is self-powered for bench work only if you
> know what you're doing; when in doubt, don't probe a live device.

What you produce here flows straight into [PLAN.md](PLAN.md) Phase 2 and the
files noted in the **"feeds"** column of each section.

---

## 0. What you need

- A **3.3 V** USB-TTL adapter (CP2102/FT232/CH340 set to **3.3 V — never 5 V**).
- A serial terminal: `picocom -b 115200 /dev/ttyUSB0` (or `screen … 115200`,
  `minicom`). Logging on: `picocom -b 115200 --logfile boot.log /dev/ttyUSB0`.
- Your soldering kit (you have your own pad/header instructions — the essentials
  below are just to confirm the signals).

---

## 1. Find the pads & wire the adapter

The DSP-W215 brings the console out as **4 points: VCC (3.3 V), GND, TX, RX.**
Confirm which is which before connecting:

- **GND** — continuity to the ground plane / shield.
- **TX (device→PC)** — idles at ~3.3 V and bursts during boot (scope/multimeter).
- **RX (PC→device)** — usually the quiet one.
- **VCC** — sits at 3.3 V; **leave it disconnected** (the board powers itself).

Wire it **crossed**:

```
adapter RX  <--->  device TX
adapter TX  <--->  device RX
adapter GND <--->  device GND
adapter VCC   x    (not connected)
```

**Port settings: 115200 8N1, no flow control** — confirmed from the firmware
(`console=ttyS0,115200`).

---

## 2. Boot, and get to the root shell

Power the device. You should see U-Boot, then the kernel, then the stock app.

**Two things to grab here:**

1. **Interrupt U-Boot** if you can (watch for "Hit any key to stop autoboot" or a
   key/sequence the log mentions) → you get the `u-boot>` prompt. Useful for
   `printenv`/`bdinfo` and later for TFTP recovery.
2. **Let it boot to Linux** and press **Enter** at the console. The stock
   `/etc/inittab` runs `ttyS0::askfirst:/bin/ash`, so you land in a **root shell
   with no password.** That's your extraction shell.

> **Capture the entire boot log to a file** (terminal logging on). It contains
> the U-Boot version, **RAM size**, **flash size + JEDEC id**, the partition
> table, and the board init that prints the **button GPIOs** — several `CONFIRM:`
> values are in there for free.

---

## 3. The one-shot collector

If you can get files onto the device (or just paste it in), run
[`tools/stock-hw-dump.sh`](tools/stock-hw-dump.sh) — it does sections 4–8 below
in one pass (read-only except a benign relay toggle) and writes the flash backup
to `/tmp`. The sections below explain each output if you'd rather go by hand or
need to interpret it.

---

## 4. Flash map, RAM, SoC

```sh
cat /proc/mtd        # partition table — THE important one
cat /proc/meminfo    # MemTotal -> RAM size
cat /proc/cpuinfo    # confirms AR9330 / MIPS 24Kc
```

`/proc/mtd` looks like (offsets/sizes are what you need):

```
dev:    size   erasesize  name
mtd0: 00030000 00010000 "u-boot"
mtd2: ........ ........ "nvram"
mtd6: ........ ........ "rootfs"
mtdN: 00010000 00010000 "art"      <- radio calibration, PRESERVE
```

Also note from U-Boot:

```text
u-boot> bdinfo        # RAM start/size, flash base (0x9f000000 region)
u-boot> printenv      # bootargs (mtdparts!), bootcmd, ethaddr
u-boot> flinfo        # flash chip / sector layout (or read JEDEC from boot log)
```

**Feeds:** `dts/ar9330_dlink_dsp-w215.dts` `partitions` node (every offset/size;
keep `u-boot`@0 and `art`) and the `memory` node; `image/dsp-w215.mk`
`IMAGE_SIZE`; and the `tiny` vs `generic` subtarget choice (≤8 MB → `tiny`).

**Known from RE:** stock root is `/dev/mtdblock6`, nvram `mtdblock2`. Image use is
~4.4 MB → flash is almost certainly **8 MB**; confirm the chip.

---

## 5. GPIO: relay, LEDs, gpio19

Stock has **no `/sys/class/gpio`** — it drives GPIO through a custom `/dev/gpio`
char driver (`gpio_module.ko`) via the **`led_gpio <action> <gpio>`** tool, where
**action 0=off, 1=on, 2=MP(test)**. Every write logs `gpio_out = 0x<old>,0x<new>`
to the kernel ring buffer — that's the trick for finding pins.

**Relay GPIO** — toggle it and watch which bit flips:

```sh
dmesg -c >/dev/null            # clear ring buffer
/var/sbin/relay 1              # socket ON  (or HNAP socket-on from your PC)
dmesg | grep gpio_out          # gpio_out = 0x<old>, 0x<new>
/var/sbin/relay 0              # socket OFF
dmesg | grep gpio_out
```

The single **bit position** that differs between `<old>` and `<new>` is the relay
GPIO number (e.g. bit 11 set → GPIO 11). You should also **hear the relay click**.

**LEDs (known: GPIO 26 green, 27 red; 19 = third line)** — confirm **polarity**:

```sh
led_gpio 1 26 ; sleep 1 ; led_gpio 0 26     # green: does "1" = lit?
led_gpio 1 27 ; sleep 1 ; led_gpio 0 27     # red
led_gpio 1 19 ; sleep 1 ; led_gpio 0 19     # gpio19 = ? (watch what changes)
```

If "1" lights the LED, it's **active-high** at the pin; the bi-colour LED may be
wired so the kernel sees it inverted — note what you observe per colour.

**Feeds:** DTS `gpio-export` relay node (number + polarity); `gpio-leds` 26/27
polarity and whether to enable a gpio19 LED; `base-files/etc/board.d/01_leds`.

---

## 6. Buttons

Stock exposes buttons as **kernel drivers**, not raw GPIO lines:

```sh
cat /proc/reset_btn      # state changes when you hold the reset button
cat /proc/relay_btn      # state changes when you press the physical socket button
```

These give *press state*, not the pin number. The button **GPIO numbers** are in
the built-in board init — find them in the **boot log** (`dmesg | grep -i
-e gpio -e button -e jumpstart -e simple_config`) or trace the button pad to the
SoC. AR9330 jumpstart/reset is commonly **GPIO 11** (and a WPS button, if
separate, often **12**) — confirm, don't assume.

**Feeds:** DTS `gpio-keys` `reset` (and `wps`) nodes.

---

## 7. Meter (Prolific PL8331)

**Known from RE:** the meter is a **PL8331 on `/dev/ttyS0`**. `prolific` issues
**no `tcsetattr`**, so it inherits 115200 8N1 — i.e. the meter **shares the
console UART**. It opens the port, writes a command, reads a ~10-byte **ASCII-hex
burst**, closes, and publishes decoded values to ramfs.

**Easy validation (do this first):** under a **known resistive load** (e.g. a
60 W incandescent bulb — *not* a switch-mode/LED load):

```sh
cat /var/tmp/MeterWatt      # %.1f  -> watts
cat /var/tmp/MeterStatus    # "v, i, w, ?, ?" integers
cat /var/tmp/MeterVersion
```

`MeterWatt` should read roughly the bulb's rating; 0-ish with the socket off.
That confirms scaling end-to-end without touching raw frames.

**Raw frames (to reimplement `poll_meter()`):** because the meter shares the
console, you can't cleanly `cat /dev/ttyS0` while things are running. Two ways:

- **Free the port first**, then sniff:
  ```sh
  killall prolific 2>/dev/null
  cat /dev/ttyS0 | od -An -tx1 | head      # observe the PL8331 chatter (hex)
  ```
  (Reboot afterwards to restore normal operation.)
- **Tap the line** with a second 3.3 V USB-TTL on the meter TX (passive listen),
  or build `dspw215-meterd -r /dev/ttyS0` later and dump from OpenWrt.

Record: the **poll command bytes**, the **response layout** (offsets of
voltage/current/power/energy/temp), and the **raw→watts scaling**.

**Feeds:** `poll_meter()` in `package/dspw215-meterd/files/src/meterd.c`. This is
the **highest-risk remaining unknown** (see [PLAN.md](PLAN.md) Phase 5) — fine to
defer past first boot.

---

## 8. Full flash backup (do this before flashing anything, ever)

```sh
for i in 0 1 2 3 4 5 6 7; do
    [ -e /dev/mtd$i ] && dd if=/dev/mtd$i of=/tmp/mtd$i.bin 2>/dev/null && \
        echo "saved /tmp/mtd$i.bin ($(wc -c </tmp/mtd$i.bin) bytes)"
done
```

Then **pull the files off the device** (busybox tools vary — try in order):

```sh
# A) tftp client on the device, tftpd on your PC:
tftp -p -l /tmp/mtd6.bin <your-PC-ip>

# B) netcat: on PC ->  nc -l -p 5555 > mtd6.bin
#            on dev->  nc <PC-ip> 5555 < /tmp/mtd6.bin

# C) if wget/scp exist, push to a host you control
```

Verify each `mtd*.bin` is the **expected size** and non-zero. **Keep the `art`
image somewhere safe and separate** — it's your radio calibration; losing it puts
Wi-Fi out of spec and it's unique per unit.

**Feeds:** unbrick/rollback in [PLAN.md](PLAN.md) Phases 4/6; the `art` image is
also the reference for the DTS `mtd-cal-data` offset.

---

## 9. Did I get everything? — checklist

| Captured | Fills | Lands in |
|----------|-------|----------|
| Full boot log (RAM, flash size, JEDEC, button GPIOs) | several | DTS, recipe |
| `/proc/mtd` offsets/sizes | partition map | DTS `partitions` |
| `art` partition offset + backup | radio cal | DTS `mtd-cal-data`, backup |
| Relay GPIO # + polarity | relay | DTS `gpio-export` |
| Reset/WPS button GPIO # | buttons | DTS `gpio-keys` |
| LED 26/27 polarity, gpio19 role | LEDs | DTS `gpio-leds`, `board.d` |
| RAM size | memory | DTS `memory` |
| `MeterWatt`/`MeterStatus` under load | scaling sanity | `meterd` (Phase 5) |
| PL8331 raw command/response (optional now) | frame decode | `meterd.c` |
| **Every `mtd*.bin` copied off + verified** | **unbrick** | backups |

When the top rows are done, no `CONFIRM:` should remain in `dts/` or `image/` —
go to [PLAN.md](PLAN.md) Phase 2.
