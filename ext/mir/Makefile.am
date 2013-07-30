plugin_LTLIBRARIES = libgstmirsink.la

libgstmirsink_la_SOURCES = gstmirsink.c mirpool.c
libgstmirsink_la_CFLAGS = $(GST_CFLAGS) $(GST_PLUGINS_BASE_CFLAGS) \
                               $(MIR_CFLAGS)
libgstmirsink_la_LIBADD = $(GST_PLUGINS_BASE_LIBS) \
                               -lgstvideo-$(GST_API_VERSION) \
                               -lubuntu_application_api \
                               -lgstmiralloc-$(GST_API_VERSION) \
                               -lmedia \
                               $(MIR_LIBS)
libgstmirsink_la_LDFLAGS = $(GST_PLUGIN_LDFLAGS)
libgstmirsink_la_LIBTOOLFLAGS = $(GST_PLUGIN_LIBTOOLFLAGS)
include_HEADERS = mirpool.h gstmirsink.h
noinst_HEADERS = 
