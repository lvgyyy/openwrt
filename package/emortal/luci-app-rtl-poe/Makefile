include $(TOPDIR)/rules.mk

PKG_NAME:=luci-app-rtl-poe
PKG_VERSION:=20260928
PKG_RELEASE:=1
PKG_PO_VERSION:=$(PKG_VERSION)-r$(PKG_RELEASE)
PKG_LICENSE:=GPL-2.0-only

LUCI_TITLE:=LuCI control for Realtek RTL8238B PoE PSE
LUCI_DEPENDS:=+libuci +luci-base +luci-compat
LUCI_DESCRIPTION:=LuCI interface and native PoE control for the Realtek RTL8238B PSE controller.

define Package/luci-app-rtl-poe/conffiles
/etc/config/rtl-poe
endef

include $(TOPDIR)/feeds/luci/luci.mk

# call BuildPackage - OpenWrt buildroot signature