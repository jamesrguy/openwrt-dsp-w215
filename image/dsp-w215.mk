# Append to target/linux/ath79/image/tiny.mk
# (move to generic.mk if the board is confirmed >8 MB flash / >32 MB RAM)
#
# CONFIRM IMAGE_SIZE against the real flash 'firmware' partition size.

define Device/dlink_dsp-w215
  SOC := ar9330
  DEVICE_VENDOR := D-Link
  DEVICE_MODEL := DSP-W215
  DEVICE_PACKAGES := kmod-ath9k wpad-basic-mbedtls \
		     dspw215-meterd libmosquitto-nossl
  IMAGE_SIZE := 8000k
  # No factory image yet (would need the D-Link "HORNET" header wrapper).
  # Initial install is via U-Boot TFTP; updates via sysupgrade. See BUILD.md.
  IMAGES := sysupgrade.bin
  IMAGE/sysupgrade.bin := append-kernel | append-rootfs | pad-rootfs | \
			  check-size | append-metadata
endef
TARGET_DEVICES += dlink_dsp-w215
