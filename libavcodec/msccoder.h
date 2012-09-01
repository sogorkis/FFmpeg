

#ifndef MSCCODER_H
#define MSCCODER_H

#include "internal.h"
#include "get_bits.h"
#include "dsputil.h"
#include "msccoder_arith_model.h"

typedef struct MscCodecContext {
	MscCoderArithModel lastZeroCodingModel;
	MscCoderArithModel arithModelIndexCodingModel;
	MscCoderArithModel arithModels[10];
	int arithModelAddValue[10];

	DSPContext dsp;

	ScanTable scantable;
	uint16_t intra_matrix[64];
	int q_intra_matrix[64];

	int inv_qscale;

	int mb_width, mb_height;
	DECLARE_ALIGNED(16, DCTELEM, block)[6][64];
	DECLARE_ALIGNED(16, DCTELEM, tmpBlock)[6][64];

	AVFrame *referenceFrame;
} MscCodecContext;

typedef struct MscDecoderContext {
	MscCodecContext mscContext;
} MscDecoderContext;

typedef struct MscEncoderContext {
	MscCodecContext mscContext;

	uint8_t *arithBuff;
	size_t arithBuffSize;
//	uint8_t *rleBuff, *arithBuff;
//	size_t rleBuffSize, arithBuffSize;
} MscEncoderContext;

void init_common(AVCodecContext *avctx, MscCodecContext *mscContext);

void get_blocks(MscEncoderContext *mscContext, const AVFrame *frame, int mb_x, int mb_y, DCTELEM block[6][64]);

void idct_put_block(MscCodecContext *mscContext, AVFrame *frame, int mb_x, int mb_y);

void idct_add_block(MscCodecContext *mscContext, AVFrame *frame, int mb_x, int mb_y);

void diff_blocks(DCTELEM block[6][64], DCTELEM referenceBlock[6][64]);

void copy_macroblock(const AVFrame *source, AVFrame *dest, int mb_x, int mb_y);

#endif
