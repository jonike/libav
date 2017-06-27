/*
 * Cinepak encoder (c) 2011 Tomas Härdin
 * http://titan.codemill.se/~tomhar/cinepakenc.patch
 *
 * Fixes and improvements, vintage decoders compatibility
 *  (c) 2013, 2014 Rl, Aetey Global Technologies AB

Permission is hereby granted, free of charge, to any person obtaining a
copy of this software and associated documentation files (the "Software"),
to deal in the Software without restriction, including without limitation
the rights to use, copy, modify, merge, publish, distribute, sublicense,
and/or sell copies of the Software, and to permit persons to whom the
Software is furnished to do so, subject to the following conditions:

The above copyright notice and this permission notice shall be included
in all copies or substantial portions of the Software.

THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL
THE AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR
OTHER LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE,
ARISING FROM, OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR
OTHER DEALINGS IN THE SOFTWARE.

 * TODO:
 * - optimize: color space conversion (move conversion to libswscale), ...
 * - implement options to set the min/max number of strips?
 * MAYBE:
 * - "optimally" split the frame into several non-regular areas
 *   using a separate codebook pair for each area and approximating
 *   the area by several rectangular strips (generally not full width ones)
 *   (use quadtree splitting? a simple fixed-granularity grid?)
 */

#include <string.h>

#include "libavutil/common.h"
#include "libavutil/internal.h"
#include "libavutil/intreadwrite.h"
#include "avcodec.h"
#include "libavutil/lfg.h"
#include "elbg.h"
#include "internal.h"

#include "libavutil/avassert.h"

#define CVID_HEADER_SIZE 10
#define STRIP_HEADER_SIZE 12
#define CHUNK_HEADER_SIZE 4

#define MB_SIZE 4           //4x4 MBs
#define MB_AREA (MB_SIZE*MB_SIZE)

#define VECTOR_MAX 6        //six or four entries per vector depending on format
#define CODEBOOK_MAX 256    //size of a codebook

//#define MAX_STRIPS  32      //Note: having fewer choices regarding the number of strips speeds up encoding (obviously)
#define MAX_STRIPS  3       // This seems to be max for vintage players! -- rl
// TODO: we might want to have a "vintage compatibilty" switch
#define MIN_STRIPS  1       //Note: having more strips speeds up encoding the frame (this is less obvious)
// MAX_STRIPS limits the maximum quality you can reach
//            when you want high quality on high resolutions,
// MIN_STRIPS limits the minimum efficiently encodable bit rate
//            on low resolutions
// the numbers are only used for brute force optimization for the first frame,
// for the following frames they are adaptively readjusted
// NOTE the decoder in ffmpeg has its own arbitrary limitation on the number
// of strips, currently 32

typedef enum {
    MODE_V1_ONLY = 0,
    MODE_V1_V4,
    MODE_MC,

    MODE_COUNT,
} CinepakMode;

typedef enum {
    ENC_V1,
    ENC_V4,
    ENC_SKIP,

    ENC_UNCERTAIN
} mb_encoding;

typedef struct {
    int v1_vector;                  //index into v1 codebook
    int v1_error;                   //error when using V1 encoding
    int v4_vector[4];               //indices into v4 codebook
    int v4_error;                   //error when using V4 encoding
    int skip_error;                 //error when block is skipped (aka copied from last frame)
    mb_encoding best_encoding;      //last result from calculate_mode_score()
} mb_info;

typedef struct {
    int v1_codebook[CODEBOOK_MAX*VECTOR_MAX];
    int v4_codebook[CODEBOOK_MAX*VECTOR_MAX];
    int v1_size;
    int v4_size;
    CinepakMode mode;
} strip_info;

typedef struct {
    AVCodecContext *avctx;
    unsigned char *pict_bufs[4], *strip_buf, *frame_buf;
    AVFrame *last_frame;
    AVFrame *best_frame;
    AVFrame *scratch_frame;
    AVFrame *input_frame;
    enum AVPixelFormat pix_fmt;
    int w, h;
    int frame_buf_size;
    int curframe, keyint;
    AVLFG randctx;
    uint64_t lambda;
    int *codebook_input;
    int *codebook_closest;
    mb_info *mb;                                //MB RD state
    int min_strips;          //the current limit
    int max_strips;          //the current limit
#ifdef CINEPAKENC_DEBUG
    mb_info *best_mb;                           //TODO: remove. only used for printing stats
    int num_v1_mode, num_v4_mode, num_mc_mode;
    int num_v1_encs, num_v4_encs, num_skips;
#endif
} CinepakEncContext;

static av_cold int cinepak_encode_init(AVCodecContext *avctx)
{
    CinepakEncContext *s = avctx->priv_data;
    int x, mb_count, strip_buf_size, frame_buf_size;

    if (avctx->width & 3 || avctx->height & 3) {
        av_log(avctx, AV_LOG_ERROR, "width and height must be multiples of four (got %ix%i)\n",
                avctx->width, avctx->height);
        return AVERROR(EINVAL);
    }

    if (!(s->last_frame = av_frame_alloc()))
        return AVERROR(ENOMEM);
    if (!(s->best_frame = av_frame_alloc()))
        goto enomem;
    if (!(s->scratch_frame = av_frame_alloc()))
        goto enomem;
    if (avctx->pix_fmt == AV_PIX_FMT_RGB24)
        if (!(s->input_frame = av_frame_alloc()))
            goto enomem;

    if (!(s->codebook_input = av_malloc(sizeof(int) * (avctx->pix_fmt == AV_PIX_FMT_RGB24 ? 6 : 4) * (avctx->width * avctx->height) >> 2)))
        goto enomem;

    if (!(s->codebook_closest = av_malloc(sizeof(int) * (avctx->width * avctx->height) >> 2)))
        goto enomem;

    for(x = 0; x < (avctx->pix_fmt == AV_PIX_FMT_RGB24 ? 4 : 3); x++)
        if(!(s->pict_bufs[x] = av_malloc((avctx->pix_fmt == AV_PIX_FMT_RGB24 ? 6 : 4) * (avctx->width * avctx->height) >> 2)))
            goto enomem;

    mb_count = avctx->width * avctx->height / MB_AREA;

    //the largest possible chunk is 0x31 with all MBs encoded in V4 mode
    //and full codebooks being replaced in INTER mode,
    // which is 34 bits per MB
    //and 2*256 extra flag bits per strip
    strip_buf_size = STRIP_HEADER_SIZE + 3 * CHUNK_HEADER_SIZE + 2 * VECTOR_MAX * CODEBOOK_MAX + 4 * (mb_count + (mb_count + 15) / 16) + (2 * CODEBOOK_MAX)/8;

    frame_buf_size = CVID_HEADER_SIZE + MAX_STRIPS * strip_buf_size;

    if (!(s->strip_buf = av_malloc(strip_buf_size)))
        goto enomem;

    if (!(s->frame_buf = av_malloc(frame_buf_size)))
        goto enomem;

    if (!(s->mb = av_malloc(mb_count*sizeof(mb_info))))
        goto enomem;

#ifdef CINEPAKENC_DEBUG
    if (!(s->best_mb = av_malloc(mb_count*sizeof(mb_info))))
        goto enomem;
#endif

    av_lfg_init(&s->randctx, 1);
    s->avctx = avctx;
    s->w = avctx->width;
    s->h = avctx->height;
    s->frame_buf_size = frame_buf_size;
    s->curframe = 0;
    s->keyint = avctx->keyint_min;
    s->pix_fmt = avctx->pix_fmt;

    //set up AVFrames
    s->last_frame->data[0]        = s->pict_bufs[0];
    s->last_frame->linesize[0]    = s->w;
    s->best_frame->data[0]        = s->pict_bufs[1];
    s->best_frame->linesize[0]    = s->w;
    s->scratch_frame->data[0]     = s->pict_bufs[2];
    s->scratch_frame->linesize[0] = s->w;

    if (s->pix_fmt == AV_PIX_FMT_RGB24) {
        s->last_frame->data[1]        = s->last_frame->data[0] + s->w * s->h;
        s->last_frame->data[2]        = s->last_frame->data[1] + ((s->w * s->h) >> 2);
        s->last_frame->linesize[1]    = s->last_frame->linesize[2] = s->w >> 1;

        s->best_frame->data[1]        = s->best_frame->data[0] + s->w * s->h;
        s->best_frame->data[2]        = s->best_frame->data[1] + ((s->w * s->h) >> 2);
        s->best_frame->linesize[1]    = s->best_frame->linesize[2] = s->w >> 1;

        s->scratch_frame->data[1]     = s->scratch_frame->data[0] + s->w * s->h;
        s->scratch_frame->data[2]     = s->scratch_frame->data[1] + ((s->w * s->h) >> 2);
        s->scratch_frame->linesize[1] = s->scratch_frame->linesize[2] = s->w >> 1;

        s->input_frame->data[0]       = s->pict_bufs[3];
        s->input_frame->linesize[0]   = s->w;
        s->input_frame->data[1]       = s->input_frame->data[0] + s->w * s->h;
        s->input_frame->data[2]       = s->input_frame->data[1] + ((s->w * s->h) >> 2);
        s->input_frame->linesize[1]   = s->input_frame->linesize[2] = s->w >> 1;
    }

    s->min_strips = MIN_STRIPS;
    s->max_strips = MAX_STRIPS;

#ifdef CINEPAKENC_DEBUG
    s->num_v1_mode = s->num_v4_mode = s->num_mc_mode = s->num_v1_encs = s->num_v4_encs = s->num_skips = 0;
#endif

    return 0;

enomem:
    av_frame_free(&s->last_frame);
    av_frame_free(&s->best_frame);
    av_frame_free(&s->scratch_frame);
    if (avctx->pix_fmt == AV_PIX_FMT_RGB24)
        av_frame_free(&s->input_frame);
    av_freep(&s->codebook_input);
    av_freep(&s->codebook_closest);
    av_freep(&s->strip_buf);
    av_freep(&s->frame_buf);
    av_freep(&s->mb);
#ifdef CINEPAKENC_DEBUG
    av_freep(&s->best_mb);
#endif

    for(x = 0; x < (avctx->pix_fmt == AV_PIX_FMT_RGB24 ? 4 : 3); x++)
        av_freep(&s->pict_bufs[x]);

    return AVERROR(ENOMEM);
}

static int64_t calculate_mode_score(CinepakEncContext *s, int h, strip_info *info, int report, int *training_set_v1_shrunk, int *training_set_v4_shrunk
#ifdef CINEPAK_REPORT_SERR
, int64_t *serr
#endif
)
{
    //score = FF_LAMBDA_SCALE * error + lambda * bits
    int x;
    int entry_size = s->pix_fmt == AV_PIX_FMT_RGB24 ? 6 : 4;
    int mb_count = s->w * h / MB_AREA;
    mb_info *mb;
    int64_t score1, score2, score3;
    int64_t ret = s->lambda * ((info->v1_size ? CHUNK_HEADER_SIZE + info->v1_size * entry_size : 0) +
                   (info->v4_size ? CHUNK_HEADER_SIZE + info->v4_size * entry_size : 0) +
                   CHUNK_HEADER_SIZE) << 3;

    //av_log(s->avctx, AV_LOG_INFO, "sizes %3i %3i -> %9lli score mb_count %i", info->v1_size, info->v4_size, (long long int)ret, mb_count);

#ifdef CINEPAK_REPORT_SERR
    *serr = 0;
#endif

    switch(info->mode) {
    case MODE_V1_ONLY:
        //one byte per MB
        ret += s->lambda * 8 * mb_count;

// while calculating we assume all blocks are ENC_V1
        for(x = 0; x < mb_count; x++) {
            mb = &s->mb[x];
            ret += FF_LAMBDA_SCALE * mb->v1_error;
#ifdef CINEPAK_REPORT_SERR
            *serr += mb->v1_error;
#endif
// this function is never called for report in MODE_V1_ONLY
//            if(!report)
            mb->best_encoding = ENC_V1;
        }

        break;
    case MODE_V1_V4:
        //9 or 33 bits per MB
        if(report) {
// no moves between the corresponding training sets are allowed
            *training_set_v1_shrunk = *training_set_v4_shrunk = 0;
            for(x = 0; x < mb_count; x++) {
                int mberr;
                mb = &s->mb[x];
                if(mb->best_encoding == ENC_V1)
                    score1 = s->lambda * 9  + FF_LAMBDA_SCALE * (mberr=mb->v1_error);
                else
                    score1 = s->lambda * 33 + FF_LAMBDA_SCALE * (mberr=mb->v4_error);
                ret += score1;
#ifdef CINEPAK_REPORT_SERR
                *serr += mberr;
#endif
            }
        } else { // find best mode per block
            for(x = 0; x < mb_count; x++) {
                mb = &s->mb[x];
                score1 = s->lambda * 9  + FF_LAMBDA_SCALE * mb->v1_error;
                score2 = s->lambda * 33 + FF_LAMBDA_SCALE * mb->v4_error;

                if(score1 <= score2) {
                    ret += score1;
#ifdef CINEPAK_REPORT_SERR
                    *serr += mb->v1_error;
#endif
                    mb->best_encoding = ENC_V1;
                } else {
                    ret += score2;
#ifdef CINEPAK_REPORT_SERR
                    *serr += mb->v4_error;
#endif
                    mb->best_encoding = ENC_V4;
                }
            }
        }

        break;
    case MODE_MC:
        //1, 10 or 34 bits per MB
        if(report) {
            int v1_shrunk = 0, v4_shrunk = 0;
            for(x = 0; x < mb_count; x++) {
                mb = &s->mb[x];
// it is OK to move blocks to ENC_SKIP here
// but not to any codebook encoding!
                score1 = s->lambda * 1  + FF_LAMBDA_SCALE * mb->skip_error;
                if(mb->best_encoding == ENC_SKIP) {
                    ret += score1;
#ifdef CINEPAK_REPORT_SERR
                    *serr += mb->skip_error;
#endif
                } else if(mb->best_encoding == ENC_V1) {
                    if((score2=s->lambda * 10 + FF_LAMBDA_SCALE * mb->v1_error) >= score1) {
                        mb->best_encoding = ENC_SKIP;
                        ++v1_shrunk;
                        ret += score1;
#ifdef CINEPAK_REPORT_SERR
                        *serr += mb->skip_error;
#endif
                    } else {
                        ret += score2;
#ifdef CINEPAK_REPORT_SERR
                        *serr += mb->v1_error;
#endif
                    }
                } else {
                    if((score3=s->lambda * 34 + FF_LAMBDA_SCALE * mb->v4_error) >= score1) {
                        mb->best_encoding = ENC_SKIP;
                        ++v4_shrunk;
                        ret += score1;
#ifdef CINEPAK_REPORT_SERR
                        *serr += mb->skip_error;
#endif
                    } else {
                        ret += score3;
#ifdef CINEPAK_REPORT_SERR
                        *serr += mb->v4_error;
#endif
                    }
                }
            }
            *training_set_v1_shrunk = v1_shrunk;
            *training_set_v4_shrunk = v4_shrunk;
        } else { // find best mode per block
            for(x = 0; x < mb_count; x++) {
                mb = &s->mb[x];
                score1 = s->lambda * 1  + FF_LAMBDA_SCALE * mb->skip_error;
                score2 = s->lambda * 10 + FF_LAMBDA_SCALE * mb->v1_error;
                score3 = s->lambda * 34 + FF_LAMBDA_SCALE * mb->v4_error;

                if(score1 <= score2 && score1 <= score3) {
                    ret += score1;
#ifdef CINEPAK_REPORT_SERR
                    *serr += mb->skip_error;
#endif
                    mb->best_encoding = ENC_SKIP;
                } else if(score2 <= score3) {
                    ret += score2;
#ifdef CINEPAK_REPORT_SERR
                    *serr += mb->v1_error;
#endif
                    mb->best_encoding = ENC_V1;
                } else {
                    ret += score3;
#ifdef CINEPAK_REPORT_SERR
                    *serr += mb->v4_error;
#endif
                    mb->best_encoding = ENC_V4;
                }
            }
        }

        break;
    }

    return ret;
}

static int write_chunk_header(unsigned char *buf, int chunk_type, int chunk_size)
{
    buf[0] = chunk_type;
    AV_WB24(&buf[1], chunk_size + CHUNK_HEADER_SIZE);
    return CHUNK_HEADER_SIZE;
}

static int encode_codebook(CinepakEncContext *s, int *codebook, int size, int chunk_type_yuv, int chunk_type_gray, unsigned char *buf)
{
    int x, y, ret, entry_size = s->pix_fmt == AV_PIX_FMT_RGB24 ? 6 : 4;
    int incremental_codebook_replacement_mode = 0; // hardcoded here,
                // the compiler should notice that this is a constant -- rl

    ret = write_chunk_header(buf,
          s->pix_fmt == AV_PIX_FMT_RGB24 ?
           chunk_type_yuv+(incremental_codebook_replacement_mode?1:0) :
           chunk_type_gray+(incremental_codebook_replacement_mode?1:0),
          entry_size * size
           + (incremental_codebook_replacement_mode?(size+31)/32*4:0) );

// we do codebook encoding according to the "intra" mode
// but we keep the "dead" code for reference in case we will want
// to use incremental codebook updates (which actually would give us
// "kind of" motion compensation, especially in 1 strip/frame case) -- rl
// (of course, the code will be not useful as-is)
    if(incremental_codebook_replacement_mode) {
        int flags = 0;
        int flagsind;
        for(x = 0; x < size; x++) {
            if(flags == 0) {
                flagsind = ret;
                ret += 4;
                flags = 0x80000000;
            } else
                flags = ((flags>>1) | 0x80000000);
            for(y = 0; y < entry_size; y++)
                buf[ret++] = codebook[y + x*entry_size] ^ (y >= 4 ? 0x80 : 0);
            if((flags&0xffffffff) == 0xffffffff) {
                AV_WB32(&buf[flagsind], flags);
                flags = 0;
            }
        }
        if(flags)
            AV_WB32(&buf[flagsind], flags);
    } else
        for(x = 0; x < size; x++)
            for(y = 0; y < entry_size; y++)
                buf[ret++] = codebook[y + x*entry_size] ^ (y >= 4 ? 0x80 : 0);

    return ret;
}

//sets out to the sub picture starting at (x,y) in in
static void get_sub_picture(CinepakEncContext *s, int x, int y, AVPicture *in, AVPicture *out)
{
    out->data[0] = in->data[0] + x + y * in->linesize[0];
    out->linesize[0] = in->linesize[0];

    if(s->pix_fmt == AV_PIX_FMT_RGB24) {
        out->data[1] = in->data[1] + (x >> 1) + (y >> 1) * in->linesize[1];
        out->linesize[1] = in->linesize[1];

        out->data[2] = in->data[2] + (x >> 1) + (y >> 1) * in->linesize[2];
        out->linesize[2] = in->linesize[2];
    }
}

//decodes the V1 vector in mb into the 4x4 MB pointed to by sub_pict
static void decode_v1_vector(CinepakEncContext *s, AVPicture *sub_pict, int v1_vector, strip_info *info)
{
    int entry_size = s->pix_fmt == AV_PIX_FMT_RGB24 ? 6 : 4;

    sub_pict->data[0][0] =
            sub_pict->data[0][1] =
            sub_pict->data[0][    sub_pict->linesize[0]] =
            sub_pict->data[0][1+  sub_pict->linesize[0]] = info->v1_codebook[v1_vector*entry_size];

    sub_pict->data[0][2] =
            sub_pict->data[0][3] =
            sub_pict->data[0][2+  sub_pict->linesize[0]] =
            sub_pict->data[0][3+  sub_pict->linesize[0]] = info->v1_codebook[v1_vector*entry_size+1];

    sub_pict->data[0][2*sub_pict->linesize[0]] =
            sub_pict->data[0][1+2*sub_pict->linesize[0]] =
            sub_pict->data[0][  3*sub_pict->linesize[0]] =
            sub_pict->data[0][1+3*sub_pict->linesize[0]] = info->v1_codebook[v1_vector*entry_size+2];

    sub_pict->data[0][2+2*sub_pict->linesize[0]] =
            sub_pict->data[0][3+2*sub_pict->linesize[0]] =
            sub_pict->data[0][2+3*sub_pict->linesize[0]] =
            sub_pict->data[0][3+3*sub_pict->linesize[0]] = info->v1_codebook[v1_vector*entry_size+3];

    if(s->pix_fmt == AV_PIX_FMT_RGB24) {
        sub_pict->data[1][0] =
            sub_pict->data[1][1] =
            sub_pict->data[1][    sub_pict->linesize[1]] =
            sub_pict->data[1][1+  sub_pict->linesize[1]] = info->v1_codebook[v1_vector*entry_size+4];

        sub_pict->data[2][0] =
            sub_pict->data[2][1] =
            sub_pict->data[2][    sub_pict->linesize[2]] =
            sub_pict->data[2][1+  sub_pict->linesize[2]] = info->v1_codebook[v1_vector*entry_size+5];
    }
}

//decodes the V4 vectors in mb into the 4x4 MB pointed to by sub_pict
static void decode_v4_vector(CinepakEncContext *s, AVPicture *sub_pict, int *v4_vector, strip_info *info)
{
    int i, x, y, entry_size = s->pix_fmt == AV_PIX_FMT_RGB24 ? 6 : 4;

    for(i = y = 0; y < 4; y += 2) {
        for(x = 0; x < 4; x += 2, i++) {
            sub_pict->data[0][x   +     y*sub_pict->linesize[0]] = info->v4_codebook[v4_vector[i]*entry_size];
            sub_pict->data[0][x+1 +     y*sub_pict->linesize[0]] = info->v4_codebook[v4_vector[i]*entry_size+1];
            sub_pict->data[0][x   + (y+1)*sub_pict->linesize[0]] = info->v4_codebook[v4_vector[i]*entry_size+2];
            sub_pict->data[0][x+1 + (y+1)*sub_pict->linesize[0]] = info->v4_codebook[v4_vector[i]*entry_size+3];

            if(s->pix_fmt == AV_PIX_FMT_RGB24) {
                sub_pict->data[1][(x>>1) + (y>>1)*sub_pict->linesize[1]] = info->v4_codebook[v4_vector[i]*entry_size+4];
                sub_pict->data[2][(x>>1) + (y>>1)*sub_pict->linesize[2]] = info->v4_codebook[v4_vector[i]*entry_size+5];
            }
        }
    }
}

static void copy_mb(CinepakEncContext *s, AVPicture *a, AVPicture *b)
{
    int y, p;

    for(y = 0; y < MB_SIZE; y++) {
        memcpy(a->data[0]+y*a->linesize[0], b->data[0]+y*b->linesize[0],
               MB_SIZE);
    }

    if(s->pix_fmt == AV_PIX_FMT_RGB24) {
        for(p = 1; p <= 2; p++) {
            for(y = 0; y < MB_SIZE/2; y++) {
                memcpy(a->data[p] + y*a->linesize[p],
                       b->data[p] + y*b->linesize[p],
                       MB_SIZE/2);
            }
        }
    }
}

static int encode_mode(CinepakEncContext *s, int h, AVPicture *scratch_pict, AVPicture *last_pict, strip_info *info, unsigned char *buf)
{
    int x, y, z, flags, bits, temp_size, header_ofs, ret = 0, mb_count = s->w * h / MB_AREA;
    int needs_extra_bit, should_write_temp;
    unsigned char temp[64]; //32/2 = 16 V4 blocks at 4 B each -> 64 B
    mb_info *mb;
    AVPicture sub_scratch, sub_last;

    //encode codebooks
////// MacOS vintage decoder compatibility dictates the presence of
////// the codebook chunk even when the codebook is empty - pretty dumb...
////// and also the certain order of the codebook chunks -- rl
//    if(info->v4_size)
        ret += encode_codebook(s, info->v4_codebook, info->v4_size, 0x20, 0x24, buf + ret);

//    if(info->v1_size)
        ret += encode_codebook(s, info->v1_codebook, info->v1_size, 0x22, 0x26, buf + ret);

    //update scratch picture
    for(z = y = 0; y < h; y += MB_SIZE) {
        for(x = 0; x < s->w; x += MB_SIZE, z++) {
            mb = &s->mb[z];

            get_sub_picture(s, x, y, scratch_pict, &sub_scratch);

            if(info->mode == MODE_MC && mb->best_encoding == ENC_SKIP) {
                get_sub_picture(s, x, y, last_pict, &sub_last);
                copy_mb(s, &sub_scratch, &sub_last);
            } else if(info->mode == MODE_V1_ONLY || mb->best_encoding == ENC_V1)
                decode_v1_vector(s, &sub_scratch, mb->v1_vector, info);
            else
                decode_v4_vector(s, &sub_scratch, mb->v4_vector, info);
        }
    }

    switch(info->mode) {
    case MODE_V1_ONLY:
        //av_log(s->avctx, AV_LOG_INFO, "mb_count = %i\n", mb_count);
        ret += write_chunk_header(buf + ret, 0x32, mb_count);

        for(x = 0; x < mb_count; x++)
            buf[ret++] = s->mb[x].v1_vector;

        break;
    case MODE_V1_V4:
        //remember header position
        header_ofs = ret;
        ret += CHUNK_HEADER_SIZE;

        for(x = 0; x < mb_count; x += 32) {
            flags = 0;
            for(y = x; y < FFMIN(x+32, mb_count); y++)
                if(s->mb[y].best_encoding == ENC_V4)
                    flags |= 1 << (31 - y + x);

            AV_WB32(&buf[ret], flags);
            ret += 4;

            for(y = x; y < FFMIN(x+32, mb_count); y++) {
                mb = &s->mb[y];

                if(mb->best_encoding == ENC_V1)
                    buf[ret++] = mb->v1_vector;
                else
                    for(z = 0; z < 4; z++)
                        buf[ret++] = mb->v4_vector[z];
            }
        }

        write_chunk_header(buf + header_ofs, 0x30, ret - header_ofs - CHUNK_HEADER_SIZE);

        break;
    case MODE_MC:
        //remember header position
        header_ofs = ret;
        ret += CHUNK_HEADER_SIZE;
        flags = bits = temp_size = 0;

        for(x = 0; x < mb_count; x++) {
            mb = &s->mb[x];
            flags |= (mb->best_encoding != ENC_SKIP) << (31 - bits++);
            needs_extra_bit = 0;
            should_write_temp = 0;

            if(mb->best_encoding != ENC_SKIP) {
                if(bits < 32)
                    flags |= (mb->best_encoding == ENC_V4) << (31 - bits++);
                else
                    needs_extra_bit = 1;
            }

            if(bits == 32) {
                AV_WB32(&buf[ret], flags);
                ret += 4;
                flags = bits = 0;

                if(mb->best_encoding == ENC_SKIP || needs_extra_bit) {
                    memcpy(&buf[ret], temp, temp_size);
                    ret += temp_size;
                    temp_size = 0;
                } else
                    should_write_temp = 1;
            }

            if(needs_extra_bit) {
                flags = (mb->best_encoding == ENC_V4) << 31;
                bits = 1;
            }

            if(mb->best_encoding == ENC_V1)
                temp[temp_size++] = mb->v1_vector;
            else if(mb->best_encoding == ENC_V4)
                for(z = 0; z < 4; z++)
                    temp[temp_size++] = mb->v4_vector[z];

            if(should_write_temp) {
                memcpy(&buf[ret], temp, temp_size);
                ret += temp_size;
                temp_size = 0;
            }
        }

        if(bits > 0) {
            AV_WB32(&buf[ret], flags);
            ret += 4;
            memcpy(&buf[ret], temp, temp_size);
            ret += temp_size;
        }

        write_chunk_header(buf + header_ofs, 0x31, ret - header_ofs - CHUNK_HEADER_SIZE);

        break;
    }

    return ret;
}

//computes distortion of 4x4 MB in b compared to a
static int compute_mb_distortion(CinepakEncContext *s, AVPicture *a, AVPicture *b)
{
    int x, y, p, d, ret = 0;

    for(y = 0; y < MB_SIZE; y++) {
        for(x = 0; x < MB_SIZE; x++) {
            d = a->data[0][x + y*a->linesize[0]] - b->data[0][x + y*b->linesize[0]];
            ret += d*d;
        }
    }

    if(s->pix_fmt == AV_PIX_FMT_RGB24) {
        for(p = 1; p <= 2; p++) {
            for(y = 0; y < MB_SIZE/2; y++) {
                for(x = 0; x < MB_SIZE/2; x++) {
                    d = a->data[p][x + y*a->linesize[p]] - b->data[p][x + y*b->linesize[p]];
                    ret += d*d;
                }
            }
        }
    }

    return ret;
}

// return the possibly adjusted size of the codebook
#define CERTAIN(x) ((x)!=ENC_UNCERTAIN)
static int quantize(CinepakEncContext *s, int h, AVPicture *pict,
                    int v1mode, strip_info *info,
                    mb_encoding encoding)
{
    int x, y, i, j, k, x2, y2, x3, y3, plane, shift, mbn;
    int entry_size = s->pix_fmt == AV_PIX_FMT_RGB24 ? 6 : 4;
    int *codebook = v1mode ? info->v1_codebook : info->v4_codebook;
    int size = v1mode ? info->v1_size : info->v4_size;
    int64_t total_error = 0;
    uint8_t vq_pict_buf[(MB_AREA*3)/2];
    AVPicture sub_pict, vq_pict;

    for(mbn = i = y = 0; y < h; y += MB_SIZE) {
        for(x = 0; x < s->w; x += MB_SIZE, ++mbn) {
            int *base;

            if(CERTAIN(encoding)) {
// use for the training only the blocks known to be to be encoded [sic:-]
               if(s->mb[mbn].best_encoding != encoding) continue;
            }

            base = s->codebook_input + i*entry_size;
            if(v1mode) {
                //subsample
                for(j = y2 = 0; y2 < entry_size; y2 += 2) {
                    for(x2 = 0; x2 < 4; x2 += 2, j++) {
                        plane = y2 < 4 ? 0 : 1 + (x2 >> 1);
                        shift = y2 < 4 ? 0 : 1;
                        x3 = shift ? 0 : x2;
                        y3 = shift ? 0 : y2;
                        base[j] = (pict->data[plane][((x+x3) >> shift) +      ((y+y3) >> shift)      * pict->linesize[plane]] +
                                   pict->data[plane][((x+x3) >> shift) + 1 +  ((y+y3) >> shift)      * pict->linesize[plane]] +
                                   pict->data[plane][((x+x3) >> shift) +     (((y+y3) >> shift) + 1) * pict->linesize[plane]] +
                                   pict->data[plane][((x+x3) >> shift) + 1 + (((y+y3) >> shift) + 1) * pict->linesize[plane]]) >> 2;
                    }
                }
            } else {
                //copy
                for(j = y2 = 0; y2 < MB_SIZE; y2 += 2) {
                    for(x2 = 0; x2 < MB_SIZE; x2 += 2) {
                        for(k = 0; k < entry_size; k++, j++) {
                            plane = k >= 4 ? k - 3 : 0;

                            if(k >= 4) {
                                x3 = (x+x2) >> 1;
                                y3 = (y+y2) >> 1;
                            } else {
                                x3 = x + x2 + (k & 1);
                                y3 = y + y2 + (k >> 1);
                            }

                            base[j] = pict->data[plane][x3 + y3*pict->linesize[plane]];
                        }
                    }
                }
            }
            i += v1mode ? 1 : 4;
        }
    }
//    if(i < mbn*(v1mode ? 1 : 4)) {
//        av_log(s->avctx, AV_LOG_INFO, "reducing training set for %s from %i to %i (encoding %i)\n", v1mode?"v1":"v4", mbn*(v1mode ? 1 : 4), i, encoding);
//    }

    if(i == 0) // empty training set, nothing to do
        return 0;
    if(i < size) {
        //av_log(s->avctx, (CERTAIN(encoding) ? AV_LOG_ERROR : AV_LOG_INFO), "WOULD WASTE: %s cbsize %i bigger than training set size %i (encoding %i)\n", v1mode?"v1":"v4", size, i, encoding);
        size = i;
    }

    ff_init_elbg(s->codebook_input, entry_size, i, codebook, size, 1, s->codebook_closest, &s->randctx);
    ff_do_elbg(s->codebook_input, entry_size, i, codebook, size, 1, s->codebook_closest, &s->randctx);

    //setup vq_pict, which contains a single MB
    vq_pict.data[0] = vq_pict_buf;
    vq_pict.linesize[0] = MB_SIZE;
    vq_pict.data[1] = &vq_pict_buf[MB_AREA];
    vq_pict.data[2] = vq_pict.data[1] + (MB_AREA >> 2);
    vq_pict.linesize[1] = vq_pict.linesize[2] = MB_SIZE >> 1;

    //copy indices
    for(i = j = y = 0; y < h; y += MB_SIZE) {
        for(x = 0; x < s->w; x += MB_SIZE, j++) {
            mb_info *mb = &s->mb[j];
// skip uninteresting blocks if we know their preferred encoding
            if(CERTAIN(encoding) && mb->best_encoding != encoding)
                continue;

            //point sub_pict to current MB
            get_sub_picture(s, x, y, pict, &sub_pict);

            if(v1mode) {
                mb->v1_vector = s->codebook_closest[i];

                //fill in vq_pict with V1 data
                decode_v1_vector(s, &vq_pict, mb->v1_vector, info);

                mb->v1_error = compute_mb_distortion(s, &sub_pict, &vq_pict);
                total_error += mb->v1_error;
            } else {
                for(k = 0; k < 4; k++)
                    mb->v4_vector[k] = s->codebook_closest[i+k];

                //fill in vq_pict with V4 data
                decode_v4_vector(s, &vq_pict, mb->v4_vector, info);

                mb->v4_error = compute_mb_distortion(s, &sub_pict, &vq_pict);
                total_error += mb->v4_error;
            }
            i += v1mode ? 1 : 4;
        }
    }
// check that we did it right in the beginning of the function
    av_assert0(i >= size); // training set is no smaller than the codebook

    //av_log(s->avctx, AV_LOG_INFO, "isv1 %i size= %i i= %i error %lli\n", v1mode, size, i, (long long int)total_error);

    return size;
}

static void calculate_skip_errors(CinepakEncContext *s, int h, AVPicture *last_pict, AVPicture *pict, strip_info *info)
{
    int x, y, i;
    AVPicture sub_last, sub_pict;

    for(i = y = 0; y < h; y += MB_SIZE) {
        for(x = 0; x < s->w; x += MB_SIZE, i++) {
            get_sub_picture(s, x, y, last_pict, &sub_last);
            get_sub_picture(s, x, y, pict,      &sub_pict);

            s->mb[i].skip_error = compute_mb_distortion(s, &sub_last, &sub_pict);
        }
    }
}

static void write_strip_header(CinepakEncContext *s, int y, int h, int keyframe, unsigned char *buf, int strip_size)
{
// actually we are exclusively using intra strip coding (how much can we win
// otherwise? how to choose which part of a codebook to update?),
// keyframes are different only because we disallow ENC_SKIP on them -- rl
// (besides, the logic here used to be inverted: )
//    buf[0] = keyframe ? 0x11: 0x10;
    buf[0] = keyframe ? 0x10: 0x11;
    AV_WB24(&buf[1], strip_size + STRIP_HEADER_SIZE);
//    AV_WB16(&buf[4], y); /* using absolute y values works -- rl */
    AV_WB16(&buf[4], 0); /* using relative values works as well -- rl */
    AV_WB16(&buf[6], 0);
//    AV_WB16(&buf[8], y+h); /* using absolute y values works -- rl */
    AV_WB16(&buf[8], h); /* using relative values works as well -- rl */
    AV_WB16(&buf[10], s->w);
    //av_log(s->avctx, AV_LOG_INFO, "write_strip_header() %x keyframe=%d\n", buf[0], keyframe);
}

static int rd_strip(CinepakEncContext *s, int y, int h, int keyframe, AVPicture *last_pict, AVPicture *pict, AVPicture *scratch_pict, unsigned char *buf, int64_t *best_score
#ifdef CINEPAK_REPORT_SERR
, int64_t *best_serr
#endif
)
{
    int64_t score = 0;
#ifdef CINEPAK_REPORT_SERR
    int64_t serr;
#endif
    int best_size = 0;
    strip_info info;
// for codebook optimization:
    int v1enough, v1_size, v4enough, v4_size;
    int new_v1_size, new_v4_size;
    int v1shrunk, v4shrunk;

    if(!keyframe)
        calculate_skip_errors(s, h, last_pict, pict, &info);

    //try some powers of 4 for the size of the codebooks
    //constraint the v4 codebook to be no bigger than v1 one,
    //(and no less than v1_size/4)
    //thus making v1 preferable and possibly losing small details? should be ok
#define SMALLEST_CODEBOOK 1
    for(v1enough = 0, v1_size = SMALLEST_CODEBOOK; v1_size <= CODEBOOK_MAX && !v1enough; v1_size <<= 2) {
        for(v4enough = 0, v4_size = 0; v4_size <= v1_size && !v4enough; v4_size = v4_size ? v4_size << 2 : v1_size >= SMALLEST_CODEBOOK << 2 ? v1_size >> 2 : SMALLEST_CODEBOOK) {
            //try all modes
            for(CinepakMode mode = 0; mode < MODE_COUNT; mode++) {
                //don't allow MODE_MC in intra frames
                if(keyframe && mode == MODE_MC)
                    continue;

                if(mode == MODE_V1_ONLY) {
                    info.v1_size = v1_size;
// the size may shrink even before optimizations if the input is short:
                    info.v1_size = quantize(s, h, pict, 1, &info, ENC_UNCERTAIN);
                    if(info.v1_size < v1_size)
// too few eligible blocks, no sense in trying bigger sizes
                        v1enough = 1;

                    info.v4_size = 0;
                } else { // mode != MODE_V1_ONLY
                    // if v4 codebook is empty then only allow V1-only mode
                    if(!v4_size)
                        continue;

                    if(mode == MODE_V1_V4) {
                        info.v4_size = v4_size;
                        info.v4_size = quantize(s, h, pict, 0, &info, ENC_UNCERTAIN);
                        if(info.v4_size < v4_size)
// too few eligible blocks, no sense in trying bigger sizes
                            v4enough = 1;
                    }
                }

                info.mode = mode;
// choose the best encoding per block, based on current experience
                score = calculate_mode_score(s, h, &info, 0,
                                             &v1shrunk, &v4shrunk
#ifdef CINEPAK_REPORT_SERR
, &serr
#endif
);

                if(mode != MODE_V1_ONLY){
// recompute the codebooks, omitting the extra blocks
// we assume we _may_ come here with more blocks to encode than before
                    info.v1_size = v1_size;
                    new_v1_size = quantize(s, h, pict, 1, &info, ENC_V1);
                    if(new_v1_size < info.v1_size){
                        //av_log(s->avctx, AV_LOG_INFO, "mode %i, %3i, %3i: cut v1 codebook to %i entries\n", mode, v1_size, v4_size, new_v1_size);
                        info.v1_size = new_v1_size;
                    }
// we assume we _may_ come here with more blocks to encode than before
                    info.v4_size = v4_size;
                    new_v4_size = quantize(s, h, pict, 0, &info, ENC_V4);
                    if(new_v4_size < info.v4_size) {
                        //av_log(s->avctx, AV_LOG_INFO, "mode %i, %3i, %3i: cut v4 codebook to %i entries at first iteration\n", mode, v1_size, v4_size, new_v4_size);
                        info.v4_size = new_v4_size;
                    }
// calculate the resulting score
// (do not move blocks to codebook encodings now, as some blocks may have
// got bigger errors despite a smaller training set - but we do not
// ever grow the training sets back)
                    for(;;) {
                        score = calculate_mode_score(s, h, &info, 1,
                                                     &v1shrunk, &v4shrunk
#ifdef CINEPAK_REPORT_SERR
, &serr
#endif
);
// do we have a reason to reiterate?
                        if(!v1shrunk && !v4shrunk) break;
// recompute the codebooks, omitting the extra blocks
                        if(v1shrunk) {
                            info.v1_size = v1_size;
                            new_v1_size = quantize(s, h, pict, 1, &info, ENC_V1);
                            if(new_v1_size < info.v1_size){
                                //av_log(s->avctx, AV_LOG_INFO, "mode %i, %3i, %3i: cut v1 codebook to %i entries\n", mode, v1_size, v4_size, new_v1_size);
                                info.v1_size = new_v1_size;
                            }
                        }
                        if(v4shrunk) {
                            info.v4_size = v4_size;
                            new_v4_size = quantize(s, h, pict, 0, &info, ENC_V4);
                            if(new_v4_size < info.v4_size) {
                                //av_log(s->avctx, AV_LOG_INFO, "mode %i, %3i, %3i: cut v4 codebook to %i entries\n", mode, v1_size, v4_size, new_v4_size);
                                info.v4_size = new_v4_size;
                            }
                        }
                    }
                }

                //av_log(s->avctx, AV_LOG_INFO, "%3i %3i score = %lli\n", v1_size, v4_size, (long long int)score);

                if(best_size == 0 || score < *best_score) {

                    *best_score = score;
#ifdef CINEPAK_REPORT_SERR
                    *best_serr = serr;
#endif
                    best_size = encode_mode(s, h, scratch_pict, last_pict, &info, s->strip_buf + STRIP_HEADER_SIZE);

                    //av_log(s->avctx, AV_LOG_INFO, "mode %i, %3i, %3i: %18lli %i B", mode, info.v1_size, info.v4_size, (long long int)score, best_size);
                    //av_log(s->avctx, AV_LOG_INFO, "\n");
#ifdef CINEPAK_REPORT_SERR
                    av_log(s->avctx, AV_LOG_INFO, "mode %i, %3i, %3i: %18lli %i B\n", mode, v1_size, v4_size, (long long int)serr, best_size);
#endif

#ifdef CINEPAKENC_DEBUG
                    //save MB encoding choices
                    memcpy(s->best_mb, s->mb, mb_count*sizeof(mb_info));
#endif

                    //memcpy(strip_temp + STRIP_HEADER_SIZE, strip_temp, best_size);
                    write_strip_header(s, y, h, keyframe, s->strip_buf, best_size);

                }
            }
        }
    }

#ifdef CINEPAKENC_DEBUG
    //gather stats. this will only work properly of MAX_STRIPS == 1
    if(best_info.mode == MODE_V1_ONLY) {
        s->num_v1_mode++;
        s->num_v1_encs += s->w*h/MB_AREA;
    } else {
        if(best_info.mode == MODE_V1_V4)
            s->num_v4_mode++;
        else
            s->num_mc_mode++;

        int x;
        for(x = 0; x < s->w*h/MB_AREA; x++)
            if(s->best_mb[x].best_encoding == ENC_V1)
                s->num_v1_encs++;
            else if(s->best_mb[x].best_encoding == ENC_V4)
                s->num_v4_encs++;
            else
                s->num_skips++;
    }
#endif

    best_size += STRIP_HEADER_SIZE;
    memcpy(buf, s->strip_buf, best_size);

    return best_size;
}

static int write_cvid_header(CinepakEncContext *s, unsigned char *buf, int num_strips, int data_size, int isakeyframe)
{
    buf[0] = isakeyframe ? 0 : 1;
    AV_WB24(&buf[1], data_size + CVID_HEADER_SIZE);
    AV_WB16(&buf[4], s->w);
    AV_WB16(&buf[6], s->h);
    AV_WB16(&buf[8], num_strips);

    return CVID_HEADER_SIZE;
}

static int rd_frame(CinepakEncContext *s, AVFrame *frame, int isakeyframe, unsigned char *buf, int buf_size)
{
    int num_strips, strip, i, y, nexty, size, temp_size, best_size;
    AVPicture last_pict, pict, scratch_pict;
    int64_t best_score = 0, score, score_temp;
#ifdef CINEPAK_REPORT_SERR
    int64_t best_serr = 0, serr, serr_temp;
#endif

    int best_nstrips;

    if(s->pix_fmt == AV_PIX_FMT_RGB24) {
        int x;
// build a copy of the given frame in the correct colorspace
        for(y = 0; y < s->h; y += 2) {
            for(x = 0; x < s->w; x += 2) {
                uint8_t *ir[2]; int32_t r, g, b, rr, gg, bb;
                ir[0] = ((AVPicture*)frame)->data[0] + x*3 + y*((AVPicture*)frame)->linesize[0];
                ir[1] = ir[0] + ((AVPicture*)frame)->linesize[0];
                get_sub_picture(s, x, y, (AVPicture*)s->input_frame, &scratch_pict);
                r = g = b = 0;
                for(i=0; i<4; ++i) {
                    int i1, i2;
                    i1 = (i&1); i2 = (i>=2);
                    rr = ir[i2][i1*3+0];
                    gg = ir[i2][i1*3+1];
                    bb = ir[i2][i1*3+2];
                    r += rr; g += gg; b += bb;
// using fixed point arithmetic for portable repeatability, scaling by 2^23
// "Y"
//                    rr = 0.2857*rr + 0.5714*gg + 0.1429*bb;
                    rr = (2396625*rr + 4793251*gg + 1198732*bb) >> 23;
                    if(      rr <   0) rr =   0;
                    else if (rr > 255) rr = 255;
                    scratch_pict.data[0][i1 + i2*scratch_pict.linesize[0]] = rr;
                }
// let us scale down as late as possible
//                r /= 4; g /= 4; b /= 4;
// "U"
//                rr = -0.1429*r - 0.2857*g + 0.4286*b;
                rr = (-299683*r - 599156*g + 898839*b) >> 23;
                if(      rr < -128) rr = -128;
                else if (rr >  127) rr =  127;
                scratch_pict.data[1][0] = rr + 128; // quantize needs unsigned
// "V"
//                rr = 0.3571*r - 0.2857*g - 0.0714*b;
                rr = (748893*r - 599156*g - 149737*b) >> 23;
                if(      rr < -128) rr = -128;
                else if (rr >  127) rr =  127;
                scratch_pict.data[2][0] = rr + 128; // quantize needs unsigned
            }
        }
    }

    //would be nice but quite certainly incompatible with vintage players:
    // support encoding zero strips (meaning skip the whole frame)
    for(num_strips = s->min_strips; num_strips <= s->max_strips && num_strips <= s->h / MB_SIZE; num_strips++) {
        score = 0;
        size = 0;
#ifdef CINEPAK_REPORT_SERR
        serr = 0;
#endif

        for(y = 0, strip = 1; y < s->h; strip++, y = nexty) {
            int strip_height;

            nexty = strip * s->h / num_strips; // <= s->h
            //make nexty the next multiple of 4 if not already there
            if(nexty & 3)
                nexty += 4 - (nexty & 3);

            strip_height = nexty - y;
            if(strip_height <= 0) { // can this ever happen?
                av_log(s->avctx, AV_LOG_INFO, "skipping zero height strip %i of %i\n", strip, num_strips);
                continue;
            }

            if(s->pix_fmt == AV_PIX_FMT_RGB24)
                get_sub_picture(s, 0, y, (AVPicture*)s->input_frame,    &pict);
            else
                get_sub_picture(s, 0, y, (AVPicture*)frame,              &pict);
            get_sub_picture(s, 0, y, (AVPicture*)s->last_frame,    &last_pict);
            get_sub_picture(s, 0, y, (AVPicture*)s->scratch_frame, &scratch_pict);

            if((temp_size = rd_strip(s, y, strip_height, isakeyframe, &last_pict, &pict, &scratch_pict, s->frame_buf + size + CVID_HEADER_SIZE, &score_temp
#ifdef CINEPAK_REPORT_SERR
, &serr_temp
#endif
)) < 0)
                return temp_size;

            score += score_temp;
#ifdef CINEPAK_REPORT_SERR
            serr += serr_temp;
#endif
            size += temp_size;
            //av_log(s->avctx, AV_LOG_INFO, "strip %d, isakeyframe=%d", strip, isakeyframe);
            //av_log(s->avctx, AV_LOG_INFO, "\n");
        }

        if(best_score == 0 || score < best_score) {
            best_score = score;
#ifdef CINEPAK_REPORT_SERR
            best_serr = serr;
#endif
            best_size = size + write_cvid_header(s, s->frame_buf, num_strips, size, isakeyframe);
            //av_log(s->avctx, AV_LOG_INFO, "best number of strips so far: %2i, %12lli, %i B\n", num_strips, (long long int)score, best_size);
#ifdef CINEPAK_REPORT_SERR
            av_log(s->avctx, AV_LOG_INFO, "best number of strips so far: %2i, %12lli, %i B\n", num_strips, (long long int)serr, best_size);
#endif

            FFSWAP(AVFrame *, s->best_frame, s->scratch_frame);
            memcpy(buf, s->frame_buf, best_size);
            best_nstrips = num_strips;
        }
// avoid trying too many strip numbers without a real reason
// (this makes the processing of the very first frame faster)
        if(num_strips - best_nstrips > 4)
            break;
    }

// let the number of strips slowly adapt to the changes in the contents,
// compared to full bruteforcing every time this will occasionally lead
// to some r/d performance loss but makes encoding up to several times faster
#ifdef CINEPAK_AGGRESSIVE_STRIP_NUMBER_ADAPTIVITY
    s->max_strips = best_nstrips + 4;
    if(s->max_strips >= MAX_STRIPS)
        s->max_strips = MAX_STRIPS;
    s->min_strips = best_nstrips - 4;
    if(s->min_strips < MIN_STRIPS)
        s->min_strips = MIN_STRIPS;
#else
    if(best_nstrips == s->max_strips) { // let us try to step up
        s->max_strips = best_nstrips + 1;
        if(s->max_strips >= MAX_STRIPS)
            s->max_strips = MAX_STRIPS;
    } else { // try to step down
        s->max_strips = best_nstrips;
    }
    s->min_strips = s->max_strips - 1;
    if(s->min_strips < MIN_STRIPS)
        s->min_strips = MIN_STRIPS;
#endif

    return best_size;
}

static int cinepak_encode_frame(AVCodecContext *avctx, AVPacket *pkt,
                                const AVFrame *frame, int *got_packet)
{
    CinepakEncContext *s = avctx->priv_data;
    int ret;

    s->lambda = frame->quality ? frame->quality - 1 : 2 * FF_LAMBDA_SCALE;

    if ((ret = ff_alloc_packet(pkt, s->frame_buf_size)) < 0)
        return ret;
    ret = rd_frame(s, frame, (s->curframe == 0), pkt->data, s->frame_buf_size);
    pkt->size = ret;
    if (s->curframe == 0)
        pkt->flags |= AV_PKT_FLAG_KEY;
    *got_packet = 1;

    FFSWAP(AVFrame *, s->last_frame, s->best_frame);

    if (++s->curframe >= s->keyint)
        s->curframe = 0;

    return 0;
}

static av_cold int cinepak_encode_end(AVCodecContext *avctx)
{
    CinepakEncContext *s = avctx->priv_data;
    int x;

    av_frame_free(&s->last_frame);
    av_frame_free(&s->best_frame);
    av_frame_free(&s->scratch_frame);
    if (avctx->pix_fmt == AV_PIX_FMT_RGB24)
        av_frame_free(&s->input_frame);
    av_freep(&s->codebook_input);
    av_freep(&s->codebook_closest);
    av_freep(&s->strip_buf);
    av_freep(&s->frame_buf);
    av_freep(&s->mb);
#ifdef CINEPAKENC_DEBUG
    av_freep(&s->best_mb);
#endif

    for(x = 0; x < (avctx->pix_fmt == AV_PIX_FMT_RGB24 ? 4 : 3); x++)
        av_freep(&s->pict_bufs[x]);

#ifdef CINEPAKENC_DEBUG
    av_log(avctx, AV_LOG_INFO, "strip coding stats: %i V1 mode, %i V4 mode, %i MC mode (%i V1 encs, %i V4 encs, %i skips)\n",
        s->num_v1_mode, s->num_v4_mode, s->num_mc_mode, s->num_v1_encs, s->num_v4_encs, s->num_skips);
#endif

    return 0;
}

AVCodec ff_cinepak_encoder = {
    .name           = "cinepak",
    .type           = AVMEDIA_TYPE_VIDEO,
    .id             = AV_CODEC_ID_CINEPAK,
    .priv_data_size = sizeof(CinepakEncContext),
    .init           = cinepak_encode_init,
    .encode2        = cinepak_encode_frame,
    .close          = cinepak_encode_end,
    .pix_fmts       = (const enum AVPixelFormat[]){AV_PIX_FMT_RGB24, AV_PIX_FMT_GRAY8, AV_PIX_FMT_NONE},
    .long_name      = NULL_IF_CONFIG_SMALL("Cinepak"),
};