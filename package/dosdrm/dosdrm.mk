################################################################################
#
# dosdrm - DRM/KMS diagnostics for the fbcon DOS image
#
################################################################################

DOSDRM_VERSION = 1.0
DOSDRM_SITE = $(TOPDIR)/package/dosdrm
DOSDRM_SITE_METHOD = local
DOSDRM_LICENSE = MIT
DOSDRM_DEPENDENCIES = libdrm

define DOSDRM_BUILD_CMDS
	$(TARGET_CC) $(TARGET_CFLAGS) $(TARGET_LDFLAGS) -o $(@D)/dosdrm \
		$(@D)/dosdrm.c -ldrm
endef

define DOSDRM_INSTALL_TARGET_CMDS
	$(INSTALL) -D -m 0755 $(@D)/dosdrm $(TARGET_DIR)/usr/bin/dosdrm
endef

$(eval $(generic-package))
