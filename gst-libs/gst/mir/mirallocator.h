/*
 * Mir GstMemory allocator
 * Copyright (C) 2013 Collabora Ltd.
 *   @author: Jim Hodapp <jim.hodapp@canonical.com>
 * *
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

#ifndef __GST_EGL_H__
#define __GST_EGL_H__

#include <gst/gst.h>
#include <gst/video/video.h>
#include <EGL/egl.h>
#include <EGL/eglext.h>

#include <media_codec_layer.h>
#include <surface_texture_client_ubuntu.h>

#define GST_MIR_IMAGE_MEMORY_TYPE "MirImage"

#define GST_CAPS_FEATURE_MEMORY_MIR_IMAGE "memory:MirImage"

typedef struct _GstMirDisplay GstMirDisplay;

typedef struct
{
  GstMemory parent;

  MediaCodecDelegate codec_delegate;
  gsize buffer_index;

  gpointer user_data;
  GDestroyNotify user_data_destroy;
} GstMirImageMemory;

/* MirImage GstMemory handling */
gboolean gst_mir_image_memory_is_mappable (void);
gboolean gst_is_mir_image_memory (GstMemory * mem);
gsize gst_mir_image_memory_get_buffer_index (GstMemory * mem);
MediaCodecDelegate gst_mir_image_memory_get_codec (GstMemory * mem);
void gst_mir_image_memory_set_codec (GstMemory * mem, MediaCodecDelegate delegate);
void gst_mir_image_memory_set_buffer_index (GstMemory * mem, gsize index);

/* Generic MirImage allocator that doesn't support mapping, copying or anything */
GstAllocator *gst_mir_image_allocator_obtain (void);
GstMemory *gst_mir_image_allocator_alloc (GstAllocator * allocator,
    gint width, gint height, gsize * size);
GstMemory *gst_mir_image_allocator_wrap (GstAllocator * allocator, MediaCodecDelegate delegate,
    gsize buffer_id, GstMemoryFlags flags, gsize size, gpointer user_data,
    GDestroyNotify user_data_destroy);

#endif /* __GST_EGL_H__ */
