/*
 *			GPAC - Multimedia Framework C SDK
 *
 *  This file is part of GPAC / WavPack decoder filter, based on libwavpack
 *  (https://github.com/dbry/WavPack).
 *
 *  Hybrid files are decoded from their .wv part alone: the .wvc correction
 *  file, when there is one, is a separate file the filter never sees.
 *  Samples wider than 16 bits are shifted down to 16 bits.
 */

#include <gpac/filters.h>
#include <gpac/constants.h>
#include <string.h>
#include <stdlib.h>

#include <wavpack.h>

#define WVDEC_CHUNK_FRAMES 4096

typedef struct
{
	const u8 *data;
	u32 size, pos;
} WVMemReader;

typedef struct
{
	GF_FilterPid *ipid, *opid;
	Bool is_playing;
	WVMemReader mem;
} GF_WVDecCtx;

/* libwavpack reads through this callback table; it walks the packet in memory
 * instead of a file. */
static int32_t wv_read_bytes(void *id, void *data, int32_t bcount)
{
	WVMemReader *r = (WVMemReader *)id;
	u32 avail = r->size - r->pos;
	if ((u32)bcount > avail)
		bcount = (int32_t)avail;
	if (bcount > 0)
	{
		memcpy(data, r->data + r->pos, bcount);
		r->pos += bcount;
	}
	return bcount;
}
static uint32_t wv_get_pos(void *id) { return ((WVMemReader *)id)->pos; }
static int wv_set_pos_abs(void *id, uint32_t pos)
{
	WVMemReader *r = (WVMemReader *)id;
	if (pos > r->size)
		return -1;
	r->pos = pos;
	return 0;
}
static int wv_set_pos_rel(void *id, int32_t delta, int mode)
{
	WVMemReader *r = (WVMemReader *)id;
	s64 base = (mode == SEEK_SET) ? 0 : (mode == SEEK_CUR) ? r->pos : r->size;
	s64 target = base + delta;
	if ((target < 0) || (target > (s64)r->size))
		return -1;
	r->pos = (u32)target;
	return 0;
}
static int wv_push_back_byte(void *id, int c)
{
	WVMemReader *r = (WVMemReader *)id;
	if (!r->pos)
		return EOF;
	r->pos--;
	return c;
}
static uint32_t wv_get_length(void *id) { return ((WVMemReader *)id)->size; }
static int wv_can_seek(void *id) { (void)id; return 1; }
static int32_t wv_write_bytes(void *id, void *data, int32_t bcount)
{
	(void)id; (void)data; (void)bcount;
	return 0;
}

static WavpackStreamReader wv_reader = {
	wv_read_bytes, wv_get_pos, wv_set_pos_abs, wv_set_pos_rel,
	wv_push_back_byte, wv_get_length, wv_can_seek, wv_write_bytes};

static GF_Err wvdec_configure_pid(GF_Filter *filter, GF_FilterPid *pid, Bool is_remove)
{
	GF_WVDecCtx *ctx = (GF_WVDecCtx *)gf_filter_get_udta(filter);

	if (is_remove)
	{
		if (ctx->opid)
		{
			gf_filter_pid_remove(ctx->opid);
			ctx->opid = NULL;
		}
		ctx->ipid = NULL;
		return GF_OK;
	}
	if (!gf_filter_pid_check_caps(pid))
		return GF_NOT_SUPPORTED;

	ctx->ipid = pid;
	gf_filter_pid_set_framing_mode(pid, GF_TRUE);

	if (!ctx->opid)
		ctx->opid = gf_filter_pid_new(filter);

	gf_filter_pid_set_property(ctx->opid, GF_PROP_PID_STREAM_TYPE, &PROP_UINT(GF_STREAM_AUDIO));
	gf_filter_pid_set_property(ctx->opid, GF_PROP_PID_CODECID, &PROP_UINT(GF_CODECID_RAW));
	gf_filter_pid_set_property(ctx->opid, GF_PROP_PID_AUDIO_FORMAT, &PROP_UINT(GF_AUDIO_FMT_S16));
	/* Rate and channel count are only known once the header is read; they are
	 * set again in process(), but the pid needs plausible values up front for
	 * the graph to resolve. */
	gf_filter_pid_set_property(ctx->opid, GF_PROP_PID_SAMPLE_RATE, &PROP_UINT(44100));
	gf_filter_pid_set_property(ctx->opid, GF_PROP_PID_TIMESCALE, &PROP_UINT(44100));
	gf_filter_pid_set_property(ctx->opid, GF_PROP_PID_NUM_CHANNELS, &PROP_UINT(2));

	return GF_OK;
}

static Bool wvdec_process_event(GF_Filter *filter, const GF_FilterEvent *evt)
{
	GF_WVDecCtx *ctx = (GF_WVDecCtx *)gf_filter_get_udta(filter);
	switch (evt->base.type)
	{
	case GF_FEVT_PLAY:
		ctx->is_playing = GF_TRUE;
		return GF_FALSE;
	case GF_FEVT_STOP:
		ctx->is_playing = GF_FALSE;
		return GF_FALSE;
	default:
		return GF_FALSE;
	}
}

static GF_Err wvdec_process(GF_Filter *filter)
{
	GF_FilterPacket *pck, *dst_pck;
	u8 *data, *output;
	u32 size, sample_rate, channels, i;
	u32 total_frames, done = 0;
	int bytes_per_sample, shift;
	char error[80];
	WavpackContext *wpc;
	s32 *unpack = NULL;
	s16 *pcm = NULL;
	GF_WVDecCtx *ctx = (GF_WVDecCtx *)gf_filter_get_udta(filter);

	pck = gf_filter_pid_get_packet(ctx->ipid);
	if (!pck)
	{
		if (gf_filter_pid_is_eos(ctx->ipid))
		{
			gf_filter_pid_set_eos(ctx->opid);
			return GF_EOS;
		}
		return GF_OK;
	}
	data = (u8 *)gf_filter_pck_get_data(pck, &size);
	if (!data)
	{
		gf_filter_pid_drop_packet(ctx->ipid);
		return GF_IO_ERR;
	}

	ctx->mem.data = data;
	ctx->mem.size = size;
	ctx->mem.pos = 0;
	error[0] = 0;

	wpc = WavpackOpenFileInputEx(&wv_reader, &ctx->mem, NULL, error, 0, 0);
	if (!wpc)
	{
		gf_filter_pid_drop_packet(ctx->ipid);
		GF_LOG(GF_LOG_ERROR, GF_LOG_CODEC, ("[WVDec] %s\n", error[0] ? error : "not a WavPack file"));
		return GF_NON_COMPLIANT_BITSTREAM;
	}

	sample_rate = WavpackGetSampleRate(wpc);
	channels = (u32)WavpackGetNumChannels(wpc);
	bytes_per_sample = WavpackGetBytesPerSample(wpc);
	total_frames = WavpackGetNumSamples(wpc);
	/* libwavpack always hands back one int32 per sample, left aligned on the
	 * file's own bit depth, hence the shift down to 16 bits. */
	shift = (bytes_per_sample > 2) ? (bytes_per_sample - 2) * 8 : 0;

	if (!sample_rate || !channels || !total_frames)
	{
		WavpackCloseFile(wpc);
		gf_filter_pid_drop_packet(ctx->ipid);
		return GF_NON_COMPLIANT_BITSTREAM;
	}

	unpack = (s32 *)gf_malloc((size_t)WVDEC_CHUNK_FRAMES * channels * sizeof(s32));
	pcm = (s16 *)gf_malloc((size_t)total_frames * channels * sizeof(s16));
	if (!unpack || !pcm)
	{
		if (unpack) gf_free(unpack);
		if (pcm) gf_free(pcm);
		WavpackCloseFile(wpc);
		gf_filter_pid_drop_packet(ctx->ipid);
		return GF_OUT_OF_MEM;
	}

	while (done < total_frames)
	{
		u32 got = WavpackUnpackSamples(wpc, unpack, WVDEC_CHUNK_FRAMES);
		if (!got)
			break;
		for (i = 0; i < got * channels; i++)
			pcm[done * channels + i] = (s16)(shift ? (unpack[i] >> shift) : unpack[i]);
		done += got;
	}
	gf_free(unpack);
	WavpackCloseFile(wpc);
	gf_filter_pid_drop_packet(ctx->ipid);

	if (!done)
	{
		gf_free(pcm);
		return GF_NON_COMPLIANT_BITSTREAM;
	}

	gf_filter_pid_set_property(ctx->opid, GF_PROP_PID_SAMPLE_RATE, &PROP_UINT(sample_rate));
	gf_filter_pid_set_property(ctx->opid, GF_PROP_PID_TIMESCALE, &PROP_UINT(sample_rate));
	gf_filter_pid_set_property(ctx->opid, GF_PROP_PID_NUM_CHANNELS, &PROP_UINT(channels));
	gf_filter_pid_set_property(ctx->opid, GF_PROP_PID_CHANNEL_LAYOUT, &PROP_LONGUINT((channels == 1) ? GF_AUDIO_CH_FRONT_CENTER : (GF_AUDIO_CH_FRONT_LEFT | GF_AUDIO_CH_FRONT_RIGHT)));

	dst_pck = gf_filter_pck_new_alloc(ctx->opid, done * channels * (u32)sizeof(s16), &output);
	if (!dst_pck)
	{
		gf_free(pcm);
		return GF_OUT_OF_MEM;
	}
	memcpy(output, pcm, (size_t)done * channels * sizeof(s16));
	gf_free(pcm);

	gf_filter_pck_set_cts(dst_pck, 0);
	gf_filter_pck_set_sap(dst_pck, GF_FILTER_SAP_1);
	gf_filter_pck_set_duration(dst_pck, done);
	gf_filter_pck_send(dst_pck);

	gf_filter_pid_set_eos(ctx->opid);
	return GF_EOS;
}

static void wvdec_finalize(GF_Filter *filter)
{
}

static const GF_FilterCapability WVDecCaps[] =
	{
		CAP_UINT(GF_CAPS_INPUT, GF_PROP_PID_STREAM_TYPE, GF_STREAM_FILE),
		CAP_STRING(GF_CAPS_INPUT, GF_PROP_PID_FILE_EXT, "wv"),
		CAP_STRING(GF_CAPS_INPUT, GF_PROP_PID_MIME, "audio/x-wavpack|audio/wavpack"),
		CAP_UINT(GF_CAPS_OUTPUT, GF_PROP_PID_STREAM_TYPE, GF_STREAM_AUDIO),
		CAP_UINT(GF_CAPS_OUTPUT, GF_PROP_PID_CODECID, GF_CODECID_RAW),
};

GF_FilterRegister WVDecoderRegister = {
	.name = "wvdec",
	GF_FS_SET_DESCRIPTION("WavPack decoder")
		GF_FS_SET_HELP("This filter decodes WavPack (.wv) audio using libwavpack.")
			.private_size = sizeof(GF_WVDecCtx),
	SETCAPS(WVDecCaps),
	.configure_pid = wvdec_configure_pid,
	.process = wvdec_process,
	.process_event = wvdec_process_event,
	.finalize = wvdec_finalize,
};

const GF_FilterRegister *EMSCRIPTEN_KEEPALIVE wvdec_register(GF_FilterSession *session)
{
	return &WVDecoderRegister;
}

#include "filter_register.h"
__attribute__((constructor))
void register_wvdec(void) {
    gf_filter_auto_register("wvdec", wvdec_register);
}
