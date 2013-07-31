/* GStreamer
 * Copyright (C) 2012 Smart TV Alliance
 *  Author: Thiago Sousa Santos <thiago.sousa.santos@collabora.com>, Collabora Ltd.
 *
 * gstmssmanifest.c:
 *
 * This library is free software; you can redistribute it and/or
 * modify it under the terms of the GNU Library General Public
 * License as published by the Free Software Foundation; either
 * version 2 of the License, or (at your option) any later version.
 *
 * This library is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU
 * Library General Public License for more details.
 *
 * You should have received a copy of the GNU Library General Public
 * License along with this library; if not, write to the
 * Free Software Foundation, Inc., 59 Temple Place - Suite 330,
 * Boston, MA 02111-1307, USA.
 */

#include <glib.h>
#include <string.h>
#include <stdio.h>
#include <ctype.h>
#include <libxml/parser.h>
#include <libxml/tree.h>

/* for parsing h264 codec data */
#include <gst/codecparsers/gsth264parser.h>

#include "gstmssmanifest.h"

#define DEFAULT_TIMESCALE             10000000

#define MSS_NODE_STREAM_FRAGMENT      "c"
#define MSS_NODE_STREAM_QUALITY       "QualityLevel"

#define MSS_PROP_BITRATE              "Bitrate"
#define MSS_PROP_DURATION             "d"
#define MSS_PROP_NUMBER               "n"
#define MSS_PROP_STREAM_DURATION      "Duration"
#define MSS_PROP_TIME                 "t"
#define MSS_PROP_TIMESCALE            "TimeScale"
#define MSS_PROP_URL                  "Url"

/* TODO check if atoi is successful? */

typedef struct _GstMssStreamFragment
{
  guint number;
  guint64 time;
  guint64 duration;
} GstMssStreamFragment;

typedef struct _GstMssStreamQuality
{
  xmlNodePtr xmlnode;

  gchar *bitrate_str;
  guint64 bitrate;
} GstMssStreamQuality;

struct _GstMssStream
{
  xmlNodePtr xmlnode;

  gboolean active;              /* if the stream is currently being used */
  gint selectedQualityIndex;

  GList *fragments;
  GList *qualities;

  gchar *url;

  GList *current_fragment;
  GList *current_quality;

  /* TODO move this to somewhere static */
  GRegex *regex_bitrate;
  GRegex *regex_position;
};

struct _GstMssManifest
{
  xmlDocPtr xml;
  xmlNodePtr xmlrootnode;

  gboolean is_live;

  GSList *streams;
};

static gboolean
node_has_type (xmlNodePtr node, const gchar * name)
{
  return strcmp ((gchar *) node->name, name) == 0;
}

static GstMssStreamQuality *
gst_mss_stream_quality_new (xmlNodePtr node)
{
  GstMssStreamQuality *q = g_slice_new (GstMssStreamQuality);

  q->xmlnode = node;
  q->bitrate_str = (gchar *) xmlGetProp (node, (xmlChar *) MSS_PROP_BITRATE);

  if (q->bitrate_str != NULL)
    q->bitrate = g_ascii_strtoull (q->bitrate_str, NULL, 10);
  else
    q->bitrate = 0;

  return q;
}

static void
gst_mss_stream_quality_free (GstMssStreamQuality * quality)
{
  g_return_if_fail (quality != NULL);

  xmlFree (quality->bitrate_str);
  g_slice_free (GstMssStreamQuality, quality);
}

static gint
compare_bitrate (GstMssStreamQuality * a, GstMssStreamQuality * b)
{
  if (a->bitrate > b->bitrate)
    return 1;
  if (a->bitrate < b->bitrate)
    return -1;
  return 0;

}

static void
_gst_mss_stream_init (GstMssStream * stream, xmlNodePtr node)
{
  xmlNodePtr iter;
  GstMssStreamFragment *previous_fragment = NULL;
  guint fragment_number = 0;
  guint64 fragment_time_accum = 0;

  stream->xmlnode = node;

  /* get the base url path generator */
  stream->url = (gchar *) xmlGetProp (node, (xmlChar *) MSS_PROP_URL);

  for (iter = node->children; iter; iter = iter->next) {
    if (node_has_type (iter, MSS_NODE_STREAM_FRAGMENT)) {
      gchar *duration_str;
      gchar *time_str;
      gchar *seqnum_str;
      GstMssStreamFragment *fragment = g_new (GstMssStreamFragment, 1);

      duration_str = (gchar *) xmlGetProp (iter, (xmlChar *) MSS_PROP_DURATION);
      time_str = (gchar *) xmlGetProp (iter, (xmlChar *) MSS_PROP_TIME);
      seqnum_str = (gchar *) xmlGetProp (iter, (xmlChar *) MSS_PROP_NUMBER);

      /* use the node's seq number or use the previous + 1 */
      if (seqnum_str) {
        fragment->number = g_ascii_strtoull (seqnum_str, NULL, 10);
        xmlFree (seqnum_str);
        fragment_number = fragment->number;
      } else {
        fragment->number = fragment_number;
      }
      fragment_number = fragment->number + 1;

      if (time_str) {
        fragment->time = g_ascii_strtoull (time_str, NULL, 10);

        xmlFree (time_str);
        fragment_time_accum = fragment->time;
      } else {
        fragment->time = fragment_time_accum;
      }

      /* if we have a previous fragment, means we need to set its duration */
      if (previous_fragment)
        previous_fragment->duration = fragment->time - previous_fragment->time;

      if (duration_str) {
        fragment->duration = g_ascii_strtoull (duration_str, NULL, 10);

        previous_fragment = NULL;
        fragment_time_accum += fragment->duration;
        xmlFree (duration_str);
      } else {
        /* store to set the duration at the next iteration */
        previous_fragment = fragment;
      }

      /* we reverse it later */
      stream->fragments = g_list_prepend (stream->fragments, fragment);
    } else if (node_has_type (iter, MSS_NODE_STREAM_QUALITY)) {
      GstMssStreamQuality *quality = gst_mss_stream_quality_new (iter);
      stream->qualities = g_list_prepend (stream->qualities, quality);
    } else {
      /* TODO gst log this */
    }
  }

  stream->fragments = g_list_reverse (stream->fragments);

  /* order them from smaller to bigger based on bitrates */
  stream->qualities =
      g_list_sort (stream->qualities, (GCompareFunc) compare_bitrate);

  stream->current_fragment = stream->fragments;
  stream->current_quality = stream->qualities;

  stream->regex_bitrate = g_regex_new ("\\{[Bb]itrate\\}", 0, 0, NULL);
  stream->regex_position = g_regex_new ("\\{start[ _]time\\}", 0, 0, NULL);
}

GstMssManifest *
gst_mss_manifest_new (const GstBuffer * data)
{
  GstMssManifest *manifest;
  xmlNodePtr root;
  xmlNodePtr nodeiter;
  gchar *live_str;

  manifest = g_malloc0 (sizeof (GstMssManifest));

  manifest->xml = xmlReadMemory ((const gchar *) GST_BUFFER_DATA (data),
      GST_BUFFER_SIZE (data), "manifest", NULL, 0);
  root = manifest->xmlrootnode = xmlDocGetRootElement (manifest->xml);

  live_str = (gchar *) xmlGetProp (root, (xmlChar *) "IsLive");
  if (live_str) {
    manifest->is_live = g_ascii_strcasecmp (live_str, "true") == 0;
    xmlFree (live_str);
  }

  for (nodeiter = root->children; nodeiter; nodeiter = nodeiter->next) {
    if (nodeiter->type == XML_ELEMENT_NODE
        && (strcmp ((const char *) nodeiter->name, "StreamIndex") == 0)) {
      GstMssStream *stream = g_new0 (GstMssStream, 1);

      manifest->streams = g_slist_append (manifest->streams, stream);
      _gst_mss_stream_init (stream, nodeiter);
    }
  }

  return manifest;
}

static void
gst_mss_stream_free (GstMssStream * stream)
{
  g_list_free_full (stream->fragments, g_free);
  g_list_free_full (stream->qualities,
      (GDestroyNotify) gst_mss_stream_quality_free);
  xmlFree (stream->url);
  g_regex_unref (stream->regex_position);
  g_regex_unref (stream->regex_bitrate);
  g_free (stream);
}

void
gst_mss_manifest_free (GstMssManifest * manifest)
{
  g_return_if_fail (manifest != NULL);

  g_slist_free_full (manifest->streams, (GDestroyNotify) gst_mss_stream_free);

  xmlFreeDoc (manifest->xml);
  g_free (manifest);
}

GSList *
gst_mss_manifest_get_streams (GstMssManifest * manifest)
{
  return manifest->streams;
}

GstMssStreamType
gst_mss_stream_get_type (GstMssStream * stream)
{
  gchar *prop = (gchar *) xmlGetProp (stream->xmlnode, (xmlChar *) "Type");
  GstMssStreamType ret = MSS_STREAM_TYPE_UNKNOWN;

  if (prop == NULL)
    return MSS_STREAM_TYPE_UNKNOWN;

  if (strcmp (prop, "video") == 0) {
    ret = MSS_STREAM_TYPE_VIDEO;
  } else if (strcmp (prop, "audio") == 0) {
    ret = MSS_STREAM_TYPE_AUDIO;
  }
  xmlFree (prop);
  return ret;
}

static GstCaps *
_gst_mss_stream_video_caps_from_fourcc (gchar * fourcc)
{
  if (!fourcc)
    return NULL;

  if (strcmp (fourcc, "H264") == 0 || strcmp (fourcc, "AVC1") == 0) {
    return gst_caps_new_simple ("video/x-h264", "stream-format", G_TYPE_STRING,
        "avc", NULL);
  } else if (strcmp (fourcc, "WVC1") == 0) {
    return gst_caps_new_simple ("video/x-wmv", "wmvversion", G_TYPE_INT, 3,
        NULL);
  }
  return NULL;
}

static GstCaps *
_gst_mss_stream_audio_caps_from_fourcc (gchar * fourcc)
{
  if (!fourcc)
    return NULL;

  if (strcmp (fourcc, "AACL") == 0) {
    return gst_caps_new_simple ("audio/mpeg", "mpegversion", G_TYPE_INT, 4,
        NULL);
  } else if (strcmp (fourcc, "WmaPro") == 0) {
    return gst_caps_new_simple ("audio/x-wma", "wmaversion", G_TYPE_INT, 2,
        NULL);
  }
  return NULL;
}

/* copied and adapted from h264parse */
static GstBuffer *
_make_h264_codec_data (GstBuffer * sps, GstBuffer * pps)
{
  GstBuffer *buf;
  gint sps_size = 0, pps_size = 0, num_sps = 0, num_pps = 0;
  guint8 profile_idc = 0, profile_comp = 0, level_idc = 0;
  guint8 *data;
  gint nl;

  if (GST_BUFFER_SIZE (sps) < 4)
    return NULL;

  sps_size += GST_BUFFER_SIZE (sps) + 2;
  profile_idc = GST_BUFFER_DATA (sps)[1];
  profile_comp = GST_BUFFER_DATA (sps)[2];
  level_idc = GST_BUFFER_DATA (sps)[3];
  num_sps = 1;

  pps_size += GST_BUFFER_SIZE (pps) + 2;
  num_pps = 1;

  buf = gst_buffer_new_and_alloc (5 + 1 + sps_size + 1 + pps_size);
  data = GST_BUFFER_DATA (buf);
  nl = 4;

  data[0] = 1;                  /* AVC Decoder Configuration Record ver. 1 */
  data[1] = profile_idc;        /* profile_idc                             */
  data[2] = profile_comp;       /* profile_compability                     */
  data[3] = level_idc;          /* level_idc                               */
  data[4] = 0xfc | (nl - 1);    /* nal_length_size_minus1                  */
  data[5] = 0xe0 | num_sps;     /* number of SPSs */

  data += 6;
  GST_WRITE_UINT16_BE (data, GST_BUFFER_SIZE (sps));
  memcpy (data + 2, GST_BUFFER_DATA (sps), GST_BUFFER_SIZE (sps));
  data += 2 + GST_BUFFER_SIZE (sps);

  data[0] = num_pps;
  data++;
  GST_WRITE_UINT16_BE (data, GST_BUFFER_SIZE (pps));
  memcpy (data + 2, GST_BUFFER_DATA (pps), GST_BUFFER_SIZE (pps));
  data += 2 + GST_BUFFER_SIZE (pps);

  return buf;
}

static void
_gst_mss_stream_add_h264_codec_data (GstCaps * caps, const gchar * codecdatastr)
{
  GValue sps_value = { 0, };
  GValue pps_value = { 0, };
  GstBuffer *sps;
  GstBuffer *pps;
  GstBuffer *buffer;
  gchar *sps_str;
  gchar *pps_str;
  GstH264NalUnit nalu;
  GstH264SPS sps_struct;
  GstH264ParserResult parseres;

  /* search for the sps start */
  if (g_str_has_prefix (codecdatastr, "00000001")) {
    sps_str = (gchar *) codecdatastr + 8;
  } else {
    return;                     /* invalid mss codec data */
  }

  /* search for the pps start */
  pps_str = g_strstr_len (sps_str, -1, "00000001");
  if (!pps_str) {
    return;                     /* invalid mss codec data */
  }

  g_value_init (&sps_value, GST_TYPE_BUFFER);
  pps_str[0] = '\0';
  gst_value_deserialize (&sps_value, sps_str);
  pps_str[0] = '0';

  g_value_init (&pps_value, GST_TYPE_BUFFER);
  pps_str = pps_str + 8;
  gst_value_deserialize (&pps_value, pps_str);

  sps = gst_value_get_buffer (&sps_value);
  pps = gst_value_get_buffer (&pps_value);

  nalu.ref_idc = (GST_BUFFER_DATA (sps)[0] & 0x60) >> 5;
  nalu.type = GST_H264_NAL_SPS;
  nalu.size = GST_BUFFER_SIZE (sps);
  nalu.data = GST_BUFFER_DATA (sps);
  nalu.offset = 0;
  nalu.sc_offset = 0;
  nalu.valid = TRUE;

  parseres = gst_h264_parse_sps (&nalu, &sps_struct, TRUE);
  if (parseres == GST_H264_PARSER_OK) {
    gst_caps_set_simple (caps, "framerate", GST_TYPE_FRACTION,
        sps_struct.fps_num, sps_struct.fps_den, NULL);
  }

  buffer = _make_h264_codec_data (sps, pps);
  g_value_reset (&sps_value);
  g_value_reset (&pps_value);

  if (buffer != NULL) {
    gst_caps_set_simple (caps, "codec_data", GST_TYPE_BUFFER, buffer, NULL);
    gst_buffer_unref (buffer);
  }
}

static GstCaps *
_gst_mss_stream_video_caps_from_qualitylevel_xml (xmlNodePtr node)
{
  GstCaps *caps;
  GstStructure *structure;
  gchar *fourcc = (gchar *) xmlGetProp (node, (xmlChar *) "FourCC");
  gchar *max_width = (gchar *) xmlGetProp (node, (xmlChar *) "MaxWidth");
  gchar *max_height = (gchar *) xmlGetProp (node, (xmlChar *) "MaxHeight");
  gchar *codec_data =
      (gchar *) xmlGetProp (node, (xmlChar *) "CodecPrivateData");

  if (!max_width)
    max_width = (gchar *) xmlGetProp (node, (xmlChar *) "Width");
  if (!max_height)
    max_height = (gchar *) xmlGetProp (node, (xmlChar *) "Height");

  caps = _gst_mss_stream_video_caps_from_fourcc (fourcc);
  if (!caps)
    goto end;

  structure = gst_caps_get_structure (caps, 0);

  if (max_width)
    gst_structure_set (structure, "width", G_TYPE_INT,
        g_ascii_strtoull (max_width, NULL, 10), NULL);
  if (max_height)
    gst_structure_set (structure, "height", G_TYPE_INT,
        g_ascii_strtoull (max_height, NULL, 10), NULL);

  if (codec_data && strlen (codec_data)) {
    if (strcmp (fourcc, "H264") == 0 || strcmp (fourcc, "AVC1") == 0) {
      _gst_mss_stream_add_h264_codec_data (caps, codec_data);
    } else {
      GValue *value = g_new0 (GValue, 1);
      g_value_init (value, GST_TYPE_BUFFER);
      gst_value_deserialize (value, (gchar *) codec_data);
      gst_structure_take_value (structure, "codec_data", value);
    }
  }

end:
  xmlFree (fourcc);
  xmlFree (max_width);
  xmlFree (max_height);
  xmlFree (codec_data);

  return caps;
}

static guint8
_frequency_index_from_sampling_rate (guint sampling_rate)
{
  static const guint aac_sample_rates[] = { 96000, 88200, 64000, 48000, 44100,
    32000, 24000, 22050, 16000, 12000, 11025, 8000, 7350
  };

  guint8 i;

  for (i = 0; i < G_N_ELEMENTS (aac_sample_rates); i++) {
    if (aac_sample_rates[i] == sampling_rate)
      return i;
  }
  return 15;
}

static GstBuffer *
_make_aacl_codec_data (guint64 sampling_rate, guint64 channels)
{
  GstBuffer *buf;
  guint8 *data;
  guint8 frequency_index;
  guint8 buf_size;

  buf_size = 2;
  frequency_index = _frequency_index_from_sampling_rate (sampling_rate);
  if (frequency_index == 15)
    buf_size += 3;

  buf = gst_buffer_new_and_alloc (buf_size);
  data = GST_BUFFER_DATA (buf);

  data[0] = 2 << 3;             /* AAC-LC object type is 2 */
  data[0] += frequency_index >> 1;
  data[1] = (frequency_index & 0x01) << 7;

  /* Sampling rate is not in frequencies table, write manually */
  if (frequency_index == 15) {
    data[1] += sampling_rate >> 17;
    data[2] = (sampling_rate >> 9) & 0xFF;
    data[3] = (sampling_rate >> 1) & 0xFF;
    data[4] = sampling_rate & 0x01;
    data += 3;
  }

  data[1] += (channels & 0x0F) << 3;

  return buf;
}

static GstCaps *
_gst_mss_stream_audio_caps_from_qualitylevel_xml (xmlNodePtr node)
{
  GstCaps *caps;
  GstStructure *structure;
  gchar *fourcc = (gchar *) xmlGetProp (node, (xmlChar *) "FourCC");
  gchar *channels = (gchar *) xmlGetProp (node, (xmlChar *) "Channels");
  gchar *rate = (gchar *) xmlGetProp (node, (xmlChar *) "SamplingRate");
  gchar *codec_data =
      (gchar *) xmlGetProp (node, (xmlChar *) "CodecPrivateData");

  if (!fourcc)                  /* sometimes the fourcc is omitted, we fallback to the Subtype in the StreamIndex node */
    fourcc = (gchar *) xmlGetProp (node->parent, (xmlChar *) "Subtype");
  if (!codec_data)
    codec_data = (gchar *) xmlGetProp (node, (xmlChar *) "WaveFormatEx");

  caps = _gst_mss_stream_audio_caps_from_fourcc (fourcc);
  if (!caps)
    goto end;

  structure = gst_caps_get_structure (caps, 0);

  if (channels)
    gst_structure_set (structure, "channels", G_TYPE_INT,
        g_ascii_strtoull (channels, NULL, 10), NULL);
  if (rate)
    gst_structure_set (structure, "rate", G_TYPE_INT,
        g_ascii_strtoull (rate, NULL, 10), NULL);

  if (codec_data && strlen (codec_data)) {
    GValue *value = g_new0 (GValue, 1);
    g_value_init (value, GST_TYPE_BUFFER);
    gst_value_deserialize (value, (gchar *) codec_data);
    gst_structure_take_value (structure, "codec_data", value);
  } else if (strcmp (fourcc, "AACL") == 0 && rate && channels) {
    GstBuffer *buffer =
        _make_aacl_codec_data (g_ascii_strtoull (rate, NULL, 10),
        g_ascii_strtoull (channels, NULL, 10));
    gst_caps_set_simple (caps, "codec_data", GST_TYPE_BUFFER, buffer, NULL);
    gst_buffer_unref (buffer);
  }

end:
  xmlFree (fourcc);
  xmlFree (channels);
  xmlFree (rate);
  xmlFree (codec_data);

  return caps;
}

void
gst_mss_stream_set_active (GstMssStream * stream, gboolean active)
{
  stream->active = active;
}

guint64
gst_mss_stream_get_timescale (GstMssStream * stream)
{
  gchar *timescale;
  guint64 ts = DEFAULT_TIMESCALE;

  timescale =
      (gchar *) xmlGetProp (stream->xmlnode, (xmlChar *) MSS_PROP_TIMESCALE);
  if (!timescale) {
    timescale =
        (gchar *) xmlGetProp (stream->xmlnode->parent,
        (xmlChar *) MSS_PROP_TIMESCALE);
  }

  if (timescale) {
    ts = g_ascii_strtoull (timescale, NULL, 10);
    xmlFree (timescale);
  }
  return ts;
}

guint64
gst_mss_manifest_get_timescale (GstMssManifest * manifest)
{
  gchar *timescale;
  guint64 ts = DEFAULT_TIMESCALE;

  timescale =
      (gchar *) xmlGetProp (manifest->xmlrootnode,
      (xmlChar *) MSS_PROP_TIMESCALE);
  if (timescale) {
    ts = g_ascii_strtoull (timescale, NULL, 10);
    xmlFree (timescale);
  }
  return ts;
}

guint64
gst_mss_manifest_get_duration (GstMssManifest * manifest)
{
  gchar *duration;
  guint64 dur = -1;

  duration =
      (gchar *) xmlGetProp (manifest->xmlrootnode,
      (xmlChar *) MSS_PROP_STREAM_DURATION);
  if (duration) {
    dur = g_ascii_strtoull (duration, NULL, 10);
    xmlFree (duration);
  }
  return dur;
}


/**
 * Gets the duration in nanoseconds
 */
GstClockTime
gst_mss_manifest_get_gst_duration (GstMssManifest * manifest)
{
  guint64 duration = -1;
  guint64 timescale;
  GstClockTime gstdur = GST_CLOCK_TIME_NONE;

  duration = gst_mss_manifest_get_duration (manifest);
  timescale = gst_mss_manifest_get_timescale (manifest);

  if (duration != -1 && timescale != -1)
    gstdur =
        (GstClockTime) gst_util_uint64_scale_round (duration, GST_SECOND,
        timescale);

  return gstdur;
}

GstCaps *
gst_mss_stream_get_caps (GstMssStream * stream)
{
  GstMssStreamType streamtype = gst_mss_stream_get_type (stream);
  GstMssStreamQuality *qualitylevel = stream->current_quality->data;
  GstCaps *caps = NULL;

  if (streamtype == MSS_STREAM_TYPE_VIDEO)
    caps =
        _gst_mss_stream_video_caps_from_qualitylevel_xml
        (qualitylevel->xmlnode);
  else if (streamtype == MSS_STREAM_TYPE_AUDIO)
    caps =
        _gst_mss_stream_audio_caps_from_qualitylevel_xml
        (qualitylevel->xmlnode);

  return caps;
}

GstFlowReturn
gst_mss_stream_get_fragment_url (GstMssStream * stream, gchar ** url)
{
  gchar *tmp;
  gchar *start_time_str;
  GstMssStreamFragment *fragment;
  GstMssStreamQuality *quality = stream->current_quality->data;

  g_return_val_if_fail (stream->active, GST_FLOW_ERROR);

  if (stream->current_fragment == NULL) /* stream is over */
    return GST_FLOW_UNEXPECTED;

  fragment = stream->current_fragment->data;

  start_time_str = g_strdup_printf ("%" G_GUINT64_FORMAT, fragment->time);

  tmp = g_regex_replace_literal (stream->regex_bitrate, stream->url,
      strlen (stream->url), 0, quality->bitrate_str, 0, NULL);
  *url = g_regex_replace_literal (stream->regex_position, tmp,
      strlen (tmp), 0, start_time_str, 0, NULL);

  g_free (tmp);
  g_free (start_time_str);

  if (*url == NULL)
    return GST_FLOW_ERROR;

  return GST_FLOW_OK;
}

GstClockTime
gst_mss_stream_get_fragment_gst_timestamp (GstMssStream * stream)
{
  guint64 time;
  guint64 timescale;
  GstMssStreamFragment *fragment;

  g_return_val_if_fail (stream->active, GST_FLOW_ERROR);

  if (!stream->current_fragment)
    return GST_CLOCK_TIME_NONE;

  fragment = stream->current_fragment->data;

  time = fragment->time;
  timescale = gst_mss_stream_get_timescale (stream);
  return (GstClockTime) gst_util_uint64_scale_round (time, GST_SECOND,
      timescale);
}

GstClockTime
gst_mss_stream_get_fragment_gst_duration (GstMssStream * stream)
{
  guint64 dur;
  guint64 timescale;
  GstMssStreamFragment *fragment;

  g_return_val_if_fail (stream->active, GST_FLOW_ERROR);

  if (!stream->current_fragment)
    return GST_CLOCK_TIME_NONE;

  fragment = stream->current_fragment->data;

  dur = fragment->duration;
  timescale = gst_mss_stream_get_timescale (stream);
  return (GstClockTime) gst_util_uint64_scale_round (dur, GST_SECOND,
      timescale);
}

GstFlowReturn
gst_mss_stream_advance_fragment (GstMssStream * stream)
{
  g_return_val_if_fail (stream->active, GST_FLOW_ERROR);

  if (stream->current_fragment == NULL)
    return GST_FLOW_UNEXPECTED;

  stream->current_fragment = g_list_next (stream->current_fragment);
  if (stream->current_fragment == NULL)
    return GST_FLOW_UNEXPECTED;
  return GST_FLOW_OK;
}

const gchar *
gst_mss_stream_type_name (GstMssStreamType streamtype)
{
  switch (streamtype) {
    case MSS_STREAM_TYPE_VIDEO:
      return "video";
    case MSS_STREAM_TYPE_AUDIO:
      return "audio";
    case MSS_STREAM_TYPE_UNKNOWN:
    default:
      return "unknown";
  }
}

/**
 * Seeks all streams to the fragment that contains the set time
 *
 * @time: time in nanoseconds
 */
gboolean
gst_mss_manifest_seek (GstMssManifest * manifest, guint64 time)
{
  gboolean ret = TRUE;
  GSList *iter;

  for (iter = manifest->streams; iter; iter = g_slist_next (iter)) {
    ret = gst_mss_stream_seek (iter->data, time) & ret;
  }

  return ret;
}

/**
 * Seeks this stream to the fragment that contains the sample at time
 *
 * @time: time in nanoseconds
 */
gboolean
gst_mss_stream_seek (GstMssStream * stream, guint64 time)
{
  GList *iter;
  guint64 timescale;

  timescale = gst_mss_stream_get_timescale (stream);
  time = gst_util_uint64_scale_round (time, timescale, GST_SECOND);

  for (iter = stream->fragments; iter; iter = g_list_next (iter)) {
    GList *next = g_list_next (iter);
    if (next) {
      GstMssStreamFragment *fragment = next->data;

      if (fragment->time > time) {
        stream->current_fragment = iter;
        break;
      }
    } else {
      GstMssStreamFragment *fragment = iter->data;
      if (fragment->time + fragment->duration > time) {
        stream->current_fragment = iter;
      } else {
        stream->current_fragment = NULL;        /* EOS */
      }
      break;
    }
  }

  return TRUE;
}

guint64
gst_mss_manifest_get_current_bitrate (GstMssManifest * manifest)
{
  guint64 bitrate = 0;
  GSList *iter;

  for (iter = gst_mss_manifest_get_streams (manifest); iter;
      iter = g_slist_next (iter)) {
    GstMssStream *stream = iter->data;
    if (stream->active && stream->current_quality) {
      GstMssStreamQuality *q = stream->current_quality->data;

      bitrate += q->bitrate;
    }
  }

  return bitrate;
}

gboolean
gst_mss_manifest_is_live (GstMssManifest * manifest)
{
  return manifest->is_live;
}

static void
gst_mss_stream_reload_fragments (GstMssStream * stream, xmlNodePtr streamIndex)
{
  xmlNodePtr iter;
  GList *new_fragments = NULL;
  GstMssStreamFragment *previous_fragment = NULL;
  GstMssStreamFragment *current_fragment =
      stream->current_fragment ? stream->current_fragment->data : NULL;
  guint64 current_time = gst_mss_stream_get_fragment_gst_timestamp (stream);
  guint fragment_number = 0;
  guint64 fragment_time_accum = 0;

  if (!current_fragment && stream->fragments) {
    current_fragment = g_list_last (stream->fragments)->data;
  } else if (g_list_previous (stream->current_fragment)) {
    /* rewind one as this is the next to be pushed */
    current_fragment = g_list_previous (stream->current_fragment)->data;
  } else {
    current_fragment = NULL;
  }

  if (current_fragment) {
    current_time = current_fragment->time;
    fragment_number = current_fragment->number;
    fragment_time_accum = current_fragment->time;
  }

  for (iter = streamIndex->children; iter; iter = iter->next) {
    if (node_has_type (iter, MSS_NODE_STREAM_FRAGMENT)) {
      gchar *duration_str;
      gchar *time_str;
      gchar *seqnum_str;
      GstMssStreamFragment *fragment = g_new (GstMssStreamFragment, 1);

      duration_str = (gchar *) xmlGetProp (iter, (xmlChar *) MSS_PROP_DURATION);
      time_str = (gchar *) xmlGetProp (iter, (xmlChar *) MSS_PROP_TIME);
      seqnum_str = (gchar *) xmlGetProp (iter, (xmlChar *) MSS_PROP_NUMBER);

      /* use the node's seq number or use the previous + 1 */
      if (seqnum_str) {
        fragment->number = g_ascii_strtoull (seqnum_str, NULL, 10);
        xmlFree (seqnum_str);
      } else {
        fragment->number = fragment_number;
      }
      fragment_number = fragment->number + 1;

      if (time_str) {
        fragment->time = g_ascii_strtoull (time_str, NULL, 10);
        xmlFree (time_str);
        fragment_time_accum = fragment->time;
      } else {
        fragment->time = fragment_time_accum;
      }

      /* if we have a previous fragment, means we need to set its duration */
      if (previous_fragment)
        previous_fragment->duration = fragment->time - previous_fragment->time;

      if (duration_str) {
        fragment->duration = g_ascii_strtoull (duration_str, NULL, 10);

        previous_fragment = NULL;
        fragment_time_accum += fragment->duration;
        xmlFree (duration_str);
      } else {
        /* store to set the duration at the next iteration */
        previous_fragment = fragment;
      }

      if (fragment->time > current_time) {
        new_fragments = g_list_append (new_fragments, fragment);
      } else {
        previous_fragment = NULL;
        g_free (fragment);
      }

    } else {
      /* TODO gst log this */
    }
  }

  /* store the new fragments list */
  if (new_fragments) {
    g_list_free_full (stream->fragments, g_free);
    stream->fragments = new_fragments;
    stream->current_fragment = new_fragments;
  }
}

static void
gst_mss_manifest_reload_fragments_from_xml (GstMssManifest * manifest,
    xmlNodePtr root)
{
  xmlNodePtr nodeiter;
  GSList *streams = manifest->streams;

  /* we assume the server is providing the streams in the same order in
   * every manifest */
  for (nodeiter = root->children; nodeiter && streams;
      nodeiter = nodeiter->next) {
    if (nodeiter->type == XML_ELEMENT_NODE
        && (strcmp ((const char *) nodeiter->name, "StreamIndex") == 0)) {
      gst_mss_stream_reload_fragments (streams->data, nodeiter);
      streams = g_slist_next (streams);
    }
  }
}

void
gst_mss_manifest_reload_fragments (GstMssManifest * manifest, GstBuffer * data)
{
  xmlDocPtr xml;
  xmlNodePtr root;

  g_return_if_fail (manifest->is_live);

  xml = xmlReadMemory ((const gchar *) GST_BUFFER_DATA (data),
      GST_BUFFER_SIZE (data), "manifest", NULL, 0);
  root = xmlDocGetRootElement (xml);

  gst_mss_manifest_reload_fragments_from_xml (manifest, root);

  xmlFreeDoc (xml);
}

static gboolean
gst_mss_stream_select_bitrate (GstMssStream * stream, guint64 bitrate)
{
  GList *iter = stream->current_quality;
  GList *next;
  GstMssStreamQuality *q = iter->data;

  while (q->bitrate > bitrate) {
    next = g_list_previous (iter);
    if (next) {
      iter = next;
      q = iter->data;
    } else {
      break;
    }
  }

  while (q->bitrate < bitrate) {
    GstMssStreamQuality *next_q;
    next = g_list_next (iter);
    if (next) {
      next_q = next->data;
      if (next_q->bitrate < bitrate) {
        iter = next;
        q = iter->data;
      } else {
        break;
      }
    } else {
      break;
    }
  }

  if (iter == stream->current_quality)
    return FALSE;
  stream->current_quality = iter;
  return TRUE;
}

/**
 * gst_mss_manifest_change_bitrate:
 * @manifest: the manifest
 * @bitrate: the maximum bitrate to use (bps)
 *
 * Iterates over the active streams and changes their bitrates to the maximum
 * value so that the bitrates of all streams are not larger than
 * @bitrate.
 *
 * Return: %TRUE if any stream changed its bitrate
 */
gboolean
gst_mss_manifest_change_bitrate (GstMssManifest * manifest, guint64 bitrate)
{
  gboolean ret = FALSE;
  GSList *iter;

  /* TODO This algorithm currently sets the same bitrate for all streams,
   * it should actually use the sum of all streams bitrates to compare to
   * the target value */

  if (bitrate == 0) {
    /* use maximum */
    bitrate = G_MAXUINT64;
  }

  for (iter = gst_mss_manifest_get_streams (manifest); iter;
      iter = g_slist_next (iter)) {
    GstMssStream *stream = iter->data;
    if (stream->active) {
      ret = ret | gst_mss_stream_select_bitrate (stream, bitrate);
    }
  }

  return ret;
}
