#include "msccoder.h"
#include "avcodec.h"
#include "rle.h"
#include "mpeg12data.h"

#include "libavutil/log.h"
#include "libavutil/imgutils.h"

const int ARITH_CODER_BITS = 8;

const uint16_t ff_jpeg_matrix[64] = {
	16, 11, 10, 16, 24,  40,  51,  61,
	12, 12, 14, 19, 26,  58,  60,  55,
	14, 13, 16, 24, 40,  57,  69,  56,
	14, 17, 22, 29, 51,  87,  80,  62,
	18, 22, 37, 56, 68,  109, 103, 77,
	24, 35, 55, 64, 81,  104, 113, 92,
	49, 64, 78, 87, 103, 121, 120, 101,
	72, 92, 95, 98, 112, 100, 103, 99
};

const uint8_t ff_scan_row[64] = {
	 0,  1,  2,  3,  4,  5,  6,  7,
	 8,  9, 10, 11, 12, 13, 14, 15,
	16, 17, 18, 19, 20, 21, 22, 23,
	24, 25, 26, 27, 28, 29, 30, 31,
	32, 33, 34, 35, 36, 37, 38, 39,
	40, 41, 42, 43, 44, 45, 46, 47,
	48, 49, 50, 51, 52, 53, 54, 55,
	56, 57, 58, 59, 60, 61, 62, 63
};

const uint8_t ff_scan_column[64] = {
	 0,  8, 16, 24, 32, 40, 48, 56,
	 1,  9, 17, 25, 33, 41, 49, 57,
	 2, 10, 18, 26, 34, 42, 50, 58,
	 3, 11, 19, 27, 35, 43, 51, 59,
	 4, 12, 20, 28, 36, 44, 52, 60,
	 5, 13, 21, 29, 37, 45, 53, 61,
	 6, 14, 22, 30, 38, 46, 54, 62,
	 7, 15, 23, 31, 39, 47, 55, 63
};

//const uint16_t * quant_intra_matrix = ff_mpeg1_default_intra_matrix;
const uint16_t * quant_intra_matrix = ff_jpeg_matrix;
//const uint16_t * quant_intra_matrix = ff_mpeg1_default_non_intra_matrix;;

//static const uint8_t *scantab = ff_scan_row;
//static const uint8_t *scantab = ff_scan_column;
static const uint8_t *scantab = ff_zigzag_direct;
//static const uint8_t *scantab = ff_alternate_vertical_scan;

void init_common(AVCodecContext *avctx, MscCodecContext *mscContext) {

	ff_dsputil_init(&mscContext->dsp, avctx);

	mscContext->mb_width   = (avctx->width  + 15) / 16;
	mscContext->mb_height  = (avctx->height + 15) / 16;

	initialize_model(&mscContext->arithModelIndexCodingModel, 4);
	initialize_model(&mscContext->lastZeroCodingModel, 6);
	/*
	 * Initialize arithmetic coder models. First model: arithModels[0],
	 * can store 0, 1 values (2^1). Second one arithModels[1], 0, 1, 2, 3
	 * (2^2) and so on.
	 */
	for (int i = 0; i < 10; ++i) {
		initialize_model(&mscContext->arithModels[i], i + 1);
		mscContext->arithModelAddValue[i] = pow(2, i) - 1;
	}

	mscContext->referenceFrame = avcodec_alloc_frame();
	if (!mscContext->referenceFrame) {
		av_log(avctx, AV_LOG_ERROR, "Could not allocate frame.\n");
	}
}

static int isKeyFrame(int frameIndex) {
	return frameIndex % 25 == 0;
}

/*
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
*/

static int get_arith_model_index(int value) {
	if (value < 4) {
		return 2;
	}
	else if (value < 8) {
		return 3;
	}
	else if (value < 16) {
		return 4;
	}
	else if (value < 32) {
		return 5;
	}
	else if (value < 64) {
		return 6;
	}
	else if (value < 128) {
		return 7;
	}
	else if (value < 256) {
		return 8;
	}
	else if (value < 512) {
		return 9;
	}
	else if (value < 1024) {
		return 10;
	}
	return -1;
}

static void encode_arith_symbol(MscCoderArithModel *arithModel, PutBitContext *pb, int value) {
	MscCoderArithSymbol arithSymbol;

	// convert value to range symbol
	convert_int_to_symbol(arithModel, value, &arithSymbol);

	// encode symbol by arith encoder
	encode_symbol(pb, &arithSymbol);

	// update arithmetic model
	update_model(arithModel, value);
}

static int decode_arith_symbol(MscCoderArithModel *arithModel, GetBitContext *gb) {
	MscCoderArithSymbol arithSymbol;
	int count, value;

	// get range symbol
	get_symbol_scale(arithModel, &arithSymbol);

	// get value for symbol
	count = get_current_count(&arithSymbol);
	value = convert_symbol_to_int(arithModel, count, &arithSymbol);

	// remove symbol from stream
	remove_symbol_from_stream(gb, &arithSymbol);

	// update arithmetic coder model
	update_model(arithModel, value);

	return value;
}

static int quantize(DCTELEM *block, int *q_intra_matrix, int *maxAbsElement) {
	int lastNonZeroElement = -1;
	*maxAbsElement = 0;

	for (int i = 63; i > 0; --i) {
		const int index	= scantab[i];
		block[index]	= (block[index] * q_intra_matrix[index] + (1 << 15)) >> 16;

		*maxAbsElement = FFMAX(*maxAbsElement, FFABS(block[index]));

		if (lastNonZeroElement < 0 && block[index] != 0) {
			lastNonZeroElement = i;
		}
	}

	block[0] = (block[0] + 32) >> 6;

	return lastNonZeroElement < 0 ? 0 : lastNonZeroElement;
}

static void dequantize(DCTELEM *block, MscCodecContext *mscContext, int intraMatrix) {
	DECLARE_ALIGNED(16, DCTELEM, tmp)[64];
	DCTELEM firstElemValue;
	uint16_t *qmatrix = intraMatrix ? mscContext->intra_matrix : mscContext->non_intra_matrix;

	firstElemValue = 8 * block[0];
	block[0] = 0;

	for (int i = 0; i < 64; ++i) {
		const int index	= scantab[i];
		tmp[mscContext->scantable.permutated[i]]= (block[index] * qmatrix[i]) >> 4;
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

	if(avctx->extradata_size < 1 || (mscContext->inv_qscale= avctx->extradata[0]) == 0) {
		av_log(avctx, AV_LOG_ERROR, "illegal qscale 0\n");
		mscContext->inv_qscale = 8;
	}

	for(int i = 0; i < 64; i++){
		int index= scantab[i];

		mscContext->intra_matrix[i]= 64 * scale * quant_intra_matrix[index] / mscContext->inv_qscale;
		mscContext->non_intra_matrix[i]= 64 * scale * ff_mpeg1_default_non_intra_matrix[index] / mscContext->inv_qscale;
	}

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
		int q= 32*scale * quant_intra_matrix[i];
		int qNonIntra= 32*scale * ff_mpeg1_default_non_intra_matrix[i];
		mscContext->q_intra_matrix[i]= ((mscContext->inv_qscale << 16) + q/2) / q;
		mscContext->q_non_intra_matrix[i]= ((mscContext->inv_qscale << 16) + qNonIntra/2) / qNonIntra;
	}

	avctx->extradata= av_mallocz(8);
	avctx->extradata_size=8;
	((uint32_t*)avctx->extradata)[0]= av_le2ne32(mscContext->inv_qscale);
	((uint32_t*)avctx->extradata)[1]= av_le2ne32(AV_RL32("MSC0"));

	//check TODO
	ff_init_scantable(mscContext->dsp.idct_permutation, &mscContext->scantable, scantab);
	for(int i = 0; i < 64; i++){
			int index= scantab[i];
			mscContext->intra_matrix[i]= 64 * scale * quant_intra_matrix[index] / mscContext->inv_qscale;
			mscContext->non_intra_matrix[i]= 64 * scale * ff_mpeg1_default_non_intra_matrix[index] / mscContext->inv_qscale;
		}

	// allocate frame
	avctx->coded_frame = avcodec_alloc_frame();
	if (!avctx->coded_frame) {
		av_log(avctx, AV_LOG_ERROR, "Could not allocate frame.\n");
		return AVERROR(ENOMEM);
	}

	// allocate buffers
	mscEncoderContext->arithBuffSize	= 6 * avctx->coded_width * avctx->coded_height;
	mscEncoderContext->arithBuff		= av_malloc(mscEncoderContext->arithBuffSize);

	if (mscEncoderContext->arithBuff == NULL) {
		av_log(avctx, AV_LOG_ERROR, "Could not allocate buffer.\n");
		return AVERROR(ENOMEM);
	}

	return 0;
}

static int decode(AVCodecContext * avctx, void *outdata, int *outdata_size, AVPacket *avpkt) {
	AVFrame *frame = avctx->coded_frame;
	MscDecoderContext * mscDecoderContext;
	MscCodecContext * mscContext;
	GetBitContext gb;
	int lastNonZero, value, arithCoderIndex = -1, keyFrame;

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

	keyFrame = isKeyFrame(avctx->frame_number);

	if (avctx->frame_number == 0) {
		av_image_alloc(mscContext->referenceFrame->data, mscContext->referenceFrame->linesize, frame->width, frame->height, PIX_FMT_YUV420P, 128);
	}

	if (!keyFrame) {
		av_image_copy(frame->data, frame->linesize, mscContext->referenceFrame->data,
				mscContext->referenceFrame->linesize, PIX_FMT_YUV420P, frame->width, frame->height);
	}

	frame->key_frame = 1;
	frame->pict_type = AV_PICTURE_TYPE_I;

	// init encoded data bit buffer
	init_get_bits(&gb, avpkt->data, avpkt->size * 8);

	initialize_arithmetic_decoder(&gb);

	for (int mb_x = 0; mb_x < mscContext->mb_width; mb_x++) {
		for (int mb_y = 0; mb_y < mscContext->mb_height; mb_y++) {

			for (int n = 0; n < 6; ++n) {

				mscContext->dsp.clear_block(mscContext->block[n]);

				lastNonZero = decode_arith_symbol(&mscContext->lastZeroCodingModel, &gb);

				if (lastNonZero > 0) {
					arithCoderIndex = decode_arith_symbol(&mscContext->arithModelIndexCodingModel, &gb);
				}

				for (int i = 0; i <= lastNonZero; ++i) {
					int arithCoderBits = i == 0 ? ARITH_CODER_BITS : arithCoderIndex;

					value = decode_arith_symbol(&mscContext->arithModels[arithCoderBits], &gb);

					mscContext->block[n][scantab[i]] = value - mscContext->arithModelAddValue[arithCoderBits];
				}

//				if (avctx->frame_number == 0 && mb_x == 3 && mb_y == 0) {
//					av_log(avctx, AV_LOG_INFO, "Quantized x=%d, y=%d, n=%d\n", mb_x, mb_y, n);
//					print_debug_block(avctx, mscContext->block[n]);
//				}

				dequantize(mscContext->block[n], mscContext, keyFrame);

//				if (avctx->frame_number == 0 && mb_x == 0 && mb_y == 0) {
//					av_log(avctx, AV_LOG_INFO, "Dequantized x=%d, y=%d, n=%d\n", mb_x, mb_y, n);
//					print_debug_block(avctx, mscContext->block[n]);
//				}
			}

//			if (avctx->frame_number == 0 && mb_x == 0 && mb_y == 0) {
//				av_log(avctx, AV_LOG_INFO, "IDCT x=%d, y=%d, n=%d\n", mb_x, mb_y, 0);
//				print_debug_block(avctx, mscContext->block[0]);
//			}
			if (keyFrame) {
				idct_put_block(mscContext, frame, mb_x, mb_y);
			}
			else {
				idct_add_block(mscContext, frame, mb_x, mb_y);
			}

			copy_macroblock(frame, mscContext->referenceFrame, mb_x, mb_y);
		}
	}

	emms_c();

	*outdata_size = sizeof(AVFrame);
	*(AVFrame *) outdata = *frame;

	return avpkt->size;
}

static int decode_close(AVCodecContext *avctx) {
	if (avctx->coded_frame->data[0])
		avctx->release_buffer(avctx, avctx->coded_frame);

//	av_freep(&avctx->coded_frame);

	return 0;
}

static int encode_frame(AVCodecContext *avctx, AVPacket *avpkt,	const AVFrame *frame, int *got_packet_ptr) {
	MscEncoderContext * mscEncoderContext;
	MscCodecContext * mscContext;
	uint32_t arithBytesEncoded;
	PutBitContext pb;
	int mb_y, mb_x, value, lastNonZero, max, arithCoderIndex = -1, keyFrame;

	// initialize arithmetic encoder registers
	initialize_arithmetic_encoder();

	mscEncoderContext = avctx->priv_data;
	mscContext = &mscEncoderContext->mscContext;

	init_put_bits(&pb, mscEncoderContext->arithBuff, mscEncoderContext->arithBuffSize);

	keyFrame = isKeyFrame(avctx->frame_number);

	if (avctx->frame_number == 0) {
		av_image_alloc(mscContext->referenceFrame->data, mscContext->referenceFrame->linesize, frame->width, frame->height, frame->format, 128);
	}

	avctx->coded_frame->reference = 0;
	avctx->coded_frame->key_frame = 1;
	avctx->coded_frame->pict_type = AV_PICTURE_TYPE_I;

	int * qmatrix = keyFrame ? mscContext->q_intra_matrix : mscContext->q_non_intra_matrix;

	for (mb_x = 0; mb_x < mscContext->mb_width; mb_x++) {
		for (mb_y = 0; mb_y < mscContext->mb_height; mb_y++) {
			get_blocks(mscEncoderContext, frame, mb_x, mb_y, mscContext->block);

			if (!keyFrame) {
				get_blocks(mscEncoderContext, mscContext->referenceFrame, mb_x, mb_y, mscContext->tmpBlock);

				diff_blocks(mscContext->block, mscContext->tmpBlock);
			}

			for (int n = 0; n < 6; ++n) {

//				if (avctx->frame_number == 1 && mb_x == 0 && mb_y == 0) {
//					av_log(avctx, AV_LOG_INFO, "Block x=%d, y=%d, n=%d\n", mb_x, mb_y, n);
//					print_debug_block(avctx, mscContext->block[n]);
//				}

				mscContext->dsp.fdct(mscContext->block[n]);

//				if (avctx->frame_number == 0 && mb_x == 0 && mb_y == 0) {
//					av_log(avctx, AV_LOG_INFO, "DCT block x=%d, y=%d, n=%d\n", mb_x, mb_y, n);
//					print_debug_block(avctx, mscContext->block[n]);
//				}

				lastNonZero = quantize(mscContext->block[n], qmatrix, &max);

				av_assert1(lastNonZero < 64);

//				if (overflow) {
//					clip_coeffs(m, m->block[n], m->block_last_index[n]);
//					av_log(avctx, AV_LOG_WARNING, "Overflow detected, frame: %d, mb_x: %d, mb_y: %d, n: %d\n",
//							avctx->frame_number, mb_x, mb_y, n);
//				}

//				if (avctx->frame_number == 0 && mb_x == 3 && mb_y == 0) {
//					av_log(avctx, AV_LOG_INFO, "DCT quantized block x=%d, y=%d, n=%d\n", mb_x, mb_y, n);
//					print_debug_block(avctx, mscContext->block[n]);
//				}

				encode_arith_symbol(&mscContext->lastZeroCodingModel, &pb, lastNonZero);

				if (lastNonZero > 0) {
					arithCoderIndex = get_arith_model_index(max);

					encode_arith_symbol(&mscContext->arithModelIndexCodingModel, &pb, arithCoderIndex);
				}

				for (int i = 0; i <= lastNonZero; ++i) {
					int arithCoderBits = i == 0 ? ARITH_CODER_BITS : arithCoderIndex;

					value = mscContext->block[n][scantab[i]] + mscContext->arithModelAddValue[arithCoderBits];

			        encode_arith_symbol(&mscContext->arithModels[arithCoderBits], &pb, value);
				}

				dequantize(mscContext->block[n], mscContext, keyFrame);
			}

			if (keyFrame) {
				idct_put_block(mscContext, mscContext->referenceFrame, mb_x, mb_y);
			}
			else {
				idct_add_block(mscContext, mscContext->referenceFrame, mb_x, mb_y);
			}
		}
	}

	emms_c();

	// flush arithmetic encoder
	flush_arithmetic_encoder(&pb);
	flush_put_bits(&pb);

	arithBytesEncoded = pb.buf_ptr - pb.buf;

	// alocate packet
	if ((value = ff_alloc_packet(avpkt, arithBytesEncoded)) < 0) {
		return value;
	}

	avpkt->flags |= AV_PKT_FLAG_KEY;

	// store encoded data
	memcpy(avpkt->data, mscEncoderContext->arithBuff, arithBytesEncoded);
	*got_packet_ptr = 1;

	return 0;
}

void get_blocks(MscEncoderContext *mscEncoderContext, const AVFrame *frame, int mb_x, int mb_y, DCTELEM block[6][64]) {
	MscCodecContext *mscContext = &mscEncoderContext->mscContext;
    int linesize= frame->linesize[0];

    uint8_t *ptr_y = frame->data[0] + (mb_y * 16* linesize          ) + mb_x * 16;
    uint8_t *ptr_u = frame->data[1] + (mb_y * 8 * frame->linesize[1]) + mb_x * 8;
    uint8_t *ptr_v = frame->data[2] + (mb_y * 8 * frame->linesize[2]) + mb_x * 8;

    mscContext->dsp.get_pixels(block[0], ptr_y                 , linesize);
    mscContext->dsp.get_pixels(block[1], ptr_y              + 8, linesize);
    mscContext->dsp.get_pixels(block[2], ptr_y + 8*linesize    , linesize);
    mscContext->dsp.get_pixels(block[3], ptr_y + 8*linesize + 8, linesize);

    mscContext->dsp.get_pixels(block[4], ptr_u, frame->linesize[1]);
    mscContext->dsp.get_pixels(block[5], ptr_v, frame->linesize[2]);
}

void idct_put_block(MscCodecContext *mscContext, AVFrame *frame, int mb_x, int mb_y) {
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

void idct_add_block(MscCodecContext *mscContext, AVFrame *frame, int mb_x, int mb_y) {
	int linesize= frame->linesize[0];

	uint8_t *ptr_y = frame->data[0] + (mb_y * 16* linesize         ) + mb_x * 16;
	uint8_t *ptr_u = frame->data[1] + (mb_y * 8 * frame->linesize[1]) + mb_x * 8;
	uint8_t *ptr_v = frame->data[2] + (mb_y * 8 * frame->linesize[2]) + mb_x * 8;

	mscContext->dsp.idct_add(ptr_y                 , linesize, mscContext->block[0]);
	mscContext->dsp.idct_add(ptr_y              + 8, linesize, mscContext->block[1]);
	mscContext->dsp.idct_add(ptr_y + 8*linesize    , linesize, mscContext->block[2]);
	mscContext->dsp.idct_add(ptr_y + 8*linesize + 8, linesize, mscContext->block[3]);

	mscContext->dsp.idct_add(ptr_u                 , frame->linesize[1], mscContext->block[4]);
	mscContext->dsp.idct_add(ptr_v                 , frame->linesize[2], mscContext->block[5]);
}

void diff_blocks(DCTELEM block[6][64], DCTELEM referenceBlock[6][64]) {
	for (int i = 0; i < 6; ++i) {
		for (int j = 0; j < 64; ++j) {
			block[i][j] -= referenceBlock[i][j];
		}
	}
}

void copy_macroblock(const AVFrame *src, AVFrame *dst, int mb_x, int mb_y) {
	int linesize_src = src->linesize[0];
	int linesize_dst = dst->linesize[0];

	const uint8_t *ptr_y_src = src->data[0] + (mb_y * 16* linesize_src    ) + mb_x * 16;
	const uint8_t *ptr_u_src = src->data[1] + (mb_y * 8 * src->linesize[1]) + mb_x * 8;
	const uint8_t *ptr_v_src = src->data[2] + (mb_y * 8 * src->linesize[2]) + mb_x * 8;

	uint8_t *ptr_y_dst = dst->data[0] + (mb_y * 16* linesize_dst    ) + mb_x * 16;
	uint8_t *ptr_u_dst = dst->data[1] + (mb_y * 8 * dst->linesize[1]) + mb_x * 8;
	uint8_t *ptr_v_dst = dst->data[2] + (mb_y * 8 * dst->linesize[2]) + mb_x * 8;

	copy_block8(ptr_y_dst                     , ptr_y_src                     , linesize_dst, linesize_src, 8);
	copy_block8(ptr_y_dst                  + 8, ptr_y_src                  + 8, linesize_dst, linesize_src, 8);
	copy_block8(ptr_y_dst + 8*linesize_dst    , ptr_y_src + 8*linesize_src    , linesize_dst, linesize_src, 8);
	copy_block8(ptr_y_dst + 8*linesize_dst + 8, ptr_y_src + 8*linesize_src + 8, linesize_dst, linesize_src, 8);

	copy_block8(ptr_u_dst, ptr_u_src, dst->linesize[1], src->linesize[1], 8);
	copy_block8(ptr_v_dst, ptr_v_src, dst->linesize[2], src->linesize[2], 8);
}

static int encode_close(AVCodecContext *avctx) {
	MscEncoderContext * mscContext;

	mscContext = avctx->priv_data;

//	av_freep(&avctx->coded_frame);
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

