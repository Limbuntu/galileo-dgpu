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

# AM_PATH_SDL looks for sdl-config in PATH; buildroot keeps it in staging.
DOSBOX_CONF_ENV = SDL_CONFIG="$(STAGING_DIR)/usr/bin/sdl-config"

DOSBOX_CONF_OPTS = \
	--disable-sdltest \
	--disable-opengl \
	--enable-alsa-midi

$(eval $(autotools-package))
