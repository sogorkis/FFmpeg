/*
 * Copyright (c) 2003 Michael Niedermayer
 * Copyright (c) 2012 Jeremy Tran
 *
 * This file is part of FFmpeg.
 *
 * FFmpeg is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation; either version 2 of the License, or
 * (at your option) any later version.
 *
 * FFmpeg is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License along
 * with FFmpeg; if not, write to the Free Software Foundation, Inc.,
 * 51 Franklin Street, Fifth Floor, Boston, MA 02110-1301 USA.
 */

/**
 * @file
 * Apply a hue/saturation filter to the input video
 * Ported from MPlayer libmpcodecs/vf_hue.c.
 */

#include "libavutil/imgutils.h"
#include "libavutil/pixdesc.h"

#include "avfilter.h"
#include "formats.h"
#include "internal.h"
#include "video.h"

typedef struct {
    float    hue;
    float    saturation;
    int      hsub;
    int      vsub;
    int32_t hue_sin;
    int32_t hue_cos;
} HueContext;

static av_cold int init(AVFilterContext *ctx, const char *args)
{
    HueContext *hue = ctx->priv;
    float h = 0, s = 1;
    int n;
    char c1 = 0, c2 = 0;

    if (args) {
        n = sscanf(args, "%f%c%f%c", &h, &c1, &s, &c2);
        if (n != 0 && n != 1 && (n != 3 || c1 != ':')) {
            av_log(ctx, AV_LOG_ERROR,
                   "Invalid syntax for argument '%s': "
                   "must be in the form 'hue[:saturation]'\n", args);
            return AVERROR(EINVAL);
        }
    }

    if (s < -10 || s > 10) {
        av_log(ctx, AV_LOG_ERROR,
               "Invalid value for saturation %0.1f: "
               "must be included between range -10 and +10\n", s);
        return AVERROR(EINVAL);
    }

    /* Convert angle from degree to radian */
    hue->hue = h * M_PI / 180;
    hue->saturation = s;

    return 0;
}

static int query_formats(AVFilterContext *ctx)
{
    static const enum PixelFormat pix_fmts[] = {
        PIX_FMT_YUV444P,      PIX_FMT_YUV422P,
        PIX_FMT_YUV420P,      PIX_FMT_YUV411P,
        PIX_FMT_YUV410P,      PIX_FMT_YUV440P,
        PIX_FMT_YUVA420P,
        PIX_FMT_NONE
    };

    ff_set_common_formats(ctx, ff_make_format_list(pix_fmts));

    return 0;
}

static int config_props(AVFilterLink *inlink)
{
    HueContext *hue = inlink->dst->priv;
    const AVPixFmtDescriptor *desc = &av_pix_fmt_descriptors[inlink->format];

    hue->hsub = desc->log2_chroma_w;
    hue->vsub = desc->log2_chroma_h;
    /*
     * Scale the value to the norm of the resulting (U,V) vector, that is
     * the saturation.
     * This will be useful in the process_chrominance function.
     */
    hue->hue_sin = rint(sin(hue->hue) * (1 << 16) * hue->saturation);
    hue->hue_cos = rint(cos(hue->hue) * (1 << 16) * hue->saturation);

    return 0;
}

static void process_chrominance(uint8_t *udst, uint8_t *vdst, const int dst_linesize,
                                uint8_t *usrc, uint8_t *vsrc, const int src_linesize,
                                int w, int h,
                                const int32_t c, const int32_t s)
{
    int32_t u, v, new_u, new_v;
    int i;

    /*
     * If we consider U and V as the components of a 2D vector then its angle
     * is the hue and the norm is the saturation
     */
    while (h--) {
        for (i = 0; i < w; i++) {
            /* Normalize the components from range [16;140] to [-112;112] */
            u = usrc[i] - 128;
            v = vsrc[i] - 128;
            /*
             * Apply the rotation of the vector : (c * u) - (s * v)
             *                                    (s * u) + (c * v)
             * De-normalize the components (without forgetting to scale 128
             * by << 16)
             * Finally scale back the result by >> 16
             */
            new_u = ((c * u) - (s * v) + (1 << 15) + (128 << 16)) >> 16;
            new_v = ((s * u) + (c * v) + (1 << 15) + (128 << 16)) >> 16;

            /* Prevent a potential overflow */
            udst[i] = av_clip_uint8_c(new_u);
            vdst[i] = av_clip_uint8_c(new_v);
        }

        usrc += src_linesize;
        vsrc += src_linesize;
        udst += dst_linesize;
        vdst += dst_linesize;
    }
}

static int draw_slice(AVFilterLink *inlink, int y, int h, int slice_dir)
{
    HueContext        *hue    = inlink->dst->priv;
    AVFilterBufferRef *inpic  = inlink->cur_buf;
    AVFilterBufferRef *outpic = inlink->dst->outputs[0]->out_buf;
    uint8_t *inrow[3], *outrow[3]; // 0 : Y, 1 : U, 2 : V
    int plane;

    inrow[0]  = inpic->data[0]  + y * inpic->linesize[0];
    outrow[0] = outpic->data[0] + y * outpic->linesize[0];

    for (plane = 1; plane < 3; plane++) {
        inrow[plane]  = inpic->data[plane]  + (y >> hue->vsub) * inpic->linesize[plane];
        outrow[plane] = outpic->data[plane] + (y >> hue->vsub) * outpic->linesize[plane];
    }

    av_image_copy_plane(outrow[0], outpic->linesize[0],
                        inrow[0],  inpic->linesize[0],
                        inlink->w, inlink->h);

    process_chrominance(outrow[1], outrow[2], outpic->linesize[1],
                        inrow[1], inrow[2], inpic->linesize[1],
                        inlink->w >> hue->hsub, inlink->h >> hue->vsub,
                        hue->hue_cos, hue->hue_sin);

    return ff_draw_slice(inlink->dst->outputs[0], y, h, slice_dir);
}

AVFilter avfilter_vf_hue = {
    .name        = "hue",
    .description = NULL_IF_CONFIG_SMALL("Adjust the hue and saturation of the input video."),

    .priv_size = sizeof(HueContext),

    .init          = init,
    .query_formats = query_formats,

    .inputs = (const AVFilterPad[]) {
        {
            .name         = "default",
            .type         = AVMEDIA_TYPE_VIDEO,
            .draw_slice   = draw_slice,
            .config_props = config_props,
            .min_perms    = AV_PERM_READ,
        },
        { .name = NULL }
    },
    .outputs = (const AVFilterPad[]) {
        {
            .name         = "default",
            .type         = AVMEDIA_TYPE_VIDEO,
        },
        { .name = NULL }
    }
};
