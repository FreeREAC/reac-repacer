# SPDX-License-Identifier: GPL-3.0-or-later
# Copyright (C) 2026 Pau Aliagas <linuxnow@gmail.com>

# OpenWrt feed recipe for the reac-repacer relay (single binary, procd + UCI).
# Copy this directory into an OpenWrt feed (or package/) and build with the SDK.
# On OpenWrt 25.12 the SDK emits .apk packages.
include $(TOPDIR)/rules.mk

PKG_NAME:=reac-repacer
PKG_VERSION:=0.2.0
PKG_RELEASE:=1
PKG_MAINTAINER:=Pau Aliagas <linuxnow@gmail.com>
PKG_LICENSE:=GPL-3.0-or-later

include $(INCLUDE_DIR)/package.mk

define Package/reac-repacer
  SECTION:=sound
  CATEGORY:=Sound
  TITLE:=REAC Wi-Fi de-jitter re-pacer
  URL:=https://github.com/linuxnow/reac-aes67
  DEPENDS:=+kmod-sched-core +tc-full +ip-full +libubus +libblobmsg-json
endef

define Package/reac-repacer/description
  Transparent Layer-2 relay that de-jitters a Roland REAC stream carried over a
  bursty Wi-Fi link. It buffers the master broadcast and re-emits a constant
  cadence on a frozen, rate-detected clock to the local stagebox, so a
  clock-slave stagebox stays locked. Self-tuning latency (grow-only adaptive),
  gap concealment, and live sample-rate (44.1/48/96 kHz) re-lock. Managed via
  UCI + procd; configurable from LuCI (luci-app-reac-repacer).
endef

define Package/reac-repacer/conffiles
/etc/config/reac-repacer
endef

# The C source is vendored under ./src (populated from the repo's tools/ at
# build time, like the reac-aes67 recipe).
define Build/Prepare
	mkdir -p $(PKG_BUILD_DIR)
	$(CP) ./src/reac_repacer.c $(PKG_BUILD_DIR)/
endef

# -DHAVE_UBUS enables the live ubus control object (get stats / set hot params); without
# it the daemon still builds + runs with SIGHUP-only live reconfig. On-device we link
# libubus + libblobmsg_json (the get/set surface the LuCI depth slider drives).
define Build/Compile
	$(TARGET_CC) $(TARGET_CFLAGS) -std=c11 -O2 -pthread -DHAVE_UBUS \
		-o $(PKG_BUILD_DIR)/reac-repacer $(PKG_BUILD_DIR)/reac_repacer.c \
		-lubus -lblobmsg_json -lubox
endef

define Package/reac-repacer/install
	$(INSTALL_DIR) $(1)/usr/bin
	$(INSTALL_BIN) $(PKG_BUILD_DIR)/reac-repacer $(1)/usr/bin/reac-repacer
	$(INSTALL_DIR) $(1)/etc/config
	$(INSTALL_CONF) ./files/reac-repacer.config $(1)/etc/config/reac-repacer
	$(INSTALL_DIR) $(1)/etc/init.d
	$(INSTALL_BIN) ./files/reac-repacer.init $(1)/etc/init.d/reac-repacer
	$(INSTALL_DIR) $(1)/usr/share/man/man8
	gzip -9nc ./files/reac-repacer.8 > $(PKG_BUILD_DIR)/reac-repacer.8.gz
	$(INSTALL_DATA) $(PKG_BUILD_DIR)/reac-repacer.8.gz $(1)/usr/share/man/man8/reac-repacer.8.gz
endef

$(eval $(call BuildPackage,reac-repacer))
