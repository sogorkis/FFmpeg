#include "msccoder.h"
#include "avcodec.h"
#include "rle.h"

#include "libavutil/log.h"
#include "libavutil/imgutils.h"

const int ARITH_CODER_BITS = 9;

static int is_key_frame(int frameIndex) {
	return frameIndex % 25 == 0 ? 1 : 0;
}

/*
 * Initialize arithmetic coder models. First model: arithModels[0],
 * can store 0, 1 values (2^1). Second one arithModels[1], 0, 1, 2, 3
 * (2^2) and so on.
 */
void init_arith_models(MscCoderArithModel arithModels[10], int arithModelAddValue[10]) {
	for (int i = 0; i < 10; ++i) {
		initialize_model(&arithModels[i], i + 1);
		arithModelAddValue[i] = pow(2, i) - 1;

	}
}

void init_mpeg_context(AVCodecContext *avctx, MpegEncContext *m) {
	enum CodecID codecId;
	void *privData;

	codecId = avctx->codec_id;
	m->avctx = avctx;
	privData = avctx->priv_data;

	m->qscale= 8;

	avctx->codec_id =
	avctx->codec->id = AV_CODEC_ID_MPEG1VIDEO;

	avctx->priv_data = m;
	ff_MPV_encode_init(avctx);

	avctx->codec_id =
	avctx->codec->id = codecId;

	avctx->priv_data = privData;

	m->dct_unquantize_intra = m->dct_unquantize_mpeg1_intra;
	m->dct_unquantize_inter = m->dct_unquantize_mpeg1_inter;

	m->min_qcoeff = -511;
	m->max_qcoeff = 511;

	for(int i=1; i < 64; i++) {
		int j= m->dsp.idct_permutation[i];

		m->intra_matrix[j] = av_clip_uint8((ff_mpeg1_default_intra_matrix[i] * m->qscale) >> 3);
	}
	m->y_dc_scale_table=
	m->c_dc_scale_table= ff_mpeg2_dc_scale_table[m->intra_dc_precision];
	m->intra_matrix[0] = ff_mpeg2_dc_scale_table[m->intra_dc_precision][8];
	ff_convert_matrix(&m->dsp, m->q_intra_matrix, m->q_intra_matrix16,
			m->intra_matrix, m->intra_quant_bias, 8, 8, 1);
	m->qscale= 8;
}

static void print_debug_block(AVCodecContext *avctx, DCTELEM *block) {
	av_log(avctx, AV_LOG_INFO, "\n ");
	for (int i = 0; i < 64;) {
		av_log(avctx, AV_LOG_INFO, "%3d ", block[i++]);
		if (i % 8 == 0) {
			av_log(avctx, AV_LOG_INFO, "\n ");
		}
	}
}

static void print_debug_int(AVCodecContext *avctx, int * block) {
	av_log(avctx, AV_LOG_INFO, "\n ");
	for (int i = 0; i < 64;) {
		av_log(avctx, AV_LOG_INFO, "%3d ", block[i++]);
		if (i % 8 == 0) {
			av_log(avctx, AV_LOG_INFO, "\n ");
		}
	}
}

static int decode_init(AVCodecContext *avctx) {
	MscDecoderContext * mscContext;

	avctx->pix_fmt = PIX_FMT_YUV420P;

	// allocate frame
	avctx->coded_frame = avcodec_alloc_frame();
	if (!avctx->coded_frame) {
		av_log(avctx, AV_LOG_ERROR, "Could not allocate frame.\n");
		return AVERROR(ENOMEM);
	}

	mscContext = avctx->priv_data;

	// initialize arithmetic coder models
	init_arith_models(mscContext->arithModels, mscContext->arithModelAddValue);

	// allocate buffer
	mscContext->buffSize	= 6 * avctx->coded_width * avctx->coded_height;
	mscContext->buff		= av_malloc(mscContext->buffSize);

	if (!mscContext->buff) {
		av_log(avctx, AV_LOG_ERROR, "Could not allocate buffer.\n");
		return AVERROR(ENOMEM);
	}

	init_mpeg_context(avctx, &mscContext->m);

	return 0;
}


static int decode(AVCodecContext * avctx, void *outdata, int *outdata_size, AVPacket *avpkt) {
	AVFrame *frame = avctx->coded_frame;
	uint32_t rleBytesEncoded;
	MscDecoderContext * mscContext;
	MpegEncContext *m;
	GetBitContext gb;
	MscCoderArithSymbol arithSymbol;
	uint32_t count, value;
	int qscale;

	mscContext = avctx->priv_data;

	m = &mscContext->m;

	if (frame->data[0]) {
		avctx->release_buffer(avctx, frame);
	}

	frame->reference = 0;
	frame->key_frame = 1;
	frame->pict_type = AV_PICTURE_TYPE_I;

	if (avctx->get_buffer(avctx, frame) < 0) {
		av_log(avctx, AV_LOG_ERROR, "Could not allocate buffer.\n");
		return AVERROR(ENOMEM);
	}

	// read header
	memcpy(&rleBytesEncoded, avpkt->data, 4);

	// init encoded data bit buffer
	init_get_bits(&gb, avpkt->data + PACKET_HEADER_SIZE, (avpkt->size - PACKET_HEADER_SIZE) * 8);

	initialize_arithmetic_decoder(&gb);

	qscale = 2;

	m->mb_intra = 0;

	for (int mb_x = 0; mb_x < m->mb_width; mb_x++) {
		for (int mb_y = 0; mb_y < m->mb_height; mb_y++) {

			for (int n = 0; n < 6; ++n) {

				for (int i = 0; i < 64; ++i) {
					// get range symbol
					get_symbol_scale(&mscContext->arithModels[ARITH_CODER_BITS], &arithSymbol);

					// get value for symbol
					count = get_current_count(&arithSymbol);
					value = convert_symbol_to_int(&mscContext->arithModels[ARITH_CODER_BITS], count, &arithSymbol);

					// remove symbol from stream
					remove_symbol_from_stream(&gb, &arithSymbol);

					// update arithmetic coder model
					update_model(&mscContext->arithModels[ARITH_CODER_BITS], value);

					mscContext->block[n][i] = value - mscContext->arithModelAddValue[ARITH_CODER_BITS];
				}

				if (avctx->frame_number == 0 && mb_x == 0 && mb_y == 0) {
					av_log(avctx, AV_LOG_INFO, "Quantized x=%d, y=%d, n=%d\n", mb_x, mb_y, n);
					print_debug_block(avctx, mscContext->block[n]);
				}

				m->dct_unquantize_inter(m, mscContext->block[n], n, qscale);
				m->dsp.idct(mscContext->block[n]);

				if (avctx->frame_number == 0 && mb_x == 0 && mb_y == 0) {
					av_log(avctx, AV_LOG_INFO, "IDCT x=%d, y=%d, n=%d\n", mb_x, mb_y, n);
					print_debug_block(avctx, mscContext->block[n]);
				}
			}

			put_block(mscContext, frame, mb_x, mb_y);
		}
	}

	*outdata_size = sizeof(AVFrame);
	*(AVFrame *) outdata = *frame;

	return avpkt->size;
}

static int decode_close(AVCodecContext *avctx) {
	MscDecoderContext * mscContext;
	mscContext = avctx->priv_data;

	if (avctx->coded_frame->data[0])
		avctx->release_buffer(avctx, avctx->coded_frame);

//	av_freep(&avctx->coded_frame);
	av_freep(&mscContext->buff);

	return 0;
}

static int encode_init(AVCodecContext *avctx) {
	MscEncoderContext * mscContext;

	// allocate frame
	avctx->coded_frame = avcodec_alloc_frame();
	if (!avctx->coded_frame) {
		av_log(avctx, AV_LOG_ERROR, "Could not allocate frame.\n");
		return AVERROR(ENOMEM);
	}

	mscContext = avctx->priv_data;

	// allocate buffers
	mscContext->rleBuffSize		= 3 * avctx->coded_width;
	mscContext->arithBuffSize	= 6 * avctx->coded_width * avctx->coded_height;
	mscContext->rleBuff			= av_malloc(mscContext->rleBuffSize);
	mscContext->arithBuff		= av_malloc(mscContext->arithBuffSize);

	if (mscContext->rleBuff == NULL || mscContext->arithBuff == NULL) {
		av_log(avctx, AV_LOG_ERROR, "Could not allocate buffers.\n");
		return AVERROR(ENOMEM);
	}

	// initialize arithmetic coder model
	init_arith_models(mscContext->arithModels, mscContext->arithModelAddValue);

	init_mpeg_context(avctx, &mscContext->m);

	return 0;
}

static inline void clip_coeffs(MpegEncContext *s, DCTELEM *block,
                               int last_index)
{
    int i;
    const int maxlevel = s->max_qcoeff;
    const int minlevel = s->min_qcoeff;
    int overflow = 0;

    if (s->mb_intra) {
        i = 1; // skip clipping of intra dc
    } else
        i = 0;

    for (; i <= last_index; i++) {
        const int j =  s->intra_scantable.permutated[i];
        int level = block[j];

        if (level > maxlevel) {
            level = maxlevel;
            overflow++;
        } else if (level < minlevel) {
            level = minlevel;
            overflow++;
        }

        block[j] = level;
    }
}


static int encode_frame(AVCodecContext *avctx, AVPacket *avpkt,	const AVFrame *frame, int *got_packet_ptr) {
	MscEncoderContext * mscContext;
	MpegEncContext *m;
	uint32_t rleBytesEncoded, arithBytesEncoded;
	int retCode, isKeyFrame;
	PutBitContext pb;
	int mb_y, mb_x, qscale, value;
	MscCoderArithSymbol arithSymbol;

	// initialize arithmetic encoder registers
	initialize_arithmetic_encoder();

	mscContext = avctx->priv_data;

	init_put_bits(&pb, mscContext->arithBuff, mscContext->arithBuffSize);

	isKeyFrame = is_key_frame(avctx->frame_number);

	avctx->coded_frame->reference = 0; // TODO: is this really necessary to set???
	avctx->coded_frame->key_frame = isKeyFrame ? 1 : 0;
	avctx->coded_frame->pict_type = isKeyFrame ? AV_PICTURE_TYPE_I : AV_PICTURE_TYPE_P;

	m = &mscContext->m;

	qscale = 2;

	m->mb_intra = 0;

//	print_debug_int(avctx, m->q_intra_matrix[2]);
//	print_debug_int(avctx, m->q_chroma_intra_matrix[2]);

	for (mb_x = 0; mb_x < m->mb_width; mb_x++) {
		for (mb_y = 0; mb_y < m->mb_height; mb_y++) {
			get_blocks(mscContext, frame, mb_x, mb_y);

			for (int n = 0, overflow = 0; n < 6; ++n) {

				if (avctx->frame_number == 0 && mb_x == 0 && mb_y == 0) {
					av_log(avctx, AV_LOG_INFO, "Block x=%d, y=%d, n=%d\n", mb_x, mb_y, n);
					print_debug_block(avctx, mscContext->block[n]);
				}

				m->block_last_index[n] = m->dct_quantize(m, mscContext->block[n], n, qscale, &overflow);

				if (overflow) {
					clip_coeffs(m, m->block[n], m->block_last_index[n]);
					av_log(avctx, AV_LOG_WARNING, "Overflow detected, frame: %d, mb_x: %d, mb_y: %d, n: %d\n",
							avctx->frame_number, mb_x, mb_y, n);
				}

				if (avctx->frame_number == 0 && mb_x == 0 && mb_y == 0) {
					av_log(avctx, AV_LOG_INFO, "DCT quantized block x=%d, y=%d, n=%d\n", mb_x, mb_y, n);
					print_debug_block(avctx, mscContext->block[n]);
				}

				//TODO: encode to last non zero coefficient
				for (int i = 0; i < 64; ++i) {
					value = mscContext->block[n][i] + mscContext->arithModelAddValue[ARITH_CODER_BITS];

					// convert value to range symbol
					convert_int_to_symbol(&mscContext->arithModels[ARITH_CODER_BITS], value, &arithSymbol);

					// encode symbol by arith encoder
					encode_symbol(&pb, &arithSymbol);

					// update arithmetic model
					update_model(&mscContext->arithModels[ARITH_CODER_BITS], value);
				}
			}
		}
	}

	// flush arithmetic encoder
	flush_arithmetic_encoder(&pb);
	flush_put_bits(&pb);

	arithBytesEncoded = pb.buf_ptr - pb.buf;

	// alocate packet
	if ((retCode = ff_alloc_packet(avpkt, arithBytesEncoded + PACKET_HEADER_SIZE)) < 0) {
		return retCode;
	}

	avpkt->flags |= AV_PKT_FLAG_KEY;

	// store header data
	memcpy(avpkt->data, &rleBytesEncoded, 4);

	// store encoded data
	memcpy(avpkt->data + PACKET_HEADER_SIZE, mscContext->arithBuff, arithBytesEncoded);
	*got_packet_ptr = 1;

	return 0;
}

void get_blocks(MscEncoderContext *mscContext, const AVFrame *frame, int mb_x, int mb_y) {
    int linesize= frame->linesize[0];

    uint8_t *ptr_y = frame->data[0] + (mb_y * 16* linesize          ) + mb_x * 16;
    uint8_t *ptr_u = frame->data[1] + (mb_y * 8 * frame->linesize[1]) + mb_x * 8;
    uint8_t *ptr_v = frame->data[2] + (mb_y * 8 * frame->linesize[2]) + mb_x * 8;

    mscContext->m.dsp.get_pixels(mscContext->block[0], ptr_y                 , linesize);
    mscContext->m.dsp.get_pixels(mscContext->block[1], ptr_y              + 8, linesize);
    mscContext->m.dsp.get_pixels(mscContext->block[2], ptr_y + 8*linesize    , linesize);
    mscContext->m.dsp.get_pixels(mscContext->block[3], ptr_y + 8*linesize + 8, linesize);

    mscContext->m.dsp.get_pixels(mscContext->block[4], ptr_u, frame->linesize[1]);
    mscContext->m.dsp.get_pixels(mscContext->block[5], ptr_v, frame->linesize[2]);
}

void put_block(MscDecoderContext *mscContext, AVFrame *frame, int mb_x, int mb_y) {
	int linesize= frame->linesize[0];

	uint8_t *ptr_y = frame->data[0] + (mb_y * 16* linesize         ) + mb_x * 16;
	uint8_t *ptr_u = frame->data[1] + (mb_y * 8 * frame->linesize[1]) + mb_x * 8;
	uint8_t *ptr_v = frame->data[2] + (mb_y * 8 * frame->linesize[2]) + mb_x * 8;

	mscContext->m.dsp.put_pixels_clamped(mscContext->block[0], ptr_y                 , linesize);
	mscContext->m.dsp.put_pixels_clamped(mscContext->block[1], ptr_y              + 8, linesize);
	mscContext->m.dsp.put_pixels_clamped(mscContext->block[2], ptr_y + 8*linesize    , linesize);
	mscContext->m.dsp.put_pixels_clamped(mscContext->block[3], ptr_y + 8*linesize + 8, linesize);

	mscContext->m.dsp.put_pixels_clamped(mscContext->block[4], ptr_u, frame->linesize[1]);
	mscContext->m.dsp.put_pixels_clamped(mscContext->block[5], ptr_v, frame->linesize[2]);
}

static int encode_close(AVCodecContext *avctx) {
	MscEncoderContext * mscContext;

	mscContext = avctx->priv_data;

//	av_freep(&avctx->coded_frame);
	av_freep(&mscContext->rleBuff);
	av_freep(&mscContext->arithBuff);

	return 0;
}

AVCodec ff_msc_decoder = {
	.name = "msc",
	.type = AVMEDIA_TYPE_VIDEO,
	.id = CODEC_ID_MSC,
	.priv_data_size = sizeof(MscDecoderContext),
	.init =	decode_init,
	.close = decode_close,
	.decode = decode,
	.capabilities = CODEC_CAP_DR1,
	.pix_fmts =	(const enum PixelFormat[]) {PIX_FMT_YUV420P, PIX_FMT_NONE}, // force single thread decoding
	.long_name = NULL_IF_CONFIG_SMALL("Msc coder"),
};

AVCodec ff_msc_encoder = {
	.name = "msc",
	.type = AVMEDIA_TYPE_VIDEO,
	.id = CODEC_ID_MSC,
	.priv_data_size = sizeof(MscEncoderContext),
	.init = encode_init,
	.close = encode_close,
	.encode2 = encode_frame,
	.pix_fmts =	(const enum PixelFormat[]) {PIX_FMT_YUV420P, PIX_FMT_NONE},
	.long_name = NULL_IF_CONFIG_SMALL("Msc coder"),
};

