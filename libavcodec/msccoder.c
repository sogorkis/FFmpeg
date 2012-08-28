#include "msccoder.h"
#include "avcodec.h"
#include "rle.h"
#include "mpeg12data.h"

#include "libavutil/log.h"
#include "libavutil/imgutils.h"

const int ARITH_CODER_BITS = 9;

static const uint8_t scantab[64]={
    0x00,0x08,0x01,0x09,0x10,0x18,0x11,0x19,
    0x02,0x0A,0x03,0x0B,0x12,0x1A,0x13,0x1B,
    0x04,0x0C,0x05,0x0D,0x20,0x28,0x21,0x29,
    0x06,0x0E,0x07,0x0F,0x14,0x1C,0x15,0x1D,
    0x22,0x2A,0x23,0x2B,0x30,0x38,0x31,0x39,
    0x16,0x1E,0x17,0x1F,0x24,0x2C,0x25,0x2D,
    0x32,0x3A,0x33,0x3B,0x26,0x2E,0x27,0x2F,
    0x34,0x3C,0x35,0x3D,0x36,0x3E,0x37,0x3F,
};

void init_common(AVCodecContext *avctx, MscCodecContext *mscContext) {

	ff_dsputil_init(&mscContext->dsp, avctx);

	mscContext->mb_width   = (avctx->width  + 15) / 16;
	mscContext->mb_height  = (avctx->height + 15) / 16;

	/*
	 * Initialize arithmetic coder models. First model: arithModels[0],
	 * can store 0, 1 values (2^1). Second one arithModels[1], 0, 1, 2, 3
	 * (2^2) and so on.
	 */
	for (int i = 0; i < 10; ++i) {
		initialize_model(&mscContext->arithModels[i], i + 1);
		mscContext->arithModelAddValue[i] = pow(2, i) - 1;
	}
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

static void print_debug_int(AVCodecContext *avctx, uint16_t * block) {
	av_log(avctx, AV_LOG_INFO, "\n ");
	for (int i = 0; i < 64;) {
		av_log(avctx, AV_LOG_INFO, "%3d ", block[i++]);
		if (i % 8 == 0) {
			av_log(avctx, AV_LOG_INFO, "\n ");
		}
	}
}

static int quantize(DCTELEM *block, int *q_intra_matrix) {
	int lastNonZeroElement = -1;

	for (int i = 63; i > 0; --i) {
		const int index	= scantab[i];
		block[index]	= (block[index] * q_intra_matrix[index] + (1 << 15)) >> 16;

		if (lastNonZeroElement < 0 && block[index] != 0) {
			lastNonZeroElement = i;
		}
	}

	block[0] = (block[0] + 32) >> 6;

	return lastNonZeroElement;
}

static void dequantize(DCTELEM *block, MscCodecContext *mscContext) {
	DECLARE_ALIGNED(16, DCTELEM, tmp)[64];
	DCTELEM firstElemValue;

	firstElemValue = 8 * block[0];
	block[0] = 0;

	for (int i = 0; i < 64; ++i) {
		const int index	= scantab[i];
		tmp[mscContext->scantable.permutated[i]]= (block[index] * mscContext->intra_matrix[i]) >> 4;
	}

	tmp[0] = firstElemValue;
	for (int i = 0; i < 64; ++i) {
		block[i] = tmp[i];
	}
}

static int decode_init(AVCodecContext *avctx) {
	MscDecoderContext * mscDecoderContext;
	MscCodecContext * mscContext;
	AVFrame * p;
	const int scale = 1;

	mscDecoderContext	= avctx->priv_data;
	mscContext			= &mscDecoderContext->mscContext;

	init_common(avctx, &mscDecoderContext->mscContext);

	ff_init_scantable(mscContext->dsp.idct_permutation, &mscContext->scantable, scantab);
	avctx->pix_fmt = PIX_FMT_YUV420P;

	mscContext->inv_qscale = 8;

	for(int i = 0; i < 64; i++){
		int index= scantab[i];

		mscContext->intra_matrix[i]= 64 * scale * ff_mpeg1_default_intra_matrix[index] / mscContext->inv_qscale;
	}

	print_debug_int(avctx, mscContext->intra_matrix);

	// allocate frame
	p = avctx->coded_frame = avcodec_alloc_frame();
	if (!avctx->coded_frame) {
		av_log(avctx, AV_LOG_ERROR, "Could not allocate frame.\n");
		return AVERROR(ENOMEM);
	}

	p->qstride		= mscContext->mb_width;
	p->qscale_table	= av_malloc( p->qstride * mscContext->mb_height);
	p->quality		= (32 * scale + mscContext->inv_qscale / 2) / mscContext->inv_qscale;
	memset(p->qscale_table, p->quality, p->qstride * mscContext->mb_height);

	// allocate buffer
	mscDecoderContext->buffSize	= 6 * avctx->coded_width * avctx->coded_height;
	mscDecoderContext->buff		= av_malloc(mscDecoderContext->buffSize);

	if (!mscDecoderContext->buff) {
		av_log(avctx, AV_LOG_ERROR, "Could not allocate buffer.\n");
		return AVERROR(ENOMEM);
	}

	return 0;
}

static int encode_init(AVCodecContext *avctx) {
	MscEncoderContext * mscEncoderContext;
	MscCodecContext * mscContext;
	const int scale = 1;

	mscEncoderContext	= avctx->priv_data;
	mscContext			= &mscEncoderContext->mscContext;

	init_common(avctx, &mscEncoderContext->mscContext);

	if(avctx->global_quality == 0) avctx->global_quality= 4*FF_QUALITY_SCALE;

	mscContext->inv_qscale = (32*scale*FF_QUALITY_SCALE +  avctx->global_quality/2) / avctx->global_quality;

	for(int i = 0; i < 64; i++) {
		int q= 32*scale*ff_mpeg1_default_intra_matrix[i];
		mscContext->q_intra_matrix[i]= ((mscContext->inv_qscale << 16) + q/2) / q;
	}

	// allocate frame
	avctx->coded_frame = avcodec_alloc_frame();
	if (!avctx->coded_frame) {
		av_log(avctx, AV_LOG_ERROR, "Could not allocate frame.\n");
		return AVERROR(ENOMEM);
	}

	// allocate buffers
	mscEncoderContext->rleBuffSize		= 3 * avctx->coded_width;
	mscEncoderContext->arithBuffSize	= 6 * avctx->coded_width * avctx->coded_height;
	mscEncoderContext->rleBuff			= av_malloc(mscEncoderContext->rleBuffSize);
	mscEncoderContext->arithBuff		= av_malloc(mscEncoderContext->arithBuffSize);

	if (mscEncoderContext->rleBuff == NULL || mscEncoderContext->arithBuff == NULL) {
		av_log(avctx, AV_LOG_ERROR, "Could not allocate buffers.\n");
		return AVERROR(ENOMEM);
	}

	return 0;
}

static int decode(AVCodecContext * avctx, void *outdata, int *outdata_size, AVPacket *avpkt) {
	AVFrame *frame = avctx->coded_frame;
	uint32_t rleBytesEncoded;
	MscDecoderContext * mscDecoderContext;
	MscCodecContext * mscContext;
	GetBitContext gb;
	MscCoderArithSymbol arithSymbol;
	uint32_t count, value;

	mscDecoderContext = avctx->priv_data;
	mscContext = &mscDecoderContext->mscContext;

	if (frame->data[0]) {
		avctx->release_buffer(avctx, frame);
	}

	frame->reference = 0;
	if (avctx->get_buffer(avctx, frame) < 0) {
		av_log(avctx, AV_LOG_ERROR, "Could not allocate buffer.\n");
		return AVERROR(ENOMEM);
	}

	frame->key_frame = 1;
	frame->pict_type = AV_PICTURE_TYPE_I;

	// read header
	memcpy(&rleBytesEncoded, avpkt->data, 4);

	// init encoded data bit buffer
	init_get_bits(&gb, avpkt->data + PACKET_HEADER_SIZE, (avpkt->size - PACKET_HEADER_SIZE) * 8);

	initialize_arithmetic_decoder(&gb);

	for (int mb_x = 0; mb_x < mscContext->mb_width; mb_x++) {
		for (int mb_y = 0; mb_y < mscContext->mb_height; mb_y++) {

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

				dequantize(mscContext->block[n], mscContext);

				if (avctx->frame_number == 0 && mb_x == 0 && mb_y == 0) {
					av_log(avctx, AV_LOG_INFO, "Dequantized x=%d, y=%d, n=%d\n", mb_x, mb_y, n);
					print_debug_block(avctx, mscContext->block[n]);
				}
			}

			idct_put_block(mscDecoderContext, frame, mb_x, mb_y);

			if (avctx->frame_number == 0 && mb_x == 0 && mb_y == 0) {
				av_log(avctx, AV_LOG_INFO, "IDCT x=%d, y=%d, n=%d\n", mb_x, mb_y, 0);
				print_debug_block(avctx, mscContext->block[0]);
			}
		}
	}

	emms_c();

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

static int encode_frame(AVCodecContext *avctx, AVPacket *avpkt,	const AVFrame *frame, int *got_packet_ptr) {
	MscEncoderContext * mscEncoderContext;
	MscCodecContext * mscContext;
	uint32_t rleBytesEncoded, arithBytesEncoded;
	int retCode;
	PutBitContext pb;
	int mb_y, mb_x, value;
	MscCoderArithSymbol arithSymbol;

	// initialize arithmetic encoder registers
	initialize_arithmetic_encoder();

	mscEncoderContext = avctx->priv_data;
	mscContext = &mscEncoderContext->mscContext;

	init_put_bits(&pb, mscEncoderContext->arithBuff, mscEncoderContext->arithBuffSize);

	avctx->coded_frame->reference = 0;
	avctx->coded_frame->key_frame = 1;
	avctx->coded_frame->pict_type = AV_PICTURE_TYPE_I;

	for (mb_x = 0; mb_x < mscContext->mb_width; mb_x++) {
		for (mb_y = 0; mb_y < mscContext->mb_height; mb_y++) {
			get_blocks(mscEncoderContext, frame, mb_x, mb_y);

			for (int n = 0; n < 6; ++n) {

				if (avctx->frame_number == 0 && mb_x == 0 && mb_y == 0) {
					av_log(avctx, AV_LOG_INFO, "Block x=%d, y=%d, n=%d\n", mb_x, mb_y, n);
					print_debug_block(avctx, mscContext->block[n]);
				}

				mscContext->dsp.fdct(mscContext->block[n]);

				if (avctx->frame_number == 0 && mb_x == 0 && mb_y == 0) {
					av_log(avctx, AV_LOG_INFO, "DCT block x=%d, y=%d, n=%d\n", mb_x, mb_y, n);
					print_debug_block(avctx, mscContext->block[n]);
				}

				quantize(mscContext->block[n], mscContext->q_intra_matrix);

//				if (overflow) {
//					clip_coeffs(m, m->block[n], m->block_last_index[n]);
//					av_log(avctx, AV_LOG_WARNING, "Overflow detected, frame: %d, mb_x: %d, mb_y: %d, n: %d\n",
//							avctx->frame_number, mb_x, mb_y, n);
//				}

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

	emms_c();

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
	memcpy(avpkt->data + PACKET_HEADER_SIZE, mscEncoderContext->arithBuff, arithBytesEncoded);
	*got_packet_ptr = 1;

	return 0;
}

void get_blocks(MscEncoderContext *mscEncoderContext, const AVFrame *frame, int mb_x, int mb_y) {
	MscCodecContext *mscContext = &mscEncoderContext->mscContext;
    int linesize= frame->linesize[0];

    uint8_t *ptr_y = frame->data[0] + (mb_y * 16* linesize          ) + mb_x * 16;
    uint8_t *ptr_u = frame->data[1] + (mb_y * 8 * frame->linesize[1]) + mb_x * 8;
    uint8_t *ptr_v = frame->data[2] + (mb_y * 8 * frame->linesize[2]) + mb_x * 8;

    mscContext->dsp.get_pixels(mscContext->block[0], ptr_y                 , linesize);
    mscContext->dsp.get_pixels(mscContext->block[1], ptr_y              + 8, linesize);
    mscContext->dsp.get_pixels(mscContext->block[2], ptr_y + 8*linesize    , linesize);
    mscContext->dsp.get_pixels(mscContext->block[3], ptr_y + 8*linesize + 8, linesize);

    mscContext->dsp.get_pixels(mscContext->block[4], ptr_u, frame->linesize[1]);
    mscContext->dsp.get_pixels(mscContext->block[5], ptr_v, frame->linesize[2]);
}

void idct_put_block(MscDecoderContext *mscDecoderContext, AVFrame *frame, int mb_x, int mb_y) {
	MscCodecContext *mscContext = &mscDecoderContext->mscContext;
	int linesize= frame->linesize[0];

	uint8_t *ptr_y = frame->data[0] + (mb_y * 16* linesize         ) + mb_x * 16;
	uint8_t *ptr_u = frame->data[1] + (mb_y * 8 * frame->linesize[1]) + mb_x * 8;
	uint8_t *ptr_v = frame->data[2] + (mb_y * 8 * frame->linesize[2]) + mb_x * 8;

	mscContext->dsp.idct_put(ptr_y                 , linesize, mscContext->block[0]);
	mscContext->dsp.idct_put(ptr_y              + 8, linesize, mscContext->block[1]);
	mscContext->dsp.idct_put(ptr_y + 8*linesize    , linesize, mscContext->block[2]);
	mscContext->dsp.idct_put(ptr_y + 8*linesize + 8, linesize, mscContext->block[3]);

	mscContext->dsp.idct_put(ptr_u                 , frame->linesize[1], mscContext->block[4]);
	mscContext->dsp.idct_put(ptr_v                 , frame->linesize[2], mscContext->block[5]);
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

