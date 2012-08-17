#include "avcodec.h"
#include "internal.h"
#include "libavutil/log.h"
#include "rle.h"
#include "msccoder_arith_model.h"

typedef struct MscDecoderContext {
	// decoder context
	uint8_t * buff;
	size_t buff_size;
	GetBitContext gb;
} MscDecoderContext;

typedef struct MscEncoderContext {
	uint8_t * buff, * buff2;
	size_t buff_size;
	PutBitContext pb;
} MscEncoderContext;

static int decode_init(AVCodecContext *avctx) {
	MscDecoderContext * mscContext;

	av_log(avctx, AV_LOG_INFO, "MSC decode init\n");

	avctx->pix_fmt = PIX_FMT_YUV420P;

	avctx->coded_frame = avcodec_alloc_frame();

	if (!avctx->coded_frame) {
		av_log(avctx, AV_LOG_ERROR, "Could not allocate frame.\n");
		return AVERROR(ENOMEM);
	}

	mscContext = avctx->priv_data;

	initialize_model();

	mscContext->buff_size = 50 * avctx->coded_width * avctx->coded_height;
	mscContext->buff  = av_malloc(mscContext->buff_size);

	if (!mscContext->buff) {
		av_log(avctx, AV_LOG_ERROR, "Could not allocate buffer.\n");
		return AVERROR(ENOMEM);
	}

	return 0;
}


static int decode(AVCodecContext * avctx, void *outdata, int *outdata_size, AVPacket *avpkt) {
	AVFrame *frame = avctx->coded_frame;
	const uint8_t *src = avpkt->data;
	int i, j, channelIndex, count, c, bytesEncoded;
	uint8_t * buff;
	MscDecoderContext * mscContext;
	MSC_CODER_ARITH_SYMBOL arithSymbol;

	mscContext = avctx->priv_data;
	buff = mscContext->buff;

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

	memcpy(&bytesEncoded, src, 4);

	init_get_bits(&mscContext->gb, src + 4, (avpkt->size - 4) * 8);

	initialize_arithmetic_decoder(&mscContext->gb);

	i = 0;
	while(i < bytesEncoded) {
		get_symbol_scale(&arithSymbol);
		count = get_current_count(&arithSymbol);
		c = convert_symbol_to_int(count, &arithSymbol);
		remove_symbol_from_stream(&mscContext->gb, &arithSymbol);
		update_model(c);
		buff[i++] = c;
	}

	// RLE decoding
	for (channelIndex = 0; channelIndex < 3; ++channelIndex) {
		int height = channelIndex == 0 ? avctx->height : (avctx->height + 1) >> 1;
		int width = channelIndex == 0 ? avctx->width : (avctx->width + 1) >> 1;
		int linesize = frame->linesize[channelIndex];
		for (i = 0; i < height; ++i) {
			uint8_t * row = frame->data[channelIndex] + linesize * i;
			uint8_t * rowEnd = row + width;

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

	*outdata_size = sizeof(AVFrame);
	*(AVFrame *) outdata = *frame;

	return avpkt->size;
}

static int decode_close(AVCodecContext *avctx) {
	MscEncoderContext * mscContext;
	mscContext = avctx->priv_data;

	av_log(avctx, AV_LOG_INFO, "MSC decode close\n");

	if (avctx->coded_frame->data[0])
		avctx->release_buffer(avctx, avctx->coded_frame);

	av_freep(&avctx->coded_frame);

	av_freep(&mscContext->buff);

	return 0;
}


static int encode_init(AVCodecContext *avctx) {
	MscEncoderContext * mscContext;

	avctx->coded_frame = avcodec_alloc_frame();

	if (!avctx->coded_frame) {
		av_log(avctx, AV_LOG_ERROR, "Could not allocate frame.\n");
		return AVERROR(ENOMEM);
	}

	mscContext = avctx->priv_data;
	mscContext->buff_size = 50 * avctx->coded_width * avctx->coded_height;
	mscContext->buff  = av_malloc(mscContext->buff_size);
	mscContext->buff2 = av_malloc(mscContext->buff_size);

	if (!mscContext->buff || !mscContext->buff2) {
		av_log(avctx, AV_LOG_ERROR, "Could not allocate buffer.\n");
		return AVERROR(ENOMEM);
	}

	initialize_model();

	av_log(avctx, AV_LOG_INFO, "MSC encoder initialization complete.\n");

	return 0;
}

static int encode_frame(AVCodecContext *avctx, AVPacket *avpkt,	const AVFrame *frame, int *got_packet_ptr) {
	MscEncoderContext * mscContext;
	uint8_t * buff, *endBuff;
	int ret, i, j, channelIndex, bytesEncoded, rleBytesEncoded;
	MSC_CODER_ARITH_SYMBOL arithSymbol;

	rleBytesEncoded = 0;

	mscContext = avctx->priv_data;
	buff = mscContext->buff;
	endBuff = mscContext->buff + mscContext->buff_size;

	avctx->coded_frame->reference = 0;
	avctx->coded_frame->key_frame = 1;
	avctx->coded_frame->pict_type = AV_PICTURE_TYPE_I;

	initialize_arithmetic_encoder();

	// initialize PutBitContext
	init_put_bits(&mscContext->pb, mscContext->buff2, mscContext->buff_size);

	// RLE encoding
	for (channelIndex = 0; channelIndex < 3; ++channelIndex) {
		int height = channelIndex == 0 ? avctx->height : (avctx->height + 1) >> 1;
		int width = channelIndex == 0 ? avctx->width : (avctx->width + 1) >> 1;
		int linesize = frame->linesize[channelIndex];
		for (i = 0; i < height; i++) {
			uint8_t * row = frame->data[channelIndex] + linesize * i;
			if ((bytesEncoded = ff_rle_encode(buff, endBuff - buff - 1, row, 1, width, 0, 0, 0x80, 0)) < 0) {
				return AVERROR_BUG;
			}

			// encode arith
			for (j = 0; j < bytesEncoded; ++j) {
				ret = buff[j];
				convert_int_to_symbol(ret, &arithSymbol);
				encode_symbol(&mscContext->pb, &arithSymbol);
				update_model(ret);
			}

			rleBytesEncoded += bytesEncoded;
			buff += bytesEncoded;
		}
	}

	flush_arithmetic_encoder(&mscContext->pb);
	flush_put_bits(&mscContext->pb);

	// calculating encoded bytes count
	bytesEncoded = (int) (mscContext->pb.buf_ptr - mscContext->buff2);

	if ((ret = ff_alloc_packet(avpkt, bytesEncoded + 4)) < 0) {
		return ret;
	}

	av_log(avctx, AV_LOG_INFO, "Encoded bytes in packet: %d\n", rleBytesEncoded);

	avpkt->flags |= AV_PKT_FLAG_KEY;
	memcpy(avpkt->data, &rleBytesEncoded, 4);
	memcpy(avpkt->data + 4, mscContext->buff2, bytesEncoded);
	*got_packet_ptr = 1;

	return 0;
}

static int encode_close(AVCodecContext *avctx) {
	MscEncoderContext * mscContext;

	mscContext = avctx->priv_data;

	av_freep(&avctx->coded_frame);
	av_freep(&mscContext->buff);
	av_freep(&mscContext->buff2);

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

