################################################################################
#
# openarena
#
################################################################################

OPENARENA_VERSION = 0.8.8
OPENARENA_SITE = https://archive.org/download/openarena-linux-windows
OPENARENA_SOURCE = openarena-linux-windows.zip
OPENARENA_LICENSE = GPL-2.0+ (engine), CC-BY-SA-2.5 (assets)
OPENARENA_LICENSE_FILES = readme_088.txt
OPENARENA_DEPENDENCIES = sdl

# The archive ships a prebuilt i386 Linux engine (openarena.i386) next to the
# free asset paks of the 0.8.8 release plus a Windows build we do not need.
define OPENARENA_INSTALL_TARGET_CMDS
	mkdir -p $(TARGET_DIR)/opt/openarena/baseoa
	$(INSTALL) -m 0755 $(@D)/openarena.i386 $(TARGET_DIR)/opt/openarena/openarena
	$(INSTALL) -m 0644 $(@D)/baseoa/*.pk3 $(TARGET_DIR)/opt/openarena/baseoa/
	printf '#!/bin/sh\ncd /opt/openarena || exit 1\nexec ./openarena "$$@"\n' \
		> $(TARGET_DIR)/usr/bin/openarena
	chmod 0755 $(TARGET_DIR)/usr/bin/openarena
endef

$(eval $(generic-package))
