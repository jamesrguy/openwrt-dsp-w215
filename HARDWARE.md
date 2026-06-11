# Hardware bring-up checklist

Everything OpenWrt needs that **isn't** derivable from the stock firmware. Work
through this with the device **open and unplugged from mains**, powered for
bench work only via the low-voltage side if you understand the board — when in
doubt, don't probe a live mains device.

Tools: Phillips/spudger, **3.3 V USB-TTL adapter**, multimeter, fine soldering
iron, optionally a logic analyzer for the meter capture.

> **Fast path:** once you have the serial root shell (§1), run
> [`tools/stock-hw-dump.sh`](tools/stock-hw-dump.sh) on the device — it collects
> `/proc/mtd`, `dmesg`, RAM/flash size, the relay-GPIO diff, meter samples, and a
> full flash backup in one pass. Sections 2–5 explain what each output means.

## 0. Known from firmware RE (no teardown needed)

Established by disassembling the stock 1.14 rootfs (`prolific`, `led_gpio`,
`system_manager`, `wlan_manager`, `gpio_module.ko`):

- **SoC:** Atheros AR9330 (Hornet), MIPS 24Kc, big-endian.
- **Console:** `ttyS0` @ **115200 8N1** (`console=ttyS0,115200`). The serial
  console is an **askfirst root `ash` shell with no password**
  (`/etc/inittab`: `ttyS0::askfirst:/bin/ash`) — just press Enter after boot.
- **Stock flash use:** rootfs on `/dev/mtdblock6`, nvram on `mtdblock2`.
- **Status LED:** bi-colour — **GPIO 26 (green) + GPIO 27 (red)**. Managers drive
  `26 on / 27 off` for one state and `26 off / 27 on` for the other (green vs
  red; both ⇒ amber).
- **GPIO 19:** a third output line driven through the same path
  (`led_gpio 0 19` in `system_manager`'s init sequence) — an LED/indicator whose
  exact function is TBD on the bench.
- **GPIO access (stock):** a custom char driver **`/dev/gpio`** (`gpio_module.ko`)
  driven by the `led_gpio <action> <gpio>` tool, where **action 0=off, 1=on,
  2=MP(test)**. On every write the module logs `gpio_out = 0x<old>, 0x<new>` —
  diffing those two words reveals exactly which bit (= GPIO number) an LED/relay
  op toggled. OpenWrt uses the standard `gpio-ath79` controller, so these GPIO
  numbers carry over directly.
- **Buttons:** handled by **kernel drivers** exposing `/proc/reset_btn` and
  `/proc/relay_btn` (the `reset_gpio`/`wps_gpio` daemons just poll these). The
  button GPIO *numbers* live in the built-in board code, not userspace — get them
  from the boot log or by tracing (see §4).
- **Relay:** driven via a `led_gpio`-style shell-out plus `CheckRelayValue`,
  gated by config (`sp_relay_info`, `relay_enabled`) and **safety cut-outs**
  (`overload_watt`, `overload_temperature`). The pin isn't a static immediate
  (variadic `_system()` + config), so confirm it live (§4).
- **Power meter:** **Prolific PL8331** on **`/dev/ttyS0`**. `prolific` issues **no
  `tcsetattr`** — it inherits the port's 115200 8N1, confirming the meter shares
  the console UART. It opens the port, writes a command, reads a ~10-byte
  ASCII-hex burst, closes, and publishes decoded values to ramfs:
  `/var/tmp/MeterWatt` (`%.1f` W), `/var/tmp/MeterStatus` (5 ints),
  `/var/tmp/MeterVersion`, `/var/tmp/MeterSigNumber`.
- **Cloud phone-home (security):** `prolific` POSTs SOAP `funPushNotification`
  to `wrpd.dlink.com` on events — worth blocking; an OpenWrt image drops it.

## 1. UART console

1. Find the UART pads/header (4 pads: VCC 3.3 V, GND, TX, RX). Identify GND
   (continuity to ground plane) and TX (idles at 3.3 V, bursts on boot).
2. Solder leads; connect USB-TTL **TX↔RX / RX↔TX / GND↔GND** (leave VCC
   disconnected — board is self-powered). 115200 8N1.
3. Interrupt U-Boot (watch for "Hit any key…"/a key combo) to get the U-Boot
   prompt. **Record the full boot log** — it contains the U-Boot version, RAM
   size, flash size/JEDEC id, and often the partition table.

> **Key question — UART sharing.** The AR9330 has one primary UART. The console
> *and* the PL8331 meter both use `ttyS0`. Determine on the bench whether they
> are physically the same pins (meter wired to UART0) or whether a second UART
> exists (`hornet_serial.nr_uarts`). This decides whether `meterd` can own the
> port while you keep a debug console. Note what you find — it drives the final
> `console=`/meterd config.

## 2. Memory & flash size

From the U-Boot log / `bdinfo`:

- **RAM:** `CONFIRM:` (AR9330 plugs are typically 32 MB or 64 MB DDR).
- **Flash:** `CONFIRM:` (image uses ~4.4 MB → likely **8 MB**; confirm the SPI
  chip part number / JEDEC id).

Update `IMAGE_SIZE` in `image/dsp-w215.mk` and the flash node size in the DTS.

## 3. Partition map

On the stock console (Linux), record the real layout:

```sh
cat /proc/mtd
# expect something like:
# mtd0: u-boot ...   mtd2: nvram ...   mtd6: rootfs ...   mtdN: art ...
```

For OpenWrt you only need to **preserve `u-boot` and `art`** at their real
offsets; everything else becomes the OpenWrt `firmware` partition. Copy the
offsets into the `partitions` node of the DTS (replace the `CONFIRM:` values).
Also back up every partition before flashing:

```sh
for i in $(seq 0 7); do dd if=/dev/mtd$i of=/tmp/mtd$i.bin 2>/dev/null; done
# scp them off the device; KEEP the art partition safe (radio calibration).
```

## 4. GPIO map (the numbers to confirm)

LEDs are known (**26** green, **27** red; **19** = third line). Still to pin down:
polarity, and the **relay + button** numbers. Do this from the stock serial
console — note that stock has **no `/sys/class/gpio`**; it drives GPIO through
`/dev/gpio` via the `led_gpio` tool.

| Function | Status | How to confirm on stock |
|----------|--------|--------------------------|
| LED green / red | **GPIO 26 / 27** | `led_gpio 1 26` (on) / `led_gpio 0 26` (off); watch the LED for polarity |
| 3rd line | **GPIO 19** | `led_gpio 1 19` / `led_gpio 0 19`; observe what changes |
| Relay | confirm number | clear log → toggle relay → read the `gpio_out` diff (below) |
| Reset button | from boot log / trace | `cat /proc/reset_btn` shows press state, not the pin number |
| Relay button | from boot log / trace | `cat /proc/relay_btn` shows press state, not the pin number |

**Relay GPIO via the module's debug print** (the reliable trick, since there's
no sysfs on stock):

```sh
dmesg -c >/dev/null            # clear the ring buffer
/var/sbin/relay 1              # socket ON  (or HNAP socket-on from your PC)
dmesg | grep gpio_out          # -> gpio_out = 0x<old>, 0x<new>
/var/sbin/relay 0              # socket OFF
dmesg | grep gpio_out
# The one bit that flips between <old> and <new> is the relay GPIO number.
```

The same `gpio_out` diff confirms LED polarity: drive `led_gpio 1 26`, see which
bit sets and whether green lights with the bit high or low. The button GPIO
numbers aren't exposed this way — read them from the U-Boot/kernel boot log
(board init usually prints them) or trace the button pads to the SoC.

## 5. Meter serial capture (to implement `poll_meter()`)

Confirmed from `prolific`: it opens `/dev/ttyS0` (no `tcsetattr`, so 115200 8N1
inherited), **writes a command then reads a ~10-byte ASCII-hex burst**, and
publishes `/var/tmp/MeterWatt` (`%.1f` W) and `/var/tmp/MeterStatus` (5 ints).
The exact on-wire command/response bytes still need a capture to reimplement
`poll_meter()` cleanly.

> **Caveat:** the meter shares `ttyS0` with the console, so you can't cleanly
> `cat /dev/ttyS0` while the getty/`prolific` are running. Easiest validations:
> (a) read `/var/tmp/MeterWatt` under a known load to verify scaling; (b) to get
> raw frames, `killall prolific` first, then tap the line with a second USB-TTL
> or `meterd -r`. This is the **highest-risk remaining unknown** — relay + LEDs +
> Wi-Fi can be brought up on OpenWrt before metering is finished.

With the stock firmware running, capture the PL8331 traffic on `ttyS0`
(115200 8N1) — tap the line with a logic analyzer / second USB-TTL, or build
`dspw215-meterd` in **raw mode** (`meterd -r /dev/ttyS0`). Drive a known
resistive load (e.g. a 60 W bulb) and correlate decoded values with
`/var/tmp/MeterWatt`. Record:

- the exact poll command bytes (ASCII-hex on the wire),
- the response frame layout (offsets of voltage / current / power / energy / temp),
- the scaling (raw → watts).

Then fill in `poll_meter()` in `package/dspw215-meterd/files/src/meterd.c`.

## 6. Done → build

Once the `CONFIRM:` values are filled in, proceed to [BUILD.md](BUILD.md).
