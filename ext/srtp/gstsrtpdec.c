/*
 * GStreamer - GStreamer SRTP decoder
 *
 * Copyright 2009-2011 Collabora Ltd.
 *  @author: Gabriel Millaire <gabriel.millaire@collabora.co.uk>
 *  @author: Olivier Crete <olivier.crete@collabora.com>
 *
 * Permission is hereby granted, free of charge, to any person obtaining a
 * copy of this software and associated documentation files (the "Software"),
 * to deal in the Software without restriction, including without limitation
 * the rights to use, copy, modify, merge, publish, distribute, sublicense,
 * and/or sell copies of the Software, and to permit persons to whom the
 * Software is furnished to do so, subject to the following conditions:
 *
 * The above copyright notice and this permission notice shall be included in
 * all copies or substantial portions of the Software.
 *
 * THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
 * IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
 * FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL THE
 * AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
 * LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING
 * FROM, OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER
 * DEALINGS IN THE SOFTWARE.
 *
 * Alternatively, the contents of this file may be used under the
 * GNU Lesser General Public License Version 2.1 (the "LGPL"), in
 * which case the following provisions apply instead of the ones
 * mentioned above:
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
 * SECTION:element-srtpdec
 * @see_also: srtpenc
 *
 * gstrtpdec acts as a decoder that removes security from SRTP and SRTCP
 * packets (encryption and authentication) and out RTP and RTCP. It
 * receives packet of type 'application/x-srtp' or 'application/x-srtcp'
 * on its sink pad, and outs packets of type 'application/x-rtp' or
 * 'application/x-rtcp' on its sink pad.
 *
 * For each packet received, it checks if the internal SSRC is in the list
 * of streams already in use. If this is not the case, it sends a signal to
 * the user to get the needed parameters to create a new stream : master
 * key, encryption and authentication mecanisms for both RTP and RTCP. If
 * the user can't provide those parameters, the buffer is dropped and a
 * warning is emitted.
 *
 * This element uses libsrtp library. The encryption and authentication
 * mecanisms available are :
 *
 * Encryption
 * - AES_128_ICM (default, maximum security)
 * - STRONGHOLD_CIPHER (same as AES_128_ICM)
 * - NULL
 *
 * Authentication
 * - HMAC_SHA1 (default, maximum protection)
 * - STRONGHOLD_AUTH (same as HMAC_SHA1)
 * - NULL
 *
 * Note that for SRTP protection, authentication is mandatory (non-null)
 * if encryption is used (non-null).
 *
 * Each packet received is first analysed (checked for valid SSRC) then
 * its buffer is unprotected with libsrtp, then pushed on the source pad.
 * If protection failed or the stream could not be created, the buffer
 * is dropped and a warning is emitted.
 *
 * When the maximum usage of the master key is reached, a soft-limit
 * signal is sent to the user, and new parameters (master key) are needed
 * in return. If the hard limit is reached, a flag is set and every
 * subsequent packet is dropped, until a new key is set and the stream
 * has been updated.
 *
 * <refsect2>
 * <title>Example pipelines</title>
 * |[
 * gst-launch-1.0 udpsrc port=5004 caps='application/x-srtp, payload=(int)8, ssrc=(uint)1356955624, srtp-key=(buffer)012345678901234567890123456789012345678901234567890123456789, srtp-cipher=(string)aes-128-icm, srtp-auth=(string)hmac-sha1-80, srtcp-cipher=(string)aes-128-icm, srtcp-auth=(string)hmac-sha1-80' !  srtpdec ! rtppcmadepay ! alawdec ! pulsesink
 * ]| Receive PCMA SRTP packets through UDP using caps to specify
 * master key and protection.
 * |[
 * gst-launch-1.0 audiotestsrc ! alawenc ! rtppcmapay ! 'application/x-rtp, payload=(int)8, ssrc=(uint)1356955624' ! srtpenc key="012345678901234567890123456789012345678901234567890123456789" ! udpsink port=5004
 * ]| Send PCMA SRTP packets through UDP, nothing how the SSRC is forced so
 * that the receiver will recognize it.
 * </refsect2>
 */

#ifdef HAVE_CONFIG_H
#include <config.h>
#endif

#include <gst/gst.h>
#include <gst/rtp/gstrtpbuffer.h>
#include <string.h>

#include "gstsrtp.h"
#include "gstsrtp-enumtypes.h"

#include "gstsrtpdec.h"

GST_DEBUG_CATEGORY_STATIC (gst_srtp_dec_debug);
#define GST_CAT_DEFAULT gst_srtp_dec_debug

/* Filter signals and args */
enum
{
  SIGNAL_REQUEST_KEY = 1,
  SIGNAL_CLEAR_KEYS,
  SIGNAL_SOFT_LIMIT,
  SIGNAL_HARD_LIMIT,
  LAST_SIGNAL
};

enum
{
  PROP_0
};

/* the capabilities of the inputs and outputs.
 *
 * describe the real formats here.
 */
static GstStaticPadTemplate rtp_sink_template =
GST_STATIC_PAD_TEMPLATE ("rtp_sink",
    GST_PAD_SINK,
    GST_PAD_ALWAYS,
    GST_STATIC_CAPS ("application/x-srtp")
    );

static GstStaticPadTemplate rtp_src_template =
GST_STATIC_PAD_TEMPLATE ("rtp_src",
    GST_PAD_SRC,
    GST_PAD_ALWAYS,
    GST_STATIC_CAPS ("application/x-rtp")
    );

static GstStaticPadTemplate rtcp_sink_template =
GST_STATIC_PAD_TEMPLATE ("rtcp_sink",
    GST_PAD_SINK,
    GST_PAD_ALWAYS,
    GST_STATIC_CAPS ("application/x-srtcp")
    );

static GstStaticPadTemplate rtcp_src_template =
GST_STATIC_PAD_TEMPLATE ("rtcp_src",
    GST_PAD_SRC,
    GST_PAD_ALWAYS,
    GST_STATIC_CAPS ("application/x-rtcp")
    );

static guint gst_srtp_dec_signals[LAST_SIGNAL] = { 0 };

G_DEFINE_TYPE (GstSrtpDec, gst_srtp_dec, GST_TYPE_ELEMENT);

static void gst_srtp_dec_clear_streams (GstSrtpDec * filter);

static gboolean gst_srtp_dec_sink_event_rtp (GstPad * pad, GstObject * parent,
    GstEvent * event);
static gboolean gst_srtp_dec_sink_event_rtcp (GstPad * pad, GstObject * parent,
    GstEvent * event);

static gboolean gst_srtp_dec_sink_query_rtp (GstPad * pad, GstObject * parent,
    GstQuery * query);
static gboolean gst_srtp_dec_sink_query_rtcp (GstPad * pad,
    GstObject * parent, GstQuery * query);


static GstIterator *gst_srtp_dec_iterate_internal_links_rtp (GstPad * pad,
    GstObject * parent);
static GstIterator *gst_srtp_dec_iterate_internal_links_rtcp (GstPad * pad,
    GstObject * parent);

static GstFlowReturn gst_srtp_dec_chain_rtp (GstPad * pad,
    GstObject * parent, GstBuffer * buf);
static GstFlowReturn gst_srtp_dec_chain_rtcp (GstPad * pad,
    GstObject * parent, GstBuffer * buf);

static GstStateChangeReturn gst_srtp_dec_change_state (GstElement * element,
    GstStateChange transition);

static GstSrtpDecSsrcStream *request_key_with_signal (GstSrtpDec * filter,
    guint32 ssrc, gint signal);

struct _GstSrtpDecSsrcStream
{
  guint32 ssrc;

  GstCaps *caps;
  GstBuffer *key;
  GstSrtpCipherType rtp_cipher;
  GstSrtpAuthType rtp_auth;
  GstSrtpCipherType rtcp_cipher;
  GstSrtpAuthType rtcp_auth;
};

/* initialize the srtpdec's class */
static void
gst_srtp_dec_class_init (GstSrtpDecClass * klass)
{
  GstElementClass *gstelement_class;

  gstelement_class = (GstElementClass *) klass;

  gst_element_class_add_pad_template (gstelement_class,
      gst_static_pad_template_get (&rtp_src_template));
  gst_element_class_add_pad_template (gstelement_class,
      gst_static_pad_template_get (&rtp_sink_template));
  gst_element_class_add_pad_template (gstelement_class,
      gst_static_pad_template_get (&rtcp_src_template));
  gst_element_class_add_pad_template (gstelement_class,
      gst_static_pad_template_get (&rtcp_sink_template));

  gst_element_class_set_static_metadata (gstelement_class, "SRTP decoder",
      "Filter/Network/SRTP",
      "A SRTP and SRTCP decoder",
      "Gabriel Millaire <millaire.gabriel@collabora.com>");

  gstelement_class->change_state =
      GST_DEBUG_FUNCPTR (gst_srtp_dec_change_state);
  klass->clear_streams = GST_DEBUG_FUNCPTR (gst_srtp_dec_clear_streams);

  /**
   * GstSrtpDec::request-key:
   * @gstsrtpdec: the element on which the signal is emitted
   * @ssrc: The unique SSRC of the stream
   *
   * Signal emited to get the parameters relevant to stream
   * with @ssrc. User should provide the key and the RTP and
   * RTCP encryption ciphers and authentication, and return
   * them wrapped in a GstCaps.
   */
  gst_srtp_dec_signals[SIGNAL_REQUEST_KEY] =
      g_signal_new ("request-key", G_TYPE_FROM_CLASS (klass),
      G_SIGNAL_RUN_LAST, 0, NULL, NULL, NULL, GST_TYPE_CAPS, 1, G_TYPE_UINT);

  /**
   * GstSrtpDec::clear-keys:
   * @gstsrtpdec: the element on which the signal is emitted
   *
   * Clear the internal list of streams
   */
  gst_srtp_dec_signals[SIGNAL_CLEAR_KEYS] =
      g_signal_new ("clear-keys", G_TYPE_FROM_CLASS (klass),
      G_SIGNAL_RUN_LAST | G_SIGNAL_ACTION,
      G_STRUCT_OFFSET (GstSrtpDecClass, clear_streams), NULL, NULL, NULL,
      G_TYPE_NONE, 0, G_TYPE_NONE);

  /**
   * GstSrtpDec::soft-limit:
   * @gstsrtpdec: the element on which the signal is emitted
   * @ssrc: The unique SSRC of the stream
   *
   * Signal emited when the stream with @ssrc has reached the
   * soft limit of utilisation of it's master encryption key.
   * User should provide a new key and new RTP and RTCP encryption
   * ciphers and authentication, and return them wrapped in a
   * GstCaps.
   */
  gst_srtp_dec_signals[SIGNAL_SOFT_LIMIT] =
      g_signal_new ("soft-limit", G_TYPE_FROM_CLASS (klass),
      G_SIGNAL_RUN_LAST, 0, NULL, NULL, NULL, GST_TYPE_CAPS, 1, G_TYPE_UINT);

  /**
   * GstSrtpDec::hard-limit:
   * @gstsrtpdec: the element on which the signal is emitted
   * @ssrc: The unique SSRC of the stream
   *
   * Signal emited when the stream with @ssrc has reached the
   * hard limit of utilisation of it's master encryption key.
   * User should provide a new key and new RTP and RTCP encryption
   * ciphers and authentication, and return them wrapped in a
   * GstCaps. If user could not provide those parameters or signal
   * is not answered, the buffers of this stream will be dropped.
   */
  gst_srtp_dec_signals[SIGNAL_HARD_LIMIT] =
      g_signal_new ("hard-limit", G_TYPE_FROM_CLASS (klass),
      G_SIGNAL_RUN_LAST, 0, NULL, NULL, NULL, GST_TYPE_CAPS, 1, G_TYPE_UINT);
}

/* initialize the new element
 * instantiate pads and add them to element
 * set pad calback functions
 * initialize instance structure
 */
static void
gst_srtp_dec_init (GstSrtpDec * filter)
{
  filter->rtp_sinkpad =
      gst_pad_new_from_static_template (&rtp_sink_template, "rtp_sink");
  gst_pad_set_event_function (filter->rtp_sinkpad,
      GST_DEBUG_FUNCPTR (gst_srtp_dec_sink_event_rtp));
  gst_pad_set_query_function (filter->rtp_sinkpad,
      GST_DEBUG_FUNCPTR (gst_srtp_dec_sink_query_rtp));
  gst_pad_set_iterate_internal_links_function (filter->rtp_sinkpad,
      GST_DEBUG_FUNCPTR (gst_srtp_dec_iterate_internal_links_rtp));
  gst_pad_set_chain_function (filter->rtp_sinkpad,
      GST_DEBUG_FUNCPTR (gst_srtp_dec_chain_rtp));

  filter->rtp_srcpad =
      gst_pad_new_from_static_template (&rtp_src_template, "rtp_src");
  gst_pad_set_iterate_internal_links_function (filter->rtp_srcpad,
      GST_DEBUG_FUNCPTR (gst_srtp_dec_iterate_internal_links_rtp));

  gst_pad_set_element_private (filter->rtp_sinkpad, filter->rtp_srcpad);
  gst_pad_set_element_private (filter->rtp_srcpad, filter->rtp_sinkpad);

  gst_element_add_pad (GST_ELEMENT (filter), filter->rtp_sinkpad);
  gst_element_add_pad (GST_ELEMENT (filter), filter->rtp_srcpad);


  filter->rtcp_sinkpad =
      gst_pad_new_from_static_template (&rtcp_sink_template, "rtcp_sink");
  gst_pad_set_event_function (filter->rtcp_sinkpad,
      GST_DEBUG_FUNCPTR (gst_srtp_dec_sink_event_rtcp));
  gst_pad_set_query_function (filter->rtcp_sinkpad,
      GST_DEBUG_FUNCPTR (gst_srtp_dec_sink_query_rtcp));
  gst_pad_set_iterate_internal_links_function (filter->rtcp_sinkpad,
      GST_DEBUG_FUNCPTR (gst_srtp_dec_iterate_internal_links_rtcp));
  gst_pad_set_chain_function (filter->rtcp_sinkpad,
      GST_DEBUG_FUNCPTR (gst_srtp_dec_chain_rtcp));

  filter->rtcp_srcpad =
      gst_pad_new_from_static_template (&rtcp_src_template, "rtcp_src");
  gst_pad_set_iterate_internal_links_function (filter->rtcp_srcpad,
      GST_DEBUG_FUNCPTR (gst_srtp_dec_iterate_internal_links_rtcp));

  gst_pad_set_element_private (filter->rtcp_sinkpad, filter->rtcp_srcpad);
  gst_pad_set_element_private (filter->rtcp_srcpad, filter->rtcp_sinkpad);

  gst_element_add_pad (GST_ELEMENT (filter), filter->rtcp_sinkpad);
  gst_element_add_pad (GST_ELEMENT (filter), filter->rtcp_srcpad);

  filter->first_session = TRUE;
}

static void
remove_stream_by_ssrc (GstSrtpDec * filter, guint32 ssrc)
{
  GstSrtpDecSsrcStream *stream = NULL;

  stream = g_hash_table_lookup (filter->streams, GUINT_TO_POINTER (ssrc));

  if (stream) {
    srtp_remove_stream (filter->session, ssrc);
    g_hash_table_remove (filter->streams, GUINT_TO_POINTER (ssrc));
  }
}

static GstSrtpDecSsrcStream *
find_stream_by_ssrc (GstSrtpDec * filter, guint32 ssrc)
{
  return g_hash_table_lookup (filter->streams, GUINT_TO_POINTER (ssrc));
}


/* get info from buffer caps
 */
static GstSrtpDecSsrcStream *
get_stream_from_caps (GstSrtpDec * filter, GstCaps * caps, guint32 ssrc)
{
  GstSrtpDecSsrcStream *stream;
  GstStructure *s;
  GstBuffer *buf;
  const gchar *rtp_cipher, *rtp_auth, *rtcp_cipher, *rtcp_auth;

  /* Create new stream structure and set default values */
  stream = g_slice_new0 (GstSrtpDecSsrcStream);
  stream->ssrc = ssrc;
  stream->key = NULL;

  /* Get info from caps */
  s = gst_caps_get_structure (caps, 0);
  if (!s)
    goto error;

  rtp_cipher = gst_structure_get_string (s, "srtp-cipher");
  rtp_auth = gst_structure_get_string (s, "srtp-auth");
  rtcp_cipher = gst_structure_get_string (s, "srtcp-cipher");
  rtcp_auth = gst_structure_get_string (s, "srtcp-auth");
  if (!rtp_cipher || !rtp_auth || !rtcp_cipher || !rtcp_auth)
    goto error;


  stream->rtp_cipher = enum_value_from_nick (GST_TYPE_SRTP_CIPHER_TYPE,
      rtp_cipher);
  stream->rtp_auth = enum_value_from_nick (GST_TYPE_SRTP_AUTH_TYPE, rtp_auth);
  stream->rtcp_cipher = enum_value_from_nick (GST_TYPE_SRTP_CIPHER_TYPE,
      rtcp_cipher);
  stream->rtcp_auth = enum_value_from_nick (GST_TYPE_SRTP_AUTH_TYPE, rtcp_auth);

  if (stream->rtp_cipher == -1 || stream->rtp_auth == -1 ||
      stream->rtcp_cipher == -1 || stream->rtcp_auth == -1) {
    GST_WARNING_OBJECT (filter, "Invalid caps for stream,"
        " unknown cipher or auth type");
    goto error;
  }

  if (stream->rtcp_cipher != NULL_CIPHER && stream->rtcp_auth == NULL_AUTH) {
    GST_WARNING_OBJECT (filter,
        "Cannot have SRTP NULL authentication with a not-NULL encryption"
        " cipher.");
    goto error;
  }

  if (gst_structure_get (s, "srtp-key", GST_TYPE_BUFFER, &buf, NULL) || !buf) {
    GST_DEBUG ("Got key [%p]", buf);
    stream->key = buf;
  } else if (stream->rtp_cipher != GST_SRTP_CIPHER_NULL ||
      stream->rtcp_cipher != GST_SRTP_CIPHER_NULL ||
      stream->rtp_auth != GST_SRTP_AUTH_NULL ||
      stream->rtcp_auth != GST_SRTP_AUTH_NULL) {
    goto error;
  }

  return stream;

error:
  g_slice_free (GstSrtpDecSsrcStream, stream);
  return NULL;
}

/* Get SRTP params by signal
 */
static GstCaps *
signal_get_srtp_params (GstSrtpDec * filter, guint32 ssrc, gint signal)
{
  GstCaps *caps = NULL;

  g_signal_emit (filter, gst_srtp_dec_signals[signal], 0, ssrc, &caps);

  if (caps != NULL)
    GST_DEBUG_OBJECT (filter, "Caps received");

  return caps;
}

/* Create a stream in the session
 */
static err_status_t
init_session_stream (GstSrtpDec * filter, guint32 ssrc,
    GstSrtpDecSsrcStream * stream)
{
  err_status_t ret;
  srtp_policy_t policy;
  GstMapInfo map;
  guchar tmp[1];

  memset (&policy, 0, sizeof (srtp_policy_t));

  if (!stream)
    return err_status_bad_param;

  GST_INFO_OBJECT (filter, "Setting RTP policy...");
  set_crypto_policy_cipher_auth (stream->rtp_cipher, stream->rtp_auth,
      &policy.rtp);
  GST_INFO_OBJECT (filter, "Setting RTCP policy...");
  set_crypto_policy_cipher_auth (stream->rtcp_cipher, stream->rtcp_auth,
      &policy.rtcp);

  if (stream->key) {
    gst_buffer_map (stream->key, &map, GST_MAP_READ);
    policy.key = (guchar *) map.data;
  } else {
    policy.key = tmp;
  }

  policy.ssrc.value = ssrc;
  policy.ssrc.type = ssrc_specific;
  policy.next = NULL;

  /* If it is the first stream, create the session
   * If not, add the stream policy to the session
   */
  if (filter->first_session)
    ret = srtp_create (&filter->session, &policy);
  else
    ret = srtp_add_stream (filter->session, &policy);

  if (stream->key)
    gst_buffer_unmap (stream->key, &map);

  if (ret == err_status_ok) {
    filter->first_session = FALSE;
    g_hash_table_insert (filter->streams, GUINT_TO_POINTER (stream->ssrc),
        stream);
  }

  return ret;
}

/* Return a stream structure for a given buffer
 */
static GstSrtpDecSsrcStream *
validate_buffer (GstSrtpDec * filter, GstBuffer * buf, guint32 * ssrc,
    gboolean is_rtcp)
{
  GstSrtpDecSsrcStream *stream = NULL;

  if (!is_rtcp) {
    GstRTPBuffer rtpbuf = GST_RTP_BUFFER_INIT;

    if (!gst_rtp_buffer_map (buf, GST_MAP_READ, &rtpbuf)) {
      GST_WARNING_OBJECT (filter, "Invalid SRTP packet");
      return NULL;
    }

    *ssrc = gst_rtp_buffer_get_ssrc (&rtpbuf);

    gst_rtp_buffer_unmap (&rtpbuf);
  } else if (!rtcp_buffer_get_ssrc (buf, ssrc)) {
    GST_WARNING_OBJECT (filter, "No SSRC found in buffer");
    return NULL;
  }

  stream = find_stream_by_ssrc (filter, *ssrc);

  if (stream)
    return stream;

  return request_key_with_signal (filter, *ssrc, SIGNAL_REQUEST_KEY);
}

/* Create new stream from params in caps
 */
static GstSrtpDecSsrcStream *
update_session_stream_from_caps (GstSrtpDec * filter, guint32 ssrc,
    GstCaps * caps)
{
  GstSrtpDecSsrcStream *stream = NULL;
  err_status_t err;

  g_return_val_if_fail (GST_IS_SRTP_DEC (filter), NULL);
  g_return_val_if_fail (GST_IS_CAPS (caps), NULL);

  /* Remove existing stream, if any */
  remove_stream_by_ssrc (filter, ssrc);
  stream = get_stream_from_caps (filter, caps, ssrc);

  if (stream) {
    /* Create new session stream */
    err = init_session_stream (filter, ssrc, stream);

    if (err != err_status_ok) {
      if (stream->key)
        gst_buffer_unref (stream->key);
      g_slice_free (GstSrtpDecSsrcStream, stream);
      stream = NULL;
    }
  }

  return stream;
}

static void
clear_stream (GstSrtpDecSsrcStream * stream)
{
  if (stream->key)
    gst_buffer_unref (stream->key);
  g_slice_free (GstSrtpDecSsrcStream, stream);
}

static gboolean
remove_yes (gpointer key, gpointer value, gpointer user_data)
{
  return TRUE;
}

/* Clear the policy list
 */
static void
gst_srtp_dec_clear_streams (GstSrtpDec * filter)
{
  guint nb = 0;

  GST_OBJECT_LOCK (filter);

  if (!filter->first_session)
    srtp_dealloc (filter->session);

  if (filter->streams)
    nb = g_hash_table_foreach_remove (filter->streams, remove_yes, NULL);

  filter->first_session = TRUE;

  GST_OBJECT_UNLOCK (filter);

  GST_DEBUG_OBJECT (filter, "Cleared %d streams", nb);
}

/* Send a signal
 */
static GstSrtpDecSsrcStream *
request_key_with_signal (GstSrtpDec * filter, guint32 ssrc, gint signal)
{
  GstCaps *caps;
  GstSrtpDecSsrcStream *stream = NULL;

  caps = signal_get_srtp_params (filter, ssrc, signal);

  if (caps) {
    GstSrtpDecSsrcStream *stream =
        update_session_stream_from_caps (filter, ssrc, caps);
    if (stream)
      GST_DEBUG_OBJECT (filter, "New stream set with SSRC %d", ssrc);
    else
      GST_WARNING_OBJECT (filter, "Could not set stream with SSRC %d", ssrc);
    gst_caps_unref (caps);
  }

  return stream;
}

static gboolean
gst_srtp_dec_sink_setcaps (GstPad * pad, GstObject * parent,
    GstCaps * caps, gboolean is_rtcp)
{
  GstSrtpDec *filter = GST_SRTP_DEC (parent);
  GstPad *otherpad;
  GstStructure *ps;
  gboolean ret = FALSE;

  g_return_val_if_fail (gst_caps_is_fixed (caps), FALSE);

  ps = gst_caps_get_structure (caps, 0);

  if (gst_structure_has_field_typed (ps, "ssrc", G_TYPE_UINT) &&
      gst_structure_has_field_typed (ps, "srtp-cipher", G_TYPE_STRING) &&
      gst_structure_has_field_typed (ps, "srtp-auth", G_TYPE_STRING) &&
      gst_structure_has_field_typed (ps, "srtcp-cipher", G_TYPE_STRING) &&
      gst_structure_has_field_typed (ps, "srtcp-auth", G_TYPE_STRING)) {
    guint ssrc;

    gst_structure_get_uint (ps, "ssrc", &ssrc);

    if (!update_session_stream_from_caps (filter, ssrc, caps)) {
      GST_WARNING_OBJECT (pad, "Could not create session from pad caps: %"
          GST_PTR_FORMAT, caps);
      return FALSE;
    }
  }

  caps = gst_caps_copy (caps);
  ps = gst_caps_get_structure (caps, 0);
  gst_structure_remove_fields (ps, "srtp-key", "srtp-cipher", "srtp-auth",
      "srtcp-cipher", "srtcp-auth", NULL);

  if (is_rtcp)
    gst_structure_set_name (ps, "application/x-rtcp");
  else
    gst_structure_set_name (ps, "application/x-rtp");

  otherpad = gst_pad_get_element_private (pad);

  ret = gst_pad_set_caps (otherpad, caps);

  gst_caps_unref (caps);

  return ret;
}

static gboolean
gst_srtp_dec_sink_event (GstPad * pad, GstObject * parent,
    GstEvent * event, gboolean is_rtcp)
{
  GstCaps *caps;

  switch (GST_EVENT_TYPE (event)) {
    case GST_EVENT_CAPS:
      gst_event_parse_caps (event, &caps);
      return gst_srtp_dec_sink_setcaps (pad, parent, caps, FALSE);
    default:
      return gst_pad_event_default (pad, parent, event);
  }
}

static gboolean
gst_srtp_dec_sink_event_rtp (GstPad * pad, GstObject * parent, GstEvent * event)
{
  return gst_srtp_dec_sink_event (pad, parent, event, FALSE);
}

static gboolean
gst_srtp_dec_sink_event_rtcp (GstPad * pad, GstObject * parent,
    GstEvent * event)
{
  return gst_srtp_dec_sink_event (pad, parent, event, TRUE);
}

static gboolean
gst_srtp_dec_sink_query (GstPad * pad, GstObject * parent, GstQuery * query,
    gboolean is_rtcp)
{
  switch (GST_QUERY_TYPE (query)) {
    case GST_QUERY_CAPS:
    {
      GstCaps *filter = NULL;
      GstCaps *other_filter = NULL;
      GstCaps *template_caps;
      GstPad *otherpad;
      GstCaps *other_caps;
      GstCaps *ret;
      int i;

      gst_query_parse_caps (query, &filter);

      otherpad = (GstPad *) gst_pad_get_element_private (pad);

      if (filter) {
        other_filter = gst_caps_copy (filter);

        for (i = 0; i < gst_caps_get_size (other_filter); i++) {
          GstStructure *ps = gst_caps_get_structure (other_filter, i);
          if (is_rtcp)
            gst_structure_set_name (ps, "application/x-rtcp");
          else
            gst_structure_set_name (ps, "application/x-rtp");
          gst_structure_remove_fields (ps, "srtp-key", "srtp-cipher",
              "srtp-auth", "srtcp-cipher", "srtcp-auth", NULL);
        }
      }


      other_caps = gst_pad_peer_query_caps (otherpad, other_filter);
      if (other_filter)
        gst_caps_unref (other_filter);
      if (!other_caps) {
        goto return_template;
      }

      template_caps = gst_pad_get_pad_template_caps (otherpad);
      ret = gst_caps_intersect_full (other_caps, template_caps,
          GST_CAPS_INTERSECT_FIRST);
      gst_caps_unref (other_caps);
      gst_caps_unref (template_caps);

      ret = gst_caps_make_writable (ret);

      for (i = 0; i < gst_caps_get_size (ret); i++) {
        GstStructure *ps = gst_caps_get_structure (ret, i);
        if (is_rtcp)
          gst_structure_set_name (ps, "application/x-srtcp");
        else
          gst_structure_set_name (ps, "application/x-srtp");
      }

      if (filter) {
        GstCaps *tmp;

        tmp = gst_caps_intersect (ret, filter);
        gst_caps_unref (ret);
        ret = tmp;
      }

      gst_query_set_caps_result (query, ret);
      return TRUE;

    return_template:

      ret = gst_pad_get_pad_template_caps (pad);
      gst_query_set_caps_result (query, ret);
      gst_caps_unref (ret);
      return TRUE;
    }
    default:
      return gst_pad_query_default (pad, parent, query);
  }
}

static gboolean
gst_srtp_dec_sink_query_rtp (GstPad * pad, GstObject * parent, GstQuery * query)
{
  return gst_srtp_dec_sink_query (pad, parent, query, FALSE);
}

static gboolean
gst_srtp_dec_sink_query_rtcp (GstPad * pad, GstObject * parent,
    GstQuery * query)
{
  return gst_srtp_dec_sink_query (pad, parent, query, TRUE);
}

static GstIterator *
gst_srtp_dec_iterate_internal_links (GstPad * pad, GstObject * parent,
    gboolean is_rtcp)
{
  GstSrtpDec *filter = GST_SRTP_DEC (parent);
  GstPad *otherpad = NULL;
  GstIterator *it = NULL;

  otherpad = (GstPad *) gst_pad_get_element_private (pad);

  if (otherpad) {
    GValue val = { 0 };

    g_value_init (&val, GST_TYPE_PAD);
    g_value_set_object (&val, otherpad);
    it = gst_iterator_new_single (GST_TYPE_PAD, &val);
    g_value_unset (&val);
  } else {
    GST_ELEMENT_ERROR (GST_ELEMENT_CAST (filter), CORE, PAD, (NULL),
        ("Unable to get linked pad"));
  }

  return it;
}

static GstIterator *
gst_srtp_dec_iterate_internal_links_rtp (GstPad * pad, GstObject * parent)
{
  return gst_srtp_dec_iterate_internal_links (pad, parent, FALSE);
}

static GstIterator *
gst_srtp_dec_iterate_internal_links_rtcp (GstPad * pad, GstObject * parent)
{
  return gst_srtp_dec_iterate_internal_links (pad, parent, TRUE);
}

static GstFlowReturn
gst_srtp_dec_chain (GstPad * pad, GstObject * parent, GstBuffer * buf,
    gboolean is_rtcp)
{
  GstSrtpDec *filter = GST_SRTP_DEC (parent);
  GstPad *otherpad;
  err_status_t err = err_status_ok;
  GstSrtpDecSsrcStream *stream = NULL;
  GstFlowReturn ret = GST_FLOW_OK;
  gint size;
  guint32 ssrc = 0;
  GstMapInfo map;

  GST_OBJECT_LOCK (filter);

  /* Check if this stream exists, if not create a new stream */

  if (!(stream = validate_buffer (filter, buf, &ssrc, is_rtcp))) {
    GST_OBJECT_UNLOCK (filter);
    GST_WARNING_OBJECT (filter, "Invalid buffer, dropping");
    goto drop_buffer;
  }

  GST_LOG_OBJECT (pad, "Received %s buffer of size %" G_GSIZE_FORMAT
      " with SSRC = %u", is_rtcp ? "RTCP" : "RTP", gst_buffer_get_size (buf),
      ssrc);

  /* Change buffer to remove protection */
  buf = gst_buffer_make_writable (buf);

unprotect:

  gst_buffer_map (buf, &map, GST_MAP_READWRITE);
  size = map.size;

  gst_srtp_init_event_reporter ();

  if (is_rtcp)
    err = srtp_unprotect_rtcp (filter->session, map.data, &size);
  else
    err = srtp_unprotect (filter->session, map.data, &size);

  gst_buffer_unmap (buf, &map);

  GST_OBJECT_UNLOCK (filter);

  if (err == err_status_ok) {
    gst_buffer_set_size (buf, size);
    otherpad = (GstPad *) gst_pad_get_element_private (pad);

    /* If all is well, we may have reached soft limit */
    if (gst_srtp_get_soft_limit_reached ())
      request_key_with_signal (filter, ssrc, SIGNAL_SOFT_LIMIT);

    /* Push buffer to source pad */
    ret = gst_pad_push (otherpad, buf);

  } else {                      /* srtp_unprotect failed */
    GST_WARNING_OBJECT (pad,
        "Unable to unprotect buffer (unprotect failed code %d)", err);

    /* Signal user depending on type of error */
    switch (err) {
      case err_status_key_expired:
        GST_OBJECT_LOCK (filter);

        /* Update stream */
        if ((stream = find_stream_by_ssrc (filter, ssrc))) {
          GST_OBJECT_UNLOCK (filter);
          if (request_key_with_signal (filter, ssrc, SIGNAL_HARD_LIMIT)) {
            GST_OBJECT_LOCK (filter);
            goto unprotect;
          }
          goto drop_buffer;
        }
        break;

      case err_status_auth_fail:
      case err_status_cipher_fail:
        GST_ELEMENT_WARNING (filter, STREAM, DECRYPT,
            ("Error while decryption stream"), (NULL));
        ret = GST_FLOW_ERROR;
        goto drop_buffer;

      default:
        GST_WARNING_OBJECT (filter, "Other error");
        goto drop_buffer;
    }
  }

  return ret;

  /* Drop buffer, except if gst_pad_push returned OK or an error */

drop_buffer:
  GST_WARNING_OBJECT (pad, "Dropping buffer");

  gst_buffer_unref (buf);

  return ret;
}

static GstFlowReturn
gst_srtp_dec_chain_rtp (GstPad * pad, GstObject * parent, GstBuffer * buf)
{
  return gst_srtp_dec_chain (pad, parent, buf, FALSE);
}

static GstFlowReturn
gst_srtp_dec_chain_rtcp (GstPad * pad, GstObject * parent, GstBuffer * buf)
{
  return gst_srtp_dec_chain (pad, parent, buf, TRUE);
}

static GstStateChangeReturn
gst_srtp_dec_change_state (GstElement * element, GstStateChange transition)
{
  GstStateChangeReturn res;
  GstSrtpDec *filter;

  filter = GST_SRTP_DEC (element);
  GST_OBJECT_LOCK (filter);

  switch (transition) {
    case GST_STATE_CHANGE_READY_TO_PAUSED:
      filter->streams = g_hash_table_new_full (g_direct_hash, g_direct_equal,
          NULL, (GDestroyNotify) clear_stream);
      break;
    case GST_STATE_CHANGE_PAUSED_TO_PLAYING:
      break;
    default:
      break;
  }

  GST_OBJECT_UNLOCK (filter);

  res = GST_ELEMENT_CLASS (gst_srtp_dec_parent_class)->change_state (element,
      transition);

  switch (transition) {
    case GST_STATE_CHANGE_PLAYING_TO_PAUSED:
      break;
    case GST_STATE_CHANGE_PAUSED_TO_READY:
      gst_srtp_dec_clear_streams (filter);
      break;
    case GST_STATE_CHANGE_READY_TO_NULL:
      g_hash_table_unref (filter->streams);
      filter->streams = NULL;
      break;
    default:
      break;
  }
  return res;
}


/* entry point to initialize the plug-in
 * initialize the plug-in itself
 * register the element factories and other features
 */
gboolean
gst_srtp_dec_plugin_init (GstPlugin * srtpdec)
{
  GST_DEBUG_CATEGORY_INIT (gst_srtp_dec_debug, "srtpdec", 0, "SRTP dec");

  return gst_element_register (srtpdec, "srtpdec", GST_RANK_NONE,
      GST_TYPE_SRTP_DEC);
}
