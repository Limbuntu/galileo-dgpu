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

# dosbox links against libpng (screenshots) and zlib at runtime;
# without them the binary dies with "libpng16.so.16: cannot open
# shared object file" before it ever opens the framebuffer.
DOSBOX_DEPENDENCIES = sdl sdl_sound alsa-lib libpng zlib

# AM_PATH_SDL looks for sdl-config in PATH; buildroot keeps it in staging.
DOSBOX_CONF_ENV = SDL_CONFIG="$(STAGING_DIR)/usr/bin/sdl-config"

DOSBOX_CONF_OPTS = \
	--disable-sdltest \
	--disable-opengl \
	--enable-alsa-midi

$(eval $(autotools-package))
