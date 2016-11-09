/*******************************************************************************
* Copyright 2016 Intel Corporation
*
* Licensed under the Apache License, Version 2.0 (the "License");
* you may not use this file except in compliance with the License.
* You may obtain a copy of the License at
*
*     http://www.apache.org/licenses/LICENSE-2.0
*
* Unless required by applicable law or agreed to in writing, software
* distributed under the License is distributed on an "AS IS" BASIS,
* WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
* See the License for the specific language governing permissions and
* limitations under the License.
*******************************************************************************/

#include "c_types_map.hpp"
#include "nstl.hpp"
#include "type_helpers.hpp"
#include "utils.hpp"

#include "jit_avx2_conv_kernel_f32.hpp"

namespace mkldnn {
namespace impl {
namespace cpu {

using namespace mkldnn::impl::memory_format;
using namespace mkldnn::impl::utils;

void jit_avx2_conv_fwd_kernel_f32::oh_step_unroll_kw(int ur_w, int pad_l,
        int pad_r) {
    using Xbyak::Ymm;

    int iw = jcp.iw;
    int ih = jcp.ih;
    int kw = jcp.kw;
    int kh = jcp.kh;
    int nb_ic = jcp.nb_ic;
    int stride_w = jcp.stride_w;
    int nb_oc_block = jcp.nb_oc_blocking;
    int ic_blk = jcp.ic_block;
    int oc_blk = jcp.oc_block;

    for (int ki = 0; ki < kw; ki++) {
        int jj_start = nstl::max(0, (pad_l - ki + stride_w - 1)/stride_w);
        int jj_end = ur_w - nstl::max(0, (ki + pad_r - (kw - 1) + stride_w - 1)/stride_w);
        for (int ifm2 = 0; ifm2 < ic_blk; ifm2++) {
            for (int jj = jj_start; jj < jj_end; jj++) {
                int inp_off;
                if (jcp.src_fmt == nchw)
                    inp_off = ifm2 * ih * iw + (ki + jj * stride_w - pad_l);
                else
                    inp_off = (ki + jj * stride_w - pad_l) * ic_blk + ifm2;
                vbroadcastss(Ymm(nb_oc_block * ur_w + jj),
                        ptr[aux_reg_input + sizeof(float) * inp_off]);
            }
            for (int ii = 0; ii < nb_oc_block; ii++) {
                int ker_off = ii * nb_ic * kh * kw * ic_blk * oc_blk
                        + ki * ic_blk * oc_blk + ifm2 * oc_blk;
                vmovups(ymm15, ptr[aux_reg_kernel + sizeof(float) * ker_off]);
                for (int jj = jj_start; jj < jj_end; jj++)
                    vfmadd231ps(Ymm(ur_w * ii + jj),
                            Ymm(nb_oc_block * ur_w + jj), ymm15);
            }
        }
    }
}

void jit_avx2_conv_fwd_kernel_f32::oh_step_nopad(int ur_w, int pad_l, int pad_r,
        char pad_label) {
    using Xbyak::Ymm;
    char kw_label[4] = ".wP";
    kw_label[2] = pad_label;

    int iw = jcp.iw;
    int ih = jcp.ih;
    int kw = jcp.kw;
    int kh = jcp.kh;
    int nb_ic = jcp.nb_ic;
    int stride_w = jcp.stride_w;
    int nb_oc_block = jcp.nb_oc_blocking;
    int ic_blk = jcp.ic_block;
    int oc_blk = jcp.oc_block;

    xor_(ki_iter, ki_iter);
    L(kw_label);
    {
        int jj_start = 0;
        int jj_end = ur_w;
        for (int ifm2 = 0; ifm2 < ic_blk; ifm2++) {
            for (int jj = jj_start; jj < jj_end; jj++) {
                int inp_off;
                if (jcp.src_fmt == nchw)
                    inp_off = ifm2 * ih * iw + (jj * stride_w - pad_l);
                else
                    inp_off = (jj * stride_w - pad_l) * ic_blk + ifm2;
                vbroadcastss(Ymm(nb_oc_block * ur_w + jj),
                        ptr[aux_reg_input + sizeof(float) * inp_off]);
            }
            for (int ii = 0; ii < nb_oc_block; ii++) {
                int aux_kernel_offset = ii * nb_ic * kh * kw * ic_blk * oc_blk
                    + ifm2 * oc_blk;
                vmovups(ymm15, ptr[aux_reg_kernel
                        + sizeof(float) * aux_kernel_offset]);
                for (int jj = jj_start; jj < jj_end; jj++)
                    vfmadd231ps(Ymm(ur_w * ii + jj),
                            Ymm(nb_oc_block * ur_w + jj), ymm15);
            }
        }
        add(aux_reg_kernel, sizeof(float) * oc_blk * ic_blk);
        add(aux_reg_input, sizeof(float) * (jcp.src_fmt == nchw ? 1 : ic_blk));

        inc(ki_iter);
        cmp(ki_iter, kw);
        jl(kw_label, T_NEAR);
    }
}

void jit_avx2_conv_fwd_kernel_f32::width_blk_step(int ur_w, int pad_l, int pad_r,
        char pad_label) {
    using Xbyak::Ymm;

    int iw = jcp.iw;
    int kw = jcp.kw;
    int ow = jcp.ow;
    int oh = jcp.oh;
    int nb_oc_block = jcp.nb_oc_blocking;
    int ic_blk = jcp.ic_block;
    int oc_blk = jcp.oc_block;
    const int inp_mult = jcp.src_fmt == nchw ? 1 : ic_blk;

    char init_done_label[4] = {'.', 'i', pad_label, '\0'};
    char init_first_label[4] = {'.', 'f', pad_label, '\0'};

    test(reg_ci_flag, IC_FLAG_FIRST);
    jne(init_first_label, T_NEAR);

    for (int ii = 0; ii < nb_oc_block; ii++)
        for (int jj = 0; jj < ur_w; jj++)
            vmovups(Ymm(ur_w * ii + jj), YWORD[reg_output
                    + sizeof(float) * (ii * oh * ow + jj) * oc_blk]);
    jmp(init_done_label);

    L(init_first_label);
    if (this->jcp.with_bias) {
        for (int ii = 0; ii < nb_oc_block; ii++)
            for (int jj = 0; jj < ur_w; jj++)
                vmovups(Ymm(ur_w * ii + jj),
                        YWORD[reg_bias + sizeof(float)*ii*oc_blk]);
    } else {
        for (int ii = 0; ii < nb_oc_block; ii++)
            for (int jj = 0; jj < ur_w; jj++)
                vpxor(Ymm(ur_w * ii + jj), Ymm(ur_w * ii + jj));
    }

    L(init_done_label);

    mov(aux_reg_input, reg_input);
    mov(aux_reg_kernel, reg_kernel);

    mov(kj, reg_kh);
    char kh_label[4] = {'.', 'h', pad_label, '\0'};
    L(kh_label);
    {
        if (jcp.kw >= 5 && pad_l == 0 && pad_r == 0) {
            oh_step_nopad(ur_w, pad_l, pad_r, pad_label);
            sub(aux_reg_input, sizeof(float) * kw * inp_mult);
            add(aux_reg_input, sizeof(float) * iw * inp_mult);
        } else {
            oh_step_unroll_kw(ur_w, pad_l, pad_r);
            add(aux_reg_kernel, sizeof(float) * kw * oc_blk * ic_blk);
            add(aux_reg_input, sizeof(float) * iw * inp_mult);
        }

        dec(kj);
        cmp(kj, 0);
        jg(kh_label, T_NEAR);
    }

    char done_label[4] = {'.', 'd', pad_label, '\0'};
    char regular_store_label[4] = {'.', 's', pad_label, '\0'};
    if (this->jcp.with_relu) {
        assert(nb_oc_block*ur_w < 15);
        test(reg_ci_flag, IC_FLAG_LAST);
        je(regular_store_label, T_NEAR);

        Ymm yzero = ymm15, ymask = ymm14;
        vxorps(yzero, yzero, yzero);
        for (int ii = 0; ii < nb_oc_block; ii++) {
            for (int jj = 0; jj < ur_w; jj++) {
                const size_t o_off = (ii * oh * ow + jj) * oc_blk;
                Ymm reg_out = Ymm(ur_w * ii + jj);

                vcmpgtps(ymask, reg_out, yzero);
                vblendvps(reg_out, yzero, reg_out, ymask);
                vmovups(YWORD[reg_output + sizeof(float) * o_off], reg_out);
            }
        }

        jmp(done_label);
        L(regular_store_label);
    }
    for (int ii = 0; ii < nb_oc_block; ii++) {
        for (int jj = 0; jj < ur_w; jj++) {
            const size_t o_off = (ii * oh * ow + jj) * oc_blk;
            Ymm reg_out = Ymm(ur_w * ii + jj);
            vmovups(YWORD[reg_output + sizeof(float) * o_off], reg_out);
        }
    }
    L(done_label);
}

void jit_avx2_conv_fwd_kernel_f32::generate() {
    using Xbyak::Ymm;
    this->preamble();

#   define GET_OFF(field) offsetof(jit_conv_call_s, field)
    mov(reg_input, ptr[this->param1 + GET_OFF(src)]);
    mov(reg_output, ptr[this->param1 + GET_OFF(dst)]);
    mov(reg_kernel, ptr[this->param1 + GET_OFF(filt)]);
    if (jcp.with_bias)
        mov(reg_bias, ptr[this->param1 + GET_OFF(bias)]);
    mov(reg_kh, ptr[this->param1 + GET_OFF(kh_padding)]);
    mov(reg_ci_flag, ptr[this->param1 + GET_OFF(ic_flag)]);
#   undef GET_OFF

    // NB: works only for jcp.ur_w == 3 && jcp.nb_oc % 4 == 0
    int ur_w = jcp.ur_w;
    int ur_w_tail = jcp.ur_w_tail;
    int n_oi = jcp.ow / ur_w;
    int iw = jcp.iw;
    int kw = jcp.kw;
    int ic_blk = jcp.ic_block;
    int oc_blk = jcp.oc_block;
    int str_w = jcp.stride_w;
    const int inp_mult = jcp.src_fmt == nchw ? 1 : ic_blk;

    int l_pad = jcp.l_pad;
    int r_pad = nstl::max(0, (int(jcp.ow) - 1) * str_w + kw - 1
            - (iw + l_pad - 1));
    int r_pad1 = (ur_w * n_oi - 1) * str_w + kw - 1 - (iw + l_pad - 1);
    if (r_pad1 > 0) n_oi--;

    if (l_pad > 0) {
        n_oi--;
        if (n_oi < 0 && r_pad1 > 0) {
            width_blk_step(ur_w, l_pad, r_pad1, 'l'); // "lrpad"
        } else {
            width_blk_step(ur_w, l_pad, 0, 'l'); // "lpad"
        }
        add(reg_input, sizeof(float) * (ur_w * str_w - l_pad) * inp_mult);
        add(reg_output, sizeof(float) * ur_w * oc_blk);
    }

    xor_(oi_iter, oi_iter);
    if (n_oi > 0) {
        L(".ow_loop");

        width_blk_step(ur_w, 0, 0, 'm'); // "middle"
        add(reg_input, sizeof(float) * ur_w * str_w * inp_mult);
        add(reg_output, sizeof(float) * ur_w * oc_blk);

        inc(oi_iter);
        cmp(oi_iter, n_oi);
        jl(".ow_loop", T_NEAR);
    }

    if (r_pad1 > 0 && n_oi >=0) {
        width_blk_step(ur_w, 0, r_pad1, 'r'); // "rpad"
        add(reg_input, sizeof(float) * ur_w * str_w * inp_mult);
        add(reg_output, sizeof(float) * ur_w * oc_blk);
    }

    if (ur_w_tail != 0)
        width_blk_step(ur_w_tail, 0, r_pad, 't'); // "tail"

    this->postamble();
}

status_t jit_avx2_conv_fwd_kernel_f32::init_conf(jit_conv_conf_t &jcp,
        const convolution_desc_t &cd, const memory_desc_wrapper &src_d,
        const memory_desc_wrapper &weights_d, const memory_desc_wrapper &dst_d,
        bool with_relu, double relu_negative_slope)
{
    const bool with_groups = weights_d.ndims() == src_d.ndims() + 1;

    jcp.ngroups = with_groups ? weights_d.dims()[0] : 1;
    jcp.mb = src_d.dims()[0];

    jcp.oc = dst_d.dims()[1] / jcp.ngroups;
    jcp.ic = src_d.dims()[1] / jcp.ngroups;

    jcp.ih = src_d.dims()[2];
    jcp.iw = src_d.dims()[3];
    jcp.oh = dst_d.dims()[2];
    jcp.ow = dst_d.dims()[3];

    jcp.kh = weights_d.dims()[with_groups + 2];
    jcp.kw = weights_d.dims()[with_groups + 3];

    jcp.t_pad = cd.padding[0][0];
    jcp.l_pad = cd.padding[0][1];

    jcp.stride_h = cd.strides[0];
    jcp.stride_w = cd.strides[1];

    jcp.src_fmt = src_d.format();
    jcp.with_bias = cd.bias_desc.format != memory_format::undef;
    jcp.with_relu = with_relu;
    jcp.relu_negative_slope = relu_negative_slope;

    const bool flat = jcp.ic == 3;
    const bool mimo = !flat;

    bool args_ok = true
        && implication(flat, one_of(src_d.format(), nchw, nhwc))
        && implication(mimo, src_d.format() == nChw8c)
        && weights_d.format() ==
                (with_groups ? gOIhw8i8o : (flat ? Ohwi8o : OIhw8i8o))
        && one_of(cd.bias_desc.format, memory_format::undef, any, x)
        && dst_d.format() == nChw8c;
    if (!args_ok) return status::unimplemented;

    const int simd_w = 8;

    jcp.ur_h = 1; /* no code-unrolling by h so far */
    jcp.ur_w = 3;
    if (jcp.ow < jcp.ur_w) jcp.ur_w = jcp.ow;
    jcp.ur_w_tail = jcp.ow % jcp.ur_w;

    args_ok = true
        && jcp.oc % simd_w == 0
        && jcp.l_pad <= jcp.ur_w
        && implication(jcp.kw > 7, (jcp.t_pad == 0 && jcp.l_pad == 0)
                || (jcp.stride_w == 1 && jcp.stride_h == 1))
        && implication(mimo, jcp.ic % simd_w == 0);
    if (!args_ok) return status::unimplemented;

    int r_pad_no_tail = nstl::max(0,
            (jcp.ow - jcp.ur_w_tail - 1) * jcp.stride_w + (jcp.kw - 1)
            - (jcp.iw + jcp.l_pad - 1));

    /* maximum 1 ur_w block with r_pad so far */
    if (r_pad_no_tail > jcp.ur_w) return status::unimplemented;

    jcp.ic_block = (jcp.ic % simd_w != 0) ? jcp.ic : simd_w;
    jcp.nb_ic = jcp.ic / jcp.ic_block;

    jcp.oc_block = simd_w;
    jcp.nb_oc = jcp.oc / jcp.oc_block;
    jcp.nb_ic_blocking =  jcp.nb_oc_blocking = 1;
    for (int b = 4; b > 1; b--) {
        if (jcp.nb_oc % b == 0) {
            jcp.nb_oc_blocking = b;
            break;
        }
    }

    return status::success;
}

}
}
}

// vim: et ts=4 sw=4 cindent cino^=l0,\:0,N-s
