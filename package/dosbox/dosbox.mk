################################################################################
#
# dosbox
#
################################################################################

DOSBOX_VERSION = 0.74-3
DOSBOX_SOURCE = dosbox-$(DOSBOX_VERSION).tar.gz
DOSBOX_SITE = https://sourceforge.net/projects/dosbox/files/dosbox/0.74-3
DOSBOX_LICENSE = GPL-2.0+
DOSBOX_LICENSE_FILES = COPYING

DOSBOX_DEPENDENCIES = sdl sdl_sound alsa-lib
DOSBOX_CONF_OPTS = \
	--disable-sdltest \
	--enable-alsa-midi \
	ac_cv_func_memcmp_working=yes

$(eval $(autotools-package))
