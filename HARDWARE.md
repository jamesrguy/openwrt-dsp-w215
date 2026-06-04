# Hardware bring-up checklist

Everything OpenWrt needs that **isn't** derivable from the stock firmware. Work
through this with the device **open and unplugged from mains**, powered for
bench work only via the low-voltage side if you understand the board — when in
doubt, don't probe a live mains device.

Tools: Phillips/spudger, **3.3 V USB-TTL adapter**, multimeter, fine soldering
iron, optionally a logic analyzer for the meter capture.

## 0. Known already (from firmware RE — no teardown needed)

- SoC: **Atheros AR9330 (Hornet)**, MIPS 24Kc.
- Console: **`ttyS0`, 115200 8N1** (`console=ttyS0,115200`).
- Root on stock: `/dev/mtdblock6`; nvram on `mtdblock2`.
- Status LED: bi-colour on **GPIO 26 / 27**.
- Relay: a GPIO with IRQ (kernel: `RELAY_SWITCH_GPIO`).
- Buttons via Atheros `simple_config` (jumpstart/WPS + reset).
- Power meter: **Prolific PL8331** on **`ttyS0`** @115200, ASCII-hex framing.

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

Determine these and update the DTS:

| Function | DTS placeholder | How to confirm |
|----------|-----------------|----------------|
| Relay | `CONFIRM: relay gpio` | toggle GPIOs from U-Boot/Linux and listen for the relay click; or trace from the relay-driver transistor |
| Reset button | `CONFIRM: ~gpio 11` | read GPIO state while pressing; AR933x `simple_config`/jumpstart is commonly GPIO 11 |
| WPS button | `CONFIRM:` | may be the same physical button as reset; check |
| 3rd LED? | `CONFIRM: gpio 19` | stock boot toggles GPIO 19 alongside 26/27 — identify what it drives |
| LED 26/27 polarity | `CONFIRM:` | set/clear and observe green vs red |

From Linux you can probe with the sysfs GPIO interface (or `gpio` tool):

```sh
echo N > /sys/class/gpio/export; cat /sys/class/gpio/gpioN/value   # read buttons
echo out > /sys/class/gpio/gpioN/direction; echo 1 > .../value      # drive relay/LED
```

## 5. Meter serial capture (to implement `poll_meter()`)

With the stock firmware running, capture the PL8331 traffic on `ttyS0`
(115200 8N1) — either tap the line with a logic analyzer / second USB-TTL, or
build `dspw215-meterd` in **raw mode** (`meterd -r /dev/ttyS0`) which just dumps
bytes. Drive a known resistive load (e.g. a 60 W bulb) and correlate the decoded
values with `/var/tmp/MeterWatt`. Record:

- the exact poll command bytes (ASCII-hex on the wire),
- the response frame layout (offsets of voltage / current / power / energy / temp),
- the scaling (raw → watts).

Then fill in `poll_meter()` in `package/dspw215-meterd/files/src/meterd.c`.

## 6. Done → build

Once the `CONFIRM:` values are filled in, proceed to [BUILD.md](BUILD.md).
