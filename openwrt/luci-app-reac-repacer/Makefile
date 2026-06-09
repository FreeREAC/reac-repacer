# SPDX-License-Identifier: GPL-3.0-or-later
# Copyright (C) 2026 Pau Aliagas <linuxnow@gmail.com>

# OpenWrt/LuCI feed recipe for luci-app-reac-repacer (client-side JS app).
include $(TOPDIR)/rules.mk

PKG_NAME:=luci-app-reac-repacer
PKG_VERSION:=0.2.0
PKG_RELEASE:=1
PKG_MAINTAINER:=Pau Aliagas <linuxnow@gmail.com>
PKG_LICENSE:=GPL-3.0-or-later

LUCI_TITLE:=LuCI app for the REAC Wi-Fi de-jitter re-pacer
LUCI_DEPENDS:=+reac-repacer
LUCI_PKGARCH:=all

# Standard LuCI app build (luci.mk handles htdocs/ + root/ install).
include $(TOPDIR)/feeds/luci/luci.mk

# call BuildPackage - OpenWrt buildroot signature to be discarded
