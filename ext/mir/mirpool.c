/*
 * GStreamer Mir buffer pool
 * Copyright (C) 2013 Canonical Ltd

 * This library is free software; you can redistribute it and/or
 * modify it under the terms of the GNU Library General Public
 * License as published by the Free Software Foundation; either
 * version 3 of the License, or (at your option) any later version.
 *
 * This library is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU
 * Library General Public License for more details.
 *
 * You should have received a copy of the GNU Library General Public
 * License along with this library; if not, write to the
 * Free Software Foundation, Inc., 51 Franklin St, Fifth Floor,
 * Boston, MA 02110-1301, USA.
 */

#ifdef HAVE_CONFIG_H
#include "config.h"
#endif

/* Object header */
#include "gstmirsink.h"

#include <gst/mir/mirallocator.h>

/* Debugging category */
#include <gst/gstinfo.h>

/* Helper functions */
#include <gst/video/video.h>
#include <gst/video/gstvideometa.h>
#include <gst/video/gstvideopool.h>

GST_DEBUG_CATEGORY (gstmirbufferpool_debug);
#define GST_CAT_DEFAULT gstmirbufferpool_debug

/* mir metadata */
GType
gst_mir_meta_api_get_type (void)
{
  static volatile GType type;
  static const gchar *tags[] =
      { "memory", "size", "colorspace", "orientation", NULL };

  if (g_once_init_enter (&type)) {
    GType _type = gst_meta_api_type_register ("GstMirMetaAPI", tags);
    g_once_init_leave (&type, _type);
  }
  return type;
}

static void
gst_mir_meta_free (GstMirMeta * meta, GstBuffer * buffer)
{
  gst_object_unref (meta->sink);
}

const GstMetaInfo *
gst_mir_meta_get_info (void)
{
  static const GstMetaInfo *mir_meta_info = NULL;

  if (g_once_init_enter (&mir_meta_info)) {
    const GstMetaInfo *meta =
        gst_meta_register (GST_MIR_META_API_TYPE, "GstMirMeta",
        sizeof (GstMirMeta), (GstMetaInitFunction) NULL,
        (GstMetaFreeFunction) gst_mir_meta_free,
        (GstMetaTransformFunction) NULL);
    g_once_init_leave (&mir_meta_info, meta);
  }
  return mir_meta_info;
}

/* bufferpool */
static void gst_mir_buffer_pool_finalize (GObject * object);

#define gst_mir_buffer_pool_parent_class parent_class
G_DEFINE_TYPE (GstMirBufferPool, gst_mir_buffer_pool, GST_TYPE_BUFFER_POOL);

static gboolean
mir_buffer_pool_set_config (GstBufferPool * pool, GstStructure * config)
{
  GstMirBufferPool *mpool = GST_MIR_BUFFER_POOL_CAST (pool);
  GstVideoInfo info;
  GstCaps *caps;

  GST_DEBUG_OBJECT (mpool, "%s", __PRETTY_FUNCTION__);

  if (!gst_buffer_pool_config_get_params (config, &caps, NULL, NULL, NULL))
    goto wrong_config;

  if (caps == NULL)
    goto no_caps;

  if (mpool->allocator)
    gst_object_unref (mpool->allocator);
  mpool->allocator = NULL;

  /* now parse the caps from the config */
  if (!gst_video_info_from_caps (&info, caps))
    goto wrong_caps;

  if (!gst_buffer_pool_config_get_allocator (config, &mpool->allocator,
          &mpool->params))
    return FALSE;
  if (mpool->allocator)
    gst_object_ref (mpool->allocator);

  GST_LOG_OBJECT (mpool, "%dx%d, caps %" GST_PTR_FORMAT, info.width,
      info.height, caps);

  /*Fixme: Enable metadata checking handling based on the config of pool */

  mpool->caps = gst_caps_ref (caps);
  mpool->info = info;
  mpool->width = info.width;
  mpool->height = info.height;

  GST_DEBUG_OBJECT (mpool, "Calling set_config() on the parent class");
  return GST_BUFFER_POOL_CLASS (parent_class)->set_config (pool, config);
  /* ERRORS */
wrong_config:
  {
    GST_WARNING_OBJECT (pool, "invalid config");
    return FALSE;
  }
no_caps:
  {
    GST_WARNING_OBJECT (pool, "no caps in config");
    return FALSE;
  }
wrong_caps:
  {
    GST_WARNING_OBJECT (pool,
        "failed getting geometry from caps %" GST_PTR_FORMAT, caps);
    return FALSE;
  }
}

static GstMirMeta *
gst_buffer_add_mir_meta (GstBuffer * buffer, GstMirBufferPool * mpool)
{
  GstMirMeta *mmeta;
  GstMirSink *sink;
  guint stride = 0;
  guint size = 0;

  sink = mpool->sink;
  stride = mpool->width * 4;
  size = stride * mpool->height;

  GST_DEBUG_OBJECT (mpool, "%s", __PRETTY_FUNCTION__);

  /* Add metadata so that the render function can tell the difference between a zero-copy
   * rendering buffer vs one that it must manually copy through the main CPU */
  mmeta = (GstMirMeta *) gst_buffer_add_meta (buffer, GST_MIR_META_INFO, NULL);
  mmeta->sink = gst_object_ref (sink);

  mmeta->size = size;

  return mmeta;
}

// FIXME: rename this function since it no longer makes sense
static GstBuffer *
gst_mir_allocate_native_window_buffer (GstBufferPool * pool,
    GstAllocator * allocator, GstBufferPoolAcquireParams * params,
    GstVideoFormat format, gint width, gint height)
{
  GstMirBufferPool *m_pool = GST_MIR_BUFFER_POOL_CAST (pool);
  GstBuffer *buffer;
  GstMemory *mem = { NULL };
  gsize size = 0;
  gint stride = 0;
  GstMemoryFlags flags = 0;

  GST_DEBUG_OBJECT (pool, "%s", __PRETTY_FUNCTION__);

  if (!gst_mir_image_memory_is_mappable ())
    flags |= GST_MEMORY_FLAG_NOT_MAPPABLE;

  flags |= GST_MEMORY_FLAG_NO_SHARE;

  switch (format) {
      gsize buffer_id = 0;

    case GST_VIDEO_FORMAT_RGB:
    case GST_VIDEO_FORMAT_BGR:
    case GST_VIDEO_FORMAT_RGB16:
    case GST_VIDEO_FORMAT_NV12:
    case GST_VIDEO_FORMAT_NV21:
    case GST_VIDEO_FORMAT_RGBA:
    case GST_VIDEO_FORMAT_BGRA:
    case GST_VIDEO_FORMAT_ARGB:
    case GST_VIDEO_FORMAT_ABGR:
    case GST_VIDEO_FORMAT_RGBx:
    case GST_VIDEO_FORMAT_BGRx:
    case GST_VIDEO_FORMAT_xRGB:
    case GST_VIDEO_FORMAT_xBGR:
    case GST_VIDEO_FORMAT_AYUV:
    case GST_VIDEO_FORMAT_YV12:
    case GST_VIDEO_FORMAT_I420:
    case GST_VIDEO_FORMAT_Y444:
    case GST_VIDEO_FORMAT_Y42B:
    case GST_VIDEO_FORMAT_Y41B:{

      GST_DEBUG_OBJECT (m_pool, "Allocating new Mir image");

      stride = size / height;
      size = stride * height;

      if (m_pool->sink->surface_texture_client) {
        buffer_id = 0;

        mem =
            gst_mir_image_allocator_wrap (allocator, buffer_id, flags, size,
            NULL, NULL);
        if (mem == NULL)
          GST_WARNING_OBJECT (m_pool, "mem is NULL!");
      }

      break;
    }
    default:
      GST_WARNING_OBJECT (m_pool,
          "Using the default buffer allocator, hit the default case");
      if (GST_BUFFER_POOL_CLASS (gst_mir_buffer_pool_parent_class)->alloc_buffer
          (pool, &buffer, params) != GST_FLOW_OK)
        return NULL;
      break;
  }

  buffer = gst_buffer_new ();
  if (!buffer) {
    GST_WARNING_OBJECT (m_pool, "Fallback memory allocation");
    if (GST_BUFFER_POOL_CLASS (gst_mir_buffer_pool_parent_class)->alloc_buffer
        (pool, &buffer, params) != GST_FLOW_OK)
      return NULL;
  }

  GST_DEBUG ("Appending memory to GstBuffer");
  gst_buffer_append_memory (buffer, mem);

  return buffer;
}

static GstFlowReturn
mir_buffer_pool_alloc (GstBufferPool * pool, GstBuffer ** buffer,
    GstBufferPoolAcquireParams * params)
{
  GstMirBufferPool *m_pool = GST_MIR_BUFFER_POOL_CAST (pool);
  GstMirMeta *meta;

  GST_DEBUG_OBJECT (m_pool, "%s", __PRETTY_FUNCTION__);

  if (m_pool->allocator == NULL) {
    GST_ERROR_OBJECT (m_pool, "Can't create buffer, couldn't get allocator");
    return GST_FLOW_ERROR;
  }

  *buffer =
      gst_mir_allocate_native_window_buffer (pool, m_pool->allocator, params,
      m_pool->info.finfo->format, m_pool->width, m_pool->height);

  meta = gst_buffer_add_mir_meta (*buffer, m_pool);
  if (meta == NULL) {
    gst_buffer_unref (*buffer);
    goto no_buffer;
  }

  return GST_FLOW_OK;

  /* ERROR */
no_buffer:
  {
    GST_WARNING_OBJECT (pool, "can't create buffer");
    return GST_FLOW_ERROR;
  }
}

static void
gst_mir_buffer_pool_release_buffer (GstBufferPool * pool, GstBuffer * buffer)
{
  GstMemory *mem = { NULL };
  int err = 0;

  /* Get access to the GstMemory stored in the GstBuffer */
  if (gst_buffer_n_memory (buffer) >= 1 &&
      (mem = gst_buffer_peek_memory (buffer, 0))
      && gst_is_mir_image_memory (mem)) {
    GST_DEBUG_OBJECT (pool, "It is Mir image memory");
  } else
    GST_DEBUG_OBJECT (pool, "It is NOT Mir image memory");

  GST_DEBUG_OBJECT (pool, "mem: %p", mem);
  GST_DEBUG_OBJECT (pool, "gst_mir_image_memory_get_codec (mem): %p",
      gst_mir_image_memory_get_codec (mem));
  GST_DEBUG_OBJECT (pool, "gst_mir_image_memory_get_buffer_index (mem): %d",
      gst_mir_image_memory_get_buffer_index (mem));
  GST_DEBUG_OBJECT (pool, "Rendering buffer: %d",
      gst_mir_image_memory_get_buffer_index (mem));
  GST_DEBUG_OBJECT (pool, "Releasing output buffer index: %d",
      gst_mir_image_memory_get_buffer_index (mem));

  /* Render and release the output buffer back to the decoder */
  err =
      media_codec_release_output_buffer (gst_mir_image_memory_get_codec (mem),
      gst_mir_image_memory_get_buffer_index (mem));
  if (err < 0)
    GST_WARNING_OBJECT (pool,
        "Failed to release output buffer. Rendering will probably be affected (err: %d).",
        err);

  GST_BUFFER_POOL_CLASS (parent_class)->release_buffer (pool, buffer);
}

GstBufferPool *
gst_mir_buffer_pool_new (GstMirSink * mirsink)
{
  GstMirBufferPool *pool;

  g_return_val_if_fail (GST_IS_MIR_SINK (mirsink), NULL);
  pool = g_object_new (GST_TYPE_MIR_BUFFER_POOL, NULL);
  GST_DEBUG_OBJECT (pool, "%s", __PRETTY_FUNCTION__);
  pool->sink = gst_object_ref (mirsink);

  return GST_BUFFER_POOL_CAST (pool);
}

void
gst_mir_buffer_pool_set_surface_texture_client (GstBufferPool * pool,
    SurfaceTextureClientHybris sfc)
{
  GstMirBufferPool *m_pool = GST_MIR_BUFFER_POOL_CAST (pool);

  GST_DEBUG_OBJECT (m_pool, "%s", __PRETTY_FUNCTION__);
  m_pool->surface_texture_client = sfc;
}

void
gst_mir_buffer_pool_set_codec_delegate (GstBufferPool * pool,
    MediaCodecDelegate * delegate)
{
  GstMirBufferPool *m_pool = GST_MIR_BUFFER_POOL_CAST (pool);

  GST_DEBUG_OBJECT (m_pool, "%s", __PRETTY_FUNCTION__);
  m_pool->codec_delegate = delegate;
}

static void
gst_mir_buffer_pool_class_init (GstMirBufferPoolClass * klass)
{
  GObjectClass *gobject_class = (GObjectClass *) klass;
  GstBufferPoolClass *gstbufferpool_class = (GstBufferPoolClass *) klass;

  gobject_class->finalize = gst_mir_buffer_pool_finalize;

  gstbufferpool_class->set_config = mir_buffer_pool_set_config;
  gstbufferpool_class->alloc_buffer = mir_buffer_pool_alloc;
  gstbufferpool_class->release_buffer = gst_mir_buffer_pool_release_buffer;
}

static void
gst_mir_buffer_pool_init (GstMirBufferPool * pool)
{
  GST_DEBUG_CATEGORY_INIT (gstmirbufferpool_debug, "mirbufferpool", 0,
      " mir buffer pool");
}

static void
gst_mir_buffer_pool_finalize (GObject * object)
{
  GstMirBufferPool *pool = GST_MIR_BUFFER_POOL_CAST (object);

  GST_DEBUG_OBJECT (pool, "%s", __PRETTY_FUNCTION__);
  gst_object_unref (pool->sink);

  G_OBJECT_CLASS (gst_mir_buffer_pool_parent_class)->finalize (object);
}
