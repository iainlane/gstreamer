/* GStreamer
 * Copyright (C) <2007> Wim Taymans <wim@fluendo.com>
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
 * SECTION:element-videodetect
 * @see_also: #GstVideoMark
 *
 * This plugin detects #GstVideoDetect:pattern-count squares in the bottom left
 * corner of the video frames. The squares have a width and height of
 * respectively #GstVideoDetect:pattern-width and #GstVideoDetect:pattern-height.
 * Even squares must be black and odd squares must be white.
 * 
 * When the pattern has been found, #GstVideoDetect:pattern-data-count squares
 * after the pattern squares are read as a bitarray. White squares represent a 1
 * bit and black squares a 0 bit. The bitarray will will included in the element
 * message that is posted (see below).
 * 
 * After the pattern has been found and the data pattern has been read, an
 * element message called <classname>&quot;GstVideoDetect&quot;</classname> will
 * be posted on the bus. If the pattern is no longer found in the frame, the
 * same element message is posted with the have-pattern field set to #FALSE.
 * The message is only posted if the #GstVideoDetect:message property is #TRUE.
 * 
 * The message's structure contains these fields:
 * <itemizedlist>
 * <listitem>
 *   <para>
 *   #gboolean
 *   <classname>&quot;have-pattern&quot;</classname>:
 *   if the pattern was found. This field will be set to #TRUE for as long as
 *   the pattern was found in the frame and set to FALSE for the first frame
 *   that does not contain the pattern anymore.
 *   </para>
 * </listitem>
 * <listitem>
 *   <para>
 *   #GstClockTime
 *   <classname>&quot;timestamp&quot;</classname>:
 *   the timestamp of the buffer that triggered the message.
 *   </para>
 * </listitem>
 * <listitem>
 *   <para>
 *   #GstClockTime
 *   <classname>&quot;stream-time&quot;</classname>:
 *   the stream time of the buffer.
 *   </para>
 * </listitem>
 * <listitem>
 *   <para>
 *   #GstClockTime
 *   <classname>&quot;running-time&quot;</classname>:
 *   the running_time of the buffer.
 *   </para>
 * </listitem>
 * <listitem>
 *   <para>
 *   #GstClockTime
 *   <classname>&quot;duration&quot;</classname>:
 *   the duration of the buffer.
 *   </para>
 * </listitem>
 * <listitem>
 *   <para>
 *   #guint64
 *   <classname>&quot;data&quot;</classname>:
 *   the data-pattern found after the pattern or 0 when have-signal is #FALSE.
 *   </para>
 * </listitem>
 * </itemizedlist>
 * 
 * <refsect2>
 * <title>Example launch line</title>
 * |[
 * gst-launch videotestsrc ! videodetect ! videoconvert ! ximagesink
 * ]|
 * </refsect2>
 *
 * Last reviewed on 2007-05-30 (0.10.5)
 */

#ifdef HAVE_CONFIG_H
#include "config.h"
#endif

#include <gst/gst.h>
#include <gst/video/video.h>
#include <gst/video/gstvideofilter.h>
#include "gstvideodetect.h"

GST_DEBUG_CATEGORY_STATIC (gst_video_detect_debug_category);
#define GST_CAT_DEFAULT gst_video_detect_debug_category

/* prototypes */


static void gst_video_detect_set_property (GObject * object,
    guint property_id, const GValue * value, GParamSpec * pspec);
static void gst_video_detect_get_property (GObject * object,
    guint property_id, GValue * value, GParamSpec * pspec);
static void gst_video_detect_dispose (GObject * object);
static void gst_video_detect_finalize (GObject * object);

static gboolean gst_video_detect_start (GstBaseTransform * trans);
static gboolean gst_video_detect_stop (GstBaseTransform * trans);
static gboolean gst_video_detect_set_info (GstVideoFilter * filter,
    GstCaps * incaps, GstVideoInfo * in_info, GstCaps * outcaps,
    GstVideoInfo * out_info);
static GstFlowReturn gst_video_detect_transform_frame_ip (GstVideoFilter *
    filter, GstVideoFrame * frame);

enum
{
  PROP_0,
  PROP_MESSAGE,
  PROP_PATTERN_WIDTH,
  PROP_PATTERN_HEIGHT,
  PROP_PATTERN_COUNT,
  PROP_PATTERN_DATA_COUNT,
  PROP_PATTERN_CENTER,
  PROP_PATTERN_SENSITIVITY,
  PROP_LEFT_OFFSET,
  PROP_BOTTOM_OFFSET
};

#define DEFAULT_MESSAGE              TRUE
#define DEFAULT_PATTERN_WIDTH        4
#define DEFAULT_PATTERN_HEIGHT       16
#define DEFAULT_PATTERN_COUNT        4
#define DEFAULT_PATTERN_DATA_COUNT   5
#define DEFAULT_PATTERN_CENTER       0.5
#define DEFAULT_PATTERN_SENSITIVITY  0.3
#define DEFAULT_LEFT_OFFSET          0
#define DEFAULT_BOTTOM_OFFSET        0

/* pad templates */

#define VIDEO_CAPS \
    GST_VIDEO_CAPS_MAKE( \
        "{ I420, YV12, Y41B, Y42B, Y444, YUY2, UYVY, AYUV, YVYU }")


/* class initialization */

G_DEFINE_TYPE_WITH_CODE (GstVideoDetect, gst_video_detect,
    GST_TYPE_VIDEO_FILTER,
    GST_DEBUG_CATEGORY_INIT (gst_video_detect_debug_category, "videodetect", 0,
        "debug category for videodetect element"));

static void
gst_video_detect_class_init (GstVideoDetectClass * klass)
{
  GObjectClass *gobject_class = G_OBJECT_CLASS (klass);
  GstBaseTransformClass *base_transform_class =
      GST_BASE_TRANSFORM_CLASS (klass);
  GstVideoFilterClass *video_filter_class = GST_VIDEO_FILTER_CLASS (klass);

  gst_element_class_add_pad_template (GST_ELEMENT_CLASS (klass),
      gst_pad_template_new ("src", GST_PAD_SRC, GST_PAD_ALWAYS,
          gst_caps_from_string (VIDEO_CAPS)));
  gst_element_class_add_pad_template (GST_ELEMENT_CLASS (klass),
      gst_pad_template_new ("sink", GST_PAD_SINK, GST_PAD_ALWAYS,
          gst_caps_from_string (VIDEO_CAPS)));

  gst_element_class_set_static_metadata (GST_ELEMENT_CLASS (klass),
      "Video detecter", "Filter/Effect/Video",
      "Detect patterns in a video signal", "Wim Taymans <wim@fluendo.com>");

  gobject_class->set_property = gst_video_detect_set_property;
  gobject_class->get_property = gst_video_detect_get_property;
  gobject_class->dispose = gst_video_detect_dispose;
  gobject_class->finalize = gst_video_detect_finalize;
  base_transform_class->start = GST_DEBUG_FUNCPTR (gst_video_detect_start);
  base_transform_class->stop = GST_DEBUG_FUNCPTR (gst_video_detect_stop);
  video_filter_class->set_info = GST_DEBUG_FUNCPTR (gst_video_detect_set_info);
  video_filter_class->transform_frame_ip =
      GST_DEBUG_FUNCPTR (gst_video_detect_transform_frame_ip);

  g_object_class_install_property (G_OBJECT_CLASS (klass), PROP_MESSAGE,
      g_param_spec_boolean ("message", "Message",
          "Post detected data as bus messages",
          DEFAULT_MESSAGE,
          G_PARAM_READWRITE | G_PARAM_CONSTRUCT | G_PARAM_STATIC_STRINGS));
  g_object_class_install_property (gobject_class, PROP_PATTERN_WIDTH,
      g_param_spec_int ("pattern-width", "Pattern width",
          "The width of the pattern markers", 1, G_MAXINT,
          DEFAULT_PATTERN_WIDTH,
          G_PARAM_READWRITE | G_PARAM_CONSTRUCT | G_PARAM_STATIC_STRINGS));
  g_object_class_install_property (gobject_class, PROP_PATTERN_HEIGHT,
      g_param_spec_int ("pattern-height", "Pattern height",
          "The height of the pattern markers", 1, G_MAXINT,
          DEFAULT_PATTERN_HEIGHT,
          G_PARAM_READWRITE | G_PARAM_CONSTRUCT | G_PARAM_STATIC_STRINGS));
  g_object_class_install_property (gobject_class, PROP_PATTERN_COUNT,
      g_param_spec_int ("pattern-count", "Pattern count",
          "The number of pattern markers", 0, G_MAXINT,
          DEFAULT_PATTERN_COUNT,
          G_PARAM_READWRITE | G_PARAM_CONSTRUCT | G_PARAM_STATIC_STRINGS));
  g_object_class_install_property (gobject_class, PROP_PATTERN_DATA_COUNT,
      g_param_spec_int ("pattern-data-count", "Pattern data count",
          "The number of extra data pattern markers", 0, G_MAXINT,
          DEFAULT_PATTERN_DATA_COUNT,
          G_PARAM_READWRITE | G_PARAM_CONSTRUCT | G_PARAM_STATIC_STRINGS));
  g_object_class_install_property (gobject_class, PROP_PATTERN_CENTER,
      g_param_spec_double ("pattern-center", "Pattern center",
          "The center of the black/white separation (0.0 = lowest, 1.0 highest)",
          0.0, 1.0, DEFAULT_PATTERN_CENTER,
          G_PARAM_READWRITE | G_PARAM_CONSTRUCT | G_PARAM_STATIC_STRINGS));
  g_object_class_install_property (gobject_class, PROP_PATTERN_SENSITIVITY,
      g_param_spec_double ("pattern-sensitivity", "Pattern sensitivity",
          "The sensitivity around the center for detecting the markers "
          "(0.0 = lowest, 1.0 highest)", 0.0, 1.0, DEFAULT_PATTERN_SENSITIVITY,
          G_PARAM_READWRITE | G_PARAM_CONSTRUCT | G_PARAM_STATIC_STRINGS));
  g_object_class_install_property (gobject_class, PROP_LEFT_OFFSET,
      g_param_spec_int ("left-offset", "Left Offset",
          "The offset from the left border where the pattern starts", 0,
          G_MAXINT, DEFAULT_LEFT_OFFSET,
          G_PARAM_READWRITE | G_PARAM_CONSTRUCT | G_PARAM_STATIC_STRINGS));
  g_object_class_install_property (gobject_class, PROP_BOTTOM_OFFSET,
      g_param_spec_int ("bottom-offset", "Bottom Offset",
          "The offset from the bottom border where the pattern starts", 0,
          G_MAXINT, DEFAULT_BOTTOM_OFFSET,
          G_PARAM_READWRITE | G_PARAM_CONSTRUCT | G_PARAM_STATIC_STRINGS));
}

static void
gst_video_detect_init (GstVideoDetect * videodetect)
{
}

void
gst_video_detect_set_property (GObject * object, guint property_id,
    const GValue * value, GParamSpec * pspec)
{
  GstVideoDetect *videodetect = GST_VIDEO_DETECT (object);

  GST_DEBUG_OBJECT (videodetect, "set_property");

  switch (property_id) {
    case PROP_MESSAGE:
      videodetect->message = g_value_get_boolean (value);
      break;
    case PROP_PATTERN_WIDTH:
      videodetect->pattern_width = g_value_get_int (value);
      break;
    case PROP_PATTERN_HEIGHT:
      videodetect->pattern_height = g_value_get_int (value);
      break;
    case PROP_PATTERN_COUNT:
      videodetect->pattern_count = g_value_get_int (value);
      break;
    case PROP_PATTERN_DATA_COUNT:
      videodetect->pattern_data_count = g_value_get_int (value);
      break;
    case PROP_PATTERN_CENTER:
      videodetect->pattern_center = g_value_get_double (value);
      break;
    case PROP_PATTERN_SENSITIVITY:
      videodetect->pattern_sensitivity = g_value_get_double (value);
      break;
    case PROP_LEFT_OFFSET:
      videodetect->left_offset = g_value_get_int (value);
      break;
    case PROP_BOTTOM_OFFSET:
      videodetect->bottom_offset = g_value_get_int (value);
      break;
    default:
      G_OBJECT_WARN_INVALID_PROPERTY_ID (object, property_id, pspec);
      break;
  }
}

void
gst_video_detect_get_property (GObject * object, guint property_id,
    GValue * value, GParamSpec * pspec)
{
  GstVideoDetect *videodetect = GST_VIDEO_DETECT (object);

  GST_DEBUG_OBJECT (videodetect, "get_property");

  switch (property_id) {
    case PROP_MESSAGE:
      g_value_set_boolean (value, videodetect->message);
      break;
    case PROP_PATTERN_WIDTH:
      g_value_set_int (value, videodetect->pattern_width);
      break;
    case PROP_PATTERN_HEIGHT:
      g_value_set_int (value, videodetect->pattern_height);
      break;
    case PROP_PATTERN_COUNT:
      g_value_set_int (value, videodetect->pattern_count);
      break;
    case PROP_PATTERN_DATA_COUNT:
      g_value_set_int (value, videodetect->pattern_data_count);
      break;
    case PROP_PATTERN_CENTER:
      g_value_set_double (value, videodetect->pattern_center);
      break;
    case PROP_PATTERN_SENSITIVITY:
      g_value_set_double (value, videodetect->pattern_sensitivity);
      break;
    case PROP_LEFT_OFFSET:
      g_value_set_int (value, videodetect->left_offset);
      break;
    case PROP_BOTTOM_OFFSET:
      g_value_set_int (value, videodetect->bottom_offset);
      break;
    default:
      G_OBJECT_WARN_INVALID_PROPERTY_ID (object, property_id, pspec);
      break;
  }
}

void
gst_video_detect_dispose (GObject * object)
{
  GstVideoDetect *videodetect = GST_VIDEO_DETECT (object);

  GST_DEBUG_OBJECT (videodetect, "dispose");

  /* clean up as possible.  may be called multiple times */

  G_OBJECT_CLASS (gst_video_detect_parent_class)->dispose (object);
}

void
gst_video_detect_finalize (GObject * object)
{
  GstVideoDetect *videodetect = GST_VIDEO_DETECT (object);

  GST_DEBUG_OBJECT (videodetect, "finalize");

  /* clean up object here */

  G_OBJECT_CLASS (gst_video_detect_parent_class)->finalize (object);
}

static gboolean
gst_video_detect_start (GstBaseTransform * trans)
{
  GstVideoDetect *videodetect = GST_VIDEO_DETECT (trans);

  GST_DEBUG_OBJECT (videodetect, "start");

  return TRUE;
}

static gboolean
gst_video_detect_stop (GstBaseTransform * trans)
{
  GstVideoDetect *videodetect = GST_VIDEO_DETECT (trans);

  GST_DEBUG_OBJECT (videodetect, "stop");

  return TRUE;
}

static gboolean
gst_video_detect_set_info (GstVideoFilter * filter, GstCaps * incaps,
    GstVideoInfo * in_info, GstCaps * outcaps, GstVideoInfo * out_info)
{
  GstVideoDetect *videodetect = GST_VIDEO_DETECT (filter);

  GST_DEBUG_OBJECT (videodetect, "set_info");

  return TRUE;
}

static void
gst_video_detect_post_message (GstVideoDetect * videodetect, GstBuffer * buffer,
    guint64 data)
{
  GstBaseTransform *trans;
  GstMessage *m;
  guint64 duration, timestamp, running_time, stream_time;

  trans = GST_BASE_TRANSFORM_CAST (videodetect);

  /* get timestamps */
  timestamp = GST_BUFFER_TIMESTAMP (buffer);
  duration = GST_BUFFER_DURATION (buffer);
  running_time = gst_segment_to_running_time (&trans->segment, GST_FORMAT_TIME,
      timestamp);
  stream_time = gst_segment_to_stream_time (&trans->segment, GST_FORMAT_TIME,
      timestamp);

  /* post message */
  m = gst_message_new_element (GST_OBJECT_CAST (videodetect),
      gst_structure_new ("GstVideoDetect",
          "have-pattern", G_TYPE_BOOLEAN, videodetect->in_pattern,
          "timestamp", G_TYPE_UINT64, timestamp,
          "stream-time", G_TYPE_UINT64, stream_time,
          "running-time", G_TYPE_UINT64, running_time,
          "duration", G_TYPE_UINT64, duration,
          "data", G_TYPE_UINT64, data, NULL));
  gst_element_post_message (GST_ELEMENT_CAST (videodetect), m);
}

static gdouble
gst_video_detect_calc_brightness (GstVideoDetect * videodetect, guint8 * data,
    gint width, gint height, gint row_stride, gint pixel_stride)
{
  gint i, j;
  guint64 sum;

  sum = 0;
  for (i = 0; i < height; i++) {
    for (j = 0; j < width; j++) {
      sum += data[pixel_stride * j];
    }
    data += row_stride;
  }
  return sum / (255.0 * width * height);
}

static void
gst_video_detect_yuv (GstVideoDetect * videodetect, GstVideoFrame * frame)
{
  gdouble brightness;
  gint i, pw, ph, row_stride, pixel_stride;
  gint width, height, req_width, req_height;
  guint8 *d;
  guint64 pattern_data;

  width = frame->info.width;
  height = frame->info.height;

  pw = videodetect->pattern_width;
  ph = videodetect->pattern_height;
  row_stride = GST_VIDEO_FRAME_COMP_STRIDE (frame, 0);
  pixel_stride = GST_VIDEO_FRAME_COMP_PSTRIDE (frame, 0);

  req_width =
      (videodetect->pattern_count + videodetect->pattern_data_count) * pw +
      videodetect->left_offset;
  req_height = videodetect->bottom_offset + ph;
  if (req_width > width || req_height > height) {
    goto no_pattern;
  }

  /* analyse the bottom left pixels */
  for (i = 0; i < videodetect->pattern_count; i++) {
    d = GST_VIDEO_FRAME_COMP_DATA (frame, 0);
    /* move to start of bottom left, adjust for offsets */
    d += row_stride * (height - ph - videodetect->bottom_offset) +
        pixel_stride * videodetect->left_offset;
    /* move to i-th pattern */
    d += pixel_stride * pw * i;

    /* calc brightness of width * height box */
    brightness = gst_video_detect_calc_brightness (videodetect, d, pw, ph,
        row_stride, pixel_stride);

    GST_DEBUG_OBJECT (videodetect, "brightness %f", brightness);

    if (i & 1) {
      /* odd pixels must be white, all pixels darker than the center +
       * sensitivity are considered wrong. */
      if (brightness <
          (videodetect->pattern_center + videodetect->pattern_sensitivity))
        goto no_pattern;
    } else {
      /* even pixels must be black, pixels lighter than the center - sensitivity
       * are considered wrong. */
      if (brightness >
          (videodetect->pattern_center - videodetect->pattern_sensitivity))
        goto no_pattern;
    }
  }
  GST_DEBUG_OBJECT (videodetect, "found pattern");

  pattern_data = 0;

  /* get the data of the pattern */
  for (i = 0; i < videodetect->pattern_data_count; i++) {
    d = GST_VIDEO_FRAME_COMP_DATA (frame, 0);
    /* move to start of bottom left, adjust for offsets */
    d += row_stride * (height - ph - videodetect->bottom_offset) +
        pixel_stride * videodetect->left_offset;
    /* move after the fixed pattern */
    d += pixel_stride * (videodetect->pattern_count * pw);
    /* move to i-th pattern data */
    d += pixel_stride * pw * i;

    /* calc brightness of width * height box */
    brightness = gst_video_detect_calc_brightness (videodetect, d, pw, ph,
        row_stride, pixel_stride);
    /* update pattern, we just use the center to decide between black and white. */
    pattern_data <<= 1;
    if (brightness > videodetect->pattern_center)
      pattern_data |= 1;
  }

  GST_DEBUG_OBJECT (videodetect, "have data %" G_GUINT64_FORMAT, pattern_data);

  videodetect->in_pattern = TRUE;
  gst_video_detect_post_message (videodetect, frame->buffer, pattern_data);

  return;

no_pattern:
  {
    GST_DEBUG_OBJECT (videodetect, "no pattern found");
    if (videodetect->in_pattern) {
      videodetect->in_pattern = FALSE;
      gst_video_detect_post_message (videodetect, frame->buffer, 0);
    }
    return;
  }
}

static GstFlowReturn
gst_video_detect_transform_frame_ip (GstVideoFilter * filter,
    GstVideoFrame * frame)
{
  GstVideoDetect *videodetect = GST_VIDEO_DETECT (filter);

  GST_DEBUG_OBJECT (videodetect, "transform_frame_ip");

  gst_video_detect_yuv (videodetect, frame);

  return GST_FLOW_OK;
}
