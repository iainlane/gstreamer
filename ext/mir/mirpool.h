/*
 * GStreamer Mir buffer pool
 * Copyright (C) 2013 Canonical Ltd
 *
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

#ifndef __GST_MIR_BUFFER_POOL_H__
#define __GST_MIR_BUFFER_POOL_H__

G_BEGIN_DECLS

#include "gstmirsink.h"
#include <hybris/media/media_codec_layer.h>
#include <hybris/media/surface_texture_client_hybris.h>
typedef struct _GstMirMeta GstMirMeta;

typedef struct _GstMirBufferPool GstMirBufferPool;
typedef struct _GstMirBufferPoolClass GstMirBufferPoolClass;

GType gst_mir_meta_api_get_type (void);
#define GST_MIR_META_API_TYPE  (gst_mir_meta_api_get_type())
const GstMetaInfo * gst_mir_meta_get_info (void);
#define GST_MIR_META_INFO  (gst_mir_meta_get_info())

#define gst_buffer_get_mir_meta(b) ((GstMirMeta*)gst_buffer_get_meta((b),GST_MIR_META_API_TYPE))

struct _GstMirMeta {
  GstMeta meta;

  GstMirSink *sink;

  size_t size;
};

/* buffer pool functions */
#define GST_TYPE_MIR_BUFFER_POOL      (gst_mir_buffer_pool_get_type())
#define GST_IS_MIR_BUFFER_POOL(obj)   (G_TYPE_CHECK_INSTANCE_TYPE ((obj), GST_TYPE_MIR_BUFFER_POOL))
#define GST_MIR_BUFFER_POOL(obj)      (G_TYPE_CHECK_INSTANCE_CAST ((obj), GST_TYPE_MIR_BUFFER_POOL, GstMirBufferPool))
#define GST_MIR_BUFFER_POOL_CAST(obj) ((GstMirBufferPool*)(obj))

struct _GstMirBufferPool
{
  GstBufferPool bufferpool;

  GstMirSink *sink;

  /*Fixme: keep all these in GstMirBufferPoolPrivate*/
  GstCaps *caps;
  GstVideoInfo info;
  guint width;
  guint height;
  GstAllocator *allocator;
  GstAllocationParams params;
  SurfaceTextureClientHybris surface_texture_client;
  MediaCodecDelegate *codec_delegate;
};

struct _GstMirBufferPoolClass
{
  GstBufferPoolClass parent_class;
};

GType gst_mir_buffer_pool_get_type (void);

GstBufferPool *gst_mir_buffer_pool_new (GstMirSink * mirsink);
void gst_mir_buffer_pool_set_surface_texture_client (GstBufferPool * pool, SurfaceTextureClientHybris sfc);
void gst_mir_buffer_pool_set_codec_delegate (GstBufferPool * pool, MediaCodecDelegate *delegate);

G_END_DECLS

#endif /*__GST_MIR_BUFFER_POOL_H__*/
