#!/bin/sh
# stock-hw-dump.sh — collect the hardware facts OpenWrt still needs, from the
# *stock* D-Link firmware's serial root shell (ttyS0, 115200 8N1, press Enter
# for the no-password ash shell).
#
# It is read-only except for one benign relay toggle (socket clicks on/off) used
# to discover the relay GPIO. Run it, then copy the console output + the /tmp
# dumps off the device (see the "pull files off" note at the end).
#
# Busybox/ash compatible. If a command is missing, skip that block — none are
# required for the others.

say() { echo; echo "===== $* ====="; }

say "uname / cpu / mem"
uname -a 2>/dev/null
cat /proc/cpuinfo 2>/dev/null
cat /proc/meminfo 2>/dev/null | head -3      # MemTotal -> RAM size

say "flash partition map (/proc/mtd) — copy these offsets into the DTS"
cat /proc/mtd 2>/dev/null

say "boot log (dmesg) — RAM/flash size, JEDEC id, gpio + button board init"
dmesg 2>/dev/null

say "button drivers (press the button(s) while watching, then re-run)"
cat /proc/reset_btn 2>/dev/null
cat /proc/relay_btn 2>/dev/null

say "meter sample (run under a known load, e.g. a 60W bulb)"
cat /var/tmp/MeterWatt 2>/dev/null
cat /var/tmp/MeterStatus 2>/dev/null
cat /var/tmp/MeterVersion 2>/dev/null

say "RELAY GPIO discovery (watch which bit flips in gpio_out)"
dmesg -c >/dev/null 2>&1
/var/sbin/relay 1 2>/dev/null ; sleep 1
echo "-- after relay ON:"  ; dmesg 2>/dev/null | grep gpio_out
/var/sbin/relay 0 2>/dev/null ; sleep 1
echo "-- after relay OFF:" ; dmesg 2>/dev/null | grep gpio_out
echo "(the single differing bit between old/new = relay GPIO number)"

say "LED check (eyeball the status LED for each)"
echo "run manually: led_gpio 1 26 ; led_gpio 0 26   # green"
echo "             led_gpio 1 27 ; led_gpio 0 27   # red"
echo "             led_gpio 1 19 ; led_gpio 0 19   # gpio19 = ?"

say "FULL FLASH BACKUP (do this before ever flashing OpenWrt)"
for i in 0 1 2 3 4 5 6 7; do
	[ -e /dev/mtd$i ] && dd if=/dev/mtd$i of=/tmp/mtd$i.bin 2>/dev/null && \
		echo "saved /tmp/mtd$i.bin ($(wc -c </tmp/mtd$i.bin) bytes)"
done
echo
echo "Pull the /tmp/mtd*.bin off the device (KEEP the 'art'/calibration one safe):"
echo "  device:  tftp -p -l /tmp/mtd6.bin <your-PC-ip>      # if tftp client exists"
echo "  or PC:   nc -l -p 5555 > mtd6.bin   then   device: nc <PC-ip> 5555 < /tmp/mtd6.bin"
