/*
 * Initially based on gstamc.c
 *
 * Copyright (C) 2013, Canonical Ltd.
 *   Author: Jim Hodapp <jim.hodapp@canonical.com>
 *
 * This library is free software; you can redistribute it and/or
 * modify it under the terms of the GNU Lesser General Public
 * License as published by the Free Software Foundation
 * version 2.1 of the License.
 *
 * This library is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE. See the GNU
 * Lesser General Public License for more details.
 *
 * You should have received a copy of the GNU Lesser General Public
 * License along with this library; if not, write to the Free Software
 * Foundation, Inc., 51 Franklin Street, Fifth Floor, Boston, MA  02110-1301 USA
 *
 */

#ifdef HAVE_CONFIG_H
#include "config.h"
#endif

#include "gstamc.h"
#include "gstamc-constants.h"

#include "gstamcvideodec.h"
#include "gstamcaudiodec.h"

#include <gmodule.h>
#include <gst/gst.h>
#include <gst/video/video.h>
#include <gst/audio/audio.h>
#include <string.h>

#include <hybris/media/media_codec_layer.h>
#include <hybris/media/media_codec_list.h>
#include <hybris/media/media_format_layer.h>

#include <pthread.h>

GST_DEBUG_CATEGORY (gst_amc_debug);
#define GST_CAT_DEFAULT gst_amc_debug

GQuark gst_amc_codec_info_quark = 0;

static GList *codec_infos = NULL;
#ifdef GST_AMC_IGNORE_UNKNOWN_COLOR_FORMATS
static gboolean ignore_unknown_color_formats = TRUE;
#else
static gboolean ignore_unknown_color_formats = FALSE;
#endif

static gboolean accepted_color_formats (GstAmcCodecType * type,
    gboolean is_encoder);

static gchar *
locale_to_utf8 (gchar * str, gssize len)
{
  GError *error = NULL;
  gsize bytes_read = 0, bytes_written = 0;
  gchar *out = NULL;

  out = g_locale_to_utf8 (str, len, &bytes_read, &bytes_written, &error);
  if (bytes_read == 0)
    GST_WARNING ("Zero bytes read for UTF8 string conversion");
  if (bytes_written == 0)
    GST_WARNING ("Zero bytes written for UTF8 string conversion");

  return out;
}

GstAmcCodec *
gst_amc_codec_new (const gchar * name)
{
  GstAmcCodec *codec = NULL;
  gchar *name_str = NULL;

  GST_DEBUG ("%s", __PRETTY_FUNCTION__);

  g_return_val_if_fail (name != NULL, NULL);

  codec = g_slice_new0 (GstAmcCodec);

  name_str = g_strdup (name);
  name_str = locale_to_utf8 (name_str, strlen (name));
  if (name_str == NULL)
    goto error;
  GST_DEBUG ("codec name '%s'", name_str);

  codec->codec_delegate = media_codec_create_by_codec_name (name_str);
  if (codec->codec_delegate == NULL) {
    GST_ERROR ("Failed to create codec '%s'", name_str);
    goto error;
  }

done:
  if (name_str)
    g_free (name_str);
  name_str = NULL;

  return codec;

error:
  if (codec)
    g_slice_free (GstAmcCodec, codec);
  codec = NULL;
  goto done;
}

void
gst_amc_codec_free (GstAmcCodec * codec)
{
  g_return_if_fail (codec != NULL);

  GST_DEBUG ("%s", __PRETTY_FUNCTION__);

  media_codec_delegate_unref (codec->codec_delegate);
  media_codec_delegate_destroy (codec->codec_delegate);

  g_slice_free (GstAmcCodec, codec);
}

gboolean
gst_amc_codec_configure (GstAmcCodec * codec, GstAmcFormat * format,
    SurfaceTextureClientHybris stc, gint flags)
{
  gboolean ret = TRUE;
  int err = 0;

  GST_DEBUG ("%s", __PRETTY_FUNCTION__);

  g_return_val_if_fail (codec != NULL, FALSE);
  g_return_val_if_fail (format != NULL, FALSE);

  err = media_codec_configure (codec->codec_delegate, format->format, stc, 0);
  if (err > 0) {
    GST_ERROR ("Failed to configure media codec");
    ret = FALSE;
    goto done;
  }

done:
  return ret;
}

gboolean
gst_amc_codec_queue_csd (GstAmcCodec * codec, GstAmcFormat * format)
{
  gboolean ret = TRUE;
  int err = 0;

  GST_DEBUG ("%s", __PRETTY_FUNCTION__);

  g_return_val_if_fail (codec != NULL, FALSE);
  g_return_val_if_fail (format != NULL, FALSE);

  err = media_codec_queue_csd (codec->codec_delegate, format->format);
  if (err > 0) {
    GST_ERROR ("Failed to queue codec specific data");
    ret = FALSE;
    goto done;
  }

done:

  return ret;
}

GstAmcFormat *
gst_amc_codec_get_output_format (GstAmcCodec * codec)
{
  GstAmcFormat *ret = NULL;

  g_return_val_if_fail (codec != NULL, NULL);

  GST_DEBUG ("%s", __PRETTY_FUNCTION__);

  ret = g_slice_new0 (GstAmcFormat);

  ret->format = media_codec_get_output_format (codec->codec_delegate);
  if (ret->format == NULL) {
    GST_ERROR ("Failed to get output format");
    g_slice_free (GstAmcFormat, ret);
    ret = NULL;
    goto done;
  }

done:

  return ret;
}

gboolean
gst_amc_codec_start (GstAmcCodec * codec)
{
  gboolean ret = TRUE;
  int err = 0;

  g_return_val_if_fail (codec != NULL, FALSE);

  GST_DEBUG ("%s", __PRETTY_FUNCTION__);

  err = media_codec_start (codec->codec_delegate);
  if (err > 0) {
    GST_ERROR ("Failed to start media codec");
    ret = FALSE;
    goto done;
  }

done:
  return ret;
}

gboolean
gst_amc_codec_stop (GstAmcCodec * codec)
{
  gboolean ret = TRUE;
  int err = 0;

  g_return_val_if_fail (codec != NULL, FALSE);

  GST_DEBUG ("%s", __PRETTY_FUNCTION__);

  err = media_codec_stop (codec->codec_delegate);
  if (err > 0) {
    GST_ERROR ("Failed to start media codec");
    ret = FALSE;
    goto done;
  }

done:

  return ret;
}

gboolean
gst_amc_codec_flush (GstAmcCodec * codec)
{
  gboolean ret = TRUE;
  gint err = 0;

  g_return_val_if_fail (codec != NULL, FALSE);

  GST_DEBUG ("%s", __PRETTY_FUNCTION__);

  err = media_codec_flush (codec->codec_delegate);
  if (err < 0) {
    GST_ERROR ("Failed to flush the media codec (err: %d)", err);
    ret = FALSE;
    goto done;
  }

done:
  return ret;
}

gboolean
gst_amc_codec_release (GstAmcCodec * codec)
{
  gboolean ret = TRUE;
  gint err = 0;

  g_return_val_if_fail (codec != NULL, FALSE);

  GST_DEBUG ("%s", __PRETTY_FUNCTION__);

  err = media_codec_release (codec->codec_delegate);
  if (err < 0) {
    GST_ERROR ("Failed to release media codec (err: %d)", err);
    ret = FALSE;
    goto done;
  }

done:
  return ret;
}

void
gst_amc_codec_free_buffers (GstAmcBuffer * buffers, gsize n_buffers)
{
  g_return_if_fail (buffers != NULL);

  g_free (buffers);
}

GstAmcBuffer *
gst_amc_codec_get_output_buffers (GstAmcCodec * codec, gsize * n_buffers)
{
  size_t n_output_buffers;
  GstAmcBuffer *ret = NULL;
  size_t i;

  g_return_val_if_fail (codec != NULL, NULL);
  g_return_val_if_fail (n_buffers != NULL, NULL);

  GST_DEBUG ("%s", __PRETTY_FUNCTION__);

  *n_buffers = 0;
  n_output_buffers =
      media_codec_get_output_buffers_size (codec->codec_delegate);
  if (n_output_buffers == 0) {
    GST_ERROR ("Failed to get output buffers array length");
    goto done;
  }
  GST_DEBUG ("n_output_buffers: %u", n_output_buffers);

  *n_buffers = n_output_buffers;
  ret = g_new0 (GstAmcBuffer, n_output_buffers);

  for (i = 0; i < n_output_buffers; i++) {
    ret[i].data = media_codec_get_nth_output_buffer (codec->codec_delegate, i);
    if (!ret[i].data) {
      GST_ERROR ("Failed to get output buffer address %d", i);
      goto error;
    }
    ret[i].size =
        media_codec_get_nth_output_buffer_capacity (codec->codec_delegate, i);
    GST_DEBUG ("output buffer[%d] size: %d", i, ret[i].size);
  }

done:
  return ret;

error:
  if (ret)
    gst_amc_codec_free_buffers (ret, n_output_buffers);
  ret = NULL;
  *n_buffers = 0;
  goto done;
}

GstAmcBuffer *
gst_amc_codec_get_input_buffers (GstAmcCodec * codec, gsize * n_buffers)
{
  size_t n_input_buffers;
  GstAmcBuffer *ret = NULL;
  size_t i;

  g_return_val_if_fail (codec != NULL, NULL);
  g_return_val_if_fail (n_buffers != NULL, NULL);

  GST_DEBUG ("%s", __PRETTY_FUNCTION__);

  *n_buffers = 0;
  n_input_buffers = media_codec_get_input_buffers_size (codec->codec_delegate);
  if (n_input_buffers == 0) {
    GST_ERROR ("Failed to get input buffers array length");
    goto done;
  }
  GST_DEBUG ("n_input_buffers: %u", n_input_buffers);

  *n_buffers = n_input_buffers;
  ret = g_new0 (GstAmcBuffer, n_input_buffers);

  for (i = 0; i < n_input_buffers; i++) {
    ret[i].data = media_codec_get_nth_input_buffer (codec->codec_delegate, i);
    if (!ret[i].data) {
      GST_ERROR ("Failed to get input buffer address %d", i);
      goto error;
    }
    ret[i].size =
        media_codec_get_nth_input_buffer_capacity (codec->codec_delegate, i);
    GST_DEBUG ("input buffer[%d] size: %d", i, ret[i].size);
  }

done:
  return ret;

error:
  if (ret)
    gst_amc_codec_free_buffers (ret, n_input_buffers);
  ret = NULL;
  *n_buffers = 0;
  goto done;
}

gint
gst_amc_codec_dequeue_input_buffer (GstAmcCodec * codec, gint64 timeoutUs)
{
  gint ret = G_MININT;
  size_t index = 0;

  g_return_val_if_fail (codec != NULL, G_MININT);

  GST_DEBUG ("%s", __PRETTY_FUNCTION__);

  ret =
      media_codec_dequeue_input_buffer (codec->codec_delegate, &index,
      timeoutUs);
  if (ret < 0) {
    GST_WARNING ("Failed to dequeue input buffer (ret: %d)", ret);
    if (ret == -11)
      ret = INFO_TRY_AGAIN_LATER;
    goto done;
  }
  ret = index;

  GST_DEBUG ("Dequeued input buffer #%d", index);

done:
  return ret;
}

gint
gst_amc_codec_dequeue_output_buffer (GstAmcCodec * codec,
    GstAmcBufferInfo * info, gint64 timeoutUs)
{
  gint ret = G_MININT;
  MediaCodecBufferInfo priv_info;

  g_return_val_if_fail (codec != NULL, G_MININT);

  GST_DEBUG ("%s", __PRETTY_FUNCTION__);

  ret =
      media_codec_dequeue_output_buffer (codec->codec_delegate, &priv_info,
      timeoutUs);
  GST_DEBUG ("dequeue output buffer ret: %d", ret);
  if (ret == INFO_TRY_AGAIN_LATER) {
    GST_WARNING ("media_codec_dequeue_output_buffer timed out, trying again");
    info->flags = 0;
    info->offset = 0;
    info->size = 0;
    info->presentation_time_us = 0;
    goto done;
  } else if (ret == INFO_OUTPUT_FORMAT_CHANGED) {
    GST_INFO ("Output format has changed");
    goto done;
  } else if (ret == INFO_OUTPUT_BUFFERS_CHANGED) {
    GST_INFO ("Output buffers have changed");
    goto done;
  }

  info->flags = priv_info.flags;
  info->offset = priv_info.offset;
  info->size = priv_info.size;
  info->presentation_time_us = priv_info.presentation_time_us;

  GST_DEBUG ("info->flags: %d", info->flags);
  GST_DEBUG ("info->offset: %d", info->offset);
  GST_DEBUG ("info->size: %d", info->size);
  GST_DEBUG ("info->presentation_time_us: %lld", info->presentation_time_us);

done:
  return ret;
}

gboolean
gst_amc_codec_queue_input_buffer (GstAmcCodec * codec, gint index,
    const GstAmcBufferInfo * info)
{
  gboolean ret = TRUE;
  gint err = 0;
  MediaCodecBufferInfo buf_info;

  g_return_val_if_fail (codec != NULL, FALSE);
  g_return_val_if_fail (info != NULL, FALSE);

  GST_DEBUG ("%s", __PRETTY_FUNCTION__);

  buf_info.index = index;
  buf_info.offset = info->offset;
  buf_info.size = info->size;
  buf_info.presentation_time_us = info->presentation_time_us;
  buf_info.flags = info->flags;
  GST_DEBUG ("buf_info.index: %d", buf_info.index);
  GST_DEBUG ("buf_info.offset %d", buf_info.offset);
  GST_DEBUG ("buf_info.size %d", buf_info.size);
  GST_DEBUG ("buf_info.presentation_time_us %lld",
      buf_info.presentation_time_us);
  GST_DEBUG ("buf_info.flags %d", buf_info.flags);

  err = media_codec_queue_input_buffer (codec->codec_delegate, &buf_info);
  if (err < 0) {
    GST_ERROR ("Failed to queue input buffer (err: %d, index: %d)", err, index);
    ret = FALSE;
    goto done;
  }

done:
  return ret;
}

gboolean
gst_amc_codec_release_output_buffer (GstAmcCodec * codec, gint index)
{
  gboolean ret = TRUE;
  gint err = 0;

  g_return_val_if_fail (codec != NULL, FALSE);

  GST_DEBUG ("%s", __PRETTY_FUNCTION__);

  err = media_codec_release_output_buffer (codec->codec_delegate, index);
  if (err < 0) {
    GST_ERROR ("Failed to release output buffer (err: %d, index: %d)", err,
        index);
    ret = FALSE;
    goto done;
  }

done:
  return ret;
}

GstAmcFormat *
gst_amc_format_new_audio (const gchar * mime, gint sample_rate, gint channels)
{
#if 0
  JNIEnv *env;
  GstAmcFormat *format = NULL;
  jstring mime_str;
  jobject object = NULL;
#endif

  g_return_val_if_fail (mime != NULL, NULL);

#if 0
  env = gst_amc_get_jni_env ();

  mime_str = (*env)->NewStringUTF (env, mime);
  if (mime_str == NULL)
    goto error;

  format = g_slice_new0 (GstAmcFormat);

  object =
      (*env)->CallStaticObjectMethod (env, media_format.klass,
      media_format.create_audio_format, mime_str, sample_rate, channels);
  if ((*env)->ExceptionCheck (env) || !object) {
    (*env)->ExceptionClear (env);
    GST_ERROR ("Failed to create format '%s'", mime);
    goto error;
  }

  format->object = (*env)->NewGlobalRef (env, object);
  if (!format->object) {
    GST_ERROR ("Failed to create global reference");
    (*env)->ExceptionClear (env);
    goto error;
  }

done:
  if (object)
    (*env)->DeleteLocalRef (env, object);
  if (mime_str)
    (*env)->DeleteLocalRef (env, mime_str);
  mime_str = NULL;


  return format;

error:
  if (format)
    g_slice_free (GstAmcFormat, format);
  format = NULL;
  goto done;
#endif

  return NULL;
}

GstAmcFormat *
gst_amc_format_new_video (const gchar * mime, gint width, gint height)
{
  GstAmcFormat *format = NULL;
  gchar *mime_str = NULL;

  g_return_val_if_fail (mime != NULL, NULL);

  GST_DEBUG ("%s", __PRETTY_FUNCTION__);

  mime_str = g_strdup (mime);
  mime_str = locale_to_utf8 (mime_str, strlen (mime_str));
  if (mime_str == NULL)
    goto error;

  format = g_slice_new0 (GstAmcFormat);

  format->format =
      media_format_create_video_format (mime_str, width, height, 0, 0);
  if (format->format == NULL) {
    GST_ERROR ("Failed to create format '%s'", mime);
    goto error;
  }

done:
  if (mime_str)
    g_free (mime_str);
  mime_str = NULL;

  return format;

error:
  if (format)
    g_slice_free (GstAmcFormat, format);
  format = NULL;
  goto done;
}

void
gst_amc_format_free (GstAmcFormat * format)
{
  g_return_if_fail (format != NULL);

  GST_DEBUG ("%s", __PRETTY_FUNCTION__);

  media_format_destroy (format->format);
  g_slice_free (GstAmcFormat, format);
}

gchar *
gst_amc_format_to_string (GstAmcFormat * format)
{
  return NULL;
}

gboolean
gst_amc_format_contains_key (GstAmcFormat * format, const gchar * key)
{
  return FALSE;
}

gboolean
gst_amc_format_get_float (GstAmcFormat * format, const gchar * key,
    gfloat * value)
{
  return FALSE;
}

void
gst_amc_format_set_float (GstAmcFormat * format, const gchar * key,
    gfloat value)
{
}

gboolean
gst_amc_format_get_int (GstAmcFormat * format, const gchar * key, gint * value)
{
  return FALSE;
}

void
gst_amc_format_set_int (GstAmcFormat * format, const gchar * key, gint value)
{
}

gboolean
gst_amc_format_get_string (GstAmcFormat * format, const gchar * key,
    gchar ** value)
{
  return FALSE;
}

void
gst_amc_format_set_string (GstAmcFormat * format, const gchar * key,
    const gchar * value)
{
}

gboolean
gst_amc_format_get_buffer (GstAmcFormat * format, const gchar * key,
    guint8 ** data, gsize * size)
{
  return FALSE;
}

void
gst_amc_format_set_buffer (GstAmcFormat * format, const gchar * key,
    guint8 * data, gsize size)
{
  gchar *key_str = NULL;

  g_return_if_fail (format != NULL);
  g_return_if_fail (key != NULL);
  g_return_if_fail (data != NULL);

  GST_DEBUG ("%s", __PRETTY_FUNCTION__);

  key_str = g_strdup (key);
  key_str = locale_to_utf8 (key_str, strlen (key));
  if (!key_str)
    goto done;

  media_format_set_byte_buffer (format->format, key, data, size);

done:
  if (key_str)
    g_free (key_str);
  key_str = NULL;
}

static gboolean
scan_codecs (GstPlugin * plugin)
{
  gboolean ret = TRUE;
  guint32 codec_count, i;
  const GstStructure *cache_data;

  GST_DEBUG ("Scanning available codecs");

  if ((cache_data = gst_plugin_get_cache_data (plugin))) {
    const GValue *arr = gst_structure_get_value (cache_data, "codecs");
    guint i, n;

    GST_DEBUG ("Getting codecs from cache");
    n = gst_value_array_get_size (arr);
    for (i = 0; i < n; i++) {
      const GValue *cv = gst_value_array_get_value (arr, i);
      const GstStructure *cs = gst_value_get_structure (cv);
      const gchar *name;
      gboolean is_encoder;
      const GValue *starr;
      guint j, n2;
      GstAmcCodecInfo *gst_codec_info;

      gst_codec_info = g_new0 (GstAmcCodecInfo, 1);

      name = gst_structure_get_string (cs, "name");
      gst_structure_get_boolean (cs, "is-encoder", &is_encoder);
      gst_codec_info->name = g_strdup (name);
      gst_codec_info->is_encoder = is_encoder;

      starr = gst_structure_get_value (cs, "supported-types");
      n2 = gst_value_array_get_size (starr);

      gst_codec_info->n_supported_types = n2;
      gst_codec_info->supported_types = g_new0 (GstAmcCodecType, n2);

      for (j = 0; j < n2; j++) {
        const GValue *stv = gst_value_array_get_value (starr, j);
        const GstStructure *sts = gst_value_get_structure (stv);
        const gchar *mime;
        const GValue *cfarr;
        const GValue *plarr;
        guint k, n3;
        GstAmcCodecType *gst_codec_type = &gst_codec_info->supported_types[j];

        mime = gst_structure_get_string (sts, "mime");
        gst_codec_type->mime = g_strdup (mime);

        cfarr = gst_structure_get_value (sts, "color-formats");
        n3 = gst_value_array_get_size (cfarr);

        gst_codec_type->n_color_formats = n3;
        gst_codec_type->color_formats = g_new0 (gint, n3);

        for (k = 0; k < n3; k++) {
          const GValue *cfv = gst_value_array_get_value (cfarr, k);
          gint cf = g_value_get_int (cfv);

          gst_codec_type->color_formats[k] = cf;
        }

        plarr = gst_structure_get_value (sts, "profile-levels");
        n3 = gst_value_array_get_size (plarr);

        gst_codec_type->n_profile_levels = n3;
        gst_codec_type->profile_levels =
            g_malloc0 (sizeof (gst_codec_type->profile_levels[0]) * n3);

        for (k = 0; k < n3; k++) {
          const GValue *plv = gst_value_array_get_value (plarr, k);
          const GValue *p, *l;

          p = gst_value_array_get_value (plv, 0);
          l = gst_value_array_get_value (plv, 1);
          gst_codec_type->profile_levels[k].profile = g_value_get_int (p);
          gst_codec_type->profile_levels[k].level = g_value_get_int (l);
        }
      }

      codec_infos = g_list_append (codec_infos, gst_codec_info);
    }

    return TRUE;
  }

  codec_count = media_codec_list_count_codecs ();
  if (codec_count == 0) {
    GST_ERROR ("Failed to get number of available codecs");
    goto done;
  }

  GST_DEBUG ("Found %d available codecs", codec_count);

  for (i = 0; i < codec_count; i++) {
    GstAmcCodecInfo *gst_codec_info;
    const gchar *name_str = NULL;
    gboolean is_encoder;
    size_t n_supported_types = 0;
    size_t j;
    gboolean valid_codec = TRUE;

    gst_codec_info = g_new0 (GstAmcCodecInfo, 1);

    media_codec_list_get_codec_info_at_id (i);

    name_str = media_codec_list_get_codec_name (i);
    if (!name_str) {
      GST_ERROR ("Failed to get codec name");
      valid_codec = FALSE;
      goto next_codec;
    }

    GST_INFO ("Checking codec '%s'", name_str);

    /* Compatibility codec names */
    if (strcmp (name_str, "AACEncoder") == 0 ||
        strcmp (name_str, "OMX.google.raw.decoder") == 0) {
      GST_INFO ("Skipping compatibility codec '%s'", name_str);
      valid_codec = FALSE;
      goto next_codec;
    }

    if (g_str_has_suffix (name_str, ".secure")) {
      GST_INFO ("Skipping DRM codec '%s'", name_str);
      valid_codec = FALSE;
      goto next_codec;
    }

    /* FIXME: Non-Google codecs usually just don't work and hang forever
     * or crash when not used from a process that started the Java
     * VM via the non-public AndroidRuntime class. Can we somehow
     * initialize all this?
     */
#if 0
    if (!g_str_has_prefix (name_str, "OMX.google.")) {
      GST_INFO ("Skipping non-Google codec '%s' in standalone mode", name_str);
      valid_codec = FALSE;
      goto next_codec;
    }
#endif

    if (g_str_has_prefix (name_str, "OMX.ARICENT.")) {
      GST_INFO ("Skipping possible broken codec '%s'", name_str);
      valid_codec = FALSE;
      goto next_codec;
    }

    /* FIXME:
     *   - Vorbis: Generates clicks for multi-channel streams
     *   - *Law: Generates output with too low frequencies
     */
    if (strcmp (name_str, "OMX.google.vorbis.decoder") == 0 ||
        strcmp (name_str, "OMX.google.g711.alaw.decoder") == 0 ||
        strcmp (name_str, "OMX.google.g711.mlaw.decoder") == 0) {
      GST_INFO ("Skipping known broken codec '%s'", name_str);
      valid_codec = FALSE;
      goto next_codec;
    }
    gst_codec_info->name = g_strdup (name_str);

    is_encoder = media_codec_list_is_encoder (i);
    gst_codec_info->is_encoder = is_encoder;

    n_supported_types = media_codec_list_get_num_supported_types (i);

    GST_INFO ("Codec '%s' has %d supported types", name_str, n_supported_types);

    gst_codec_info->supported_types =
        g_new0 (GstAmcCodecType, n_supported_types);
    gst_codec_info->n_supported_types = n_supported_types;

    if (n_supported_types == 0) {
      valid_codec = FALSE;
      GST_ERROR ("Codec has no supported types");
      goto next_codec;
    }

    for (j = 0; j < n_supported_types; j++) {
      GstAmcCodecType *gst_codec_type;
      gchar *supported_type_str;
      guint32 *color_formats_elems = NULL;
      jsize n_elems = 0, k;
      int err = 0;
      size_t len = 0;
      gchar *mime = NULL;

      gst_codec_type = &gst_codec_info->supported_types[j];

      len = media_codec_list_get_nth_supported_type_len (i, j);
      supported_type_str = g_malloc (len);
      err = media_codec_list_get_nth_supported_type (i, supported_type_str, j);
      if (err > 0 || !supported_type_str) {
        GST_ERROR ("Failed to get %d-th supported type", j);
        valid_codec = FALSE;
        goto next_supported_type;
      }

      mime = g_malloc (len);
      mime = locale_to_utf8 (supported_type_str, len);
      if (!mime) {
        GST_ERROR ("Failed to convert supported type to UTF8");
        valid_codec = FALSE;
        goto next_supported_type;
      }

      GST_INFO ("Supported type '%s'", mime);
      gst_codec_type->mime = g_strdup (mime);

      n_elems = media_codec_list_get_num_color_formats (i, mime);
      GST_INFO ("Type '%s' has %d supported color formats", mime, n_elems);
      if (n_elems == 0) {
        GST_INFO ("Zero supported color formats for type '%s'", mime);
        valid_codec = FALSE;
        goto next_supported_type;
      }
      gst_codec_type->n_color_formats = n_elems;
      gst_codec_type->color_formats = g_new0 (gint, n_elems);

      color_formats_elems = g_new0 (guint32, n_elems);
      err =
          media_codec_list_get_codec_color_formats (i, mime,
          color_formats_elems);
      if (!color_formats_elems) {
        GST_ERROR ("Failed to get color format elements");
        valid_codec = FALSE;
        goto next_supported_type;
      }

      for (k = 0; k < n_elems; k++) {
        GST_INFO ("Color format %d: %d", k, color_formats_elems[k]);
        gst_codec_type->color_formats[k] = color_formats_elems[k];
      }

      if (g_str_has_prefix (gst_codec_type->mime, "video/")) {
        if (!n_elems) {
          GST_ERROR ("No supported color formats for video codec");
          valid_codec = FALSE;
          goto next_supported_type;
        }

        if (!ignore_unknown_color_formats
            && !accepted_color_formats (gst_codec_type, is_encoder)) {
          GST_ERROR ("Codec has unknown color formats, ignoring");
          valid_codec = FALSE;
          g_assert_not_reached ();
          goto next_supported_type;
        }
      }

      n_elems = media_codec_list_get_num_profile_levels (i, mime);
      GST_INFO ("Type '%s' has %d supported profile levels", mime, n_elems);
      if (n_elems == 0) {
        GST_INFO ("Zero supported profile levels for type '%s'", mime);
        valid_codec = FALSE;
        goto next_supported_type;
      }
      gst_codec_type->n_profile_levels = n_elems;
      gst_codec_type->profile_levels =
          g_malloc0 (sizeof (gst_codec_type->profile_levels[0]) * n_elems);

      for (k = 0; k < n_elems; k++) {
        guint32 level = 0, profile = 0;
        profile_level pro_level;

        err =
            media_codec_list_get_nth_codec_profile_level (i, mime, &pro_level,
            k);
        if (err > 0) {
          GST_ERROR ("Failed to get %d-th profile/level", k);
          valid_codec = FALSE;
          goto next_profile_level;
        }

        level = pro_level.level;
        profile = pro_level.profile;

        GST_INFO ("Level %d: 0x%08x", k, level);
        gst_codec_type->profile_levels[k].level = level;

        GST_INFO ("Profile %d: 0x%08x", k, profile);
        gst_codec_type->profile_levels[k].profile = profile;

      next_profile_level:
        if (!valid_codec)
          break;
      }

    next_supported_type:
      if (color_formats_elems)
        g_free (color_formats_elems);
      color_formats_elems = NULL;
      if (supported_type_str)
        g_free (supported_type_str);
      supported_type_str = NULL;
      if (mime)
        g_free (mime);
      mime = NULL;
      if (!valid_codec)
        break;
    }

    /* We need at least a valid supported type */
    if (valid_codec) {
      GST_LOG ("Successfully scanned codec '%s'", name_str);
      codec_infos = g_list_append (codec_infos, gst_codec_info);
      gst_codec_info = NULL;
    }

    /* Clean up of all local references we got */
  next_codec:
    if (gst_codec_info) {
      gint j;

      for (j = 0; j < gst_codec_info->n_supported_types; j++) {
        GstAmcCodecType *gst_codec_type = &gst_codec_info->supported_types[j];

        g_free (gst_codec_type->mime);
        g_free (gst_codec_type->color_formats);
        g_free (gst_codec_type->profile_levels);
      }
      g_free (gst_codec_info->supported_types);
      g_free (gst_codec_info->name);
      g_free (gst_codec_info);
    }
    gst_codec_info = NULL;
    valid_codec = TRUE;
  }

  ret = codec_infos != NULL;

  /* If successful we store a cache of the codec information in
   * the registry. Otherwise we would always load all codecs during
   * plugin initialization which can take quite some time (because
   * of hardware) and also loads lots of shared libraries (which
   * number is limited by 64 in Android).
   */
  if (ret) {
    GstStructure *new_cache_data = gst_structure_new_empty ("gst-amc-cache");
    GList *l;
    GValue arr = { 0, };

    g_value_init (&arr, GST_TYPE_ARRAY);

    for (l = codec_infos; l; l = l->next) {
      GstAmcCodecInfo *gst_codec_info = l->data;
      GValue cv = { 0, };
      GstStructure *cs = gst_structure_new_empty ("gst-amc-codec");
      GValue starr = { 0, };
      gint i;

      gst_structure_set (cs, "name", G_TYPE_STRING, gst_codec_info->name,
          "is-encoder", G_TYPE_BOOLEAN, gst_codec_info->is_encoder, NULL);

      g_value_init (&starr, GST_TYPE_ARRAY);

      for (i = 0; i < gst_codec_info->n_supported_types; i++) {
        GstAmcCodecType *gst_codec_type = &gst_codec_info->supported_types[i];
        GstStructure *sts = gst_structure_new_empty ("gst-amc-supported-type");
        GValue stv = { 0, };
        GValue tmparr = { 0, };
        gint j;

        gst_structure_set (sts, "mime", G_TYPE_STRING, gst_codec_type->mime,
            NULL);

        g_value_init (&tmparr, GST_TYPE_ARRAY);
        for (j = 0; j < gst_codec_type->n_color_formats; j++) {
          GValue tmp = { 0, };

          g_value_init (&tmp, G_TYPE_INT);
          g_value_set_int (&tmp, gst_codec_type->color_formats[j]);
          gst_value_array_append_value (&tmparr, &tmp);
          g_value_unset (&tmp);
        }
        gst_structure_set_value (sts, "color-formats", &tmparr);
        g_value_unset (&tmparr);

        g_value_init (&tmparr, GST_TYPE_ARRAY);
        for (j = 0; j < gst_codec_type->n_profile_levels; j++) {
          GValue tmparr2 = { 0, };
          GValue tmp = { 0, };

          g_value_init (&tmparr2, GST_TYPE_ARRAY);
          g_value_init (&tmp, G_TYPE_INT);
          g_value_set_int (&tmp, gst_codec_type->profile_levels[j].profile);
          gst_value_array_append_value (&tmparr2, &tmp);
          g_value_set_int (&tmp, gst_codec_type->profile_levels[j].level);
          gst_value_array_append_value (&tmparr2, &tmp);
          gst_value_array_append_value (&tmparr, &tmparr2);
          g_value_unset (&tmp);
          g_value_unset (&tmparr2);
        }
        gst_structure_set_value (sts, "profile-levels", &tmparr);

        g_value_init (&stv, GST_TYPE_STRUCTURE);
        gst_value_set_structure (&stv, sts);
        gst_value_array_append_value (&starr, &stv);
        g_value_unset (&tmparr);
        gst_structure_free (sts);
      }

      gst_structure_set_value (cs, "supported-types", &starr);
      g_value_unset (&starr);

      g_value_init (&cv, GST_TYPE_STRUCTURE);
      gst_value_set_structure (&cv, cs);
      gst_value_array_append_value (&arr, &cv);
      g_value_unset (&cv);
      gst_structure_free (cs);
    }

    gst_structure_set_value (new_cache_data, "codecs", &arr);
    g_value_unset (&arr);

    gst_plugin_set_cache_data (plugin, new_cache_data);
  }

done:
  return ret;
}

static const struct
{
  gint color_format;
  GstVideoFormat video_format;
} color_format_mapping_table[] = {
  {
  COLOR_FormatYUV420Planar, GST_VIDEO_FORMAT_I420}, {
  COLOR_FormatYUV420SemiPlanar, GST_VIDEO_FORMAT_NV12}, {
  COLOR_TI_FormatYUV420PackedSemiPlanar, GST_VIDEO_FORMAT_NV12}, {
  COLOR_TI_FormatYUV420PackedSemiPlanarInterlaced, GST_VIDEO_FORMAT_NV12}, {
  COLOR_QCOM_FormatYUV420SemiPlanar, GST_VIDEO_FORMAT_NV12}, {
  256, GST_VIDEO_FORMAT_NV12}
};

static gboolean
accepted_color_formats (GstAmcCodecType * type, gboolean is_encoder)
{
  gint i, j;
  gint accepted = 0, all = type->n_color_formats;

  for (i = 0; i < type->n_color_formats; i++) {
    gboolean found = FALSE;
    /* We ignore this one */
    if (type->color_formats[i] == COLOR_FormatAndroidOpaque)
      all--;

    for (j = 0; j < G_N_ELEMENTS (color_format_mapping_table); j++) {
      if (color_format_mapping_table[j].color_format == type->color_formats[i]) {
        found = TRUE;
        break;
      }
    }

    if (found)
      accepted++;
  }

  if (is_encoder)
    return accepted > 0;
  else
    return accepted == all && all > 0;
}

GstVideoFormat
gst_amc_color_format_to_video_format (gint color_format)
{
  gint i;

  for (i = 0; i < G_N_ELEMENTS (color_format_mapping_table); i++) {
    if (color_format_mapping_table[i].color_format == color_format)
      return color_format_mapping_table[i].video_format;
  }

  return GST_VIDEO_FORMAT_UNKNOWN;
}

gint
gst_amc_video_format_to_color_format (GstVideoFormat video_format)
{
  gint i;

  for (i = 0; i < G_N_ELEMENTS (color_format_mapping_table); i++) {
    if (color_format_mapping_table[i].video_format == video_format)
      return color_format_mapping_table[i].color_format;
  }

  return -1;
}

static const struct
{
  gint id;
  const gchar *str;
  const gchar *alt_str;
} avc_profile_mapping_table[] = {
  {
  AVCProfileBaseline, "baseline", "constrained-baseline"}, {
  AVCProfileMain, "main", NULL}, {
  AVCProfileExtended, "extended", NULL}, {
  AVCProfileHigh, "high"}, {
  AVCProfileHigh10, "high-10", "high-10-intra"}, {
  AVCProfileHigh422, "high-4:2:2", "high-4:2:2-intra"}, {
  AVCProfileHigh444, "high-4:4:4", "high-4:4:4-intra"}
};

const gchar *
gst_amc_avc_profile_to_string (gint profile, const gchar ** alternative)
{
  gint i;

  for (i = 0; i < G_N_ELEMENTS (avc_profile_mapping_table); i++) {
    if (avc_profile_mapping_table[i].id == profile) {
      *alternative = avc_profile_mapping_table[i].alt_str;
      return avc_profile_mapping_table[i].str;
    }
  }

  return NULL;
}

gint
gst_amc_avc_profile_from_string (const gchar * profile)
{
  gint i;

  g_return_val_if_fail (profile != NULL, -1);

  for (i = 0; i < G_N_ELEMENTS (avc_profile_mapping_table); i++) {
    if (strcmp (avc_profile_mapping_table[i].str, profile) == 0)
      return avc_profile_mapping_table[i].id;
  }

  return -1;
}

static const struct
{
  gint id;
  const gchar *str;
} avc_level_mapping_table[] = {
  {
  AVCLevel1, "1"}, {
  AVCLevel1b, "1b"}, {
  AVCLevel11, "1.1"}, {
  AVCLevel12, "1.2"}, {
  AVCLevel13, "1.3"}, {
  AVCLevel2, "2"}, {
  AVCLevel21, "2.1"}, {
  AVCLevel22, "2.2"}, {
  AVCLevel3, "3"}, {
  AVCLevel31, "3.1"}, {
  AVCLevel32, "3.2"}, {
  AVCLevel4, "4"}, {
  AVCLevel41, "4.1"}, {
  AVCLevel42, "4.2"}, {
  AVCLevel5, "5"}, {
  AVCLevel51, "5.1"}
};

const gchar *
gst_amc_avc_level_to_string (gint level)
{
  gint i;

  for (i = 0; i < G_N_ELEMENTS (avc_level_mapping_table); i++) {
    if (avc_level_mapping_table[i].id == level)
      return avc_level_mapping_table[i].str;
  }

  return NULL;
}

gint
gst_amc_avc_level_from_string (const gchar * level)
{
  gint i;

  g_return_val_if_fail (level != NULL, -1);

  for (i = 0; i < G_N_ELEMENTS (avc_level_mapping_table); i++) {
    if (strcmp (avc_level_mapping_table[i].str, level) == 0)
      return avc_level_mapping_table[i].id;
  }

  return -1;
}

static const struct
{
  gint id;
  gint gst_id;
} h263_profile_mapping_table[] = {
  {
  H263ProfileBaseline, 0}, {
  H263ProfileH320Coding, 1}, {
  H263ProfileBackwardCompatible, 2}, {
  H263ProfileISWV2, 3}, {
  H263ProfileISWV3, 4}, {
  H263ProfileHighCompression, 5}, {
  H263ProfileInternet, 6}, {
  H263ProfileInterlace, 7}, {
  H263ProfileHighLatency, 8}
};

gint
gst_amc_h263_profile_to_gst_id (gint profile)
{
  gint i;

  for (i = 0; i < G_N_ELEMENTS (h263_profile_mapping_table); i++) {
    if (h263_profile_mapping_table[i].id == profile)
      return h263_profile_mapping_table[i].gst_id;
  }

  return -1;
}

gint
gst_amc_h263_profile_from_gst_id (gint profile)
{
  gint i;

  for (i = 0; i < G_N_ELEMENTS (h263_profile_mapping_table); i++) {
    if (h263_profile_mapping_table[i].gst_id == profile)
      return h263_profile_mapping_table[i].id;
  }

  return -1;
}

static const struct
{
  gint id;
  gint gst_id;
} h263_level_mapping_table[] = {
  {
  H263Level10, 10}, {
  H263Level20, 20}, {
  H263Level30, 30}, {
  H263Level40, 40}, {
  H263Level50, 50}, {
  H263Level60, 60}, {
  H263Level70, 70}
};

gint
gst_amc_h263_level_to_gst_id (gint level)
{
  gint i;

  for (i = 0; i < G_N_ELEMENTS (h263_level_mapping_table); i++) {
    if (h263_level_mapping_table[i].id == level)
      return h263_level_mapping_table[i].gst_id;
  }

  return -1;
}

gint
gst_amc_h263_level_from_gst_id (gint level)
{
  gint i;

  for (i = 0; i < G_N_ELEMENTS (h263_level_mapping_table); i++) {
    if (h263_level_mapping_table[i].gst_id == level)
      return h263_level_mapping_table[i].id;
  }

  return -1;
}

static const struct
{
  gint id;
  const gchar *str;
} mpeg4_profile_mapping_table[] = {
  {
  MPEG4ProfileSimple, "simple"}, {
  MPEG4ProfileSimpleScalable, "simple-scalable"}, {
  MPEG4ProfileCore, "core"}, {
  MPEG4ProfileMain, "main"}, {
  MPEG4ProfileNbit, "n-bit"}, {
  MPEG4ProfileScalableTexture, "scalable"}, {
  MPEG4ProfileSimpleFace, "simple-face"}, {
  MPEG4ProfileSimpleFBA, "simple-fba"}, {
  MPEG4ProfileBasicAnimated, "basic-animated-texture"}, {
  MPEG4ProfileHybrid, "hybrid"}, {
  MPEG4ProfileAdvancedRealTime, "advanced-real-time"}, {
  MPEG4ProfileCoreScalable, "core-scalable"}, {
  MPEG4ProfileAdvancedCoding, "advanced-coding-efficiency"}, {
  MPEG4ProfileAdvancedCore, "advanced-core"}, {
  MPEG4ProfileAdvancedScalable, "advanced-scalable-texture"}, {
  MPEG4ProfileAdvancedSimple, "advanced-simple"}
};

const gchar *
gst_amc_mpeg4_profile_to_string (gint profile)
{
  gint i;

  for (i = 0; i < G_N_ELEMENTS (mpeg4_profile_mapping_table); i++) {
    if (mpeg4_profile_mapping_table[i].id == profile)
      return mpeg4_profile_mapping_table[i].str;
  }

  return NULL;
}

gint
gst_amc_avc_mpeg4_profile_from_string (const gchar * profile)
{
  gint i;

  g_return_val_if_fail (profile != NULL, -1);

  for (i = 0; i < G_N_ELEMENTS (mpeg4_profile_mapping_table); i++) {
    if (strcmp (mpeg4_profile_mapping_table[i].str, profile) == 0)
      return mpeg4_profile_mapping_table[i].id;
  }

  return -1;
}

static const struct
{
  gint id;
  const gchar *str;
} mpeg4_level_mapping_table[] = {
  {
  MPEG4Level0, "0"}, {
  MPEG4Level0b, "0b"}, {
  MPEG4Level1, "1"}, {
  MPEG4Level2, "2"}, {
  MPEG4Level3, "3"}, {
  MPEG4Level4, "4"}, {
  MPEG4Level4a, "4a"}, {
MPEG4Level5, "5"},};

const gchar *
gst_amc_mpeg4_level_to_string (gint level)
{
  gint i;

  for (i = 0; i < G_N_ELEMENTS (mpeg4_level_mapping_table); i++) {
    if (mpeg4_level_mapping_table[i].id == level)
      return mpeg4_level_mapping_table[i].str;
  }

  return NULL;
}

gint
gst_amc_mpeg4_level_from_string (const gchar * level)
{
  gint i;

  g_return_val_if_fail (level != NULL, -1);

  for (i = 0; i < G_N_ELEMENTS (mpeg4_level_mapping_table); i++) {
    if (strcmp (mpeg4_level_mapping_table[i].str, level) == 0)
      return mpeg4_level_mapping_table[i].id;
  }

  return -1;
}

static const struct
{
  gint id;
  const gchar *str;
} aac_profile_mapping_table[] = {
  {
  AACObjectMain, "main"}, {
  AACObjectLC, "lc"}, {
  AACObjectSSR, "ssr"}, {
  AACObjectLTP, "ltp"}
};

const gchar *
gst_amc_aac_profile_to_string (gint profile)
{
  gint i;

  for (i = 0; i < G_N_ELEMENTS (aac_profile_mapping_table); i++) {
    if (aac_profile_mapping_table[i].id == profile)
      return aac_profile_mapping_table[i].str;
  }

  return NULL;
}

gint
gst_amc_aac_profile_from_string (const gchar * profile)
{
  gint i;

  g_return_val_if_fail (profile != NULL, -1);

  for (i = 0; i < G_N_ELEMENTS (aac_profile_mapping_table); i++) {
    if (strcmp (aac_profile_mapping_table[i].str, profile) == 0)
      return aac_profile_mapping_table[i].id;
  }

  return -1;
}

static const struct
{
  guint32 mask;
  GstAudioChannelPosition pos;
} channel_mapping_table[] = {
  {
  CHANNEL_OUT_FRONT_LEFT, GST_AUDIO_CHANNEL_POSITION_FRONT_LEFT}, {
  CHANNEL_OUT_FRONT_RIGHT, GST_AUDIO_CHANNEL_POSITION_FRONT_RIGHT}, {
  CHANNEL_OUT_FRONT_CENTER, GST_AUDIO_CHANNEL_POSITION_FRONT_CENTER}, {
  CHANNEL_OUT_LOW_FREQUENCY, GST_AUDIO_CHANNEL_POSITION_LFE1}, {
  CHANNEL_OUT_BACK_LEFT, GST_AUDIO_CHANNEL_POSITION_REAR_LEFT}, {
  CHANNEL_OUT_BACK_RIGHT, GST_AUDIO_CHANNEL_POSITION_REAR_RIGHT}, {
  CHANNEL_OUT_FRONT_LEFT_OF_CENTER,
        GST_AUDIO_CHANNEL_POSITION_FRONT_LEFT_OF_CENTER}, {
  CHANNEL_OUT_FRONT_RIGHT_OF_CENTER,
        GST_AUDIO_CHANNEL_POSITION_FRONT_RIGHT_OF_CENTER}, {
  CHANNEL_OUT_BACK_CENTER, GST_AUDIO_CHANNEL_POSITION_REAR_CENTER}, {
  CHANNEL_OUT_SIDE_LEFT, GST_AUDIO_CHANNEL_POSITION_SIDE_LEFT}, {
  CHANNEL_OUT_SIDE_RIGHT, GST_AUDIO_CHANNEL_POSITION_SIDE_RIGHT}, {
  CHANNEL_OUT_TOP_CENTER, GST_AUDIO_CHANNEL_POSITION_INVALID}, {
  CHANNEL_OUT_TOP_FRONT_LEFT, GST_AUDIO_CHANNEL_POSITION_INVALID}, {
  CHANNEL_OUT_TOP_FRONT_CENTER, GST_AUDIO_CHANNEL_POSITION_INVALID}, {
  CHANNEL_OUT_TOP_FRONT_RIGHT, GST_AUDIO_CHANNEL_POSITION_INVALID}, {
  CHANNEL_OUT_TOP_BACK_LEFT, GST_AUDIO_CHANNEL_POSITION_INVALID}, {
  CHANNEL_OUT_TOP_BACK_CENTER, GST_AUDIO_CHANNEL_POSITION_INVALID}, {
  CHANNEL_OUT_TOP_BACK_RIGHT, GST_AUDIO_CHANNEL_POSITION_INVALID}
};

gboolean
gst_amc_audio_channel_mask_to_positions (guint32 channel_mask, gint channels,
    GstAudioChannelPosition * pos)
{
  gint i, j;

  if (channel_mask == 0) {
    if (channels == 1) {
      pos[0] = GST_AUDIO_CHANNEL_POSITION_MONO;
      return TRUE;
    }
    if (channels == 2) {
      pos[0] = GST_AUDIO_CHANNEL_POSITION_FRONT_LEFT;
      pos[1] = GST_AUDIO_CHANNEL_POSITION_FRONT_RIGHT;
      return TRUE;
    }

    /* Now let the guesswork begin, these are the
     * AAC default channel assignments for these numbers
     * of channels */
    if (channels == 3) {
      channel_mask =
          CHANNEL_OUT_FRONT_LEFT | CHANNEL_OUT_FRONT_RIGHT |
          CHANNEL_OUT_FRONT_CENTER;
    } else if (channels == 4) {
      channel_mask =
          CHANNEL_OUT_FRONT_LEFT | CHANNEL_OUT_FRONT_RIGHT |
          CHANNEL_OUT_FRONT_CENTER | CHANNEL_OUT_BACK_CENTER;
    } else if (channels == 5) {
      channel_mask =
          CHANNEL_OUT_FRONT_LEFT | CHANNEL_OUT_FRONT_RIGHT |
          CHANNEL_OUT_FRONT_CENTER | CHANNEL_OUT_BACK_LEFT |
          CHANNEL_OUT_BACK_RIGHT;
    } else if (channels == 6) {
      channel_mask =
          CHANNEL_OUT_FRONT_LEFT | CHANNEL_OUT_FRONT_RIGHT |
          CHANNEL_OUT_FRONT_CENTER | CHANNEL_OUT_BACK_LEFT |
          CHANNEL_OUT_BACK_RIGHT | CHANNEL_OUT_LOW_FREQUENCY;
    } else if (channels == 8) {
      channel_mask =
          CHANNEL_OUT_FRONT_LEFT | CHANNEL_OUT_FRONT_RIGHT |
          CHANNEL_OUT_FRONT_CENTER | CHANNEL_OUT_BACK_LEFT |
          CHANNEL_OUT_BACK_RIGHT | CHANNEL_OUT_LOW_FREQUENCY |
          CHANNEL_OUT_FRONT_LEFT_OF_CENTER | CHANNEL_OUT_FRONT_RIGHT_OF_CENTER;
    }
  }

  for (i = 0, j = 0; i < G_N_ELEMENTS (channel_mapping_table); i++) {
    if ((channel_mask & channel_mapping_table[i].mask)) {
      pos[j++] = channel_mapping_table[i].pos;
      if (channel_mapping_table[i].pos == GST_AUDIO_CHANNEL_POSITION_INVALID) {
        memset (pos, 0, sizeof (GstAudioChannelPosition) * channels);
        GST_ERROR ("Unable to map channel mask 0x%08x",
            channel_mapping_table[i].mask);
        return FALSE;
      }
      if (j == channels)
        break;
    }
  }

  if (j != channels) {
    memset (pos, 0, sizeof (GstAudioChannelPosition) * channels);
    GST_ERROR ("Unable to map all channel positions in mask 0x%08x",
        channel_mask);
    return FALSE;
  }

  return TRUE;
}

guint32
gst_amc_audio_channel_mask_from_positions (GstAudioChannelPosition * positions,
    gint channels)
{
  gint i, j;
  guint32 channel_mask = 0;

  if (channels == 1 && !positions)
    return CHANNEL_OUT_FRONT_CENTER;
  if (channels == 2 && !positions)
    return CHANNEL_OUT_FRONT_LEFT | CHANNEL_OUT_FRONT_RIGHT;

  for (i = 0; i < channels; i++) {
    if (positions[i] == GST_AUDIO_CHANNEL_POSITION_INVALID)
      return 0;

    for (j = 0; j < G_N_ELEMENTS (channel_mapping_table); j++) {
      if (channel_mapping_table[j].pos == positions[i]) {
        channel_mask |= channel_mapping_table[j].mask;
        break;
      }
    }

    if (j == G_N_ELEMENTS (channel_mapping_table)) {
      GST_ERROR ("Unable to map channel position %d", positions[i]);
      return 0;
    }
  }

  return channel_mask;
}

static gchar *
create_type_name (const gchar * parent_name, const gchar * codec_name)
{
  gchar *typified_name;
  gint i, k;
  gint parent_name_len = strlen (parent_name);
  gint codec_name_len = strlen (codec_name);
  gboolean upper = TRUE;

  typified_name = g_new0 (gchar, parent_name_len + 1 + strlen (codec_name) + 1);
  memcpy (typified_name, parent_name, parent_name_len);
  typified_name[parent_name_len] = '-';

  for (i = 0, k = 0; i < codec_name_len; i++) {
    if (g_ascii_isalnum (codec_name[i])) {
      if (upper)
        typified_name[parent_name_len + 1 + k++] =
            g_ascii_toupper (codec_name[i]);
      else
        typified_name[parent_name_len + 1 + k++] =
            g_ascii_tolower (codec_name[i]);

      upper = FALSE;
    } else {
      /* Skip all non-alnum chars and start a new upper case word */
      upper = TRUE;
    }
  }

  return typified_name;
}

static gchar *
create_element_name (gboolean video, gboolean encoder, const gchar * codec_name)
{
#define PREFIX_LEN 10
  static const gchar *prefixes[] = {
    "amcviddec-",
    "amcauddec-",
    "amcvidenc-",
    "amcaudenc-"
  };
  gchar *element_name;
  gint i, k;
  gint codec_name_len = strlen (codec_name);
  const gchar *prefix;

  if (video && !encoder)
    prefix = prefixes[0];
  else if (!video && !encoder)
    prefix = prefixes[1];
  else if (video && encoder)
    prefix = prefixes[2];
  else
    prefix = prefixes[3];

  element_name = g_new0 (gchar, PREFIX_LEN + strlen (codec_name) + 1);
  memcpy (element_name, prefix, PREFIX_LEN);

  for (i = 0, k = 0; i < codec_name_len; i++) {
    if (g_ascii_isalnum (codec_name[i])) {
      element_name[PREFIX_LEN + k++] = g_ascii_tolower (codec_name[i]);
    }
    /* Skip all non-alnum chars */
  }

  return element_name;
}

#undef PREFIX_LEN

static gboolean
register_codecs (GstPlugin * plugin)
{
  gboolean ret = TRUE;
  GList *l;

  GST_DEBUG ("Registering plugins");

  for (l = codec_infos; l; l = l->next) {
    GstAmcCodecInfo *codec_info = l->data;
    gboolean is_audio = FALSE;
    gboolean is_video = FALSE;
    gint i;
    gint n_types;

    GST_DEBUG ("Registering codec '%s'", codec_info->name);
    for (i = 0; i < codec_info->n_supported_types; i++) {
      GstAmcCodecType *codec_type = &codec_info->supported_types[i];

      if (g_str_has_prefix (codec_type->mime, "audio/"))
        is_audio = TRUE;
      else if (g_str_has_prefix (codec_type->mime, "video/"))
        is_video = TRUE;
    }

    n_types = 0;
    if (is_audio)
      n_types++;
    if (is_video)
      n_types++;

    for (i = 0; i < n_types; i++) {
      GTypeQuery type_query;
      GTypeInfo type_info = { 0, };
      GType type, subtype;
      gchar *type_name, *element_name;
      guint rank;

      if (is_video && !codec_info->is_encoder) {
        type = gst_amc_video_dec_get_type ();
      } else if (is_audio && !codec_info->is_encoder) {
        type = gst_amc_audio_dec_get_type ();
      } else {
        GST_DEBUG ("Skipping unsupported codec type");
        continue;
      }

      g_type_query (type, &type_query);
      memset (&type_info, 0, sizeof (type_info));
      type_info.class_size = type_query.class_size;
      type_info.instance_size = type_query.instance_size;
      type_name = create_type_name (type_query.type_name, codec_info->name);

      if (g_type_from_name (type_name) != G_TYPE_INVALID) {
        GST_ERROR ("Type '%s' already exists for codec '%s'", type_name,
            codec_info->name);
        g_free (type_name);
        continue;
      }

      subtype = g_type_register_static (type, type_name, &type_info, 0);
      g_free (type_name);

      g_type_set_qdata (subtype, gst_amc_codec_info_quark, codec_info);

      element_name =
          create_element_name (is_video, codec_info->is_encoder,
          codec_info->name);

      /* Give the Google software codec a secondary rank,
       * everything else is likely a hardware codec */
      if (g_str_has_prefix (codec_info->name, "OMX.google"))
        rank = GST_RANK_SECONDARY;
      else
        rank = GST_RANK_PRIMARY;

      ret |= gst_element_register (plugin, element_name, rank, subtype);
      g_free (element_name);

      is_video = FALSE;
    }
  }

  return ret;
}

static gboolean
plugin_init (GstPlugin * plugin)
{
  const gchar *ignore;

  GST_DEBUG_CATEGORY_INIT (gst_amc_debug, "amc", 0, "android-media-codec");

  gst_plugin_add_dependency_simple (plugin, NULL, "/etc", "media_codecs.xml",
      GST_PLUGIN_DEPENDENCY_FLAG_NONE);

  /* Set this to TRUE to allow registering decoders that have
   * any unknown color formats, or encoders that only have
   * unknown color formats
   */
  ignore = g_getenv ("GST_AMC_IGNORE_UNKNOWN_COLOR_FORMATS");
  if (ignore && strcmp (ignore, "yes") == 0)
    ignore_unknown_color_formats = TRUE;

  if (!scan_codecs (plugin))
    return FALSE;

  gst_amc_codec_info_quark = g_quark_from_static_string ("gst-amc-codec-info");

  if (!register_codecs (plugin))
    return FALSE;

  GST_DEBUG ("Finished %s", __PRETTY_FUNCTION__);

  return TRUE;
}

GST_PLUGIN_DEFINE (GST_VERSION_MAJOR,
    GST_VERSION_MINOR,
    androidmedia,
    "Android Media Hybris plugin",
    plugin_init,
    PACKAGE_VERSION, GST_LICENSE, GST_PACKAGE_NAME, GST_PACKAGE_ORIGIN)
