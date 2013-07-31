if HAVE_EGL
EGL_DIR = egl
endif

if HAVE_MIR_ALLOC
MIR_DIR = mir
endif

SUBDIRS = interfaces basecamerabinsrc codecparsers \
	 insertbin uridownloader mpegts $(EGL_DIR) $(MIR_DIR)

noinst_HEADERS = gst-i18n-plugin.h gettext.h glib-compat-private.h
DIST_SUBDIRS = interfaces egl basecamerabinsrc codecparsers \
	insertbin uridownloader mpegts
