/* GStreamer
 * Copyright (C) 2012 Smart TV Alliance
 *  Author: Thiago Sousa Santos <thiago.sousa.santos@collabora.com>, Collabora Ltd.
 *
 * gstmssdemux.c:
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

/**
 * SECTION:element-mssdemux
 *
 * Demuxes a Microsoft's Smooth Streaming manifest into its audio and/or video streams.
 *
 * TODO
 */

#ifdef HAVE_CONFIG_H
#include "config.h"
#endif

#include "gst/gst-i18n-plugin.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "gstmssdemux.h"

GST_DEBUG_CATEGORY (mssdemux_debug);

#define DEFAULT_CONNECTION_SPEED 0

enum
{
  PROP_0,

  PROP_CONNECTION_SPEED,
  PROP_LAST
};

static GstStaticPadTemplate gst_mss_demux_sink_template =
GST_STATIC_PAD_TEMPLATE ("sink",
    GST_PAD_SINK,
    GST_PAD_ALWAYS,
    GST_STATIC_CAPS ("application/vnd.ms-sstr+xml")
    );

static GstStaticPadTemplate gst_mss_demux_videosrc_template =
GST_STATIC_PAD_TEMPLATE ("video_%02u",
    GST_PAD_SRC,
    GST_PAD_SOMETIMES,
    GST_STATIC_CAPS_ANY);

static GstStaticPadTemplate gst_mss_demux_audiosrc_template =
GST_STATIC_PAD_TEMPLATE ("audio_%02u",
    GST_PAD_SRC,
    GST_PAD_SOMETIMES,
    GST_STATIC_CAPS_ANY);

GST_BOILERPLATE (GstMssDemux, gst_mss_demux, GstMssDemux, GST_TYPE_ELEMENT);

static void gst_mss_demux_dispose (GObject * object);
static void gst_mss_demux_set_property (GObject * object, guint prop_id,
    const GValue * value, GParamSpec * pspec);
static void gst_mss_demux_get_property (GObject * object, guint prop_id,
    GValue * value, GParamSpec * pspec);
static GstStateChangeReturn gst_mss_demux_change_state (GstElement * element,
    GstStateChange transition);
static GstFlowReturn gst_mss_demux_chain (GstPad * pad, GstBuffer * buffer);
static GstFlowReturn gst_mss_demux_event (GstPad * pad, GstEvent * event);

static gboolean gst_mss_demux_src_query (GstPad * pad, GstQuery * query);

static void gst_mss_demux_stream_loop (GstMssDemuxStream * stream);

static void gst_mss_demux_process_manifest (GstMssDemux * mssdemux);

static void
gst_mss_demux_base_init (gpointer klass)
{
  GstElementClass *element_class = GST_ELEMENT_CLASS (klass);

  gst_element_class_add_static_pad_template (element_class,
      &gst_mss_demux_sink_template);
  gst_element_class_add_static_pad_template (element_class,
      &gst_mss_demux_videosrc_template);
  gst_element_class_add_static_pad_template (element_class,
      &gst_mss_demux_audiosrc_template);
  gst_element_class_set_details_simple (element_class, "Smooth Streaming "
      "demuxer", "Demuxer",
      "Parse and demultiplex a Smooth Streaming manifest into audio and video "
      "streams", "Thiago Santos <thiago.sousa.santos@collabora.com>");

  GST_DEBUG_CATEGORY_INIT (mssdemux_debug, "mssdemux", 0, "mssdemux plugin");
}

static void
gst_mss_demux_class_init (GstMssDemuxClass * klass)
{
  GObjectClass *gobject_class;
  GstElementClass *gstelement_class;

  gobject_class = (GObjectClass *) klass;
  gstelement_class = (GstElementClass *) klass;

  parent_class = g_type_class_peek_parent (klass);

  gobject_class->dispose = gst_mss_demux_dispose;
  gobject_class->set_property = gst_mss_demux_set_property;
  gobject_class->get_property = gst_mss_demux_get_property;

  g_object_class_install_property (gobject_class, PROP_CONNECTION_SPEED,
      g_param_spec_uint ("connection-speed", "Connection Speed",
          "Network connection speed in kbps (0 = unknown)",
          0, G_MAXUINT / 1000, DEFAULT_CONNECTION_SPEED,
          G_PARAM_READWRITE | G_PARAM_STATIC_STRINGS));

  gstelement_class->change_state =
      GST_DEBUG_FUNCPTR (gst_mss_demux_change_state);
}

static void
gst_mss_demux_init (GstMssDemux * mssdemux, GstMssDemuxClass * klass)
{
  mssdemux->sinkpad =
      gst_pad_new_from_static_template (&gst_mss_demux_sink_template, "sink");
  gst_pad_set_chain_function (mssdemux->sinkpad,
      GST_DEBUG_FUNCPTR (gst_mss_demux_chain));
  gst_pad_set_event_function (mssdemux->sinkpad,
      GST_DEBUG_FUNCPTR (gst_mss_demux_event));
  gst_element_add_pad (GST_ELEMENT_CAST (mssdemux), mssdemux->sinkpad);
}

static GstMssDemuxStream *
gst_mss_demux_stream_new (GstMssDemux * mssdemux,
    GstMssStream * manifeststream, GstPad * srcpad)
{
  GstMssDemuxStream *stream;

  stream = g_new0 (GstMssDemuxStream, 1);
  stream->downloader = gst_uri_downloader_new ();

  /* Streaming task */
  g_static_rec_mutex_init (&stream->stream_lock);
  stream->stream_task =
      gst_task_create ((GstTaskFunction) gst_mss_demux_stream_loop, stream);
  gst_task_set_lock (stream->stream_task, &stream->stream_lock);

  stream->pad = srcpad;
  stream->manifest_stream = manifeststream;
  stream->parent = mssdemux;

  return stream;
}

static void
gst_mss_demux_stream_free (GstMssDemuxStream * stream)
{
  if (stream->stream_task) {
    if (GST_TASK_STATE (stream->stream_task) != GST_TASK_STOPPED) {
      GST_DEBUG_OBJECT (stream->parent, "Leaving streaming task %s:%s",
          GST_DEBUG_PAD_NAME (stream->pad));
      gst_task_stop (stream->stream_task);
      g_static_rec_mutex_lock (&stream->stream_lock);
      g_static_rec_mutex_unlock (&stream->stream_lock);
      GST_LOG_OBJECT (stream->parent, "Waiting for task to finish");
      gst_task_join (stream->stream_task);
      GST_LOG_OBJECT (stream->parent, "Finished");
    }
    gst_object_unref (stream->stream_task);
    g_static_rec_mutex_free (&stream->stream_lock);
    stream->stream_task = NULL;
  }

  if (stream->pending_newsegment) {
    gst_event_unref (stream->pending_newsegment);
    stream->pending_newsegment = NULL;
  }


  if (stream->downloader != NULL) {
    g_object_unref (stream->downloader);
    stream->downloader = NULL;
  }
  if (stream->pad) {
    gst_object_unref (stream->pad);
    stream->pad = NULL;
  }
  g_free (stream);
}

static void
gst_mss_demux_reset (GstMssDemux * mssdemux)
{
  GSList *iter;
  if (mssdemux->manifest_buffer) {
    gst_buffer_unref (mssdemux->manifest_buffer);
    mssdemux->manifest_buffer = NULL;
  }

  for (iter = mssdemux->streams; iter; iter = g_slist_next (iter)) {
    GstMssDemuxStream *stream = iter->data;
    gst_element_remove_pad (GST_ELEMENT_CAST (mssdemux), stream->pad);
    gst_mss_demux_stream_free (stream);
  }
  g_slist_free (mssdemux->streams);
  mssdemux->streams = NULL;

  if (mssdemux->manifest) {
    gst_mss_manifest_free (mssdemux->manifest);
    mssdemux->manifest = NULL;
  }

  mssdemux->n_videos = mssdemux->n_audios = 0;
  g_free (mssdemux->base_url);
  mssdemux->base_url = NULL;
}

static void
gst_mss_demux_dispose (GObject * object)
{
  /* GstMssDemux *mssdemux = GST_MSS_DEMUX_CAST (object); */

  G_OBJECT_CLASS (parent_class)->dispose (object);
}

static void
gst_mss_demux_set_property (GObject * object, guint prop_id,
    const GValue * value, GParamSpec * pspec)
{
  GstMssDemux *mssdemux = GST_MSS_DEMUX (object);

  switch (prop_id) {
    case PROP_CONNECTION_SPEED:
      GST_OBJECT_LOCK (mssdemux);
      mssdemux->connection_speed = g_value_get_uint (value) * 1000;
      mssdemux->update_bitrates = TRUE;
      GST_DEBUG_OBJECT (mssdemux, "Connection speed set to %llu",
          mssdemux->connection_speed);
      GST_OBJECT_UNLOCK (mssdemux);
      break;
    default:
      G_OBJECT_WARN_INVALID_PROPERTY_ID (object, prop_id, pspec);
      break;
  }
}

static void
gst_mss_demux_get_property (GObject * object, guint prop_id, GValue * value,
    GParamSpec * pspec)
{
  GstMssDemux *mssdemux = GST_MSS_DEMUX (object);

  switch (prop_id) {
    case PROP_CONNECTION_SPEED:
      g_value_set_uint (value, mssdemux->connection_speed / 1000);
      break;
    default:
      G_OBJECT_WARN_INVALID_PROPERTY_ID (object, prop_id, pspec);
      break;
  }
}

static GstStateChangeReturn
gst_mss_demux_change_state (GstElement * element, GstStateChange transition)
{
  GstMssDemux *mssdemux = GST_MSS_DEMUX_CAST (element);
  GstStateChangeReturn result = GST_STATE_CHANGE_FAILURE;

  switch (transition) {
    case GST_STATE_CHANGE_PAUSED_TO_READY:
      gst_mss_demux_reset (mssdemux);
      break;
    case GST_STATE_CHANGE_READY_TO_NULL:
      break;
    default:
      break;
  }

  result = GST_ELEMENT_CLASS (parent_class)->change_state (element, transition);

  switch (transition) {
    case GST_STATE_CHANGE_PAUSED_TO_READY:{
      break;
    }
    default:
      break;
  }

  return result;
}

static GstFlowReturn
gst_mss_demux_chain (GstPad * pad, GstBuffer * buffer)
{
  GstMssDemux *mssdemux = GST_MSS_DEMUX_CAST (GST_PAD_PARENT (pad));
  if (mssdemux->manifest_buffer == NULL)
    mssdemux->manifest_buffer = buffer;
  else
    mssdemux->manifest_buffer =
        gst_buffer_join (mssdemux->manifest_buffer, buffer);

  return GST_FLOW_OK;
}

static void
gst_mss_demux_start (GstMssDemux * mssdemux)
{
  GSList *iter;

  GST_INFO_OBJECT (mssdemux, "Starting streams' tasks");
  for (iter = mssdemux->streams; iter; iter = g_slist_next (iter)) {
    GstMssDemuxStream *stream = iter->data;
    gst_task_start (stream->stream_task);
  }
}

static gboolean
gst_mss_demux_push_src_event (GstMssDemux * mssdemux, GstEvent * event)
{
  GSList *iter;
  gboolean ret = TRUE;

  for (iter = mssdemux->streams; iter; iter = g_slist_next (iter)) {
    GstMssDemuxStream *stream = iter->data;
    gst_event_ref (event);
    ret = ret & gst_pad_push_event (stream->pad, event);
  }
  return ret;
}

static gboolean
gst_mss_demux_event (GstPad * pad, GstEvent * event)
{
  GstMssDemux *mssdemux = GST_MSS_DEMUX_CAST (GST_PAD_PARENT (pad));
  gboolean forward = TRUE;
  gboolean ret = TRUE;

  switch (GST_EVENT_TYPE (event)) {
    case GST_EVENT_EOS:
      if (mssdemux->manifest_buffer == NULL) {
        GST_WARNING_OBJECT (mssdemux, "Received EOS without a manifest.");
        break;
      }

      gst_mss_demux_process_manifest (mssdemux);
      gst_mss_demux_start (mssdemux);
      forward = FALSE;
      break;
    default:
      break;
  }

  if (forward) {
    ret = gst_pad_event_default (pad, event);
  } else {
    gst_event_unref (event);
  }

  return ret;
}

static void
gst_mss_demux_stop_tasks (GstMssDemux * mssdemux, gboolean immediate)
{
  GSList *iter;
  for (iter = mssdemux->streams; iter; iter = g_slist_next (iter)) {
    GstMssDemuxStream *stream = iter->data;

    if (immediate)
      gst_uri_downloader_cancel (stream->downloader);
    gst_task_pause (stream->stream_task);
  }
  for (iter = mssdemux->streams; iter; iter = g_slist_next (iter)) {
    GstMssDemuxStream *stream = iter->data;
    g_static_rec_mutex_lock (&stream->stream_lock);
  }
}

static void
gst_mss_demux_restart_tasks (GstMssDemux * mssdemux)
{
  GSList *iter;
  for (iter = mssdemux->streams; iter; iter = g_slist_next (iter)) {
    GstMssDemuxStream *stream = iter->data;
    g_static_rec_mutex_unlock (&stream->stream_lock);
  }
  for (iter = mssdemux->streams; iter; iter = g_slist_next (iter)) {
    GstMssDemuxStream *stream = iter->data;

    gst_task_start (stream->stream_task);
  }
}

static gboolean
gst_mss_demux_src_event (GstPad * pad, GstEvent * event)
{
  GstMssDemux *mssdemux;

  mssdemux = GST_MSS_DEMUX (GST_PAD_PARENT (pad));

  switch (event->type) {
    case GST_EVENT_SEEK:
    {
      gdouble rate;
      GstFormat format;
      GstSeekFlags flags;
      GstSeekType start_type, stop_type;
      gint64 start, stop;
      GstEvent *newsegment;
      GSList *iter;

      GST_INFO_OBJECT (mssdemux, "Received GST_EVENT_SEEK");

      gst_event_parse_seek (event, &rate, &format, &flags, &start_type, &start,
          &stop_type, &stop);

      if (format != GST_FORMAT_TIME)
        return FALSE;

      GST_DEBUG_OBJECT (mssdemux,
          "seek event, rate: %f start: %" GST_TIME_FORMAT " stop: %"
          GST_TIME_FORMAT, rate, GST_TIME_ARGS (start), GST_TIME_ARGS (stop));

      if (flags & GST_SEEK_FLAG_FLUSH) {
        GstEvent *flush = gst_event_new_flush_start ();
        GST_DEBUG_OBJECT (mssdemux, "sending flush start");

        gst_event_set_seqnum (flush, gst_event_get_seqnum (event));
        gst_mss_demux_push_src_event (mssdemux, flush);
        gst_event_unref (flush);
      }

      gst_mss_demux_stop_tasks (mssdemux, TRUE);

      if (!gst_mss_manifest_seek (mssdemux->manifest, start)) {;
        GST_WARNING_OBJECT (mssdemux, "Could not find seeked fragment");
        return FALSE;
      }

      newsegment =
          gst_event_new_new_segment (FALSE, rate, format, start, stop, start);
      gst_event_set_seqnum (newsegment, gst_event_get_seqnum (event));
      for (iter = mssdemux->streams; iter; iter = g_slist_next (iter)) {
        GstMssDemuxStream *stream = iter->data;

        stream->pending_newsegment = gst_event_ref (newsegment);
      }
      gst_event_unref (newsegment);

      if (flags & GST_SEEK_FLAG_FLUSH) {
        GstEvent *flush = gst_event_new_flush_stop ();
        GST_DEBUG_OBJECT (mssdemux, "sending flush stop");

        gst_event_set_seqnum (flush, gst_event_get_seqnum (event));
        gst_mss_demux_push_src_event (mssdemux, flush);
        gst_event_unref (flush);
      }

      gst_mss_demux_restart_tasks (mssdemux);

      return TRUE;
    }
    default:
      break;
  }

  return gst_pad_event_default (pad, event);
}

static gboolean
gst_mss_demux_src_query (GstPad * pad, GstQuery * query)
{
  GstMssDemux *mssdemux;
  gboolean ret = FALSE;

  if (query == NULL)
    return FALSE;

  mssdemux = GST_MSS_DEMUX (GST_PAD_PARENT (pad));

  switch (query->type) {
    case GST_QUERY_DURATION:{
      GstClockTime duration = -1;
      GstFormat fmt;

      gst_query_parse_duration (query, &fmt, NULL);
      if (fmt == GST_FORMAT_TIME && mssdemux->manifest) {
        /* TODO should we use the streams accumulated duration or the main manifest duration? */
        duration = gst_mss_manifest_get_gst_duration (mssdemux->manifest);

        if (GST_CLOCK_TIME_IS_VALID (duration) && duration > 0) {
          gst_query_set_duration (query, GST_FORMAT_TIME, duration);
          ret = TRUE;
        }
      }
      GST_INFO_OBJECT (mssdemux, "GST_QUERY_DURATION returns %s with duration %"
          GST_TIME_FORMAT, ret ? "TRUE" : "FALSE", GST_TIME_ARGS (duration));
      break;
    }
    case GST_QUERY_LATENCY:
      gst_query_set_latency (query, FALSE, 0, -1);
      ret = TRUE;
      break;
    case GST_QUERY_SEEKING:{
      GstFormat fmt;
      gint64 stop = -1;

      gst_query_parse_seeking (query, &fmt, NULL, NULL, NULL);
      GST_INFO_OBJECT (mssdemux, "Received GST_QUERY_SEEKING with format %d",
          fmt);
      if (fmt == GST_FORMAT_TIME) {
        GstClockTime duration;
        duration = gst_mss_manifest_get_gst_duration (mssdemux->manifest);
        if (GST_CLOCK_TIME_IS_VALID (duration) && duration > 0)
          stop = duration;
        gst_query_set_seeking (query, fmt, TRUE, 0, stop);
        ret = TRUE;
        GST_INFO_OBJECT (mssdemux, "GST_QUERY_SEEKING returning with stop : %"
            GST_TIME_FORMAT, GST_TIME_ARGS (stop));
      }
      break;
    }
    default:
      /* Don't fordward queries upstream because of the special nature of this
       *  "demuxer", which relies on the upstream element only to be fed
       *  the Manifest
       */
      break;
  }

  return ret;
}

static void
_set_src_pad_functions (GstPad * pad)
{
  gst_pad_set_query_function (pad, GST_DEBUG_FUNCPTR (gst_mss_demux_src_query));
  gst_pad_set_event_function (pad, GST_DEBUG_FUNCPTR (gst_mss_demux_src_event));
}

static GstPad *
_create_pad (GstMssDemux * mssdemux, GstMssStream * manifeststream)
{
  gchar *name;
  GstPad *srcpad = NULL;
  GstMssStreamType streamtype;

  streamtype = gst_mss_stream_get_type (manifeststream);
  GST_DEBUG_OBJECT (mssdemux, "Found stream of type: %s",
      gst_mss_stream_type_name (streamtype));

  /* TODO use stream's name/bitrate/index as the pad name? */
  if (streamtype == MSS_STREAM_TYPE_VIDEO) {
    name = g_strdup_printf ("video_%02u", mssdemux->n_videos++);
    srcpad =
        gst_pad_new_from_static_template (&gst_mss_demux_videosrc_template,
        name);
    g_free (name);
  } else if (streamtype == MSS_STREAM_TYPE_AUDIO) {
    name = g_strdup_printf ("audio_%02u", mssdemux->n_audios++);
    srcpad =
        gst_pad_new_from_static_template (&gst_mss_demux_audiosrc_template,
        name);
    g_free (name);
  }

  if (!srcpad) {
    GST_WARNING_OBJECT (mssdemux, "Ignoring unknown type stream");
    return NULL;
  }

  _set_src_pad_functions (srcpad);
  return srcpad;
}

static void
gst_mss_demux_create_streams (GstMssDemux * mssdemux)
{
  GSList *streams = gst_mss_manifest_get_streams (mssdemux->manifest);
  GSList *iter;

  if (streams == NULL) {
    GST_INFO_OBJECT (mssdemux, "No streams found in the manifest");
    GST_ELEMENT_ERROR (mssdemux, STREAM, DEMUX,
        (_("This file contains no playable streams.")),
        ("no streams found at the Manifest"));
    return;
  }

  for (iter = streams; iter; iter = g_slist_next (iter)) {
    GstPad *srcpad = NULL;
    GstMssDemuxStream *stream = NULL;
    GstMssStream *manifeststream = iter->data;

    srcpad = _create_pad (mssdemux, manifeststream);

    if (!srcpad) {
      continue;
    }

    stream = gst_mss_demux_stream_new (mssdemux, manifeststream, srcpad);
    gst_mss_stream_set_active (manifeststream, TRUE);
    mssdemux->streams = g_slist_append (mssdemux->streams, stream);
  }

  /* select initial bitrates */
  GST_OBJECT_LOCK (mssdemux);
  GST_INFO_OBJECT (mssdemux, "Changing max bitrate to %llu",
      mssdemux->connection_speed);
  gst_mss_manifest_change_bitrate (mssdemux->manifest,
      mssdemux->connection_speed);
  mssdemux->update_bitrates = FALSE;
  GST_OBJECT_UNLOCK (mssdemux);
}

static gboolean
gst_mss_demux_expose_stream (GstMssDemux * mssdemux, GstMssDemuxStream * stream)
{
  GstCaps *caps;
  GstCaps *media_caps;
  GstPad *pad = stream->pad;

  media_caps = gst_mss_stream_get_caps (stream->manifest_stream);

  if (media_caps) {
    caps = gst_caps_new_simple ("video/quicktime", "variant", G_TYPE_STRING,
        "mss-fragmented", "timescale", G_TYPE_UINT64,
        gst_mss_stream_get_timescale (stream->manifest_stream), "media-caps",
        GST_TYPE_CAPS, media_caps, NULL);
    gst_caps_unref (media_caps);
    gst_pad_set_caps (pad, caps);
    gst_caps_unref (caps);

    gst_pad_set_active (pad, TRUE);
    GST_INFO_OBJECT (mssdemux, "Adding srcpad %s:%s with caps %" GST_PTR_FORMAT,
        GST_DEBUG_PAD_NAME (pad), caps);
    gst_object_ref (pad);
    gst_element_add_pad (GST_ELEMENT_CAST (mssdemux), pad);
  } else {
    GST_WARNING_OBJECT (mssdemux,
        "Couldn't get caps from manifest stream %p %s, not exposing it", stream,
        GST_PAD_NAME (stream->pad));
    return FALSE;
  }
  return TRUE;
}

static void
gst_mss_demux_process_manifest (GstMssDemux * mssdemux)
{
  GstQuery *query;
  gchar *uri = NULL;
  gboolean ret;
  GSList *iter;

  g_return_if_fail (mssdemux->manifest_buffer != NULL);
  g_return_if_fail (mssdemux->manifest == NULL);

  query = gst_query_new_uri ();
  ret = gst_pad_peer_query (mssdemux->sinkpad, query);
  if (ret) {
    gchar *baseurl_end;
    gst_query_parse_uri (query, &uri);
    GST_INFO_OBJECT (mssdemux, "Upstream is using URI: %s", uri);

    baseurl_end = g_strrstr (uri, "/Manifest");
    if (baseurl_end) {
      /* set the new end of the string */
      baseurl_end[0] = '\0';
    } else {
      GST_WARNING_OBJECT (mssdemux, "Stream's URI didn't end with /manifest");
    }

    mssdemux->base_url = uri;
  }
  gst_query_unref (query);

  mssdemux->manifest = gst_mss_manifest_new (mssdemux->manifest_buffer);
  if (!mssdemux->manifest) {
    GST_ELEMENT_ERROR (mssdemux, STREAM, FORMAT, ("Bad manifest file"),
        ("Xml manifest file couldn't be parsed"));
    return;
  }

  gst_mss_demux_create_streams (mssdemux);
  for (iter = mssdemux->streams; iter;) {
    GSList *current = iter;
    GstMssDemuxStream *stream = iter->data;
    iter = g_slist_next (iter); /* do it ourselves as we want it done in the beginning of the loop */
    if (!gst_mss_demux_expose_stream (mssdemux, stream)) {
      gst_mss_demux_stream_free (stream);
      mssdemux->streams = g_slist_delete_link (mssdemux->streams, current);
    }
  }

  if (!mssdemux->streams) {
    /* no streams */
    GST_WARNING_OBJECT (mssdemux, "Couldn't identify the caps for any of the "
        "streams found in the manifest");
    GST_ELEMENT_ERROR (mssdemux, STREAM, DEMUX,
        (_("This file contains no playable streams.")),
        ("No known stream formats found at the Manifest"));
    return;
  }

  gst_element_no_more_pads (GST_ELEMENT_CAST (mssdemux));
}

static void
gst_mss_demux_reconfigure (GstMssDemux * mssdemux)
{
  GSList *oldpads = NULL;
  GSList *iter;

  gst_mss_demux_stop_tasks (mssdemux, FALSE);
  if (gst_mss_manifest_change_bitrate (mssdemux->manifest,
          mssdemux->connection_speed)) {

    GST_DEBUG_OBJECT (mssdemux, "Creating new pad group");
    /* if we changed the bitrate, we need to add new pads */
    for (iter = mssdemux->streams; iter; iter = g_slist_next (iter)) {
      GstMssDemuxStream *stream = iter->data;
      GstClockTime ts =
          gst_mss_stream_get_fragment_gst_timestamp (stream->manifest_stream);

      oldpads = g_slist_prepend (oldpads, stream->pad);

      stream->pad = _create_pad (mssdemux, stream->manifest_stream);
      /* TODO keep the same playback rate */
      stream->pending_newsegment =
          gst_event_new_new_segment (FALSE, 1.0, GST_FORMAT_TIME, ts, -1, ts);
      gst_mss_demux_expose_stream (mssdemux, stream);
    }

    gst_element_no_more_pads (GST_ELEMENT (mssdemux));

    for (iter = oldpads; iter; iter = g_slist_next (iter)) {
      GstPad *oldpad = iter->data;

      /* Push out EOS */
      gst_pad_push_event (oldpad, gst_event_new_eos ());
      gst_pad_set_active (oldpad, FALSE);
      gst_element_remove_pad (GST_ELEMENT (mssdemux), oldpad);
      gst_object_unref (oldpad);
    }
  }
  gst_mss_demux_restart_tasks (mssdemux);
}

static void
gst_mss_demux_stream_loop (GstMssDemuxStream * stream)
{
  GstMssDemux *mssdemux = stream->parent;
  gchar *path;
  gchar *url;
  GstFragment *fragment;
  GstBuffer *buffer;
  GstFlowReturn ret;

  GST_OBJECT_LOCK (mssdemux);
  if (mssdemux->update_bitrates) {
    mssdemux->update_bitrates = FALSE;
    GST_OBJECT_UNLOCK (mssdemux);

    GST_DEBUG_OBJECT (mssdemux,
        "Starting streams reconfiguration due to bitrate changes");
    g_thread_create ((GThreadFunc) gst_mss_demux_reconfigure, mssdemux, FALSE,
        NULL);
    GST_DEBUG_OBJECT (mssdemux, "Finished streams reconfiguration");
  } else {
    GST_OBJECT_UNLOCK (mssdemux);
  }

  GST_DEBUG_OBJECT (mssdemux, "Getting url for stream %p", stream);
  ret = gst_mss_stream_get_fragment_url (stream->manifest_stream, &path);
  switch (ret) {
    case GST_FLOW_OK:
      break;                    /* all is good, let's go */
    case GST_FLOW_UNEXPECTED:  /* EOS */
      goto eos;
    case GST_FLOW_ERROR:
      goto error;
    default:
      break;
  }
  if (!path) {
    goto no_url_error;
  }
  GST_DEBUG_OBJECT (mssdemux, "Got url path '%s' for stream %p", path, stream);

  url = g_strdup_printf ("%s/%s", mssdemux->base_url, path);

  fragment = gst_uri_downloader_fetch_uri (stream->downloader, url);
  g_free (path);
  g_free (url);

  if (!fragment) {
    GST_INFO_OBJECT (mssdemux, "No fragment downloaded");
    /* TODO check if we are truly stoping */
    return;
  }

  buffer = gst_fragment_get_buffer (fragment);
  buffer = gst_buffer_make_metadata_writable (buffer);
  gst_buffer_set_caps (buffer, GST_PAD_CAPS (stream->pad));
  GST_BUFFER_TIMESTAMP (buffer) =
      gst_mss_stream_get_fragment_gst_timestamp (stream->manifest_stream);
  GST_BUFFER_DURATION (buffer) =
      gst_mss_stream_get_fragment_gst_duration (stream->manifest_stream);

  if (GST_BUFFER_TIMESTAMP (buffer) > 10 * GST_SECOND
      && mssdemux->connection_speed != 1000) {
    mssdemux->connection_speed = 1000;
    mssdemux->update_bitrates = TRUE;
  }

  if (G_UNLIKELY (stream->pending_newsegment)) {
    gst_pad_push_event (stream->pad, stream->pending_newsegment);
    stream->pending_newsegment = NULL;
  }

  GST_DEBUG_OBJECT (mssdemux, "Pushing buffer of size %u on pad %s",
      GST_BUFFER_SIZE (buffer), GST_PAD_NAME (stream->pad));
  ret = gst_pad_push (stream->pad, buffer);
  switch (ret) {
    case GST_FLOW_UNEXPECTED:
      goto eos;                 /* EOS ? */
    case GST_FLOW_ERROR:
      goto error;
    case GST_FLOW_NOT_LINKED:
      break;                    /* TODO what to do here? pause the task or just keep pushing? */
    case GST_FLOW_OK:
    default:
      break;
  }

  gst_mss_stream_advance_fragment (stream->manifest_stream);
  return;

eos:
  {
    GstEvent *eos = gst_event_new_eos ();
    GST_DEBUG_OBJECT (mssdemux, "Pushing EOS on pad %s:%s",
        GST_DEBUG_PAD_NAME (stream->pad));
    gst_pad_push_event (stream->pad, eos);
    gst_task_stop (stream->stream_task);
    return;
  }
error:
  {
    GST_WARNING_OBJECT (mssdemux, "Error while pushing fragment");
    gst_task_stop (stream->stream_task);
    return;
  }
}
