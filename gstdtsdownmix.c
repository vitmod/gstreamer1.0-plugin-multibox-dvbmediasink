/* GStreamer DTS decoder plugin based on libdtsdownmix
 * Copyright (C) 2004 Ronald Bultje <rbultje@ronald.bitfreak.net>
 * Copyright (C) 2009 Jan Schmidt <thaytan@noraisin.net>
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
 * Free Software Foundation, Inc., 51 Franklin St, Fifth Floor,
 * Boston, MA 02110-1301, USA.
 */

/**
 * SECTION:element-dtsdownmix
 *
 * Digital Theatre System (DTS) audio decoder
 * 
 * <refsect2>
 * <title>Example launch line</title>
 * |[
 * gst-launch dvdreadsrc title=1 ! mpegpsdemux ! dtsdownmix ! audioresample ! audioconvert ! alsasink
 * ]| Play a DTS audio track from a dvd.
 * |[
 * gst-launch filesrc location=abc.dts ! dtsdownmix ! audioresample ! audioconvert ! alsasink
 * ]| Decode a standalone file and play it.
 * </refsect2>
 */

#ifdef HAVE_CONFIG_H
#include <config.h>
#endif
#include <gst/gst.h>
#include <gst/audio/audio.h>
#include <gst/base/gstbasesink.h>

#include "common.h"
#include <dca.h>
#include "gstdtsdownmix.h"

#if HAVE_ORC
#include <orc/orc.h>
#endif

#include "gstdvbsink-marshal.h"

#if defined(LIBDTS_FIXED) || defined(LIBDCA_FIXED)
#define SAMPLE_WIDTH 16
#define SAMPLE_FORMAT GST_AUDIO_NE(S16)
#define SAMPLE_TYPE GST_AUDIO_FORMAT_S16
#elif defined (LIBDTS_DOUBLE) || defined(LIBDCA_DOUBLE)
#define SAMPLE_WIDTH 64
#define SAMPLE_FORMAT GST_AUDIO_NE(F64)
#define SAMPLE_TYPE GST_AUDIO_FORMAT_F64
#else
#define SAMPLE_WIDTH 32
#define SAMPLE_FORMAT GST_AUDIO_NE(F32)
#define SAMPLE_TYPE GST_AUDIO_FORMAT_F32
#endif

GST_DEBUG_CATEGORY_STATIC (dtsdownmix_debug);
#define GST_CAT_DEFAULT (dtsdownmix_debug)

enum
{
  PROP_0,
  PROP_DRC
};

static GstStaticPadTemplate sink_factory = GST_STATIC_PAD_TEMPLATE ("sink",
    GST_PAD_SINK,
    GST_PAD_ALWAYS,
    GST_STATIC_CAPS ("audio/x-dts; audio/x-private1-dts")
    );

static GstStaticPadTemplate src_factory = GST_STATIC_PAD_TEMPLATE ("src",
    GST_PAD_SRC,
    GST_PAD_SOMETIMES,
    GST_STATIC_CAPS ("audio/x-raw, "
        "format = (string) " SAMPLE_FORMAT ", "
        "layout = (string) interleaved, "
        "rate = (int) [ 4000, 48000 ], " "channels = (int) [ 1, 6 ]")
    );

G_DEFINE_TYPE (GstDtsDec, gst_dtsdownmix, GST_TYPE_AUDIO_DECODER);
static void gst_dtsdownmix_dispose(GObject *obj);
static void gst_dtsdownmix_reset(GObject *obj);
static gboolean gst_dtsdownmix_start (GstAudioDecoder * dec);
static gboolean gst_dtsdownmix_stop (GstAudioDecoder * dec);
static gboolean gst_dtsdownmix_set_format (GstAudioDecoder * bdec, GstCaps * caps);
static gboolean gst_dtsdownmix_parse (GstAudioDecoder * dec, GstAdapter * adapter,
    gint * offset, gint * length);
static GstFlowReturn gst_dtsdownmix_handle_frame (GstAudioDecoder * dec,
    GstBuffer * buffer);

static GstFlowReturn gst_dtsdownmix_chain (GstPad * pad, GstObject * parent,
    GstBuffer * buf);

static void gst_dtsdownmix_set_property (GObject * object, guint prop_id,
    const GValue * value, GParamSpec * pspec);
static void gst_dtsdownmix_get_property (GObject * object, guint prop_id,
    GValue * value, GParamSpec * pspec);

static gboolean gst_dtsdownmix_sink_event(GstAudioDecoder * dec , GstEvent * sink_event);
static gboolean gst_dtsdownmix_src_event(GstAudioDecoder * dec , GstEvent * src_event);
static GstStateChangeReturn gst_dtsdownmix_change_state(GstElement * dec, GstStateChange transition);
static GstElementClass *parent_class = NULL;

static void
gst_dtsdownmix_class_init (GstDtsDecClass * klass)
{
  GObjectClass *gobject_class = G_OBJECT_CLASS(klass);
  GstElementClass *gstelement_class = GST_ELEMENT_CLASS(klass);
  GstAudioDecoderClass *gstbase_class;
  guint cpuflags;

  gstbase_class = (GstAudioDecoderClass *) klass;

  gobject_class->finalize = gst_dtsdownmix_reset;
  gobject_class->dispose = gst_dtsdownmix_dispose;
  gobject_class->set_property = gst_dtsdownmix_set_property;
  gobject_class->get_property = gst_dtsdownmix_get_property;

  gst_element_class_add_pad_template (gstelement_class,
      gst_static_pad_template_get (&sink_factory));
  gst_element_class_add_pad_template (gstelement_class,
      gst_static_pad_template_get (&src_factory));
  gst_element_class_set_static_metadata (gstelement_class, "DTS audio decoder",
      "Codec/Decoder/Audio",
      "Decodes DTS audio streams",
      "Jan Schmidt <thaytan@noraisin.net>, "
      "Ronald Bultje <rbultje@ronald.bitfreak.net>");

  gstelement_class->change_state = gst_dtsdownmix_change_state;
  gstbase_class->start = GST_DEBUG_FUNCPTR (gst_dtsdownmix_start);
  gstbase_class->stop = GST_DEBUG_FUNCPTR (gst_dtsdownmix_stop);
  gstbase_class->src_event = GST_DEBUG_FUNCPTR(gst_dtsdownmix_src_event);
  gstbase_class->sink_event = GST_DEBUG_FUNCPTR(gst_dtsdownmix_sink_event);
  gstbase_class->set_format = GST_DEBUG_FUNCPTR (gst_dtsdownmix_set_format);
  gstbase_class->parse = GST_DEBUG_FUNCPTR (gst_dtsdownmix_parse);
  gstbase_class->handle_frame = GST_DEBUG_FUNCPTR (gst_dtsdownmix_handle_frame);
  parent_class = g_type_class_peek_parent(klass);

  /**
   * GstDtsDec::drc
   *
   * Set to true to apply the recommended DTS dynamic range compression
   * to the audio stream. Dynamic range compression makes loud sounds
   * softer and soft sounds louder, so you can more easily listen
   * to the stream without disturbing other people.
   */
  g_object_class_install_property (G_OBJECT_CLASS (klass), PROP_DRC,
      g_param_spec_boolean ("drc", "Dynamic Range Compression",
          "Use Dynamic Range Compression", FALSE,
          G_PARAM_READWRITE | G_PARAM_STATIC_STRINGS));

  klass->dts_cpuflags = 0;

#if HAVE_ORC
  cpuflags = orc_target_get_default_flags (orc_target_get_by_name ("mmx"));
  if (cpuflags & ORC_TARGET_MMX_MMX)
    klass->dts_cpuflags |= MM_ACCEL_X86_MMX;
  if (cpuflags & ORC_TARGET_MMX_3DNOW)
    klass->dts_cpuflags |= MM_ACCEL_X86_3DNOW;
  if (cpuflags & ORC_TARGET_MMX_MMXEXT)
    klass->dts_cpuflags |= MM_ACCEL_X86_MMXEXT;
#else
  cpuflags = 0;
  klass->dts_cpuflags = 0;
#endif

  GST_LOG ("CPU flags: dts=%08x, orc=%08x", klass->dts_cpuflags, cpuflags);
}

static void
gst_dtsdownmix_init (GstDtsDec * dtsdownmix)
{
  dtsdownmix->request_channels = DCA_CHANNEL | DCA_STEREO;
  dtsdownmix->dynamic_range_compression = FALSE;
  dtsdownmix->stream_started = 0;
  GST_INFO_OBJECT(dtsdownmix, "DTSDOWNMIX_INIT");
  /* retrieve and intercept base class chain.
   * Quite HACKish, but that's dvd specs for you,
   * since one buffer needs to be split into 2 frames */
  dtsdownmix->base_chain = GST_PAD_CHAINFUNC (GST_AUDIO_DECODER_SINK_PAD (dtsdownmix));
  gst_pad_set_chain_function (GST_AUDIO_DECODER_SINK_PAD (dtsdownmix),
      GST_DEBUG_FUNCPTR (gst_dtsdownmix_chain));
}

static void gst_dtsdownmix_dispose(GObject *obj)
{
	G_OBJECT_CLASS(parent_class)->dispose(obj);
	GST_INFO("GstDtsDec DISPOSED");
}

static void gst_dtsdownmix_reset(GObject *obj)
{
	G_OBJECT_CLASS(parent_class)->finalize(obj);
	GST_INFO("GstDtsDec RESET");
}

static gboolean gst_dtsdownmix_start (GstAudioDecoder * dec)
{
	gint64 tolerance;
	tolerance = 1500; 
	gst_audio_decoder_set_tolerance(dec, tolerance);
	GstDtsDec *dts = GST_DTSDOWNMIX (dec);
	GstDtsDecClass *klass;
   
	klass = GST_DTSDOWNMIX_CLASS (G_OBJECT_GET_CLASS (dts));
	dts->state = dca_init (klass->dts_cpuflags);
	dts->samples = dca_samples (dts->state);
	dts->bit_rate = -1;
	dts->sample_rate = -1;
	dts->stream_channels = DCA_CHANNEL;
	dts->using_channels = DCA_CHANNEL;
	dts->level = 1;
	dts->bias = 0;
	dts->flag_update = TRUE;

	/* call upon legacy upstream byte support (e.g. seeking) */
	gst_audio_decoder_set_estimate_rate (dec, TRUE);
	//gst_audio_decoder_set_max_errors(dec, 100);
	gint max_errors;
	gint delay;
	gboolean needs_format;
	needs_format = gst_audio_decoder_get_needs_format(dec);
	delay = gst_audio_decoder_get_delay(dec);
	max_errors = gst_audio_decoder_get_max_errors(dec);
	GST_INFO_OBJECT (dec, "START MAX ERRORS = %d  DELAY = %d NEEDS FORMAT ? %d", max_errors, delay, needs_format);
	return TRUE;
}

static gboolean
gst_dtsdownmix_stop (GstAudioDecoder * dec)
{
  GstDtsDec *dts = GST_DTSDOWNMIX (dec);

  GST_INFO_OBJECT (dec, "stop");

  dts->base_chain = NULL;
  dts->samples = NULL;
  if (dts->state) {
    dca_free (dts->state);
    dts->state = NULL;
  }
  return TRUE;
}

static GstFlowReturn
gst_dtsdownmix_parse (GstAudioDecoder * bdec, GstAdapter * adapter,
    gint * _offset, gint * len)
{
  GstDtsDec *dts;
  guint8 *data;
  gint av, size;
  gint length = 0, flags, sample_rate, bit_rate, frame_length;
  GstFlowReturn result = GST_FLOW_EOS;

  dts = GST_DTSDOWNMIX (bdec);

  size = av = gst_adapter_available (adapter);
  data = (guint8 *) gst_adapter_map (adapter, av);

  /* find and read header */
  bit_rate = dts->bit_rate;
  sample_rate = dts->sample_rate;
  flags = 0;
  while (size >= 7) {
    length = dca_syncinfo (dts->state, data, &flags,
        &sample_rate, &bit_rate, &frame_length);

    if (length <= 0) {
      /* shift window to re-find sync */
	  GST_INFO_OBJECT (dts, "OUT OF SYNC LENGHT = %d", length);
      data++;
      size--;
    } else if (length <= size) {
      GST_LOG_OBJECT (dts, "Sync: frame size %d", length);
      result = GST_FLOW_OK;
      break;
    } else {
      GST_INFO_OBJECT (dts, "Not enough data available (needed %d had %d)",
          length, size);
      break;
    }
  }
  gst_adapter_unmap (adapter);

  *_offset = av - size;
  *len = length;

  return result;
}

static gint
gst_dtsdownmix_channels (uint32_t flags, GstAudioChannelPosition * pos)
{
  gint chans = 0;

  switch (flags & DCA_CHANNEL_MASK) {
    case DCA_MONO:
      chans = 1;
      if (pos) {
        pos[0] = GST_AUDIO_CHANNEL_POSITION_MONO;
      }
      break;
      /* case DCA_CHANNEL: */
    case DCA_STEREO:
    case DCA_STEREO_SUMDIFF:
    case DCA_STEREO_TOTAL:
    case DCA_DOLBY:
      chans = 2;
      if (pos) {
        pos[0] = GST_AUDIO_CHANNEL_POSITION_FRONT_LEFT;
        pos[1] = GST_AUDIO_CHANNEL_POSITION_FRONT_RIGHT;
      }
      break;
    case DCA_3F:
      chans = 3;
      if (pos) {
        pos[0] = GST_AUDIO_CHANNEL_POSITION_FRONT_CENTER;
        pos[1] = GST_AUDIO_CHANNEL_POSITION_FRONT_LEFT;
        pos[2] = GST_AUDIO_CHANNEL_POSITION_FRONT_RIGHT;
      }
      break;
    case DCA_2F1R:
      chans = 3;
      if (pos) {
        pos[0] = GST_AUDIO_CHANNEL_POSITION_FRONT_LEFT;
        pos[1] = GST_AUDIO_CHANNEL_POSITION_FRONT_RIGHT;
        pos[2] = GST_AUDIO_CHANNEL_POSITION_REAR_CENTER;
      }
      break;
    case DCA_3F1R:
      chans = 4;
      if (pos) {
        pos[0] = GST_AUDIO_CHANNEL_POSITION_FRONT_CENTER;
        pos[1] = GST_AUDIO_CHANNEL_POSITION_FRONT_LEFT;
        pos[2] = GST_AUDIO_CHANNEL_POSITION_FRONT_RIGHT;
        pos[3] = GST_AUDIO_CHANNEL_POSITION_REAR_CENTER;
      }
      break;
    case DCA_2F2R:
      chans = 4;
      if (pos) {
        pos[0] = GST_AUDIO_CHANNEL_POSITION_FRONT_LEFT;
        pos[1] = GST_AUDIO_CHANNEL_POSITION_FRONT_RIGHT;
        pos[2] = GST_AUDIO_CHANNEL_POSITION_REAR_LEFT;
        pos[3] = GST_AUDIO_CHANNEL_POSITION_REAR_RIGHT;
      }
      break;
    case DCA_3F2R:
      chans = 5;
      if (pos) {
        pos[0] = GST_AUDIO_CHANNEL_POSITION_FRONT_CENTER;
        pos[1] = GST_AUDIO_CHANNEL_POSITION_FRONT_LEFT;
        pos[2] = GST_AUDIO_CHANNEL_POSITION_FRONT_RIGHT;
        pos[3] = GST_AUDIO_CHANNEL_POSITION_REAR_LEFT;
        pos[4] = GST_AUDIO_CHANNEL_POSITION_REAR_RIGHT;
      }
      break;
    case DCA_4F2R:
      chans = 6;
      if (pos) {
        pos[0] = GST_AUDIO_CHANNEL_POSITION_FRONT_LEFT_OF_CENTER;
        pos[1] = GST_AUDIO_CHANNEL_POSITION_FRONT_RIGHT_OF_CENTER;
        pos[2] = GST_AUDIO_CHANNEL_POSITION_FRONT_LEFT;
        pos[3] = GST_AUDIO_CHANNEL_POSITION_FRONT_RIGHT;
        pos[4] = GST_AUDIO_CHANNEL_POSITION_REAR_LEFT;
        pos[5] = GST_AUDIO_CHANNEL_POSITION_REAR_RIGHT;
      }
      break;
    default:
      g_warning ("dtsdownmix: invalid flags 0x%x", flags);
      return 0;
  }
  if (flags & DCA_LFE) {
    if (pos) {
      pos[chans] = GST_AUDIO_CHANNEL_POSITION_LFE1;
    }
    chans += 1;
  }

  return chans;
}

static gboolean
gst_dtsdownmix_renegotiate (GstDtsDec * dts)
{
  gint channels;
  gboolean result = FALSE;
  GstAudioChannelPosition from[7], to[7];
  GstAudioInfo info;

  channels = gst_dtsdownmix_channels (dts->using_channels, from);

  if (channels <= 0 || channels > 7)
    goto done;

  GST_INFO_OBJECT (dts, "dtsdownmix renegotiate, channels=%d, rate=%d",
      channels, dts->sample_rate);

  memcpy (to, from, sizeof (GstAudioChannelPosition) * channels);
  gst_audio_channel_positions_to_valid_order (to, channels);
  gst_audio_get_channel_reorder_map (channels, from, to,
      dts->channel_reorder_map);


  gst_audio_info_init (&info);
  gst_audio_info_set_format (&info,
      SAMPLE_TYPE, dts->sample_rate, channels, (channels > 1 ? to : NULL));

  if (!gst_audio_decoder_set_output_format (GST_AUDIO_DECODER (dts), &info))
    goto done;

  result = TRUE;

done:
  return result;
}

static void
gst_dtsdownmix_update_streaminfo (GstDtsDec * dts)
{
  GstTagList *taglist;
  GST_INFO_OBJECT(dts, "UPDATING STREAMINFO");
  if (dts->bit_rate > 3) {
    taglist = gst_tag_list_new_empty ();
    /* 1 => open bitrate, 2 => variable bitrate, 3 => lossless */
    gst_tag_list_add (taglist, GST_TAG_MERGE_APPEND, GST_TAG_BITRATE,
        (guint) dts->bit_rate, NULL);
    gst_audio_decoder_merge_tags (GST_AUDIO_DECODER (dts), taglist,
        GST_TAG_MERGE_REPLACE);
  }
}

static GstFlowReturn
gst_dtsdownmix_handle_frame (GstAudioDecoder * bdec, GstBuffer * buffer)
{
  GstDtsDec *dts;
  gint channels, i, num_blocks;
  gboolean need_renegotiation = FALSE;
  guint8 *data;
  gsize size;
  GstMapInfo map;
  gint chans;
  gint length = 0, flags, sample_rate, bit_rate, frame_length;
  GstFlowReturn result = GST_FLOW_OK;
  GstBuffer *outbuf;
  
  dts = GST_DTSDOWNMIX (bdec);

  /* no fancy draining */
  if (G_UNLIKELY (!buffer))
    return GST_FLOW_OK;

  /* parsed stuff already, so this should work out fine */
  gst_buffer_map (buffer, &map, GST_MAP_READ);
  data = map.data;
  size = map.size;
  g_assert (size >= 7);

  bit_rate = dts->bit_rate;
  sample_rate = dts->sample_rate;
  flags = 0;
  length = dca_syncinfo (dts->state, data, &flags, &sample_rate, &bit_rate,
      &frame_length);
  g_assert (length == size);

  if (flags != dts->prev_flags) {
    dts->prev_flags = flags;
    dts->flag_update = TRUE;
  }

  /* go over stream properties, renegotiate or update streaminfo if needed */
  if (dts->sample_rate != sample_rate) {
    need_renegotiation = TRUE;
    dts->sample_rate = sample_rate;
  }

  if (flags) {
    dts->stream_channels = flags & (DCA_CHANNEL_MASK | DCA_LFE);
  }

  if (bit_rate != dts->bit_rate) {
    dts->bit_rate = bit_rate;
    gst_dtsdownmix_update_streaminfo (dts);
  }

  /* If we haven't had an explicit number of channels chosen through properties
   * at this point, choose what to downmix to now, based on what the peer will 
   * accept - this allows a52dec to do downmixing in preference to a 
   * downstream element such as audioconvert.
   * FIXME: Add the property back in for forcing output channels.
   */
  if (dts->request_channels != DCA_CHANNEL) {
    flags = dts->request_channels;
  } else if (dts->flag_update) {
    GstCaps *caps;

    dts->flag_update = FALSE;

    caps = gst_pad_get_allowed_caps (GST_AUDIO_DECODER_SRC_PAD (dts));
    if (caps && gst_caps_get_size (caps) > 0) {
      GstCaps *copy = gst_caps_copy_nth (caps, 0);
      GstStructure *structure = gst_caps_get_structure (copy, 0);
      gint channels;
      const int dts_channels[6] = {
        DCA_MONO,
        DCA_STEREO,
        DCA_STEREO | DCA_LFE,
        DCA_2F2R,
        DCA_2F2R | DCA_LFE,
        DCA_3F2R | DCA_LFE,
      };

      /* Prefer the original number of channels, but fixate to something 
       * preferred (first in the caps) downstream if possible.
       */
      gst_structure_fixate_field_nearest_int (structure, "channels",
          flags ? gst_dtsdownmix_channels (flags, NULL) : 6);
      gst_structure_get_int (structure, "channels", &channels);
      if (channels <= 6)
		flags = dts_channels[channels - 1];
      else
        flags = dts_channels[5];

      gst_caps_unref (copy);
    } else if (flags) {
      flags = dts->stream_channels;
    } else {
      flags = DCA_3F2R | DCA_LFE;
    }

    if (caps)
      gst_caps_unref (caps);
  } else {
    flags = dts->using_channels;
  }

  /* process */
  flags |= DCA_ADJUST_LEVEL;
  dts->level = 1;
  if (dca_frame (dts->state, data, &flags, &dts->level, dts->bias)) {
    gst_buffer_unmap (buffer, &map);
    GST_AUDIO_DECODER_ERROR (dts, 1, STREAM, DECODE, (NULL),
        ("dts_frame error"), result);
    goto exit;
  }
  gst_buffer_unmap (buffer, &map);

  channels = flags & (DCA_CHANNEL_MASK | DCA_LFE);
  if (dts->using_channels != channels) {
    need_renegotiation = TRUE;
    dts->using_channels = channels;
  }

  /* negotiate if required */
  if (need_renegotiation) {
    GST_DEBUG_OBJECT (dts,
        "dtsdownmix: sample_rate:%d stream_chans:0x%x using_chans:0x%x",
        dts->sample_rate, dts->stream_channels, dts->using_channels);
    if (!gst_dtsdownmix_renegotiate (dts))
      goto failed_negotiation;
  }

  if (dts->dynamic_range_compression == FALSE) {
    dca_dynrng (dts->state, NULL, NULL);
  }

  flags &= (DCA_CHANNEL_MASK | DCA_LFE);
  chans = gst_dtsdownmix_channels (flags, NULL);
  if (!chans)
    goto invalid_flags;

  /* handle decoded data, one block is 256 samples */
  num_blocks = dca_blocks_num (dts->state);
  outbuf =
      gst_buffer_new_and_alloc (256 * chans * (SAMPLE_WIDTH / 8) * num_blocks);

  gst_buffer_map (outbuf, &map, GST_MAP_WRITE);
  data = map.data;
  size = map.size;
  {
    guint8 *ptr = data;
    for (i = 0; i < num_blocks; i++) {
      if (dca_block (dts->state)) {
        /* also marks discont */
        GST_AUDIO_DECODER_ERROR (dts, 1, STREAM, DECODE, (NULL),
            ("error decoding block %d", i), result);
        if (result != GST_FLOW_OK)
          goto exit;
      } else {
        gint n, c;
        gint *reorder_map = dts->channel_reorder_map;

        for (n = 0; n < 256; n++) {
          for (c = 0; c < chans; c++) {
            ((sample_t *) ptr)[n * chans + reorder_map[c]] =
                dts->samples[c * 256 + n];
          }
        }
      }
      ptr += 256 * chans * (SAMPLE_WIDTH / 8);
    }
  }
  gst_buffer_unmap (outbuf, &map);
  result = gst_audio_decoder_finish_frame (bdec, outbuf, 1);

exit:
 // GST_INFO_OBJECT(dts,"STREAM IS RUNNING");
  return result;

  /* ERRORS */
failed_negotiation:
  {
    GST_ELEMENT_ERROR (dts, CORE, NEGOTIATION, (NULL), (NULL));
    return GST_FLOW_ERROR;
  }
invalid_flags:
  {
    GST_ELEMENT_ERROR (GST_ELEMENT (dts), STREAM, DECODE, (NULL),
        ("Invalid channel flags: %d", flags));
    return GST_FLOW_ERROR;
  }
}

static gboolean gst_dtsdownmix_set_format (GstAudioDecoder * bdec, GstCaps * caps)
{
	GstDtsDec *dts = GST_DTSDOWNMIX (bdec);
	GstStructure *structure;
	GST_INFO("GST_DTSDOWNMIX_SET_FORMAT");

	structure = gst_caps_get_structure (caps, 0);

	if (structure && gst_structure_has_name (structure, "audio/x-private1-dts"))
	{
		GST_INFO_OBJECT(dts,"DTS-DVD-MODE");
		dts->dvdmode = TRUE;
	}
	else
		dts->dvdmode = FALSE;


	return TRUE;
}

static GstFlowReturn
gst_dtsdownmix_chain (GstPad * pad, GstObject * parent, GstBuffer * buf)
{
  GstFlowReturn ret = GST_FLOW_OK;
  GstDtsDec *dts = GST_DTSDOWNMIX (parent);
  gint first_access;

  if (dts->dvdmode) {
    guint8 data[2];
    gsize size;
    gint offset, len;
    GstBuffer *subbuf;
	
    size = gst_buffer_get_size (buf);
    if (size < 2)
      goto not_enough_data;

    gst_buffer_extract (buf, 0, data, 2);
    first_access = (data[0] << 8) | data[1];

    /* Skip the first_access header */
    offset = 2;

    if (first_access > 1) {
      /* Length of data before first_access */
      len = first_access - 1;

      if (len <= 0 || offset + len > size)
        goto bad_first_access_parameter;

      subbuf = gst_buffer_copy_region (buf, GST_BUFFER_COPY_ALL, offset, len);
	  GST_BUFFER_DTS (subbuf) = GST_CLOCK_TIME_NONE;
	  
      ret = dts->base_chain (pad, parent, subbuf);
      if (ret != GST_FLOW_OK) {
        gst_buffer_unref (buf);
        goto done;
      }

      offset += len;
      len = size - offset;

      if (len > 0) {
        subbuf = gst_buffer_copy_region (buf, GST_BUFFER_COPY_ALL, offset, len);
 		GST_BUFFER_DTS (subbuf) = GST_BUFFER_DTS (buf);

        ret = dts->base_chain (pad, parent, subbuf);
      }
      gst_buffer_unref (buf);
    } else {
      /* first_access = 0 or 1, so if there's a timestamp it applies to the first byte */
      subbuf =
          gst_buffer_copy_region (buf, GST_BUFFER_COPY_ALL, offset,
          size - offset);
      GST_BUFFER_DTS (subbuf) = GST_BUFFER_DTS (buf);
      ret = dts->base_chain (pad, parent, subbuf);
      gst_buffer_unref (buf);
    }
  } else {
		ret = dts->base_chain (pad, parent, buf);
  }

done:
  return ret;

/* ERRORS */
not_enough_data:
  {
    GST_ELEMENT_ERROR (GST_ELEMENT (dts), STREAM, DECODE, (NULL),
        ("Insufficient data in buffer. Can't determine first_acess"));
    gst_buffer_unref (buf);
    return GST_FLOW_ERROR;
  }
bad_first_access_parameter:
  {
    GST_ELEMENT_ERROR (GST_ELEMENT (dts), STREAM, DECODE, (NULL),
        ("Bad first_access parameter (%d) in buffer", first_access));
    gst_buffer_unref (buf);
    return GST_FLOW_ERROR;
  }
}

static void
gst_dtsdownmix_set_property (GObject * object, guint prop_id, const GValue * value,
    GParamSpec * pspec)
{
  GstDtsDec *dts = GST_DTSDOWNMIX (object);

  switch (prop_id) {
    case PROP_DRC:
      dts->dynamic_range_compression = g_value_get_boolean (value);
      break;
    default:
      G_OBJECT_WARN_INVALID_PROPERTY_ID (object, prop_id, pspec);
      break;
  }
}

static void
gst_dtsdownmix_get_property (GObject * object, guint prop_id, GValue * value,
    GParamSpec * pspec)
{
  GstDtsDec *dts = GST_DTSDOWNMIX (object);

  switch (prop_id) {
    case PROP_DRC:
      g_value_set_boolean (value, dts->dynamic_range_compression);
      break;
    default:
      G_OBJECT_WARN_INVALID_PROPERTY_ID (object, prop_id, pspec);
      break;
  }
}

static gboolean gst_dtsdownmix_sink_event(GstAudioDecoder * dec , GstEvent * sink_event)
{
	GstDtsDec *dts = GST_DTSDOWNMIX (dec);
	GstDtsDecClass *klass;
	GstTagList *taglist;
	gboolean ret = TRUE;
	gboolean test = TRUE;
	GST_INFO_OBJECT(dts, "SINK EVENT %s", GST_EVENT_TYPE_NAME(sink_event));
	klass = GST_DTSDOWNMIX_CLASS (G_OBJECT_GET_CLASS (dts));
	switch (GST_EVENT_TYPE (sink_event))
	{
		case GST_EVENT_STREAM_START:
			// hack gstreamer head (1.5.1) bug.
			// the stream may only be started once.
			// not pushing the second start event to src pad.
		{
			const gchar *stream_id;
			GstStreamFlags flags;
			gst_event_parse_stream_start(sink_event, &stream_id);
			gst_event_parse_stream_flags(sink_event, &flags);
			if(dts->stream_started == 0)
			{
				if (GST_AUDIO_DECODER_SRC_PAD(dts))
				{
					ret = gst_pad_push_event(GST_AUDIO_DECODER_SRC_PAD(dts), sink_event);
					dts->stream_started++;
				}
			}
			else
			{
				dts->stream_started++;
				gst_event_unref(sink_event);
			}
			GST_INFO_OBJECT(dts,"DTS GST_EVENT_STREAM_START id is %x flags: %x", stream_id, flags);
			break;
		}
		case GST_EVENT_TOC:
			if (GST_AUDIO_DECODER_SRC_PAD(dts))
			{
				ret = gst_pad_push_event(GST_AUDIO_DECODER_SRC_PAD(dts), sink_event);
			}
			else
			{
				gst_event_unref(sink_event);
			}
			break;
		case GST_EVENT_CAPS:
			// hack gstreamer head (1.5.1) only caps from second stream start event may(and must) be pushed to src_pad.
			// Somewhere there is a bug but if it is gstreamer self or plugin that I don't...
		{
			GstCaps *caps;
			gst_event_parse_caps(sink_event, &caps);
			if (GST_AUDIO_DECODER_SRC_PAD(dts) && dts->stream_started == 2)
			{
				ret = gst_pad_push_event(GST_AUDIO_DECODER_SRC_PAD(dts), sink_event);
			}
			else
			{
				gst_event_unref(sink_event);
			}
			break;
		}
		case GST_EVENT_SEGMENT:
		{
			const GstSegment *segment;
			GstFormat format;
			gdouble rate;
			guint64 start, end, pos;
			gst_event_parse_segment(sink_event, &segment);
			format = segment->format;
			rate = segment->rate;
			start = segment->start;
			end = segment->stop;
			pos = segment->position;

			GST_INFO_OBJECT(dts, "GST_EVENT_SEGMENT rate=%f format=%d start=%"G_GUINT64_FORMAT " position=%"G_GUINT64_FORMAT, rate, format, start, pos);
			if (GST_AUDIO_DECODER_SRC_PAD(dts) && dts->stream_started == 2)
			{
				ret = gst_pad_push_event(GST_AUDIO_DECODER_SRC_PAD(dts), sink_event);
			}
			else
			{
				gst_event_unref(sink_event);
			}
			break;
		}
		case GST_EVENT_TAG:
			gst_event_parse_tag(sink_event, &taglist);
			//GST_INFO_OBJECT(dts,"TAG %"GST_PTR_FORMAT, taglist);
			if (GST_AUDIO_DECODER_SRC_PAD(dts) && dts->stream_started == 2)
			{
				gst_audio_decoder_merge_tags(dec, taglist, GST_TAG_MERGE_REPLACE);
				gst_event_unref(sink_event);
			}
			else
			{
				gst_audio_decoder_merge_tags(dec, taglist, GST_TAG_MERGE_KEEP_ALL);
				gst_event_unref(sink_event);
			}
			break;
		default :
			if (GST_AUDIO_DECODER_SRC_PAD(dts) && dts->stream_started == 2)
			{
				ret = gst_pad_push_event(GST_AUDIO_DECODER_SRC_PAD(dts), sink_event);
			}
			else
			{
				gst_event_unref(sink_event);
			}
			break;
	}
	return ret;
}

static gboolean gst_dtsdownmix_src_event(GstAudioDecoder * dec , GstEvent * src_event)
{
	GstDtsDec *dts = GST_DTSDOWNMIX(dec);
	guint64 latency;
	GstClockTime new_latency;
	GstEvent new_event;
	GstDtsDecClass *klass;
	gboolean ret = TRUE;
	GST_INFO_OBJECT(dts, "SRC EVENT %s", GST_EVENT_TYPE_NAME(src_event));
	klass = GST_DTSDOWNMIX_CLASS (G_OBJECT_GET_CLASS (dts));
	switch (GST_EVENT_TYPE (src_event))
	{
		default:
			if (GST_AUDIO_DECODER_SINK_PAD(dts) && dts->stream_started == 2)
			{
				ret = gst_pad_push_event(GST_AUDIO_DECODER_SINK_PAD (dts), src_event);
			}
			else
			{
				gst_event_unref(src_event);
			}
			break;
	}
	return ret;
}

static GstStateChangeReturn gst_dtsdownmix_change_state(GstElement * element, GstStateChange transition)
{
	GstStateChangeReturn ret = GST_STATE_CHANGE_SUCCESS;
	GstDtsDec *dts = GST_DTSDOWNMIX(element);
	GstAudioDecoder *dec = GST_AUDIO_DECODER(element);
	GstDtsDecClass *klass;
	klass = GST_DTSDOWNMIX_CLASS (G_OBJECT_GET_CLASS (dts));
	FILE *f;
	
	switch (transition) 
	{
		case GST_STATE_CHANGE_NULL_TO_READY:
			GST_INFO_OBJECT(dts, "GST_STATE_CHANGE_NULL_TO_READY Nr %d", transition);
			if (!get_downmix_setting())
			{
				dts->state = NULL;
				return GST_STATE_CHANGE_FAILURE;
			}
			f = fopen("/tmp/dtsdownmix", "w");
			if (f)
			{
				fprintf(f,"READY\n");
				fclose(f);
			}
			break;
		case GST_STATE_CHANGE_READY_TO_PAUSED:
			GST_INFO_OBJECT(dts, "GST_STATE_CHANGE_READY_TO_PAUSED");
			dts->first_paused = TRUE;
			break;
		case GST_STATE_CHANGE_PAUSED_TO_PLAYING:
			GST_INFO_OBJECT(dts, "GST_STATE_CHANGE_PAUSED_TO_PLAYING");
			break;
		default:
			break;
	}

	switch(transition)
	{
		case GST_STATE_CHANGE_PLAYING_TO_PAUSED:
			GST_INFO_OBJECT(dts, "GST_STATE_CHANGE_PLAYING_TO_PAUSED");
			dts->first_paused = FALSE;
			break;
		case GST_STATE_CHANGE_PAUSED_TO_READY:
			GST_INFO_OBJECT(dts, "GST_STATE_CHANGE_PAUSED_TO_READY Nr %d", transition);
			break;
		case GST_STATE_CHANGE_READY_TO_NULL:
			GST_INFO_OBJECT(dts, "GST_STATE_CHANGE_READY_TO_NULL Nr %d", transition);
			f = fopen("/tmp/dtsdownmix", "w");
			if (f)
			{
				fprintf(f,"NONE\n");
				fclose(f);
			}
			break;
		default:
			break;
	}

	ret = GST_ELEMENT_CLASS(parent_class)->change_state(element, transition);
	return ret;
}

static gboolean
plugin_init (GstPlugin * plugin)
{
  gst_debug_set_colored(GST_DEBUG_COLOR_MODE_OFF);
  GST_DEBUG_CATEGORY_INIT (dtsdownmix_debug, "dtsdownmix", 0, "DTS/DCA audio decoder");

#if HAVE_ORC
  orc_init ();
#endif

  if (!gst_element_register (plugin, "dtsdownmix", GST_RANK_PRIMARY,
          GST_TYPE_DTSDOWNMIX))
    return FALSE;
   return TRUE;
}

GST_PLUGIN_DEFINE (GST_VERSION_MAJOR,
    GST_VERSION_MINOR,
    dtsdownmix,
    "Decodes DTS audio streams",
	plugin_init, VERSION, "LGPL", "GStreamer", "http://gstreamer.net/");
