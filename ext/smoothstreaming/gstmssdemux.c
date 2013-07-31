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
static GstStateChangeReturn
gst_mss_demux_change_state (GstElement * element, GstStateChange transition);
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
        /* TODO should we use the streams accumulated duration? */
        guint64 dur = gst_mss_manifest_get_duration (mssdemux->manifest);
        guint64 timescale = gst_mss_manifest_get_timescale (mssdemux->manifest);

        if (dur != -1 && timescale != -1)
          duration =
              (GstClockTime) gst_util_uint64_scale_round (dur, GST_SECOND,
              timescale);

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
    gchar *name;
    GstPad *srcpad = NULL;
    GstMssDemuxStream *stream = NULL;
    GstMssStream *manifeststream = iter->data;
    GstMssStreamType streamtype;

    streamtype = gst_mss_stream_get_type (manifeststream);
    GST_DEBUG_OBJECT (mssdemux, "Found stream of type: %s",
        gst_mss_stream_type_name (streamtype));

    /* TODO use stream's name as the pad name? */
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
      continue;
    }

    _set_src_pad_functions (srcpad);

    stream = gst_mss_demux_stream_new (mssdemux, manifeststream, srcpad);
    mssdemux->streams = g_slist_append (mssdemux->streams, stream);
  }
}

static void
gst_mss_demux_expose_stream (GstMssDemux * mssdemux, GstMssDemuxStream * stream)
{
  GstCaps *caps;
  GstCaps *media_caps;
  GstPad *pad = stream->pad;

  media_caps = gst_mss_stream_get_caps (stream->manifest_stream);
  caps = gst_caps_new_simple ("video/quicktime", "variant", G_TYPE_STRING,
      "mss-fragmented", "timescale", G_TYPE_UINT64,
      gst_mss_stream_get_timescale (stream->manifest_stream), "media-caps",
      GST_TYPE_CAPS, media_caps, NULL);
  gst_caps_unref (media_caps);

  if (caps) {
    gst_pad_set_caps (pad, caps);
    gst_caps_unref (caps);

    gst_pad_set_active (pad, TRUE);
    GST_INFO_OBJECT (mssdemux, "Adding srcpad %s:%s with caps %" GST_PTR_FORMAT,
        GST_DEBUG_PAD_NAME (pad), caps);
    gst_object_ref (pad);
    gst_element_add_pad (GST_ELEMENT_CAST (mssdemux), pad);
  } else {
    GST_WARNING_OBJECT (mssdemux, "Not exposing stream of unrecognized format");
  }
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
  for (iter = mssdemux->streams; iter; iter = g_slist_next (iter)) {
    gst_mss_demux_expose_stream (mssdemux, iter->data);
  }
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

  buffer = gst_fragment_get_buffer (fragment);
  buffer = gst_buffer_make_metadata_writable (buffer);
  gst_buffer_set_caps (buffer, GST_PAD_CAPS (stream->pad));

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
no_url_error:
  {
    GST_ELEMENT_ERROR (mssdemux, STREAM, DEMUX,
        (_("Failed to get fragment URL.")),
        ("An error happened when getting fragment URL"));
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
