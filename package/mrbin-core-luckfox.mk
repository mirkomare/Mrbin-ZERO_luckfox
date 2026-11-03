################################################################################
# mrbin-core-luckfox
################################################################################

MRBIN_CORE_LUCKFOX_VERSION = 0.5.41
MRBIN_CORE_LUCKFOX_SITE = $(BR2_EXTERNAL_LUCKFOX_PATH)/../LUCKFOX
MRBIN_CORE_LUCKFOX_SITE_METHOD = local
MRBIN_CORE_LUCKFOX_DEPENDENCIES = openssl libcurl
MRBIN_CORE_LUCKFOX_CONF_OPTS = -DHOST_SIM=OFF

ifeq ($(BR2_PACKAGE_RKMPI),y)
MRBIN_CORE_LUCKFOX_CONF_OPTS += -DLUCKFOX_RKMPI=ON
endif

define MRBIN_CORE_LUCKFOX_INSTALL_TARGET_CMDS
	$(INSTALL) -D -m 0755 $(@D)/mrbin-core $(TARGET_DIR)/usr/bin/mrbin-core
	$(INSTALL) -D -m 0755 $(MRBIN_CORE_LUCKFOX_PKGDIR)/../../overlay/usr/sbin/mrbin-boot-mode \
		$(TARGET_DIR)/usr/sbin/mrbin-boot-mode
	$(INSTALL) -D -m 0755 $(MRBIN_CORE_LUCKFOX_PKGDIR)/../../overlay/etc/init.d/S00mrbin-fast \
		$(TARGET_DIR)/etc/init.d/S00mrbin-fast
	$(INSTALL) -D -m 0755 $(MRBIN_CORE_LUCKFOX_PKGDIR)/../../overlay/etc/init.d/S01mrbin-rec \
		$(TARGET_DIR)/etc/init.d/S01mrbin-rec
	$(INSTALL) -D -m 0755 $(MRBIN_CORE_LUCKFOX_PKGDIR)/../../overlay/etc/init.d/S20mrbin-norkipc \
		$(TARGET_DIR)/etc/init.d/S20mrbin-norkipc
	$(INSTALL) -D -m 0755 $(MRBIN_CORE_LUCKFOX_PKGDIR)/../../overlay/etc/init.d/S98mrbin-isp \
		$(TARGET_DIR)/etc/init.d/S98mrbin-isp
	$(INSTALL) -D -m 0755 $(MRBIN_CORE_LUCKFOX_PKGDIR)/../../overlay/etc/init.d/S99mrbin \
		$(TARGET_DIR)/etc/init.d/S99mrbin
	$(INSTALL) -D -m 0755 $(MRBIN_CORE_LUCKFOX_PKGDIR)/../../overlay/usr/sbin/mrbin-isp \
		$(TARGET_DIR)/usr/sbin/mrbin-isp
	$(INSTALL) -D -m 0755 $(MRBIN_CORE_LUCKFOX_PKGDIR)/../../overlay/etc/init.d/S25mrbin-wifi \
		$(TARGET_DIR)/etc/init.d/S25mrbin-wifi
	$(INSTALL) -D -m 0755 $(MRBIN_CORE_LUCKFOX_PKGDIR)/../../overlay/etc/init.d/S41mrbin-eth \
		$(TARGET_DIR)/etc/init.d/S41mrbin-eth
	$(INSTALL) -D -m 0644 $(MRBIN_CORE_LUCKFOX_PKGDIR)/../../overlay/etc/network/interfaces \
		$(TARGET_DIR)/etc/network/interfaces
	$(INSTALL) -D -m 0755 $(MRBIN_CORE_LUCKFOX_PKGDIR)/../../overlay/usr/sbin/usb_wifi_probe.sh \
		$(TARGET_DIR)/usr/sbin/usb_wifi_probe.sh
	$(INSTALL) -D -m 0644 $(MRBIN_CORE_LUCKFOX_PKGDIR)/../../overlay/etc/mrbin/pins.env \
		$(TARGET_DIR)/etc/mrbin/pins.env
	$(INSTALL) -D -m 0644 $(MRBIN_CORE_LUCKFOX_PKGDIR)/../../overlay/etc/mrbin/isp.env \
		$(TARGET_DIR)/etc/mrbin/isp.env
	$(INSTALL) -D -m 0755 $(MRBIN_CORE_LUCKFOX_PKGDIR)/../../overlay/etc/mrbin/RkLunch.stub \
		$(TARGET_DIR)/etc/mrbin/RkLunch.stub
	$(INSTALL) -D -m 0600 $(MRBIN_CORE_LUCKFOX_PKGDIR)/../../overlay/etc/mrbin/wifi.env \
		$(TARGET_DIR)/etc/mrbin/wifi.env
	$(INSTALL) -D -m 0644 $(MRBIN_CORE_LUCKFOX_PKGDIR)/../../overlay/etc/mrbin/hostapd.conf \
		$(TARGET_DIR)/etc/mrbin/hostapd.conf
	$(INSTALL) -D -m 0644 $(MRBIN_CORE_LUCKFOX_PKGDIR)/../../overlay/etc/mrbin/wpa_supplicant.conf \
		$(TARGET_DIR)/etc/mrbin/wpa_supplicant.conf
endef

$(eval $(cmake-package))
