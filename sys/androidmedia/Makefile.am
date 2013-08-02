plugin_LTLIBRARIES = libgstandroidmedia.la

if USE_ANDROID_MEDIA_HYBRIS
libgstandroidmedia_la_SOURCES = \
	gstamchybris.c \
	gstamcaudiodechybris.c \
	gstamcvideodechybris.c
else
libgstandroidmedia_la_SOURCES = \
	gstamc.c \
	gstamcaudiodec.c \
	gstamcvideodec.c
endif

noinst_HEADERS = \
	gstamc.h \
	gstamc-constants.h \
	gstamcaudiodec.h \
	gstamcvideodec.h

libgstandroidmedia_la_CFLAGS = \
	$(GST_PLUGINS_BAD_CFLAGS) \
	$(GST_PLUGINS_BASE_CFLAGS) \
	$(GST_BASE_CFLAGS) \
	$(GST_CFLAGS) \
	$(ORC_CFLAGS)
libgstandroidmedia_la_LIBADD = \
	$(GST_PLUGINS_BAD_LIBS) \
	$(GST_PLUGINS_BASE_LIBS) \
	-lgstaudio-@GST_API_VERSION@ \
	-lgstpbutils-@GST_API_VERSION@ \
	-lgstvideo-@GST_API_VERSION@ \
	$(GST_BASE_LIBS) \
	$(GST_LIBS) \
	$(ORC_LIBS) \
	-lmedia \
	$(top_builddir)/gst-libs/gst/mir/libgstmiralloc-@GST_API_VERSION@.la
libgstandroidmedia_la_LDFLAGS = $(GST_PLUGIN_LDFLAGS)
libgstandroidmedia_la_LIBTOOLFLAGS = $(GST_PLUGIN_LIBTOOLFLAGS)

Android.mk: Makefile.am $(BUILT_SOURCES)
	androgenizer \
	-:PROJECT libgstandroidmedia -:SHARED libgstandroidmedia \
	 -:TAGS eng debug \
         -:REL_TOP $(top_srcdir) -:ABS_TOP $(abs_top_srcdir) \
	 -:SOURCES $(libgstandroidmedia_la_SOURCES) \
	           $(nodist_libgstandroidmedia_la_SOURCES) \
	 -:CPPFLAGS $(DEFS) $(DEFAULT_INCLUDES) $(libgstandroidmedia_la_CFLAGS) \
	 -:LDFLAGS $(libgstandroidmedia_la_LDFLAGS) \
	           $(libgstandroidmedia_la_LIBADD) \
	           -ldl \
	 -:PASSTHROUGH LOCAL_ARM_MODE:=arm \
		       LOCAL_MODULE_PATH:='$$(TARGET_OUT)/lib/gstreamer-@GST_API_VERSION@' \
	> $@
