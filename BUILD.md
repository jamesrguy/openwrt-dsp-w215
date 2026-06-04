# Building & flashing

This port is supplied as drop-in source files for an OpenWrt buildroot. You need
a Linux build host (or container) with the usual OpenWrt build prerequisites.

## 1. Get OpenWrt and add this device

```sh
git clone https://github.com/openwrt/openwrt
cd openwrt

# Device tree
cp /path/to/openwrt/dts/ar9330_dlink_dsp-w215.dts \
   target/linux/ath79/dts/

# Image recipe — append the Device/ block to the ath79 'tiny' image Makefile
cat /path/to/openwrt/image/dsp-w215.mk >> target/linux/ath79/image/tiny.mk
#   (use generic.mk instead if the board turns out to be >8 MB / >32 MB RAM)

# Board config (LEDs, network)
mkdir -p target/linux/ath79/tiny/base-files/etc/board.d
cp /path/to/openwrt/base-files/etc/board.d/01_leds \
   /path/to/openwrt/base-files/etc/board.d/02_network \
   target/linux/ath79/tiny/base-files/etc/board.d/

# Meter daemon package
cp -r /path/to/openwrt/package/dspw215-meterd package/
```

## 2. Configure

```sh
./scripts/feeds update -a && ./scripts/feeds install -a
make menuconfig
#  Target System  -> Atheros ATH79
#  Subtarget      -> Generic devices with small flash  (tiny)   # CONFIRM
#  Target Profile -> D-Link DSP-W215
#  Make sure these are selected (built-in or as packages):
#    kmod-ath9k, wpad-basic-mbedtls, dnsmasq (or omit), 
#    dspw215-meterd, libmosquitto-nossl, mosquitto-nossl (broker optional)
```

## 3. Build

```sh
make -j$(nproc)
# Output:
#   bin/targets/ath79/tiny/openwrt-ath79-tiny-dlink_dsp-w215-squashfs-sysupgrade.bin
```

## 4. First install — via U-Boot TFTP (recommended)

Because there is no trusted way to flash from the stock HNAP/web UI (no signed
factory image yet), do the **initial** install from the U-Boot console over
TFTP. This is also the unbrick path.

1. Serve the image from a TFTP server on your PC (e.g. `192.168.1.2`).
2. On the UART U-Boot prompt (commands vary by this board's U-Boot — read the
   help; typical AR9330 D-Link/U-Boot):

```text
setenv ipaddr 192.168.1.1
setenv serverip 192.168.1.2
tftpboot 0x80060000 openwrt-...-dlink_dsp-w215-squashfs-sysupgrade.bin

# Erase + write the OpenWrt 'firmware' region (USE THE OFFSET/SIZE FROM /proc/mtd!)
# Example only — CONFIRM offsets against your flash map:
erase 0x9f020000 +$filesize
cp.b 0x80060000 0x9f020000 $filesize
bootm 0x9f020000   # or reset
```

> Do **not** erase the `u-boot` (offset 0) or `art` (radio cal) regions.

## 5. Subsequent updates — sysupgrade

Once OpenWrt is running:

```sh
scp openwrt-...-dlink_dsp-w215-squashfs-sysupgrade.bin root@192.168.1.1:/tmp/
ssh root@192.168.1.1 sysupgrade -n /tmp/openwrt-...-sysupgrade.bin
```

## 6. Recovery

If a flash goes bad, the soldered UART + U-Boot TFTP (step 4) restores either
OpenWrt or your **stock backup** (`mtd*.bin` from HARDWARE.md §3). Keep those
backups.

## 7. Configure the meter bridge

```sh
uci set dspw215.@meter[0].broker='192.168.1.10'
uci set dspw215.@meter[0].username='ha'
uci set dspw215.@meter[0].password='secret'
uci commit dspw215
/etc/init.d/dspw215-meterd restart
```

Entities then appear in Home Assistant via MQTT Discovery (switch + power /
energy / temperature), same as the `dspw215` host-side bridge.

## Stretch goal: a flashable factory image

To install without UART, OpenWrt's image would need to be wrapped in the format
the stock bootloader/HNAP `StartFirmwareDownload` accepts (the D-Link "HORNET"
header + uImage). That header has **no signature, only a checksum**, so it's
reproducible — a `mkdlinkfw`-style step could emit a `factory.bin`. Not done
yet; UART install is the supported path for now.
