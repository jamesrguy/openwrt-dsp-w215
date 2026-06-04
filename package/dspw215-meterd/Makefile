include $(TOPDIR)/rules.mk

PKG_NAME:=dspw215-meterd
PKG_VERSION:=0.1.0
PKG_RELEASE:=1
PKG_LICENSE:=MIT
PKG_MAINTAINER:=James Guy <jamesrguy@users.noreply.github.com>

include $(INCLUDE_DIR)/package.mk

define Package/dspw215-meterd
  SECTION:=utils
  CATEGORY:=Utilities
  TITLE:=DSP-W215 PL8331 meter + relay -> MQTT bridge
  DEPENDS:=+libmosquitto-nossl
endef

define Package/dspw215-meterd/description
  Reads the Prolific PL8331 energy meter over the UART and drives the relay GPIO
  on the D-Link DSP-W215, bridging both to MQTT with Home Assistant discovery
  (switch + power/temperature sensors). Local-only; no cloud.
endef

define Package/dspw215-meterd/conffiles
/etc/config/dspw215
endef

define Build/Prepare
	mkdir -p $(PKG_BUILD_DIR)
	$(CP) ./files/src/* $(PKG_BUILD_DIR)/
endef

define Build/Compile
	$(TARGET_CC) $(TARGET_CFLAGS) $(TARGET_LDFLAGS) \
		-o $(PKG_BUILD_DIR)/meterd $(PKG_BUILD_DIR)/meterd.c -lmosquitto
endef

define Package/dspw215-meterd/install
	$(INSTALL_DIR) $(1)/usr/sbin $(1)/etc/init.d $(1)/etc/config
	$(INSTALL_BIN) $(PKG_BUILD_DIR)/meterd $(1)/usr/sbin/meterd
	$(INSTALL_BIN) ./files/dspw215-meterd.init $(1)/etc/init.d/dspw215-meterd
	$(INSTALL_CONF) ./files/dspw215-meterd.config $(1)/etc/config/dspw215
endef

$(eval $(call BuildPackage,dspw215-meterd))
