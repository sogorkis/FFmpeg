#include "avcodec.h"
#include "internal.h"
#include "libavutil/log.h"
#include "rle.h"
#include "msccoder_arith_model.h"

#define PACKET_HEADER_SIZE 4

void decode_rle_arith_frame(
		AVFrame *frame,
		int width,
		int height,
		uint8_t * buff,
		uint32_t rleBytesEncoded,
		GetBitContext *gb);

/**
 * Performs rle + arithmetic encoding.
 *
 * Returns number of bytes after RLE encoding.
 */
uint32_t encode_rle_arith(
		const AVFrame *frame,
		int width,
		int height,
		uint8_t * rleBuff,
		uint8_t * arithBuff,
		size_t rleBuffSize,
		size_t arithBuffSize,
		uint32_t *arithBytesEncoded);

typedef struct MscDecoderContext {
	uint8_t * buff;
	size_t buffSize;
} MscDecoderContext;

typedef struct MscEncoderContext {
	uint8_t *rleBuff, *arithBuff;
	size_t rleBuffSize, arithBuffSize;
} MscEncoderContext;

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

	// initialize arithmetic coder model
	initialize_model();

	// allocate buffer
	mscContext->buffSize	= 6 * avctx->coded_width * avctx->coded_height;
	mscContext->buff		= av_malloc(mscContext->buffSize);

	if (!mscContext->buff) {
		av_log(avctx, AV_LOG_ERROR, "Could not allocate buffer.\n");
		return AVERROR(ENOMEM);
	}

	return 0;
}


static int decode(AVCodecContext * avctx, void *outdata, int *outdata_size, AVPacket *avpkt) {
	AVFrame *frame = avctx->coded_frame;
	uint32_t rleBytesEncoded;
	MscDecoderContext * mscContext;
	GetBitContext gb;

	mscContext = avctx->priv_data;

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

	// decode lossless
	decode_rle_arith_frame(frame, avctx->width, avctx->height, mscContext->buff, rleBytesEncoded, &gb);

	*outdata_size = sizeof(AVFrame);
	*(AVFrame *) outdata = *frame;

	return avpkt->size;
}

void decode_rle_arith_frame(
		AVFrame *frame,
		int width,
		int height,
		uint8_t * buff,
		uint32_t rleBytesEncoded,
		GetBitContext *gb) {
	uint32_t channelIndex, i, j, count, value;
	MscCoderArithSymbol arithSymbol;

	initialize_arithmetic_decoder(gb);

	// decode by arithmetic decoder
	i = 0;
	while(i < rleBytesEncoded) {
		// get range symbol
		get_symbol_scale(&arithSymbol);

		// get value for symbol
		count = get_current_count(&arithSymbol);
		value = convert_symbol_to_int(count, &arithSymbol);

		// remove symbol from stream
		remove_symbol_from_stream(gb, &arithSymbol);

		// update arithmetic coder model
		update_model(value);

		buff[i++] = value;
	}

	// RLE decoding row by row
	for (channelIndex = 0; channelIndex < 3; ++channelIndex) {
		int heightPlane = channelIndex == 0 ? height : (height + 1) >> 1;
		int widthPlane = channelIndex == 0 ? width : (width + 1) >> 1;
		int linesize = frame->linesize[channelIndex];
		for (i = 0; i < heightPlane; ++i) {
			uint8_t * row = frame->data[channelIndex] + linesize * i;
			uint8_t * rowEnd = row + widthPlane;

			while (row < rowEnd) {
				uint8_t run = *buff++;
				if (run > 0x80) {	// uncompressed
					run -= 0x80;
					for (j = 0; j < run; ++j) {
						*row++ = *buff++;
					}
				}
				else {				//compressed
					uint8_t level = *buff++;
					for (j = 0; j < run; ++j) {
						*row++ = level;
					}
				}
			}
		}
	}

}

static int decode_close(AVCodecContext *avctx) {
	MscDecoderContext * mscContext;
	mscContext = avctx->priv_data;

	if (avctx->coded_frame->data[0])
		avctx->release_buffer(avctx, avctx->coded_frame);

	av_freep(&avctx->coded_frame);
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
	initialize_model();

	return 0;
}

static int encode_frame(AVCodecContext *avctx, AVPacket *avpkt,	const AVFrame *frame, int *got_packet_ptr) {
	MscEncoderContext * mscContext;
	uint32_t rleBytesEncoded, arithBytesEncoded;
	int retCode;

	mscContext = avctx->priv_data;

	avctx->coded_frame->reference = 0;
	avctx->coded_frame->key_frame = 1;
	avctx->coded_frame->pict_type = AV_PICTURE_TYPE_I;

	// encode lossless
	rleBytesEncoded = encode_rle_arith(frame, frame->width, frame->height,
			mscContext->rleBuff, mscContext->arithBuff,
			mscContext->rleBuffSize, mscContext->arithBuffSize,
			&arithBytesEncoded);

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

uint32_t encode_rle_arith(
		const AVFrame *frame,
		int width,
		int height,
		uint8_t * rleBuff,
		uint8_t * arithBuff,
		size_t rleBuffSize,
		size_t arithBuffSize,
		uint32_t *arithBytesEncoded) {
	uint32_t channelIndex, rleBytesEncoded, i, j, value;
	MscCoderArithSymbol arithSymbol;
	PutBitContext pb;
	int bytesEncoded;

	// initialize arithmetic encoder registers
	initialize_arithmetic_encoder();

	// initialize PutBitContext
	init_put_bits(&pb, arithBuff, arithBuffSize);

	// RLE encoding row by row
	for (channelIndex = 0, rleBytesEncoded = 0; channelIndex < 3; ++channelIndex) {
		int heightPlane = channelIndex == 0 ? height : (height + 1) >> 1;
		int widthPlane = channelIndex == 0 ? width : (width + 1) >> 1;
		int linesize = frame->linesize[channelIndex];
		for (i = 0; i < heightPlane; i++) {
			uint8_t * row = frame->data[channelIndex] + linesize * i;
			if ((bytesEncoded = ff_rle_encode(rleBuff, rleBuffSize, row, 1, widthPlane, 0, 0, 0x80, 0)) < 0) {
				return AVERROR_BUG;
			}

			// encode by arithmetic coder
			for (j = 0; j < bytesEncoded; ++j) {
				value = rleBuff[j];

				// convert value to range symbol
				convert_int_to_symbol(value, &arithSymbol);

				// encode symbol by arith encoder
				encode_symbol(&pb, &arithSymbol);

				// update arithmetic model
				update_model(value);
			}

			// count number of bytes encoded by rle
			rleBytesEncoded += bytesEncoded;
		}
	}

	// flush arithmetic encoder
	flush_arithmetic_encoder(&pb);
	flush_put_bits(&pb);

	// store number of bytes after arithmetic encoding
	*arithBytesEncoded = pb.buf_ptr - pb.buf;

	return rleBytesEncoded;
}

static int encode_close(AVCodecContext *avctx) {
	MscEncoderContext * mscContext;

	mscContext = avctx->priv_data;

	av_freep(&avctx->coded_frame);
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

