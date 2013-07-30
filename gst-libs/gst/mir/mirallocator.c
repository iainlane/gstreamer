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

#ifdef HAVE_CONFIG_H
#include "config.h"
#endif

#include "mirallocator.h"

#include <gst/egl/egl.h>

#define GST_MIR_IMAGE_MEMORY(mem) ((GstMirImageMemory*)(mem))

gboolean
gst_mir_image_memory_is_mappable (void)
{
  return FALSE;
}

gboolean
gst_is_mir_image_memory (GstMemory * mem)
{
  g_return_val_if_fail (mem != NULL, FALSE);
  g_return_val_if_fail (mem->allocator != NULL, FALSE);

  return g_strcmp0 (mem->allocator->mem_type, GST_MIR_IMAGE_MEMORY_TYPE) == 0;
}

gsize
gst_mir_image_memory_get_buffer_index (GstMemory * mem)
{
  g_return_val_if_fail (gst_is_mir_image_memory (mem), 0);

  if (mem->parent)
    mem = mem->parent;

  return GST_MIR_IMAGE_MEMORY (mem)->buffer_index;
}

MediaCodecDelegate *
gst_mir_image_memory_get_codec (GstMemory * mem)
{
  g_return_val_if_fail (gst_is_mir_image_memory (mem), 0);

  if (mem->parent)
    mem = mem->parent;

  return GST_MIR_IMAGE_MEMORY (mem)->codec_delegate;
}

void
gst_mir_image_memory_set_codec (GstMemory * mem, MediaCodecDelegate * delegate)
{
  g_return_if_fail (gst_is_mir_image_memory (mem));
  g_return_if_fail (delegate != NULL);

  if (mem->parent)
    mem = mem->parent;

  GST_MIR_IMAGE_MEMORY (mem)->codec_delegate = delegate;
}

void
gst_mir_image_memory_set_buffer_index (GstMemory * mem, gsize index)
{
  g_return_if_fail (gst_is_mir_image_memory (mem));

  if (mem->parent)
    mem = mem->parent;

  GST_MIR_IMAGE_MEMORY (mem)->buffer_index = index;
}

static GstMemory *
gst_mir_image_allocator_alloc_vfunc (GstAllocator * allocator, gsize size,
    GstAllocationParams * params)
{
  g_warning
      ("Use gst_mir_image_allocator_alloc() to allocate from this allocator");

  return NULL;
}

static void
gst_mir_image_allocator_free_vfunc (GstAllocator * allocator, GstMemory * mem)
{
  GstMirImageMemory *emem = (GstMirImageMemory *) mem;
  GST_WARNING ("%s", __PRETTY_FUNCTION__);

  g_return_if_fail (gst_is_mir_image_memory (mem));

  /* Shared memory should not destroy all the data */
  if (!mem->parent) {

    if (emem->user_data_destroy)
      emem->user_data_destroy (emem->user_data);
  }

  g_slice_free (GstMirImageMemory, emem);
}

static gpointer
gst_mir_image_mem_map (GstMemory * mem, gsize maxsize, GstMapFlags flags)
{
  return NULL;
}

static void
gst_mir_image_mem_unmap (GstMemory * mem)
{
}

static GstMemory *
gst_mir_image_mem_share (GstMemory * mem, gssize offset, gssize size)
{
  GstMemory *sub;
  GstMemory *parent;

  GST_WARNING ("%s", __PRETTY_FUNCTION__);

  if (offset != 0)
    return NULL;

  if (size != -1 && size != mem->size)
    return NULL;

  /* find the real parent */
  if ((parent = mem->parent) == NULL)
    parent = (GstMemory *) mem;

  if (size == -1)
    size = mem->size - offset;

  sub = (GstMemory *) g_slice_new (GstMirImageMemory);

  /* the shared memory is always readonly */
  gst_memory_init (GST_MEMORY_CAST (sub), GST_MINI_OBJECT_FLAGS (parent) |
      GST_MINI_OBJECT_FLAG_LOCK_READONLY, mem->allocator, parent,
      mem->maxsize, mem->align, mem->offset + offset, size);

  return sub;
}

static GstMemory *
gst_mir_image_mem_copy (GstMemory * mem, gssize offset, gssize size)
{
  return NULL;
}

static gboolean
gst_mir_image_mem_is_span (GstMemory * mem1, GstMemory * mem2, gsize * offset)
{
  return FALSE;
}

typedef GstAllocator GstMirImageAllocator;
typedef GstAllocatorClass GstMirImageAllocatorClass;

GType gst_mir_image_allocator_get_type (void);
G_DEFINE_TYPE (GstMirImageAllocator, gst_mir_image_allocator,
    GST_TYPE_ALLOCATOR);

#define GST_TYPE_MIR_IMAGE_ALLOCATOR   (gst_mir_image_mem_allocator_get_type())
#define GST_IS_MIR_IMAGE_ALLOCATOR(obj) (G_TYPE_CHECK_INSTANCE_TYPE ((obj), GST_TYPE_MIR_IMAGE_ALLOCATOR))

static void
gst_mir_image_allocator_class_init (GstMirImageAllocatorClass * klass)
{
  GstAllocatorClass *allocator_class = (GstAllocatorClass *) klass;

  allocator_class->alloc = gst_mir_image_allocator_alloc_vfunc;
  allocator_class->free = gst_mir_image_allocator_free_vfunc;
}

static void
gst_mir_image_allocator_init (GstMirImageAllocator * allocator)
{
  GstAllocator *alloc = GST_ALLOCATOR_CAST (allocator);

  alloc->mem_type = GST_MIR_IMAGE_MEMORY_TYPE;
  alloc->mem_map = gst_mir_image_mem_map;
  alloc->mem_unmap = gst_mir_image_mem_unmap;
  alloc->mem_share = gst_mir_image_mem_share;
  alloc->mem_copy = gst_mir_image_mem_copy;
  alloc->mem_is_span = gst_mir_image_mem_is_span;

  GST_OBJECT_FLAG_SET (allocator, GST_ALLOCATOR_FLAG_CUSTOM_ALLOC);
}

static gpointer
gst_mir_image_allocator_init_instance (gpointer data)
{
  return g_object_new (gst_mir_image_allocator_get_type (), NULL);
}

GstAllocator *
gst_mir_image_allocator_obtain (void)
{
  static GOnce once = G_ONCE_INIT;

  g_once (&once, gst_mir_image_allocator_init_instance, NULL);

  g_return_val_if_fail (once.retval != NULL, NULL);

  return GST_ALLOCATOR (g_object_ref (once.retval));
}

GstMemory *
gst_mir_image_allocator_alloc (GstAllocator * allocator,
    gint width, gint height, gsize * size)
{
  GST_WARNING ("%s", __PRETTY_FUNCTION__);
  return NULL;
}

GstMemory *
gst_mir_image_allocator_wrap (GstAllocator * allocator,
    gsize buffer_id, GstMemoryFlags flags, gsize size, gpointer user_data,
    GDestroyNotify user_data_destroy)
{
  GstMirImageMemory *mem;
  //GstMemory *mem;

  GST_WARNING ("%s", __PRETTY_FUNCTION__);

  if (!allocator) {
    allocator = gst_mir_image_allocator_obtain ();
  }
#if 0
  mem = (GstMirImageMemory *)
      gst_allocator_alloc (gst_mir_image_allocator_obtain (), size, NULL);
  //mem->native_window_buffer = buffer;
  mem->user_data = user_data;
  mem->user_data_destroy = user_data_destroy;
  GST_DEBUG ("mem->user_data_destroy: %p", mem->user_data_destroy);
#else
  mem = g_slice_new (GstMirImageMemory);
  // FIXME: calling gst_mir_image_allocator_obtain() is a hack to select my allocator, this really
  // should be selected automatically by the decoder. This selection is not working correctly yet.
  gst_memory_init (GST_MEMORY_CAST (mem), flags,
      gst_mir_image_allocator_obtain (), NULL, size, 0, 0, size);

  mem->buffer_index = buffer_id;
  mem->user_data = user_data;
  mem->user_data_destroy = user_data_destroy;
  GST_DEBUG ("mem->user_data_destroy: %p", mem->user_data_destroy);
#endif

  return GST_MEMORY_CAST (mem);
}
