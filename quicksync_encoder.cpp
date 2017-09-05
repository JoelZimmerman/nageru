#include "quicksync_encoder.h"

#include <movit/image_format.h>
#include <movit/resource_pool.h>  // Must be above the Xlib includes.
#include <movit/util.h>

#include <EGL/eglplatform.h>
#include <X11/Xlib.h>
#include <assert.h>
#include <epoxy/egl.h>
#include <fcntl.h>
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <va/va.h>
#include <va/va_drm.h>
#include <va/va_drmcommon.h>
#include <va/va_enc_h264.h>
#include <va/va_x11.h>
#include <algorithm>
#include <chrono>
#include <condition_variable>
#include <cstddef>
#include <cstdint>
#include <functional>
#include <map>
#include <memory>
#include <mutex>
#include <queue>
#include <stack>
#include <string>
#include <thread>
#include <utility>

extern "C" {

#include <libavcodec/avcodec.h>
#include <libavformat/avio.h>
#include <libavutil/error.h>
#include <libdrm/drm_fourcc.h>

}  // namespace

#include "audio_encoder.h"
#include "context.h"
#include "defs.h"
#include "disk_space_estimator.h"
#include "ffmpeg_raii.h"
#include "flags.h"
#include "mux.h"
#include "print_latency.h"
#include "quicksync_encoder_impl.h"
#include "ref_counted_frame.h"
#include "timebase.h"
#include "x264_encoder.h"

using namespace movit;
using namespace std;
using namespace std::chrono;
using namespace std::placeholders;

class QOpenGLContext;
class QSurface;

namespace {

// These need to survive several QuickSyncEncoderImpl instances,
// so they are outside.
once_flag quick_sync_metrics_inited;
LatencyHistogram mixer_latency_histogram, qs_latency_histogram;
MuxMetrics current_file_mux_metrics, total_mux_metrics;
std::atomic<double> metric_current_file_start_time_seconds{0.0 / 0.0};
std::atomic<int64_t> metric_quick_sync_stalled_frames{0};

}  // namespace

#define CHECK_VASTATUS(va_status, func)                                 \
    if (va_status != VA_STATUS_SUCCESS) {                               \
        fprintf(stderr, "%s:%d (%s) failed with %d\n", __func__, __LINE__, func, va_status); \
        exit(1);                                                        \
    }

#define BUFFER_OFFSET(i) ((char *)NULL + (i))

//#include "loadsurface.h"

#define NAL_REF_IDC_NONE        0
#define NAL_REF_IDC_LOW         1
#define NAL_REF_IDC_MEDIUM      2
#define NAL_REF_IDC_HIGH        3

#define NAL_NON_IDR             1
#define NAL_IDR                 5
#define NAL_SPS                 7
#define NAL_PPS                 8
#define NAL_SEI			6

#define SLICE_TYPE_P            0
#define SLICE_TYPE_B            1
#define SLICE_TYPE_I            2
#define IS_P_SLICE(type) (SLICE_TYPE_P == (type))
#define IS_B_SLICE(type) (SLICE_TYPE_B == (type))
#define IS_I_SLICE(type) (SLICE_TYPE_I == (type))


#define ENTROPY_MODE_CAVLC      0
#define ENTROPY_MODE_CABAC      1

#define PROFILE_IDC_BASELINE    66
#define PROFILE_IDC_MAIN        77
#define PROFILE_IDC_HIGH        100
   
#define BITSTREAM_ALLOCATE_STEPPING     4096

static constexpr unsigned int MaxFrameNum = (2<<16);
static constexpr unsigned int MaxPicOrderCntLsb = (2<<8);
static constexpr unsigned int Log2MaxFrameNum = 16;
static constexpr unsigned int Log2MaxPicOrderCntLsb = 8;

using namespace std;

// Supposedly vaRenderPicture() is supposed to destroy the buffer implicitly,
// but if we don't delete it here, we get leaks. The GStreamer implementation
// does the same.
static void render_picture_and_delete(VADisplay dpy, VAContextID context, VABufferID *buffers, int num_buffers)
{
    VAStatus va_status = vaRenderPicture(dpy, context, buffers, num_buffers);
    CHECK_VASTATUS(va_status, "vaRenderPicture");

    for (int i = 0; i < num_buffers; ++i) {
        va_status = vaDestroyBuffer(dpy, buffers[i]);
        CHECK_VASTATUS(va_status, "vaDestroyBuffer");
    }
}

static unsigned int 
va_swap32(unsigned int val)
{
    unsigned char *pval = (unsigned char *)&val;

    return ((pval[0] << 24)     |
            (pval[1] << 16)     |
            (pval[2] << 8)      |
            (pval[3] << 0));
}

static void
bitstream_start(bitstream *bs)
{
    bs->max_size_in_dword = BITSTREAM_ALLOCATE_STEPPING;
    bs->buffer = (unsigned int *)calloc(bs->max_size_in_dword * sizeof(int), 1);
    bs->bit_offset = 0;
}

static void
bitstream_end(bitstream *bs)
{
    int pos = (bs->bit_offset >> 5);
    int bit_offset = (bs->bit_offset & 0x1f);
    int bit_left = 32 - bit_offset;

    if (bit_offset) {
        bs->buffer[pos] = va_swap32((bs->buffer[pos] << bit_left));
    }
}
 
static void
bitstream_put_ui(bitstream *bs, unsigned int val, int size_in_bits)
{
    int pos = (bs->bit_offset >> 5);
    int bit_offset = (bs->bit_offset & 0x1f);
    int bit_left = 32 - bit_offset;

    if (!size_in_bits)
        return;

    bs->bit_offset += size_in_bits;

    if (bit_left > size_in_bits) {
        bs->buffer[pos] = (bs->buffer[pos] << size_in_bits | val);
    } else {
        size_in_bits -= bit_left;
        if (bit_left >= 32) {
            bs->buffer[pos] = (val >> size_in_bits);
        } else {
            bs->buffer[pos] = (bs->buffer[pos] << bit_left) | (val >> size_in_bits);
        }
        bs->buffer[pos] = va_swap32(bs->buffer[pos]);

        if (pos + 1 == bs->max_size_in_dword) {
            bs->max_size_in_dword += BITSTREAM_ALLOCATE_STEPPING;
            bs->buffer = (unsigned int *)realloc(bs->buffer, bs->max_size_in_dword * sizeof(unsigned int));
        }

        bs->buffer[pos + 1] = val;
    }
}

static void
bitstream_put_ue(bitstream *bs, unsigned int val)
{
    int size_in_bits = 0;
    int tmp_val = ++val;

    while (tmp_val) {
        tmp_val >>= 1;
        size_in_bits++;
    }

    bitstream_put_ui(bs, 0, size_in_bits - 1); // leading zero
    bitstream_put_ui(bs, val, size_in_bits);
}

static void
bitstream_put_se(bitstream *bs, int val)
{
    unsigned int new_val;

    if (val <= 0)
        new_val = -2 * val;
    else
        new_val = 2 * val - 1;

    bitstream_put_ue(bs, new_val);
}

static void
bitstream_byte_aligning(bitstream *bs, int bit)
{
    int bit_offset = (bs->bit_offset & 0x7);
    int bit_left = 8 - bit_offset;
    int new_val;

    if (!bit_offset)
        return;

    assert(bit == 0 || bit == 1);

    if (bit)
        new_val = (1 << bit_left) - 1;
    else
        new_val = 0;

    bitstream_put_ui(bs, new_val, bit_left);
}

static void 
rbsp_trailing_bits(bitstream *bs)
{
    bitstream_put_ui(bs, 1, 1);
    bitstream_byte_aligning(bs, 0);
}

static void nal_start_code_prefix(bitstream *bs)
{
    bitstream_put_ui(bs, 0x00000001, 32);
}

static void nal_header(bitstream *bs, int nal_ref_idc, int nal_unit_type)
{
    bitstream_put_ui(bs, 0, 1);                /* forbidden_zero_bit: 0 */
    bitstream_put_ui(bs, nal_ref_idc, 2);
    bitstream_put_ui(bs, nal_unit_type, 5);
}

void QuickSyncEncoderImpl::sps_rbsp(YCbCrLumaCoefficients ycbcr_coefficients, bitstream *bs)
{
    int profile_idc = PROFILE_IDC_BASELINE;

    if (h264_profile  == VAProfileH264High)
        profile_idc = PROFILE_IDC_HIGH;
    else if (h264_profile  == VAProfileH264Main)
        profile_idc = PROFILE_IDC_MAIN;

    bitstream_put_ui(bs, profile_idc, 8);               /* profile_idc */
    bitstream_put_ui(bs, !!(constraint_set_flag & 1), 1);                         /* constraint_set0_flag */
    bitstream_put_ui(bs, !!(constraint_set_flag & 2), 1);                         /* constraint_set1_flag */
    bitstream_put_ui(bs, !!(constraint_set_flag & 4), 1);                         /* constraint_set2_flag */
    bitstream_put_ui(bs, !!(constraint_set_flag & 8), 1);                         /* constraint_set3_flag */
    bitstream_put_ui(bs, 0, 4);                         /* reserved_zero_4bits */
    bitstream_put_ui(bs, seq_param.level_idc, 8);      /* level_idc */
    bitstream_put_ue(bs, seq_param.seq_parameter_set_id);      /* seq_parameter_set_id */

    if ( profile_idc == PROFILE_IDC_HIGH) {
        bitstream_put_ue(bs, 1);        /* chroma_format_idc = 1, 4:2:0 */ 
        bitstream_put_ue(bs, 0);        /* bit_depth_luma_minus8 */
        bitstream_put_ue(bs, 0);        /* bit_depth_chroma_minus8 */
        bitstream_put_ui(bs, 0, 1);     /* qpprime_y_zero_transform_bypass_flag */
        bitstream_put_ui(bs, 0, 1);     /* seq_scaling_matrix_present_flag */
    }

    bitstream_put_ue(bs, seq_param.seq_fields.bits.log2_max_frame_num_minus4); /* log2_max_frame_num_minus4 */
    bitstream_put_ue(bs, seq_param.seq_fields.bits.pic_order_cnt_type);        /* pic_order_cnt_type */

    if (seq_param.seq_fields.bits.pic_order_cnt_type == 0)
        bitstream_put_ue(bs, seq_param.seq_fields.bits.log2_max_pic_order_cnt_lsb_minus4);     /* log2_max_pic_order_cnt_lsb_minus4 */
    else {
        assert(0);
    }

    bitstream_put_ue(bs, seq_param.max_num_ref_frames);        /* num_ref_frames */
    bitstream_put_ui(bs, 0, 1);                                 /* gaps_in_frame_num_value_allowed_flag */

    bitstream_put_ue(bs, seq_param.picture_width_in_mbs - 1);  /* pic_width_in_mbs_minus1 */
    bitstream_put_ue(bs, seq_param.picture_height_in_mbs - 1); /* pic_height_in_map_units_minus1 */
    bitstream_put_ui(bs, seq_param.seq_fields.bits.frame_mbs_only_flag, 1);    /* frame_mbs_only_flag */

    if (!seq_param.seq_fields.bits.frame_mbs_only_flag) {
        assert(0);
    }

    bitstream_put_ui(bs, seq_param.seq_fields.bits.direct_8x8_inference_flag, 1);      /* direct_8x8_inference_flag */
    bitstream_put_ui(bs, seq_param.frame_cropping_flag, 1);            /* frame_cropping_flag */

    if (seq_param.frame_cropping_flag) {
        bitstream_put_ue(bs, seq_param.frame_crop_left_offset);        /* frame_crop_left_offset */
        bitstream_put_ue(bs, seq_param.frame_crop_right_offset);       /* frame_crop_right_offset */
        bitstream_put_ue(bs, seq_param.frame_crop_top_offset);         /* frame_crop_top_offset */
        bitstream_put_ue(bs, seq_param.frame_crop_bottom_offset);      /* frame_crop_bottom_offset */
    }
    
    //if ( frame_bit_rate < 0 ) { //TODO EW: the vui header isn't correct
    if ( false ) {
        bitstream_put_ui(bs, 0, 1); /* vui_parameters_present_flag */
    } else {
        // See H.264 annex E for the definition of this header.
        bitstream_put_ui(bs, 1, 1); /* vui_parameters_present_flag */
        bitstream_put_ui(bs, 0, 1); /* aspect_ratio_info_present_flag */
        bitstream_put_ui(bs, 0, 1); /* overscan_info_present_flag */
        bitstream_put_ui(bs, 1, 1); /* video_signal_type_present_flag */
        {
            bitstream_put_ui(bs, 5, 3);  /* video_format (5 = Unspecified) */
            bitstream_put_ui(bs, 0, 1);  /* video_full_range_flag */
            bitstream_put_ui(bs, 1, 1);  /* colour_description_present_flag */
            {
                bitstream_put_ui(bs, 1, 8);  /* colour_primaries (1 = BT.709) */
                bitstream_put_ui(bs, 13, 8);  /* transfer_characteristics (13 = sRGB) */
                if (ycbcr_coefficients == YCBCR_REC_709) {
                    bitstream_put_ui(bs, 1, 8);  /* matrix_coefficients (1 = BT.709) */
                } else {
                    assert(ycbcr_coefficients == YCBCR_REC_601);
                    bitstream_put_ui(bs, 6, 8);  /* matrix_coefficients (6 = BT.601/SMPTE 170M) */
                }
            }
        }
        bitstream_put_ui(bs, 0, 1); /* chroma_loc_info_present_flag */
        bitstream_put_ui(bs, 1, 1); /* timing_info_present_flag */
        {
            bitstream_put_ui(bs, 1, 32);  // FPS
            bitstream_put_ui(bs, TIMEBASE * 2, 32);  // FPS
            bitstream_put_ui(bs, 1, 1);
        }
        bitstream_put_ui(bs, 1, 1); /* nal_hrd_parameters_present_flag */
        {
            // hrd_parameters 
            bitstream_put_ue(bs, 0);    /* cpb_cnt_minus1 */
            bitstream_put_ui(bs, 4, 4); /* bit_rate_scale */
            bitstream_put_ui(bs, 6, 4); /* cpb_size_scale */
           
            bitstream_put_ue(bs, frame_bitrate - 1); /* bit_rate_value_minus1[0] */
            bitstream_put_ue(bs, frame_bitrate*8 - 1); /* cpb_size_value_minus1[0] */
            bitstream_put_ui(bs, 1, 1);  /* cbr_flag[0] */

            bitstream_put_ui(bs, 23, 5);   /* initial_cpb_removal_delay_length_minus1 */
            bitstream_put_ui(bs, 23, 5);   /* cpb_removal_delay_length_minus1 */
            bitstream_put_ui(bs, 23, 5);   /* dpb_output_delay_length_minus1 */
            bitstream_put_ui(bs, 23, 5);   /* time_offset_length  */
        }
        bitstream_put_ui(bs, 0, 1);   /* vcl_hrd_parameters_present_flag */
        bitstream_put_ui(bs, 0, 1);   /* low_delay_hrd_flag */ 

        bitstream_put_ui(bs, 0, 1); /* pic_struct_present_flag */
        bitstream_put_ui(bs, 0, 1); /* bitstream_restriction_flag */
    }

    rbsp_trailing_bits(bs);     /* rbsp_trailing_bits */
}


void QuickSyncEncoderImpl::pps_rbsp(bitstream *bs)
{
    bitstream_put_ue(bs, pic_param.pic_parameter_set_id);      /* pic_parameter_set_id */
    bitstream_put_ue(bs, pic_param.seq_parameter_set_id);      /* seq_parameter_set_id */

    bitstream_put_ui(bs, pic_param.pic_fields.bits.entropy_coding_mode_flag, 1);  /* entropy_coding_mode_flag */

    bitstream_put_ui(bs, 0, 1);                         /* pic_order_present_flag: 0 */

    bitstream_put_ue(bs, 0);                            /* num_slice_groups_minus1 */

    bitstream_put_ue(bs, pic_param.num_ref_idx_l0_active_minus1);      /* num_ref_idx_l0_active_minus1 */
    bitstream_put_ue(bs, pic_param.num_ref_idx_l1_active_minus1);      /* num_ref_idx_l1_active_minus1 1 */

    bitstream_put_ui(bs, pic_param.pic_fields.bits.weighted_pred_flag, 1);     /* weighted_pred_flag: 0 */
    bitstream_put_ui(bs, pic_param.pic_fields.bits.weighted_bipred_idc, 2);	/* weighted_bipred_idc: 0 */

    bitstream_put_se(bs, pic_param.pic_init_qp - 26);  /* pic_init_qp_minus26 */
    bitstream_put_se(bs, 0);                            /* pic_init_qs_minus26 */
    bitstream_put_se(bs, 0);                            /* chroma_qp_index_offset */

    bitstream_put_ui(bs, pic_param.pic_fields.bits.deblocking_filter_control_present_flag, 1); /* deblocking_filter_control_present_flag */
    bitstream_put_ui(bs, 0, 1);                         /* constrained_intra_pred_flag */
    bitstream_put_ui(bs, 0, 1);                         /* redundant_pic_cnt_present_flag */
    
    /* more_rbsp_data */
    bitstream_put_ui(bs, pic_param.pic_fields.bits.transform_8x8_mode_flag, 1);    /*transform_8x8_mode_flag */
    bitstream_put_ui(bs, 0, 1);                         /* pic_scaling_matrix_present_flag */
    bitstream_put_se(bs, pic_param.second_chroma_qp_index_offset );    /*second_chroma_qp_index_offset */

    rbsp_trailing_bits(bs);
}

void QuickSyncEncoderImpl::slice_header(bitstream *bs)
{
    int first_mb_in_slice = slice_param.macroblock_address;

    bitstream_put_ue(bs, first_mb_in_slice);        /* first_mb_in_slice: 0 */
    bitstream_put_ue(bs, slice_param.slice_type);   /* slice_type */
    bitstream_put_ue(bs, slice_param.pic_parameter_set_id);        /* pic_parameter_set_id: 0 */
    bitstream_put_ui(bs, pic_param.frame_num, seq_param.seq_fields.bits.log2_max_frame_num_minus4 + 4); /* frame_num */

    /* frame_mbs_only_flag == 1 */
    if (!seq_param.seq_fields.bits.frame_mbs_only_flag) {
        /* FIXME: */
        assert(0);
    }

    if (pic_param.pic_fields.bits.idr_pic_flag)
        bitstream_put_ue(bs, slice_param.idr_pic_id);		/* idr_pic_id: 0 */

    if (seq_param.seq_fields.bits.pic_order_cnt_type == 0) {
        bitstream_put_ui(bs, pic_param.CurrPic.TopFieldOrderCnt, seq_param.seq_fields.bits.log2_max_pic_order_cnt_lsb_minus4 + 4);
        /* pic_order_present_flag == 0 */
    } else {
        /* FIXME: */
        assert(0);
    }

    /* redundant_pic_cnt_present_flag == 0 */
    /* slice type */
    if (IS_P_SLICE(slice_param.slice_type)) {
        bitstream_put_ui(bs, slice_param.num_ref_idx_active_override_flag, 1);            /* num_ref_idx_active_override_flag: */

        if (slice_param.num_ref_idx_active_override_flag)
            bitstream_put_ue(bs, slice_param.num_ref_idx_l0_active_minus1);

        /* ref_pic_list_reordering */
        bitstream_put_ui(bs, 0, 1);            /* ref_pic_list_reordering_flag_l0: 0 */
    } else if (IS_B_SLICE(slice_param.slice_type)) {
        bitstream_put_ui(bs, slice_param.direct_spatial_mv_pred_flag, 1);            /* direct_spatial_mv_pred: 1 */

        bitstream_put_ui(bs, slice_param.num_ref_idx_active_override_flag, 1);       /* num_ref_idx_active_override_flag: */

        if (slice_param.num_ref_idx_active_override_flag) {
            bitstream_put_ue(bs, slice_param.num_ref_idx_l0_active_minus1);
            bitstream_put_ue(bs, slice_param.num_ref_idx_l1_active_minus1);
        }

        /* ref_pic_list_reordering */
        bitstream_put_ui(bs, 0, 1);            /* ref_pic_list_reordering_flag_l0: 0 */
        bitstream_put_ui(bs, 0, 1);            /* ref_pic_list_reordering_flag_l1: 0 */
    }

    if ((pic_param.pic_fields.bits.weighted_pred_flag &&
         IS_P_SLICE(slice_param.slice_type)) ||
        ((pic_param.pic_fields.bits.weighted_bipred_idc == 1) &&
         IS_B_SLICE(slice_param.slice_type))) {
        /* FIXME: fill weight/offset table */
        assert(0);
    }

    /* dec_ref_pic_marking */
    if (pic_param.pic_fields.bits.reference_pic_flag) {     /* nal_ref_idc != 0 */
        unsigned char no_output_of_prior_pics_flag = 0;
        unsigned char long_term_reference_flag = 0;
        unsigned char adaptive_ref_pic_marking_mode_flag = 0;

        if (pic_param.pic_fields.bits.idr_pic_flag) {
            bitstream_put_ui(bs, no_output_of_prior_pics_flag, 1);            /* no_output_of_prior_pics_flag: 0 */
            bitstream_put_ui(bs, long_term_reference_flag, 1);            /* long_term_reference_flag: 0 */
        } else {
            bitstream_put_ui(bs, adaptive_ref_pic_marking_mode_flag, 1);            /* adaptive_ref_pic_marking_mode_flag: 0 */
        }
    }

    if (pic_param.pic_fields.bits.entropy_coding_mode_flag &&
        !IS_I_SLICE(slice_param.slice_type))
        bitstream_put_ue(bs, slice_param.cabac_init_idc);               /* cabac_init_idc: 0 */

    bitstream_put_se(bs, slice_param.slice_qp_delta);                   /* slice_qp_delta: 0 */

    /* ignore for SP/SI */

    if (pic_param.pic_fields.bits.deblocking_filter_control_present_flag) {
        bitstream_put_ue(bs, slice_param.disable_deblocking_filter_idc);           /* disable_deblocking_filter_idc: 0 */

        if (slice_param.disable_deblocking_filter_idc != 1) {
            bitstream_put_se(bs, slice_param.slice_alpha_c0_offset_div2);          /* slice_alpha_c0_offset_div2: 2 */
            bitstream_put_se(bs, slice_param.slice_beta_offset_div2);              /* slice_beta_offset_div2: 2 */
        }
    }

    if (pic_param.pic_fields.bits.entropy_coding_mode_flag) {
        bitstream_byte_aligning(bs, 1);
    }
}

int QuickSyncEncoderImpl::build_packed_pic_buffer(unsigned char **header_buffer)
{
    bitstream bs;

    bitstream_start(&bs);
    nal_start_code_prefix(&bs);
    nal_header(&bs, NAL_REF_IDC_HIGH, NAL_PPS);
    pps_rbsp(&bs);
    bitstream_end(&bs);

    *header_buffer = (unsigned char *)bs.buffer;
    return bs.bit_offset;
}

int
QuickSyncEncoderImpl::build_packed_seq_buffer(YCbCrLumaCoefficients ycbcr_coefficients, unsigned char **header_buffer)
{
    bitstream bs;

    bitstream_start(&bs);
    nal_start_code_prefix(&bs);
    nal_header(&bs, NAL_REF_IDC_HIGH, NAL_SPS);
    sps_rbsp(ycbcr_coefficients, &bs);
    bitstream_end(&bs);

    *header_buffer = (unsigned char *)bs.buffer;
    return bs.bit_offset;
}

int QuickSyncEncoderImpl::build_packed_slice_buffer(unsigned char **header_buffer)
{
    bitstream bs;
    int is_idr = !!pic_param.pic_fields.bits.idr_pic_flag;
    int is_ref = !!pic_param.pic_fields.bits.reference_pic_flag;

    bitstream_start(&bs);
    nal_start_code_prefix(&bs);

    if (IS_I_SLICE(slice_param.slice_type)) {
        nal_header(&bs, NAL_REF_IDC_HIGH, is_idr ? NAL_IDR : NAL_NON_IDR);
    } else if (IS_P_SLICE(slice_param.slice_type)) {
        nal_header(&bs, NAL_REF_IDC_MEDIUM, NAL_NON_IDR);
    } else {
        assert(IS_B_SLICE(slice_param.slice_type));
        nal_header(&bs, is_ref ? NAL_REF_IDC_LOW : NAL_REF_IDC_NONE, NAL_NON_IDR);
    }

    slice_header(&bs);
    bitstream_end(&bs);

    *header_buffer = (unsigned char *)bs.buffer;
    return bs.bit_offset;
}


/*
  Assume frame sequence is: Frame#0, #1, #2, ..., #M, ..., #X, ... (encoding order)
  1) period between Frame #X and Frame #N = #X - #N
  2) 0 means infinite for intra_period/intra_idr_period, and 0 is invalid for ip_period
  3) intra_idr_period % intra_period (intra_period > 0) and intra_period % ip_period must be 0
  4) intra_period and intra_idr_period take precedence over ip_period
  5) if ip_period > 1, intra_period and intra_idr_period are not  the strict periods 
     of I/IDR frames, see bellow examples
  -------------------------------------------------------------------
  intra_period intra_idr_period ip_period frame sequence (intra_period/intra_idr_period/ip_period)
  0            ignored          1          IDRPPPPPPP ...     (No IDR/I any more)
  0            ignored        >=2          IDR(PBB)(PBB)...   (No IDR/I any more)
  1            0                ignored    IDRIIIIIII...      (No IDR any more)
  1            1                ignored    IDR IDR IDR IDR...
  1            >=2              ignored    IDRII IDRII IDR... (1/3/ignore)
  >=2          0                1          IDRPPP IPPP I...   (3/0/1)
  >=2          0              >=2          IDR(PBB)(PBB)(IBB) (6/0/3)
                                              (PBB)(IBB)(PBB)(IBB)... 
  >=2          >=2              1          IDRPPPPP IPPPPP IPPPPP (6/18/1)
                                           IDRPPPPP IPPPPP IPPPPP...
  >=2          >=2              >=2        {IDR(PBB)(PBB)(IBB)(PBB)(IBB)(PBB)} (6/18/3)
                                           {IDR(PBB)(PBB)(IBB)(PBB)(IBB)(PBB)}...
                                           {IDR(PBB)(PBB)(IBB)(PBB)}           (6/12/3)
                                           {IDR(PBB)(PBB)(IBB)(PBB)}...
                                           {IDR(PBB)(PBB)}                     (6/6/3)
                                           {IDR(PBB)(PBB)}.
*/

// General pts/dts strategy:
//
// Getting pts and dts right with variable frame rate (VFR) and B-frames can be a
// bit tricky. We assume first of all that the frame rate never goes _above_
// MAX_FPS, which gives us a frame period N. The decoder can always decode
// in at least this speed, as long at dts <= pts (the frame is not attempted
// presented before it is decoded). Furthermore, we never have longer chains of
// B-frames than a fixed constant C. (In a B-frame chain, we say that the base
// I/P-frame has order O=0, the B-frame depending on it directly has order O=1,
// etc. The last frame in the chain, which no B-frames depend on, is the “tip”
// frame, with an order O <= C.)
//
// Many strategies are possible, but we establish these rules:
//
//  - Tip frames have dts = pts - (C-O)*N.
//  - Non-tip frames have dts = dts_last + N.
//
// An example, with C=2 and N=10 and the data flow showed with arrows:
//
//        I  B  P  B  B  P
//   pts: 30 40 50 60 70 80
//        ↓  ↓     ↓
//   dts: 10 30 20 60 50←40
//         |  |  ↑        ↑
//         `--|--'        |
//             `----------'
//
// To show that this works fine also with irregular spacings, let's say that
// the third frame is delayed a bit (something earlier was dropped). Now the
// situation looks like this:
//
//        I  B  P  B  B   P
//   pts: 30 40 80 90 100 110
//        ↓  ↓     ↓
//   dts: 10 30 20 90 50←40
//         |  |  ↑        ↑
//         `--|--'        |
//             `----------'
//
// The resetting on every tip frame makes sure dts never ends up lagging a lot
// behind pts, and the subtraction of (C-O)*N makes sure pts <= dts.
//
// In the output of this function, if <dts_lag> is >= 0, it means to reset the
// dts from the current pts minus <dts_lag>, while if it's -1, the frame is not
// a tip frame and should be given a dts based on the previous one.
#define FRAME_P 0
#define FRAME_B 1
#define FRAME_I 2
#define FRAME_IDR 7
void encoding2display_order(
    int encoding_order, int intra_period,
    int intra_idr_period, int ip_period,
    int *displaying_order,
    int *frame_type, int *pts_lag)
{
    int encoding_order_gop = 0;

    *pts_lag = 0;

    if (intra_period == 1) { /* all are I/IDR frames */
        *displaying_order = encoding_order;
        if (intra_idr_period == 0)
            *frame_type = (encoding_order == 0)?FRAME_IDR:FRAME_I;
        else
            *frame_type = (encoding_order % intra_idr_period == 0)?FRAME_IDR:FRAME_I;
        return;
    }

    if (intra_period == 0)
        intra_idr_period = 0;

    if (ip_period == 1) {
        // No B-frames, sequence is like IDR PPPPP IPPPPP.
        encoding_order_gop = (intra_idr_period == 0) ? encoding_order : (encoding_order % intra_idr_period);
        *displaying_order = encoding_order;

        if (encoding_order_gop == 0) { /* the first frame */
            *frame_type = FRAME_IDR;
        } else if (intra_period != 0 && /* have I frames */
                   encoding_order_gop >= 2 &&
                   (encoding_order_gop % intra_period == 0)) {
            *frame_type = FRAME_I;
        } else {
            *frame_type = FRAME_P;
        }
        return;
    } 

    // We have B-frames. Sequence is like IDR (PBB)(PBB)(IBB)(PBB).
    encoding_order_gop = (intra_idr_period == 0) ? encoding_order : (encoding_order % (intra_idr_period + 1));
    *pts_lag = -1;  // Most frames are not tip frames.
         
    if (encoding_order_gop == 0) { /* the first frame */
        *frame_type = FRAME_IDR;
        *displaying_order = encoding_order;
        // IDR frames are a special case; I honestly can't find the logic behind
        // why this is the right thing, but it seems to line up nicely in practice :-)
        *pts_lag = TIMEBASE / MAX_FPS;
    } else if (((encoding_order_gop - 1) % ip_period) != 0) { /* B frames */
        *frame_type = FRAME_B;
        *displaying_order = encoding_order - 1;
        if ((encoding_order_gop % ip_period) == 0) {
            *pts_lag = 0;  // Last B-frame.
        }
    } else if (intra_period != 0 && /* have I frames */
               encoding_order_gop >= 2 &&
               ((encoding_order_gop - 1) / ip_period % (intra_period / ip_period)) == 0) {
        *frame_type = FRAME_I;
        *displaying_order = encoding_order + ip_period - 1;
    } else {
        *frame_type = FRAME_P;
        *displaying_order = encoding_order + ip_period - 1;
    }
}


void QuickSyncEncoderImpl::enable_zerocopy_if_possible()
{
	if (global_flags.x264_video_to_disk) {
		// Quick Sync is entirely disabled.
		use_zerocopy = false;
	} else if (global_flags.uncompressed_video_to_http) {
		fprintf(stderr, "Disabling zerocopy H.264 encoding due to --http-uncompressed-video.\n");
		use_zerocopy = false;
	} else if (global_flags.x264_video_to_http) {
		fprintf(stderr, "Disabling zerocopy H.264 encoding due to --http-x264-video.\n");
		use_zerocopy = false;
	} else {
		use_zerocopy = true;
	}
	global_flags.use_zerocopy = use_zerocopy;
}

VADisplay QuickSyncEncoderImpl::va_open_display(const string &va_display)
{
	if (va_display.empty()) {
		x11_display = XOpenDisplay(NULL);
		if (!x11_display) {
			fprintf(stderr, "error: can't connect to X server!\n");
			return NULL;
		}
		return vaGetDisplay(x11_display);
	} else if (va_display[0] != '/') {
		x11_display = XOpenDisplay(va_display.c_str());
		if (!x11_display) {
			fprintf(stderr, "error: can't connect to X server!\n");
			return NULL;
		}
		return vaGetDisplay(x11_display);
	} else {
		drm_fd = open(va_display.c_str(), O_RDWR);
		if (drm_fd == -1) {
			perror(va_display.c_str());
			return NULL;
		}
		use_zerocopy = false;
		return vaGetDisplayDRM(drm_fd);
	}
}

void QuickSyncEncoderImpl::va_close_display(VADisplay va_dpy)
{
	if (x11_display) {
		XCloseDisplay(x11_display);
		x11_display = nullptr;
	}
	if (drm_fd != -1) {
		close(drm_fd);
	}
}

int QuickSyncEncoderImpl::init_va(const string &va_display)
{
    VAProfile profile_list[]={VAProfileH264High, VAProfileH264Main, VAProfileH264Baseline, VAProfileH264ConstrainedBaseline};
    VAEntrypoint *entrypoints;
    int num_entrypoints, slice_entrypoint;
    int support_encode = 0;    
    int major_ver, minor_ver;
    VAStatus va_status;
    unsigned int i;

    va_dpy = va_open_display(va_display);
    va_status = vaInitialize(va_dpy, &major_ver, &minor_ver);
    CHECK_VASTATUS(va_status, "vaInitialize");

    num_entrypoints = vaMaxNumEntrypoints(va_dpy);
    entrypoints = (VAEntrypoint *)malloc(num_entrypoints * sizeof(*entrypoints));
    if (!entrypoints) {
        fprintf(stderr, "error: failed to initialize VA entrypoints array\n");
        exit(1);
    }

    /* use the highest profile */
    for (i = 0; i < sizeof(profile_list)/sizeof(profile_list[0]); i++) {
        if ((h264_profile != ~0) && h264_profile != profile_list[i])
            continue;
        
        h264_profile = profile_list[i];
        vaQueryConfigEntrypoints(va_dpy, h264_profile, entrypoints, &num_entrypoints);
        for (slice_entrypoint = 0; slice_entrypoint < num_entrypoints; slice_entrypoint++) {
            if (entrypoints[slice_entrypoint] == VAEntrypointEncSlice) {
                support_encode = 1;
                break;
            }
        }
        if (support_encode == 1)
            break;
    }
    
    if (support_encode == 0) {
        printf("Can't find VAEntrypointEncSlice for H264 profiles. If you are using a non-Intel GPU\n");
        printf("but have one in your system, try launching Nageru with --va-display /dev/dri/renderD128\n");
        printf("to use VA-API against DRM instead of X11.\n");
        exit(1);
    } else {
        switch (h264_profile) {
            case VAProfileH264Baseline:
                ip_period = 1;
                constraint_set_flag |= (1 << 0); /* Annex A.2.1 */
                h264_entropy_mode = 0;
                break;
            case VAProfileH264ConstrainedBaseline:
                constraint_set_flag |= (1 << 0 | 1 << 1); /* Annex A.2.2 */
                ip_period = 1;
                break;

            case VAProfileH264Main:
                constraint_set_flag |= (1 << 1); /* Annex A.2.2 */
                break;

            case VAProfileH264High:
                constraint_set_flag |= (1 << 3); /* Annex A.2.4 */
                break;
            default:
                h264_profile = VAProfileH264Baseline;
                ip_period = 1;
                constraint_set_flag |= (1 << 0); /* Annex A.2.1 */
                break;
        }
    }

    VAConfigAttrib attrib[VAConfigAttribTypeMax];

    /* find out the format for the render target, and rate control mode */
    for (i = 0; i < VAConfigAttribTypeMax; i++)
        attrib[i].type = (VAConfigAttribType)i;

    va_status = vaGetConfigAttributes(va_dpy, h264_profile, VAEntrypointEncSlice,
                                      &attrib[0], VAConfigAttribTypeMax);
    CHECK_VASTATUS(va_status, "vaGetConfigAttributes");
    /* check the interested configattrib */
    if ((attrib[VAConfigAttribRTFormat].value & VA_RT_FORMAT_YUV420) == 0) {
        printf("Not find desired YUV420 RT format\n");
        exit(1);
    } else {
        config_attrib[config_attrib_num].type = VAConfigAttribRTFormat;
        config_attrib[config_attrib_num].value = VA_RT_FORMAT_YUV420;
        config_attrib_num++;
    }
    
    if (attrib[VAConfigAttribRateControl].value != VA_ATTRIB_NOT_SUPPORTED) {
        if (!(attrib[VAConfigAttribRateControl].value & VA_RC_CQP)) {
            fprintf(stderr, "ERROR: VA-API encoder does not support CQP mode.\n");
            exit(1);
        }

        config_attrib[config_attrib_num].type = VAConfigAttribRateControl;
        config_attrib[config_attrib_num].value = VA_RC_CQP;
        config_attrib_num++;
    }
    

    if (attrib[VAConfigAttribEncPackedHeaders].value != VA_ATTRIB_NOT_SUPPORTED) {
        int tmp = attrib[VAConfigAttribEncPackedHeaders].value;

        h264_packedheader = 1;
        config_attrib[config_attrib_num].type = VAConfigAttribEncPackedHeaders;
        config_attrib[config_attrib_num].value = VA_ENC_PACKED_HEADER_NONE;
        
        if (tmp & VA_ENC_PACKED_HEADER_SEQUENCE) {
            config_attrib[config_attrib_num].value |= VA_ENC_PACKED_HEADER_SEQUENCE;
        }
        
        if (tmp & VA_ENC_PACKED_HEADER_PICTURE) {
            config_attrib[config_attrib_num].value |= VA_ENC_PACKED_HEADER_PICTURE;
        }
        
        if (tmp & VA_ENC_PACKED_HEADER_SLICE) {
            config_attrib[config_attrib_num].value |= VA_ENC_PACKED_HEADER_SLICE;
        }
        
        if (tmp & VA_ENC_PACKED_HEADER_MISC) {
            config_attrib[config_attrib_num].value |= VA_ENC_PACKED_HEADER_MISC;
        }
        
        enc_packed_header_idx = config_attrib_num;
        config_attrib_num++;
    }

    if (attrib[VAConfigAttribEncInterlaced].value != VA_ATTRIB_NOT_SUPPORTED) {
        config_attrib[config_attrib_num].type = VAConfigAttribEncInterlaced;
        config_attrib[config_attrib_num].value = VA_ENC_PACKED_HEADER_NONE;
        config_attrib_num++;
    }
    
    if (attrib[VAConfigAttribEncMaxRefFrames].value != VA_ATTRIB_NOT_SUPPORTED) {
        h264_maxref = attrib[VAConfigAttribEncMaxRefFrames].value;
    }

    free(entrypoints);
    return 0;
}

int QuickSyncEncoderImpl::setup_encode()
{
	if (!global_flags.x264_video_to_disk) {
		VAStatus va_status;
		VASurfaceID *tmp_surfaceid;
		int codedbuf_size;
		VASurfaceID src_surface[SURFACE_NUM];
		VASurfaceID ref_surface[SURFACE_NUM];

		va_status = vaCreateConfig(va_dpy, h264_profile, VAEntrypointEncSlice,
				&config_attrib[0], config_attrib_num, &config_id);
		CHECK_VASTATUS(va_status, "vaCreateConfig");

		/* create source surfaces */
		va_status = vaCreateSurfaces(va_dpy,
				VA_RT_FORMAT_YUV420, frame_width_mbaligned, frame_height_mbaligned,
				&src_surface[0], SURFACE_NUM,
				NULL, 0);
		CHECK_VASTATUS(va_status, "vaCreateSurfaces");

		/* create reference surfaces */
		va_status = vaCreateSurfaces(va_dpy,
				VA_RT_FORMAT_YUV420, frame_width_mbaligned, frame_height_mbaligned,
				&ref_surface[0], SURFACE_NUM,
				NULL, 0);
		CHECK_VASTATUS(va_status, "vaCreateSurfaces");

		tmp_surfaceid = (VASurfaceID *)calloc(2 * SURFACE_NUM, sizeof(VASurfaceID));
		memcpy(tmp_surfaceid, src_surface, SURFACE_NUM * sizeof(VASurfaceID));
		memcpy(tmp_surfaceid + SURFACE_NUM, ref_surface, SURFACE_NUM * sizeof(VASurfaceID));

		for (int i = 0; i < SURFACE_NUM; i++) {
			gl_surfaces[i].src_surface = src_surface[i];
			gl_surfaces[i].ref_surface = ref_surface[i];
		}

		/* Create a context for this encode pipe */
		va_status = vaCreateContext(va_dpy, config_id,
				frame_width_mbaligned, frame_height_mbaligned,
				VA_PROGRESSIVE,
				tmp_surfaceid, 2 * SURFACE_NUM,
				&context_id);
		CHECK_VASTATUS(va_status, "vaCreateContext");
		free(tmp_surfaceid);

		codedbuf_size = (frame_width_mbaligned * frame_height_mbaligned * 400) / (16*16);

		for (int i = 0; i < SURFACE_NUM; i++) {
			/* create coded buffer once for all
			 * other VA buffers which won't be used again after vaRenderPicture.
			 * so APP can always vaCreateBuffer for every frame
			 * but coded buffer need to be mapped and accessed after vaRenderPicture/vaEndPicture
			 * so VA won't maintain the coded buffer
			 */
			va_status = vaCreateBuffer(va_dpy, context_id, VAEncCodedBufferType,
					codedbuf_size, 1, NULL, &gl_surfaces[i].coded_buf);
			CHECK_VASTATUS(va_status, "vaCreateBuffer");
		}
	}

	/* create OpenGL objects */
	for (int i = 0; i < SURFACE_NUM; i++) {
		if (use_zerocopy) {
			gl_surfaces[i].y_tex = resource_pool->create_2d_texture(GL_R8, 1, 1);
			gl_surfaces[i].cbcr_tex = resource_pool->create_2d_texture(GL_RG8, 1, 1);
		} else {
			size_t bytes_per_pixel = (global_flags.x264_bit_depth > 8) ? 2 : 1;

			// Generate a PBO to read into. It doesn't necessarily fit 1:1 with the VA-API
			// buffers, due to potentially differing pitch.
			glGenBuffers(1, &gl_surfaces[i].pbo);
			glBindBuffer(GL_PIXEL_PACK_BUFFER, gl_surfaces[i].pbo);
			glBufferStorage(GL_PIXEL_PACK_BUFFER, frame_width * frame_height * 2 * bytes_per_pixel, nullptr, GL_MAP_READ_BIT | GL_MAP_WRITE_BIT | GL_MAP_PERSISTENT_BIT);
			uint8_t *ptr = (uint8_t *)glMapBufferRange(GL_PIXEL_PACK_BUFFER, 0, frame_width * frame_height * 2 * bytes_per_pixel, GL_MAP_READ_BIT | GL_MAP_PERSISTENT_BIT);
			gl_surfaces[i].y_offset = 0;
			gl_surfaces[i].cbcr_offset = frame_width * frame_height * bytes_per_pixel;
			gl_surfaces[i].y_ptr = ptr + gl_surfaces[i].y_offset;
			gl_surfaces[i].cbcr_ptr = ptr + gl_surfaces[i].cbcr_offset;
			glBindBuffer(GL_PIXEL_PACK_BUFFER, 0);
		}
	}

	return 0;
}

// Given a list like 1 9 3 0 2 8 4 and a pivot element 3, will produce
//
//   2 1 0 [3] 4 8 9
template<class T, class C>
static void sort_two(T *begin, T *end, const T &pivot, const C &less_than)
{
	T *middle = partition(begin, end, [&](const T &elem) { return less_than(elem, pivot); });
	sort(begin, middle, [&](const T &a, const T &b) { return less_than(b, a); });
	sort(middle, end, less_than);
}

void QuickSyncEncoderImpl::update_ReferenceFrames(int current_display_frame, int frame_type)
{
    if (frame_type == FRAME_B)
        return;

    pic_param.CurrPic.frame_idx = current_ref_frame_num;

    CurrentCurrPic.flags = VA_PICTURE_H264_SHORT_TERM_REFERENCE;
    unique_lock<mutex> lock(storage_task_queue_mutex);

    // Insert the new frame at the start of the reference queue.
    reference_frames.push_front(ReferenceFrame{ CurrentCurrPic, current_display_frame });

    if (reference_frames.size() > num_ref_frames)
    {
        // The back frame frame is no longer in use as a reference.
        int display_frame_num = reference_frames.back().display_number;
        assert(surface_for_frame.count(display_frame_num));
        release_gl_surface(display_frame_num);
        reference_frames.pop_back();
    }

    // Mark this frame in use as a reference.
    assert(surface_for_frame.count(current_display_frame));
    ++surface_for_frame[current_display_frame]->refcount;
    
    current_ref_frame_num++;
    if (current_ref_frame_num > MaxFrameNum)
        current_ref_frame_num = 0;
}


void QuickSyncEncoderImpl::update_RefPicList_P(VAPictureH264 RefPicList0_P[MAX_NUM_REF2])
{
    const auto descending_by_frame_idx = [](const VAPictureH264 &a, const VAPictureH264 &b) {
        return a.frame_idx > b.frame_idx;
    };

    for (size_t i = 0; i < reference_frames.size(); ++i) {
        RefPicList0_P[i] = reference_frames[i].pic;
    }
    sort(&RefPicList0_P[0], &RefPicList0_P[reference_frames.size()], descending_by_frame_idx);
}

void QuickSyncEncoderImpl::update_RefPicList_B(VAPictureH264 RefPicList0_B[MAX_NUM_REF2], VAPictureH264 RefPicList1_B[MAX_NUM_REF2])
{
    const auto ascending_by_top_field_order_cnt = [](const VAPictureH264 &a, const VAPictureH264 &b) {
        return a.TopFieldOrderCnt < b.TopFieldOrderCnt;
    };
    const auto descending_by_top_field_order_cnt = [](const VAPictureH264 &a, const VAPictureH264 &b) {
        return a.TopFieldOrderCnt > b.TopFieldOrderCnt;
    };

    for (size_t i = 0; i < reference_frames.size(); ++i) {
        RefPicList0_B[i] = reference_frames[i].pic;
        RefPicList1_B[i] = reference_frames[i].pic;
    }
    sort_two(&RefPicList0_B[0], &RefPicList0_B[reference_frames.size()], CurrentCurrPic, ascending_by_top_field_order_cnt);
    sort_two(&RefPicList1_B[0], &RefPicList1_B[reference_frames.size()], CurrentCurrPic, descending_by_top_field_order_cnt);
}


int QuickSyncEncoderImpl::render_sequence()
{
    VABufferID seq_param_buf, rc_param_buf, render_id[2];
    VAStatus va_status;
    VAEncMiscParameterBuffer *misc_param;
    VAEncMiscParameterRateControl *misc_rate_ctrl;
    
    seq_param.level_idc = 41 /*SH_LEVEL_3*/;
    seq_param.picture_width_in_mbs = frame_width_mbaligned / 16;
    seq_param.picture_height_in_mbs = frame_height_mbaligned / 16;
    seq_param.bits_per_second = frame_bitrate;

    seq_param.intra_period = intra_period;
    seq_param.intra_idr_period = intra_idr_period;
    seq_param.ip_period = ip_period;

    seq_param.max_num_ref_frames = num_ref_frames;
    seq_param.seq_fields.bits.frame_mbs_only_flag = 1;
    seq_param.time_scale = TIMEBASE * 2;
    seq_param.num_units_in_tick = 1; /* Tc = num_units_in_tick / scale */
    seq_param.seq_fields.bits.log2_max_pic_order_cnt_lsb_minus4 = Log2MaxPicOrderCntLsb - 4;
    seq_param.seq_fields.bits.log2_max_frame_num_minus4 = Log2MaxFrameNum - 4;;
    seq_param.seq_fields.bits.frame_mbs_only_flag = 1;
    seq_param.seq_fields.bits.chroma_format_idc = 1;
    seq_param.seq_fields.bits.direct_8x8_inference_flag = 1;
    
    if (frame_width != frame_width_mbaligned ||
        frame_height != frame_height_mbaligned) {
        seq_param.frame_cropping_flag = 1;
        seq_param.frame_crop_left_offset = 0;
        seq_param.frame_crop_right_offset = (frame_width_mbaligned - frame_width)/2;
        seq_param.frame_crop_top_offset = 0;
        seq_param.frame_crop_bottom_offset = (frame_height_mbaligned - frame_height)/2;
    }
    
    va_status = vaCreateBuffer(va_dpy, context_id,
                               VAEncSequenceParameterBufferType,
                               sizeof(seq_param), 1, &seq_param, &seq_param_buf);
    CHECK_VASTATUS(va_status, "vaCreateBuffer");
    
    va_status = vaCreateBuffer(va_dpy, context_id,
                               VAEncMiscParameterBufferType,
                               sizeof(VAEncMiscParameterBuffer) + sizeof(VAEncMiscParameterRateControl),
                               1, NULL, &rc_param_buf);
    CHECK_VASTATUS(va_status, "vaCreateBuffer");
    
    vaMapBuffer(va_dpy, rc_param_buf, (void **)&misc_param);
    misc_param->type = VAEncMiscParameterTypeRateControl;
    misc_rate_ctrl = (VAEncMiscParameterRateControl *)misc_param->data;
    memset(misc_rate_ctrl, 0, sizeof(*misc_rate_ctrl));
    misc_rate_ctrl->bits_per_second = frame_bitrate;
    misc_rate_ctrl->target_percentage = 66;
    misc_rate_ctrl->window_size = 1000;
    misc_rate_ctrl->initial_qp = initial_qp;
    misc_rate_ctrl->min_qp = minimal_qp;
    misc_rate_ctrl->basic_unit_size = 0;
    vaUnmapBuffer(va_dpy, rc_param_buf);

    render_id[0] = seq_param_buf;
    render_id[1] = rc_param_buf;
    
    render_picture_and_delete(va_dpy, context_id, &render_id[0], 2);
    
    return 0;
}

static int calc_poc(int pic_order_cnt_lsb, int frame_type)
{
    static int PicOrderCntMsb_ref = 0, pic_order_cnt_lsb_ref = 0;
    int prevPicOrderCntMsb, prevPicOrderCntLsb;
    int PicOrderCntMsb, TopFieldOrderCnt;
    
    if (frame_type == FRAME_IDR)
        prevPicOrderCntMsb = prevPicOrderCntLsb = 0;
    else {
        prevPicOrderCntMsb = PicOrderCntMsb_ref;
        prevPicOrderCntLsb = pic_order_cnt_lsb_ref;
    }
    
    if ((pic_order_cnt_lsb < prevPicOrderCntLsb) &&
        ((prevPicOrderCntLsb - pic_order_cnt_lsb) >= (int)(MaxPicOrderCntLsb / 2)))
        PicOrderCntMsb = prevPicOrderCntMsb + MaxPicOrderCntLsb;
    else if ((pic_order_cnt_lsb > prevPicOrderCntLsb) &&
             ((pic_order_cnt_lsb - prevPicOrderCntLsb) > (int)(MaxPicOrderCntLsb / 2)))
        PicOrderCntMsb = prevPicOrderCntMsb - MaxPicOrderCntLsb;
    else
        PicOrderCntMsb = prevPicOrderCntMsb;
    
    TopFieldOrderCnt = PicOrderCntMsb + pic_order_cnt_lsb;

    if (frame_type != FRAME_B) {
        PicOrderCntMsb_ref = PicOrderCntMsb;
        pic_order_cnt_lsb_ref = pic_order_cnt_lsb;
    }
    
    return TopFieldOrderCnt;
}

int QuickSyncEncoderImpl::render_picture(GLSurface *surf, int frame_type, int display_frame_num, int gop_start_display_frame_num)
{
    VABufferID pic_param_buf;
    VAStatus va_status;
    size_t i = 0;

    pic_param.CurrPic.picture_id = surf->ref_surface;
    pic_param.CurrPic.frame_idx = current_ref_frame_num;
    pic_param.CurrPic.flags = 0;
    pic_param.CurrPic.TopFieldOrderCnt = calc_poc((display_frame_num - gop_start_display_frame_num) % MaxPicOrderCntLsb, frame_type);
    pic_param.CurrPic.BottomFieldOrderCnt = pic_param.CurrPic.TopFieldOrderCnt;
    CurrentCurrPic = pic_param.CurrPic;

    for (i = 0; i < reference_frames.size(); i++) {
        pic_param.ReferenceFrames[i] = reference_frames[i].pic;
    }
    for (i = reference_frames.size(); i < MAX_NUM_REF1; i++) {
        pic_param.ReferenceFrames[i].picture_id = VA_INVALID_SURFACE;
        pic_param.ReferenceFrames[i].flags = VA_PICTURE_H264_INVALID;
    }
    
    pic_param.pic_fields.bits.idr_pic_flag = (frame_type == FRAME_IDR);
    pic_param.pic_fields.bits.reference_pic_flag = (frame_type != FRAME_B);
    pic_param.pic_fields.bits.entropy_coding_mode_flag = h264_entropy_mode;
    pic_param.pic_fields.bits.deblocking_filter_control_present_flag = 1;
    pic_param.frame_num = current_ref_frame_num;  // FIXME: is this correct?
    pic_param.coded_buf = surf->coded_buf;
    pic_param.last_picture = false;  // FIXME
    pic_param.pic_init_qp = initial_qp;

    va_status = vaCreateBuffer(va_dpy, context_id, VAEncPictureParameterBufferType,
                               sizeof(pic_param), 1, &pic_param, &pic_param_buf);
    CHECK_VASTATUS(va_status, "vaCreateBuffer");

    render_picture_and_delete(va_dpy, context_id, &pic_param_buf, 1);

    return 0;
}

int QuickSyncEncoderImpl::render_packedsequence(YCbCrLumaCoefficients ycbcr_coefficients)
{
    VAEncPackedHeaderParameterBuffer packedheader_param_buffer;
    VABufferID packedseq_para_bufid, packedseq_data_bufid, render_id[2];
    unsigned int length_in_bits;
    unsigned char *packedseq_buffer = NULL;
    VAStatus va_status;

    length_in_bits = build_packed_seq_buffer(ycbcr_coefficients, &packedseq_buffer); 
    
    packedheader_param_buffer.type = VAEncPackedHeaderSequence;
    
    packedheader_param_buffer.bit_length = length_in_bits; /*length_in_bits*/
    packedheader_param_buffer.has_emulation_bytes = 0;
    va_status = vaCreateBuffer(va_dpy,
                               context_id,
                               VAEncPackedHeaderParameterBufferType,
                               sizeof(packedheader_param_buffer), 1, &packedheader_param_buffer,
                               &packedseq_para_bufid);
    CHECK_VASTATUS(va_status, "vaCreateBuffer");

    va_status = vaCreateBuffer(va_dpy,
                               context_id,
                               VAEncPackedHeaderDataBufferType,
                               (length_in_bits + 7) / 8, 1, packedseq_buffer,
                               &packedseq_data_bufid);
    CHECK_VASTATUS(va_status, "vaCreateBuffer");

    render_id[0] = packedseq_para_bufid;
    render_id[1] = packedseq_data_bufid;
    render_picture_and_delete(va_dpy, context_id, render_id, 2);

    free(packedseq_buffer);
    
    return 0;
}


int QuickSyncEncoderImpl::render_packedpicture()
{
    VAEncPackedHeaderParameterBuffer packedheader_param_buffer;
    VABufferID packedpic_para_bufid, packedpic_data_bufid, render_id[2];
    unsigned int length_in_bits;
    unsigned char *packedpic_buffer = NULL;
    VAStatus va_status;

    length_in_bits = build_packed_pic_buffer(&packedpic_buffer); 
    packedheader_param_buffer.type = VAEncPackedHeaderPicture;
    packedheader_param_buffer.bit_length = length_in_bits;
    packedheader_param_buffer.has_emulation_bytes = 0;

    va_status = vaCreateBuffer(va_dpy,
                               context_id,
                               VAEncPackedHeaderParameterBufferType,
                               sizeof(packedheader_param_buffer), 1, &packedheader_param_buffer,
                               &packedpic_para_bufid);
    CHECK_VASTATUS(va_status, "vaCreateBuffer");

    va_status = vaCreateBuffer(va_dpy,
                               context_id,
                               VAEncPackedHeaderDataBufferType,
                               (length_in_bits + 7) / 8, 1, packedpic_buffer,
                               &packedpic_data_bufid);
    CHECK_VASTATUS(va_status, "vaCreateBuffer");

    render_id[0] = packedpic_para_bufid;
    render_id[1] = packedpic_data_bufid;
    render_picture_and_delete(va_dpy, context_id, render_id, 2);

    free(packedpic_buffer);
    
    return 0;
}

void QuickSyncEncoderImpl::render_packedslice()
{
    VAEncPackedHeaderParameterBuffer packedheader_param_buffer;
    VABufferID packedslice_para_bufid, packedslice_data_bufid, render_id[2];
    unsigned int length_in_bits;
    unsigned char *packedslice_buffer = NULL;
    VAStatus va_status;

    length_in_bits = build_packed_slice_buffer(&packedslice_buffer);
    packedheader_param_buffer.type = VAEncPackedHeaderSlice;
    packedheader_param_buffer.bit_length = length_in_bits;
    packedheader_param_buffer.has_emulation_bytes = 0;

    va_status = vaCreateBuffer(va_dpy,
                               context_id,
                               VAEncPackedHeaderParameterBufferType,
                               sizeof(packedheader_param_buffer), 1, &packedheader_param_buffer,
                               &packedslice_para_bufid);
    CHECK_VASTATUS(va_status, "vaCreateBuffer");

    va_status = vaCreateBuffer(va_dpy,
                               context_id,
                               VAEncPackedHeaderDataBufferType,
                               (length_in_bits + 7) / 8, 1, packedslice_buffer,
                               &packedslice_data_bufid);
    CHECK_VASTATUS(va_status, "vaCreateBuffer");

    render_id[0] = packedslice_para_bufid;
    render_id[1] = packedslice_data_bufid;
    render_picture_and_delete(va_dpy, context_id, render_id, 2);

    free(packedslice_buffer);
}

int QuickSyncEncoderImpl::render_slice(int encoding_frame_num, int display_frame_num, int gop_start_display_frame_num, int frame_type)
{
    VABufferID slice_param_buf;
    VAStatus va_status;
    int i;

    /* one frame, one slice */
    slice_param.macroblock_address = 0;
    slice_param.num_macroblocks = frame_width_mbaligned * frame_height_mbaligned/(16*16); /* Measured by MB */
    slice_param.slice_type = (frame_type == FRAME_IDR)?2:frame_type;
    if (frame_type == FRAME_IDR) {
        if (encoding_frame_num != 0)
            ++slice_param.idr_pic_id;
    } else if (frame_type == FRAME_P) {
        VAPictureH264 RefPicList0_P[MAX_NUM_REF2];
        update_RefPicList_P(RefPicList0_P);

        int refpiclist0_max = h264_maxref & 0xffff;
        memcpy(slice_param.RefPicList0, RefPicList0_P, refpiclist0_max*sizeof(VAPictureH264));

        for (i = refpiclist0_max; i < MAX_NUM_REF2; i++) {
            slice_param.RefPicList0[i].picture_id = VA_INVALID_SURFACE;
            slice_param.RefPicList0[i].flags = VA_PICTURE_H264_INVALID;
        }
    } else if (frame_type == FRAME_B) {
        VAPictureH264 RefPicList0_B[MAX_NUM_REF2], RefPicList1_B[MAX_NUM_REF2];
        update_RefPicList_B(RefPicList0_B, RefPicList1_B);

        int refpiclist0_max = h264_maxref & 0xffff;
        int refpiclist1_max = (h264_maxref >> 16) & 0xffff;

        memcpy(slice_param.RefPicList0, RefPicList0_B, refpiclist0_max*sizeof(VAPictureH264));
        for (i = refpiclist0_max; i < MAX_NUM_REF2; i++) {
            slice_param.RefPicList0[i].picture_id = VA_INVALID_SURFACE;
            slice_param.RefPicList0[i].flags = VA_PICTURE_H264_INVALID;
        }

        memcpy(slice_param.RefPicList1, RefPicList1_B, refpiclist1_max*sizeof(VAPictureH264));
        for (i = refpiclist1_max; i < MAX_NUM_REF2; i++) {
            slice_param.RefPicList1[i].picture_id = VA_INVALID_SURFACE;
            slice_param.RefPicList1[i].flags = VA_PICTURE_H264_INVALID;
        }
    }

    slice_param.slice_alpha_c0_offset_div2 = 0;
    slice_param.slice_beta_offset_div2 = 0;
    slice_param.direct_spatial_mv_pred_flag = 1;
    slice_param.pic_order_cnt_lsb = (display_frame_num - gop_start_display_frame_num) % MaxPicOrderCntLsb;
    

    if (h264_packedheader &&
        config_attrib[enc_packed_header_idx].value & VA_ENC_PACKED_HEADER_SLICE)
        render_packedslice();

    va_status = vaCreateBuffer(va_dpy, context_id, VAEncSliceParameterBufferType,
                               sizeof(slice_param), 1, &slice_param, &slice_param_buf);
    CHECK_VASTATUS(va_status, "vaCreateBuffer");

    render_picture_and_delete(va_dpy, context_id, &slice_param_buf, 1);

    return 0;
}



void QuickSyncEncoderImpl::save_codeddata(GLSurface *surf, storage_task task)
{    
	VACodedBufferSegment *buf_list = NULL;
	VAStatus va_status;

	string data;

	va_status = vaMapBuffer(va_dpy, surf->coded_buf, (void **)(&buf_list));
	CHECK_VASTATUS(va_status, "vaMapBuffer");
	while (buf_list != NULL) {
		data.append(reinterpret_cast<const char *>(buf_list->buf), buf_list->size);
		buf_list = (VACodedBufferSegment *) buf_list->next;
	}
	vaUnmapBuffer(va_dpy, surf->coded_buf);

	static int frameno = 0;
	print_latency("Current Quick Sync latency (video inputs → disk mux):",
		task.received_ts, (task.frame_type == FRAME_B), &frameno, &qs_latency_histogram);

	{
		// Add video.
		AVPacket pkt;
		memset(&pkt, 0, sizeof(pkt));
		pkt.buf = nullptr;
		pkt.data = reinterpret_cast<uint8_t *>(&data[0]);
		pkt.size = data.size();
		pkt.stream_index = 0;
		if (task.frame_type == FRAME_IDR) {
			pkt.flags = AV_PKT_FLAG_KEY;
		} else {
			pkt.flags = 0;
		}
		pkt.duration = task.duration;
		if (file_mux) {
			file_mux->add_packet(pkt, task.pts + global_delay(), task.dts + global_delay());
		}
		if (!global_flags.uncompressed_video_to_http &&
		    !global_flags.x264_video_to_http) {
			stream_mux->add_packet(pkt, task.pts + global_delay(), task.dts + global_delay());
		}
	}
}


// this is weird. but it seems to put a new frame onto the queue
void QuickSyncEncoderImpl::storage_task_enqueue(storage_task task)
{
	unique_lock<mutex> lock(storage_task_queue_mutex);
	storage_task_queue.push(move(task));
	storage_task_queue_changed.notify_all();
}

void QuickSyncEncoderImpl::storage_task_thread()
{
	pthread_setname_np(pthread_self(), "QS_Storage");
	for ( ;; ) {
		storage_task current;
		GLSurface *surf;
		{
			// wait until there's an encoded frame  
			unique_lock<mutex> lock(storage_task_queue_mutex);
			storage_task_queue_changed.wait(lock, [this]{ return storage_thread_should_quit || !storage_task_queue.empty(); });
			if (storage_thread_should_quit && storage_task_queue.empty()) return;
			current = move(storage_task_queue.front());
			storage_task_queue.pop();
			surf = surface_for_frame[current.display_order];
			assert(surf != nullptr);
		}

		VAStatus va_status;

		size_t display_order = current.display_order;
		vector<size_t> ref_display_frame_numbers = move(current.ref_display_frame_numbers);
	   
		// waits for data, then saves it to disk.
		va_status = vaSyncSurface(va_dpy, surf->src_surface);
		CHECK_VASTATUS(va_status, "vaSyncSurface");
		save_codeddata(surf, move(current));

		// Unlock the frame, and all its references.
		{
			unique_lock<mutex> lock(storage_task_queue_mutex);
			release_gl_surface(display_order);

			for (size_t frame_num : ref_display_frame_numbers) {
				release_gl_surface(frame_num);
			}
		}
	}
}

void QuickSyncEncoderImpl::release_encode()
{
	for (unsigned i = 0; i < SURFACE_NUM; i++) {
		vaDestroyBuffer(va_dpy, gl_surfaces[i].coded_buf);
		vaDestroySurfaces(va_dpy, &gl_surfaces[i].src_surface, 1);
		vaDestroySurfaces(va_dpy, &gl_surfaces[i].ref_surface, 1);
	}

	vaDestroyContext(va_dpy, context_id);
	vaDestroyConfig(va_dpy, config_id);
}

void QuickSyncEncoderImpl::release_gl_resources()
{
	assert(is_shutdown);
	if (has_released_gl_resources) {
		return;
	}

	for (unsigned i = 0; i < SURFACE_NUM; i++) {
		if (use_zerocopy) {
			resource_pool->release_2d_texture(gl_surfaces[i].y_tex);
			resource_pool->release_2d_texture(gl_surfaces[i].cbcr_tex);
		} else {
			glBindBuffer(GL_PIXEL_PACK_BUFFER, gl_surfaces[i].pbo);
			glUnmapBuffer(GL_PIXEL_PACK_BUFFER);
			glBindBuffer(GL_PIXEL_PACK_BUFFER, 0);
			glDeleteBuffers(1, &gl_surfaces[i].pbo);
		}
	}

	has_released_gl_resources = true;
}

int QuickSyncEncoderImpl::deinit_va()
{ 
    vaTerminate(va_dpy);

    va_close_display(va_dpy);

    return 0;
}

QuickSyncEncoderImpl::QuickSyncEncoderImpl(const std::string &filename, ResourcePool *resource_pool, QSurface *surface, const string &va_display, int width, int height, AVOutputFormat *oformat, X264Encoder *x264_encoder, DiskSpaceEstimator *disk_space_estimator)
	: current_storage_frame(0), resource_pool(resource_pool), surface(surface), x264_encoder(x264_encoder), frame_width(width), frame_height(height), disk_space_estimator(disk_space_estimator)
{
	file_audio_encoder.reset(new AudioEncoder(AUDIO_OUTPUT_CODEC_NAME, DEFAULT_AUDIO_OUTPUT_BIT_RATE, oformat));
	open_output_file(filename);
	file_audio_encoder->add_mux(file_mux.get());

	frame_width_mbaligned = (frame_width + 15) & (~15);
	frame_height_mbaligned = (frame_height + 15) & (~15);

	//print_input();

	if (global_flags.x264_video_to_http || global_flags.x264_video_to_disk) {
		assert(x264_encoder != nullptr);
	} else {
		assert(x264_encoder == nullptr);
	}

	enable_zerocopy_if_possible();
	if (!global_flags.x264_video_to_disk) {
		init_va(va_display);
	}
	setup_encode();

	if (!global_flags.x264_video_to_disk) {
		memset(&seq_param, 0, sizeof(seq_param));
		memset(&pic_param, 0, sizeof(pic_param));
		memset(&slice_param, 0, sizeof(slice_param));
	}

	call_once(quick_sync_metrics_inited, [](){
		mixer_latency_histogram.init("mixer");
		qs_latency_histogram.init("quick_sync");
		current_file_mux_metrics.init({{ "destination", "current_file" }});
		total_mux_metrics.init({{ "destination", "files_total" }});
		global_metrics.add("current_file_start_time_seconds", &metric_current_file_start_time_seconds, Metrics::TYPE_GAUGE);
		global_metrics.add("quick_sync_stalled_frames", &metric_quick_sync_stalled_frames);
	});

	storage_thread = thread(&QuickSyncEncoderImpl::storage_task_thread, this);

	encode_thread = thread([this]{
		QOpenGLContext *context = create_context(this->surface);
		eglBindAPI(EGL_OPENGL_API);
		if (!make_current(context, this->surface)) {
			printf("display=%p surface=%p context=%p curr=%p err=%d\n", eglGetCurrentDisplay(), this->surface, context, eglGetCurrentContext(),
				eglGetError());
			exit(1);
		}
		encode_thread_func();
		delete_context(context);
	});
}

QuickSyncEncoderImpl::~QuickSyncEncoderImpl()
{
	shutdown();
	release_gl_resources();
}

QuickSyncEncoderImpl::GLSurface *QuickSyncEncoderImpl::allocate_gl_surface()
{
	for (unsigned i = 0; i < SURFACE_NUM; ++i) {
		if (gl_surfaces[i].refcount == 0) {
			++gl_surfaces[i].refcount;
			return &gl_surfaces[i];
		}
	}
	return nullptr;
}

void QuickSyncEncoderImpl::release_gl_surface(size_t display_frame_num)
{
	assert(surface_for_frame.count(display_frame_num));
	QuickSyncEncoderImpl::GLSurface *surf = surface_for_frame[display_frame_num];
	if (--surf->refcount == 0) {
		assert(surface_for_frame.count(display_frame_num));
		surface_for_frame.erase(display_frame_num);
		storage_task_queue_changed.notify_all();
	}
}

bool QuickSyncEncoderImpl::is_zerocopy() const
{
	return use_zerocopy;
}

bool QuickSyncEncoderImpl::begin_frame(int64_t pts, int64_t duration, YCbCrLumaCoefficients ycbcr_coefficients, const vector<RefCountedFrame> &input_frames, GLuint *y_tex, GLuint *cbcr_tex)
{
	assert(!is_shutdown);
	GLSurface *surf = nullptr;
	{
		// Wait until this frame slot is done encoding.
		unique_lock<mutex> lock(storage_task_queue_mutex);
		surf = allocate_gl_surface();
		if (surf == nullptr) {
			fprintf(stderr, "Warning: No free slots for frame %d, rendering has to wait for H.264 encoder\n",
				current_storage_frame);
			++metric_quick_sync_stalled_frames;
			storage_task_queue_changed.wait(lock, [this, &surf]{
				if (storage_thread_should_quit)
					return true;
				surf = allocate_gl_surface();
				return surf != nullptr;
			});
		}
		if (storage_thread_should_quit) return false;
		assert(surf != nullptr);
		surface_for_frame[current_storage_frame] = surf;
	}

	if (use_zerocopy) {
		*y_tex = surf->y_tex;
		*cbcr_tex = surf->cbcr_tex;
	} else {
		surf->y_tex = *y_tex;
		surf->cbcr_tex = *cbcr_tex;
	}

	if (!global_flags.x264_video_to_disk) {
		VAStatus va_status = vaDeriveImage(va_dpy, surf->src_surface, &surf->surface_image);
		CHECK_VASTATUS(va_status, "vaDeriveImage");

		if (use_zerocopy) {
			VABufferInfo buf_info;
			buf_info.mem_type = VA_SURFACE_ATTRIB_MEM_TYPE_DRM_PRIME;  // or VA_SURFACE_ATTRIB_MEM_TYPE_KERNEL_DRM?
			va_status = vaAcquireBufferHandle(va_dpy, surf->surface_image.buf, &buf_info);
			CHECK_VASTATUS(va_status, "vaAcquireBufferHandle");

			// Create Y image.
			surf->y_egl_image = EGL_NO_IMAGE_KHR;
			EGLint y_attribs[] = {
				EGL_WIDTH, frame_width,
				EGL_HEIGHT, frame_height,
				EGL_LINUX_DRM_FOURCC_EXT, fourcc_code('R', '8', ' ', ' '),
				EGL_DMA_BUF_PLANE0_FD_EXT, EGLint(buf_info.handle),
				EGL_DMA_BUF_PLANE0_OFFSET_EXT, EGLint(surf->surface_image.offsets[0]),
				EGL_DMA_BUF_PLANE0_PITCH_EXT, EGLint(surf->surface_image.pitches[0]),
				EGL_NONE
			};

			surf->y_egl_image = eglCreateImageKHR(eglGetCurrentDisplay(), EGL_NO_CONTEXT, EGL_LINUX_DMA_BUF_EXT, NULL, y_attribs);
			assert(surf->y_egl_image != EGL_NO_IMAGE_KHR);

			// Associate Y image to a texture.
			glBindTexture(GL_TEXTURE_2D, *y_tex);
			glEGLImageTargetTexture2DOES(GL_TEXTURE_2D, surf->y_egl_image);

			// Create CbCr image.
			surf->cbcr_egl_image = EGL_NO_IMAGE_KHR;
			EGLint cbcr_attribs[] = {
				EGL_WIDTH, frame_width,
				EGL_HEIGHT, frame_height,
				EGL_LINUX_DRM_FOURCC_EXT, fourcc_code('G', 'R', '8', '8'),
				EGL_DMA_BUF_PLANE0_FD_EXT, EGLint(buf_info.handle),
				EGL_DMA_BUF_PLANE0_OFFSET_EXT, EGLint(surf->surface_image.offsets[1]),
				EGL_DMA_BUF_PLANE0_PITCH_EXT, EGLint(surf->surface_image.pitches[1]),
				EGL_NONE
			};

			surf->cbcr_egl_image = eglCreateImageKHR(eglGetCurrentDisplay(), EGL_NO_CONTEXT, EGL_LINUX_DMA_BUF_EXT, NULL, cbcr_attribs);
			assert(surf->cbcr_egl_image != EGL_NO_IMAGE_KHR);

			// Associate CbCr image to a texture.
			glBindTexture(GL_TEXTURE_2D, *cbcr_tex);
			glEGLImageTargetTexture2DOES(GL_TEXTURE_2D, surf->cbcr_egl_image);
		}
	}

	current_video_frame = PendingFrame{ {}, input_frames, pts, duration, ycbcr_coefficients };

	return true;
}

void QuickSyncEncoderImpl::add_audio(int64_t pts, vector<float> audio)
{
	lock_guard<mutex> lock(file_audio_encoder_mutex);
	assert(!is_shutdown);
	file_audio_encoder->encode_audio(audio, pts + global_delay());
}

RefCountedGLsync QuickSyncEncoderImpl::end_frame()
{
	assert(!is_shutdown);

	if (!use_zerocopy) {
		GLenum type = global_flags.x264_bit_depth > 8 ? GL_UNSIGNED_SHORT : GL_UNSIGNED_BYTE;
		GLSurface *surf;
		{
			unique_lock<mutex> lock(storage_task_queue_mutex);
			surf = surface_for_frame[current_storage_frame];
			assert(surf != nullptr);
		}

		glPixelStorei(GL_PACK_ROW_LENGTH, 0);
		check_error();

		glBindBuffer(GL_PIXEL_PACK_BUFFER, surf->pbo);
		check_error();

		glBindTexture(GL_TEXTURE_2D, surf->y_tex);
		check_error();
		glGetTexImage(GL_TEXTURE_2D, 0, GL_RED, type, BUFFER_OFFSET(surf->y_offset));
		check_error();

		glBindTexture(GL_TEXTURE_2D, surf->cbcr_tex);
		check_error();
		glGetTexImage(GL_TEXTURE_2D, 0, GL_RG, type, BUFFER_OFFSET(surf->cbcr_offset));
		check_error();

		// We don't own these; the caller does.
		surf->y_tex = surf->cbcr_tex = 0;

		glBindTexture(GL_TEXTURE_2D, 0);
		check_error();
		glBindBuffer(GL_PIXEL_PACK_BUFFER, 0);
		check_error();

		glMemoryBarrier(GL_TEXTURE_UPDATE_BARRIER_BIT | GL_CLIENT_MAPPED_BUFFER_BARRIER_BIT);
		check_error();
	}

	RefCountedGLsync fence = RefCountedGLsync(GL_SYNC_GPU_COMMANDS_COMPLETE, /*flags=*/0);
	check_error();
	glFlush();  // Make the H.264 thread see the fence as soon as possible.
	check_error();

	{
		unique_lock<mutex> lock(frame_queue_mutex);
		current_video_frame.fence = fence;
		pending_video_frames.push(move(current_video_frame));
		++current_storage_frame;
	}
	frame_queue_nonempty.notify_all();
	return fence;
}

void QuickSyncEncoderImpl::shutdown()
{
	if (is_shutdown) {
		return;
	}

	{
		unique_lock<mutex> lock(frame_queue_mutex);
		encode_thread_should_quit = true;
		frame_queue_nonempty.notify_all();
	}
	encode_thread.join();
	{
		unique_lock<mutex> lock(storage_task_queue_mutex);
		storage_thread_should_quit = true;
		frame_queue_nonempty.notify_all();
		storage_task_queue_changed.notify_all();
	}
	storage_thread.join();

	// Encode any leftover audio in the queues, and also any delayed frames.
	{
		lock_guard<mutex> lock(file_audio_encoder_mutex);
		file_audio_encoder->encode_last_audio();
	}

	if (!global_flags.x264_video_to_disk) {
		release_encode();
		deinit_va();
	}
	is_shutdown = true;
}

void QuickSyncEncoderImpl::close_file()
{
	file_mux.reset();
	metric_current_file_start_time_seconds = 0.0 / 0.0;
}

void QuickSyncEncoderImpl::open_output_file(const std::string &filename)
{
	AVFormatContext *avctx = avformat_alloc_context();
	avctx->oformat = av_guess_format(NULL, filename.c_str(), NULL);
	assert(filename.size() < sizeof(avctx->filename) - 1);
	strcpy(avctx->filename, filename.c_str());

	string url = "file:" + filename;
	int ret = avio_open2(&avctx->pb, url.c_str(), AVIO_FLAG_WRITE, &avctx->interrupt_callback, NULL);
	if (ret < 0) {
		char tmp[AV_ERROR_MAX_STRING_SIZE];
		fprintf(stderr, "%s: avio_open2() failed: %s\n", filename.c_str(), av_make_error_string(tmp, sizeof(tmp), ret));
		exit(1);
	}

	string video_extradata;  // FIXME: See other comment about global headers.
	if (global_flags.x264_video_to_disk) {
		video_extradata = x264_encoder->get_global_headers();
	}

	current_file_mux_metrics.reset();

	{
		lock_guard<mutex> lock(file_audio_encoder_mutex);
		AVCodecParametersWithDeleter audio_codecpar = file_audio_encoder->get_codec_parameters();
		file_mux.reset(new Mux(avctx, frame_width, frame_height, Mux::CODEC_H264, video_extradata, audio_codecpar.get(), TIMEBASE,
			std::bind(&DiskSpaceEstimator::report_write, disk_space_estimator, filename, _1),
			Mux::WRITE_BACKGROUND,
			{ &current_file_mux_metrics, &total_mux_metrics }));
	}
	metric_current_file_start_time_seconds = get_timestamp_for_metrics();

	if (global_flags.x264_video_to_disk) {
		x264_encoder->add_mux(file_mux.get());
	}
}

void QuickSyncEncoderImpl::encode_thread_func()
{
	pthread_setname_np(pthread_self(), "QS_Encode");

	int64_t last_dts = -1;
	int gop_start_display_frame_num = 0;
	for (int display_frame_num = 0; ; ++display_frame_num) {
		// Wait for the frame to be in the queue. Note that this only means
		// we started rendering it.
		PendingFrame frame;
		{
			unique_lock<mutex> lock(frame_queue_mutex);
			frame_queue_nonempty.wait(lock, [this]{
				return encode_thread_should_quit || !pending_video_frames.empty();
			});
			if (encode_thread_should_quit && pending_video_frames.empty()) {
				// We may have queued frames left in the reorder buffer
				// that were supposed to be B-frames, but have no P-frame
				// to be encoded against. If so, encode them all as
				// P-frames instead. Note that this happens under the mutex,
				// but nobody else uses it at this point, since we're shutting down,
				// so there's no contention.
				encode_remaining_frames_as_p(quicksync_encoding_frame_num, gop_start_display_frame_num, last_dts);
				return;
			} else {
				frame = move(pending_video_frames.front());
				pending_video_frames.pop();
			}
		}

		// Pass the frame on to x264 (or uncompressed to HTTP) as needed.
		// Note that this implicitly waits for the frame to be done rendering.
		pass_frame(frame, display_frame_num, frame.pts, frame.duration);

		if (global_flags.x264_video_to_disk) {
			unique_lock<mutex> lock(storage_task_queue_mutex);
			release_gl_surface(display_frame_num);
			continue;
		}

		reorder_buffer[display_frame_num] = move(frame);

		// Now encode as many QuickSync frames as we can using the frames we have available.
		// (It could be zero, or it could be multiple.) FIXME: make a function.
		for ( ;; ) {
			int pts_lag;
			int frame_type, quicksync_display_frame_num;
			encoding2display_order(quicksync_encoding_frame_num, intra_period, intra_idr_period, ip_period,
			                       &quicksync_display_frame_num, &frame_type, &pts_lag);
			if (!reorder_buffer.count(quicksync_display_frame_num)) {
				break;
			}
			frame = move(reorder_buffer[quicksync_display_frame_num]);
			reorder_buffer.erase(quicksync_display_frame_num);

			if (frame_type == FRAME_IDR) {
				// Release any reference frames from the previous GOP.
				for (const ReferenceFrame &frame : reference_frames) {
					release_gl_surface(frame.display_number);
				}
				reference_frames.clear();
				current_ref_frame_num = 0;
				gop_start_display_frame_num = quicksync_display_frame_num;
			}

			// Determine the dts of this frame.
			int64_t dts;
			if (pts_lag == -1) {
				assert(last_dts != -1);
				dts = last_dts + (TIMEBASE / MAX_FPS);
			} else {
				dts = frame.pts - pts_lag;
			}
			last_dts = dts;

			encode_frame(frame, quicksync_encoding_frame_num, quicksync_display_frame_num, gop_start_display_frame_num, frame_type, frame.pts, dts, frame.duration, frame.ycbcr_coefficients);
			++quicksync_encoding_frame_num;
		}
	}
}

void QuickSyncEncoderImpl::encode_remaining_frames_as_p(int encoding_frame_num, int gop_start_display_frame_num, int64_t last_dts)
{
	if (reorder_buffer.empty()) {
		return;
	}

	for (auto &pending_frame : reorder_buffer) {
		int display_frame_num = pending_frame.first;
		assert(display_frame_num > 0);
		PendingFrame frame = move(pending_frame.second);
		int64_t dts = last_dts + (TIMEBASE / MAX_FPS);
		printf("Finalizing encode: Encoding leftover frame %d as P-frame instead of B-frame.\n", display_frame_num);
		encode_frame(frame, encoding_frame_num++, display_frame_num, gop_start_display_frame_num, FRAME_P, frame.pts, dts, frame.duration, frame.ycbcr_coefficients);
		last_dts = dts;
	}
}

void QuickSyncEncoderImpl::add_packet_for_uncompressed_frame(int64_t pts, int64_t duration, const uint8_t *data)
{
	AVPacket pkt;
	memset(&pkt, 0, sizeof(pkt));
	pkt.buf = nullptr;
	pkt.data = const_cast<uint8_t *>(data);
	pkt.size = frame_width * frame_height * 2;
	pkt.stream_index = 0;
	pkt.flags = AV_PKT_FLAG_KEY;
	pkt.duration = duration;
	stream_mux->add_packet(pkt, pts, pts);
}

namespace {

void memcpy_with_pitch(uint8_t *dst, const uint8_t *src, size_t src_width, size_t dst_pitch, size_t height)
{
	if (src_width == dst_pitch) {
		memcpy(dst, src, src_width * height);
	} else {
		for (size_t y = 0; y < height; ++y) {
			const uint8_t *sptr = src + y * src_width;
			uint8_t *dptr = dst + y * dst_pitch;
			memcpy(dptr, sptr, src_width);
		}
	}
}

}  // namespace

void QuickSyncEncoderImpl::pass_frame(QuickSyncEncoderImpl::PendingFrame frame, int display_frame_num, int64_t pts, int64_t duration)
{
	// Wait for the GPU to be done with the frame.
	GLenum sync_status;
	do {
		sync_status = glClientWaitSync(frame.fence.get(), 0, 0);
		check_error();
		if (sync_status == GL_TIMEOUT_EXPIRED) {
			// NVIDIA likes to busy-wait; yield instead.
			this_thread::sleep_for(milliseconds(1));
		}
	} while (sync_status == GL_TIMEOUT_EXPIRED);
	assert(sync_status != GL_WAIT_FAILED);

	ReceivedTimestamps received_ts = find_received_timestamp(frame.input_frames);
	static int frameno = 0;
	print_latency("Current mixer latency (video inputs → ready for encode):",
		received_ts, false, &frameno, &mixer_latency_histogram);

	// Release back any input frames we needed to render this frame.
	frame.input_frames.clear();

	GLSurface *surf;
	{
		unique_lock<mutex> lock(storage_task_queue_mutex);
		surf = surface_for_frame[display_frame_num];
		assert(surf != nullptr);
	}
	uint8_t *data = reinterpret_cast<uint8_t *>(surf->y_ptr);
	if (global_flags.uncompressed_video_to_http) {
		add_packet_for_uncompressed_frame(pts, duration, data);
	} else if (global_flags.x264_video_to_http || global_flags.x264_video_to_disk) {
		x264_encoder->add_frame(pts, duration, frame.ycbcr_coefficients, data, received_ts);
	}
}

void QuickSyncEncoderImpl::encode_frame(QuickSyncEncoderImpl::PendingFrame frame, int encoding_frame_num, int display_frame_num, int gop_start_display_frame_num,
                                        int frame_type, int64_t pts, int64_t dts, int64_t duration, YCbCrLumaCoefficients ycbcr_coefficients)
{
	const ReceivedTimestamps received_ts = find_received_timestamp(frame.input_frames);

	GLSurface *surf;
	{
		unique_lock<mutex> lock(storage_task_queue_mutex);
		surf = surface_for_frame[display_frame_num];
		assert(surf != nullptr);
	}
	VAStatus va_status;

	if (use_zerocopy) {
		eglDestroyImageKHR(eglGetCurrentDisplay(), surf->y_egl_image);
		eglDestroyImageKHR(eglGetCurrentDisplay(), surf->cbcr_egl_image);
		va_status = vaReleaseBufferHandle(va_dpy, surf->surface_image.buf);
		CHECK_VASTATUS(va_status, "vaReleaseBufferHandle");
	} else {
		// Upload the frame to VA-API.
		unsigned char *surface_p = nullptr;
		vaMapBuffer(va_dpy, surf->surface_image.buf, (void **)&surface_p);

		unsigned char *va_y_ptr = (unsigned char *)surface_p + surf->surface_image.offsets[0];
		memcpy_with_pitch(va_y_ptr, surf->y_ptr, frame_width, surf->surface_image.pitches[0], frame_height);

		unsigned char *va_cbcr_ptr = (unsigned char *)surface_p + surf->surface_image.offsets[1];
		memcpy_with_pitch(va_cbcr_ptr, surf->cbcr_ptr, (frame_width / 2) * sizeof(uint16_t), surf->surface_image.pitches[1], frame_height / 2);

		va_status = vaUnmapBuffer(va_dpy, surf->surface_image.buf);
		CHECK_VASTATUS(va_status, "vaUnmapBuffer");
	}

	va_status = vaDestroyImage(va_dpy, surf->surface_image.image_id);
	CHECK_VASTATUS(va_status, "vaDestroyImage");

	// Schedule the frame for encoding.
	VASurfaceID va_surface = surf->src_surface;
	va_status = vaBeginPicture(va_dpy, context_id, va_surface);
	CHECK_VASTATUS(va_status, "vaBeginPicture");

	if (frame_type == FRAME_IDR) {
		// FIXME: If the mux wants global headers, we should not put the
		// SPS/PPS before each IDR frame, but rather put it into the
		// codec extradata (formatted differently?).
		//
		// NOTE: If we change ycbcr_coefficients, it will not take effect
		// before the next IDR frame. This is acceptable, as it should only
		// happen on a mode change, which is rare.
		render_sequence();
		render_picture(surf, frame_type, display_frame_num, gop_start_display_frame_num);
		if (h264_packedheader) {
			render_packedsequence(ycbcr_coefficients);
			render_packedpicture();
		}
	} else {
		//render_sequence();
		render_picture(surf, frame_type, display_frame_num, gop_start_display_frame_num);
	}
	render_slice(encoding_frame_num, display_frame_num, gop_start_display_frame_num, frame_type);

	va_status = vaEndPicture(va_dpy, context_id);
	CHECK_VASTATUS(va_status, "vaEndPicture");

	update_ReferenceFrames(display_frame_num, frame_type);

	vector<size_t> ref_display_frame_numbers;

	// Lock the references for this frame; otherwise, they could be
	// rendered to before this frame is done encoding.
	{
		unique_lock<mutex> lock(storage_task_queue_mutex);
		for (const ReferenceFrame &frame : reference_frames) {
			assert(surface_for_frame.count(frame.display_number));
			++surface_for_frame[frame.display_number]->refcount;
			ref_display_frame_numbers.push_back(frame.display_number);
		}
	}

	// so now the data is done encoding (well, async job kicked off)...
	// we send that to the storage thread
	storage_task tmp;
	tmp.display_order = display_frame_num;
	tmp.frame_type = frame_type;
	tmp.pts = pts;
	tmp.dts = dts;
	tmp.duration = duration;
	tmp.ycbcr_coefficients = ycbcr_coefficients;
	tmp.received_ts = received_ts;
	tmp.ref_display_frame_numbers = move(ref_display_frame_numbers);
	storage_task_enqueue(move(tmp));
}

// Proxy object.
QuickSyncEncoder::QuickSyncEncoder(const std::string &filename, ResourcePool *resource_pool, QSurface *surface, const string &va_display, int width, int height, AVOutputFormat *oformat, X264Encoder *x264_encoder, DiskSpaceEstimator *disk_space_estimator)
	: impl(new QuickSyncEncoderImpl(filename, resource_pool, surface, va_display, width, height, oformat, x264_encoder, disk_space_estimator)) {}

// Must be defined here because unique_ptr<> destructor needs to know the impl.
QuickSyncEncoder::~QuickSyncEncoder() {}

void QuickSyncEncoder::add_audio(int64_t pts, vector<float> audio)
{
	impl->add_audio(pts, audio);
}

bool QuickSyncEncoder::is_zerocopy() const
{
	return impl->is_zerocopy();
}

bool QuickSyncEncoder::begin_frame(int64_t pts, int64_t duration, YCbCrLumaCoefficients ycbcr_coefficients, const vector<RefCountedFrame> &input_frames, GLuint *y_tex, GLuint *cbcr_tex)
{
	return impl->begin_frame(pts, duration, ycbcr_coefficients, input_frames, y_tex, cbcr_tex);
}

RefCountedGLsync QuickSyncEncoder::end_frame()
{
	return impl->end_frame();
}

void QuickSyncEncoder::shutdown()
{
	impl->shutdown();
}

void QuickSyncEncoder::close_file()
{
	impl->shutdown();
}

void QuickSyncEncoder::set_stream_mux(Mux *mux)
{
	impl->set_stream_mux(mux);
}

int64_t QuickSyncEncoder::global_delay() const {
	return impl->global_delay();
}
