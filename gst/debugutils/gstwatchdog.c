/* GStreamer
 * Copyright (C) 2013 Rdio <ingestions@rdio.com>
 * Copyright (C) 2013 David Schleef <ds@schleef.org>
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
 * Free Software Foundation, Inc., 51 Franklin Street, Suite 500,
 * Boston, MA 02110-1335, USA.
 */
/**
 * SECTION:element-gstwatchdog
 *
 * The watchdog element watches buffers and events flowing through
 * a pipeline.  If no buffers are seen for a configurable amount of
 * time, a error message is sent to the bus.
 *
 * To use this element, insert it into a pipeline as you would an
 * identity element.  Once activated, any pause in the flow of
 * buffers through the element will cause an element error.  The
 * maximum allowed pause is determined by the timeout property.
 *
 * This element is currently intended for transcoding pipelines,
 * although may be useful in other contexts.  In particular, it is
 * not aware of expected pauses in buffer flow, such as the PAUSED
 * state.
 *
 * <refsect2>
 * <title>Example launch line</title>
 * |[
 * gst-launch -v fakesrc ! watchdog ! fakesink
 * ]|
 * </refsect2>
 */

#ifdef HAVE_CONFIG_H
#include "config.h"
#endif

#include <gst/gst.h>
#include <gst/base/gstbasetransform.h>
#include "gstwatchdog.h"

GST_DEBUG_CATEGORY_STATIC (gst_watchdog_debug_category);
#define GST_CAT_DEFAULT gst_watchdog_debug_category

/* prototypes */

static void gst_watchdog_set_property (GObject * object,
    guint property_id, const GValue * value, GParamSpec * pspec);
static void gst_watchdog_get_property (GObject * object,
    guint property_id, GValue * value, GParamSpec * pspec);

static gboolean gst_watchdog_start (GstBaseTransform * trans);
static gboolean gst_watchdog_stop (GstBaseTransform * trans);
static gboolean gst_watchdog_sink_event (GstBaseTransform * trans,
    GstEvent * event);
static gboolean gst_watchdog_src_event (GstBaseTransform * trans,
    GstEvent * event);
static GstFlowReturn gst_watchdog_transform_ip (GstBaseTransform * trans,
    GstBuffer * buf);

enum
{
  PROP_0,
  PROP_TIMEOUT
};

/* class initialization */

G_DEFINE_TYPE_WITH_CODE (GstWatchdog, gst_watchdog, GST_TYPE_BASE_TRANSFORM,
    GST_DEBUG_CATEGORY_INIT (gst_watchdog_debug_category, "watchdog", 0,
        "debug category for watchdog element"));

static void
gst_watchdog_class_init (GstWatchdogClass * klass)
{
  GObjectClass *gobject_class = G_OBJECT_CLASS (klass);
  GstBaseTransformClass *base_transform_class =
      GST_BASE_TRANSFORM_CLASS (klass);

  gst_element_class_add_pad_template (GST_ELEMENT_CLASS (klass),
      gst_pad_template_new ("src", GST_PAD_SRC, GST_PAD_ALWAYS,
          gst_caps_new_any ()));
  gst_element_class_add_pad_template (GST_ELEMENT_CLASS (klass),
      gst_pad_template_new ("sink", GST_PAD_SINK, GST_PAD_ALWAYS,
          gst_caps_new_any ()));

  gst_element_class_set_static_metadata (GST_ELEMENT_CLASS (klass),
      "Watchdog", "Generic", "Watches for pauses in stream buffers",
      "David Schleef <ds@schleef.org>");

  gobject_class->set_property = gst_watchdog_set_property;
  gobject_class->get_property = gst_watchdog_get_property;
  base_transform_class->start = GST_DEBUG_FUNCPTR (gst_watchdog_start);
  base_transform_class->stop = GST_DEBUG_FUNCPTR (gst_watchdog_stop);
  base_transform_class->sink_event =
      GST_DEBUG_FUNCPTR (gst_watchdog_sink_event);
  base_transform_class->src_event = GST_DEBUG_FUNCPTR (gst_watchdog_src_event);
  base_transform_class->transform_ip =
      GST_DEBUG_FUNCPTR (gst_watchdog_transform_ip);

  g_object_class_install_property (gobject_class, PROP_TIMEOUT,
      g_param_spec_int ("timeout", "Timeout", "Timeout (in ms) after "
          "which an element error is sent to the bus if no buffers are "
          "received.", 1, G_MAXINT, 1000,
          G_PARAM_CONSTRUCT | G_PARAM_READWRITE | G_PARAM_STATIC_STRINGS));

}

static void
gst_watchdog_init (GstWatchdog * watchdog)
{
}

void
gst_watchdog_set_property (GObject * object, guint property_id,
    const GValue * value, GParamSpec * pspec)
{
  GstWatchdog *watchdog = GST_WATCHDOG (object);

  GST_DEBUG_OBJECT (watchdog, "set_property");

  switch (property_id) {
    case PROP_TIMEOUT:
      watchdog->timeout = g_value_get_int (value);
      break;
    default:
      G_OBJECT_WARN_INVALID_PROPERTY_ID (object, property_id, pspec);
      break;
  }
}

void
gst_watchdog_get_property (GObject * object, guint property_id,
    GValue * value, GParamSpec * pspec)
{
  GstWatchdog *watchdog = GST_WATCHDOG (object);

  GST_DEBUG_OBJECT (watchdog, "get_property");

  switch (property_id) {
    case PROP_TIMEOUT:
      g_value_set_int (value, watchdog->timeout);
      break;
    default:
      G_OBJECT_WARN_INVALID_PROPERTY_ID (object, property_id, pspec);
      break;
  }
}

static gpointer
gst_watchdog_thread (gpointer user_data)
{
  GstWatchdog *watchdog = GST_WATCHDOG (user_data);

  GST_DEBUG_OBJECT (watchdog, "thread starting");

  g_main_loop_run (watchdog->main_loop);

  GST_DEBUG_OBJECT (watchdog, "thread exiting");

  return NULL;
}

static gboolean
gst_watchdog_trigger (gpointer ptr)
{
  GstWatchdog *watchdog = GST_WATCHDOG (ptr);

  GST_DEBUG_OBJECT (watchdog, "watchdog triggered");

  GST_ELEMENT_ERROR (watchdog, STREAM, FAILED, ("Watchdog triggered"),
      ("Watchdog triggered"));

  return FALSE;
}

static gboolean
gst_watchdog_quit_mainloop (gpointer ptr)
{
  GstWatchdog *watchdog = GST_WATCHDOG (ptr);

  GST_DEBUG_OBJECT (watchdog, "watchdog quit");

  g_main_loop_quit (watchdog->main_loop);

  return FALSE;
}

static void
gst_watchdog_feed (GstWatchdog * watchdog)
{
  if (watchdog->source) {
    g_source_destroy (watchdog->source);
    g_source_unref (watchdog->source);
    watchdog->source = NULL;
  }
  watchdog->source = g_timeout_source_new (watchdog->timeout);
  g_source_set_callback (watchdog->source, gst_watchdog_trigger, watchdog,
      NULL);
  g_source_attach (watchdog->source, watchdog->main_context);
}

static gboolean
gst_watchdog_start (GstBaseTransform * trans)
{
  GstWatchdog *watchdog = GST_WATCHDOG (trans);

  GST_DEBUG_OBJECT (watchdog, "start");

  watchdog->main_context = g_main_context_new ();
  watchdog->main_loop = g_main_loop_new (watchdog->main_context, TRUE);
  watchdog->thread = g_thread_new ("watchdog", gst_watchdog_thread, watchdog);

  return TRUE;
}

static gboolean
gst_watchdog_stop (GstBaseTransform * trans)
{
  GstWatchdog *watchdog = GST_WATCHDOG (trans);
  GSource *quit_source;

  GST_DEBUG_OBJECT (watchdog, "stop");

  if (watchdog->source) {
    g_source_destroy (watchdog->source);
    g_source_unref (watchdog->source);
    watchdog->source = NULL;
  }

  /* dispatch an idle event that trigger g_main_loop_quit to avoid race
   * between g_main_loop_run and g_main_loop_quit */
  quit_source = g_idle_source_new ();
  g_source_set_callback (quit_source, gst_watchdog_quit_mainloop, watchdog,
      NULL);
  g_source_attach (quit_source, watchdog->main_context);
  g_source_unref (quit_source);

  g_thread_join (watchdog->thread);
  g_main_loop_unref (watchdog->main_loop);
  g_main_context_unref (watchdog->main_context);

  return TRUE;
}

static gboolean
gst_watchdog_sink_event (GstBaseTransform * trans, GstEvent * event)
{
  GstWatchdog *watchdog = GST_WATCHDOG (trans);

  GST_DEBUG_OBJECT (watchdog, "sink_event");

  gst_watchdog_feed (watchdog);

  return
      GST_BASE_TRANSFORM_CLASS (gst_watchdog_parent_class)->sink_event (trans,
      event);
}

static gboolean
gst_watchdog_src_event (GstBaseTransform * trans, GstEvent * event)
{
  GstWatchdog *watchdog = GST_WATCHDOG (trans);

  GST_DEBUG_OBJECT (watchdog, "src_event");

  gst_watchdog_feed (watchdog);

  return GST_BASE_TRANSFORM_CLASS (gst_watchdog_parent_class)->src_event (trans,
      event);
}

static GstFlowReturn
gst_watchdog_transform_ip (GstBaseTransform * trans, GstBuffer * buf)
{
  GstWatchdog *watchdog = GST_WATCHDOG (trans);

  GST_DEBUG_OBJECT (watchdog, "transform_ip");

  gst_watchdog_feed (watchdog);

  return GST_FLOW_OK;
}
