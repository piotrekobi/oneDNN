/*******************************************************************************
* Copyright 2020-2022 Intel Corporation
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

#include <iterator>

#include <float.h>
#include <math.h>
#include <stdio.h>
#include <stdlib.h>

#include "oneapi/dnnl/dnnl.h"

#include "dnnl_common.hpp"
#include "dnnl_memory.hpp"

#include "binary/binary.hpp"
#include "conv/conv_dw_fusion.hpp"

namespace conv_dw_fusion {

dnnl_status_t init_pd(dnnl_engine_t engine, const prb_t *prb,
        dnnl_primitive_desc_t &cpd, res_t *res, dir_t dir,
        const_dnnl_primitive_desc_t hint) {
    dnnl_convolution_desc_t cd;

    auto src_d = dnn_mem_t::init_md(
            prb->ndims, prb->src_dims().data(), prb->cfg[SRC].dt, prb->stag);
    auto wei_d = dnn_mem_t::init_md(prb->ndims + prb->has_groups,
            prb->wei_dims().data(), prb->cfg[WEI].dt, prb->wtag);
    auto bia_d = dnn_mem_t::init_md(
            1, prb->bia_dims().data(), prb->cfg[BIA].dt, tag::any);
    auto dst_d = dnn_mem_t::init_md(
            prb->ndims, prb->dst_dims().data(), prb->cfg[DST].dt, prb->dtag);

    dnnl_alg_kind_t alg = dnnl_convolution_direct;
    if (prb->alg == alg_t::WINO) alg = dnnl_convolution_winograd;
    if (prb->alg == alg_t::AUTO) alg = dnnl_convolution_auto;

    switch (prb->dir) {
        case FWD_D:
        case FWD_B:
        case FWD_I:
            DNN_SAFE_STATUS(dnnl_dilated_convolution_forward_desc_init(&cd,
                    prb->dir == FWD_I ? dnnl_forward_inference
                                      : dnnl_forward_training,
                    alg, &src_d, &wei_d, prb->dir == FWD_B ? &bia_d : nullptr,
                    &dst_d, prb->strides().data(), prb->dilations().data(),
                    prb->padding().data(), prb->padding_r().data()));
            break;
        case BWD_D:
            DNN_SAFE_STATUS(dnnl_dilated_convolution_backward_data_desc_init(
                    &cd, alg, &src_d, &wei_d, &dst_d, prb->strides().data(),
                    prb->dilations().data(), prb->padding().data(),
                    prb->padding_r().data()));
            break;
        case BWD_W:
        case BWD_WB:
            DNN_SAFE_STATUS(dnnl_dilated_convolution_backward_weights_desc_init(
                    &cd, alg, &src_d, &wei_d,
                    prb->dir == BWD_W ? nullptr : &bia_d, &dst_d,
                    prb->strides().data(), prb->dilations().data(),
                    prb->padding().data(), prb->padding_r().data()));
            break;
        default: DNN_SAFE_STATUS(dnnl_invalid_arguments);
    }

    DNN_SAFE_STATUS(cd.accum_data_type == prb->cfg[ACC].dt
                    ? dnnl_success
                    : dnnl_unimplemented);

    attr_args_t attr_args;
    attr_args.prepare_output_scales(prb->attr, prb->scales, prb->oc);

    const auto dw_bia_dt = prb->dir == FWD_B ? dnnl_f32 : dnnl_data_type_undef;
    attr_args.prepare_dw_post_op(
            prb->attr, prb->cfg[WEI].dt, dw_bia_dt, prb->scales_dw, prb->oc);
    attr_args.prepare_post_ops_mds(
            prb->attr, prb->ndims, prb->dst_dims().data());
    auto dnnl_attr = make_benchdnn_dnnl_wrapper(
            create_dnnl_attr(prb->attr, attr_args));

    return dnnl_primitive_desc_create(&cpd, &cd, dnnl_attr, engine, nullptr);
}

std::unique_ptr<prb_t> get_first_conv_prb(const prb_t *prb) {
    const auto &po = prb->attr.post_ops;
    int fusion_index = po.convolution_index();

    attr_t attr;
    attr.oscale.scale = prb->attr.oscale.scale;
    attr.oscale.policy = prb->attr.oscale.policy;
    for (int i = 0; i < fusion_index; ++i) {
        attr.post_ops.entry.push_back(prb->attr.post_ops.entry[i]);
    }

    return std::unique_ptr<prb_t>(new prb_t((desc_t)*prb, prb->dir, prb->cfg,
            prb->stag, prb->wtag, tag::any, prb->alg, attr, prb->mb));
}

std::unique_ptr<prb_t> get_fused_conv_prb(const prb_t *prb) {
    const auto &po = prb->attr.post_ops;
    int fusion_index = po.convolution_index();
    if (fusion_index == -1) return nullptr;
    const auto &fused_conv_po = po.entry[fusion_index].convolution;

    attr_t fusion_attr;
    fusion_attr.oscale.scale = fused_conv_po.oscale.scale;
    fusion_attr.oscale.policy = fused_conv_po.oscale.policy;
    for (int i = fusion_index + 1; i < po.len(); ++i) {
        fusion_attr.post_ops.entry.push_back(prb->attr.post_ops.entry[i]);
    }

    const auto f32 = dnnl_f32;
    std::stringstream dw_cfg_ss;
    if (prb->cfg[DST].dt == f32 && prb->cfg[WEI].dt == f32
            && fused_conv_po.dst_dt == f32)
        dw_cfg_ss << prb->cfg[DST].dt; // f32 is a single name
    else // else have all three dt in cfg name
        dw_cfg_ss << prb->cfg[DST].dt << prb->cfg[WEI].dt
                  << fused_conv_po.dst_dt;
    auto p_dw_cfg = conv::str2cfg(dw_cfg_ss.str().c_str());

    auto stride = fused_conv_po.stride;
    bool is_3d = prb->ndims >= 5;
    bool is_2d = prb->ndims >= 4;

    desc_t cd {0};
    cd.g = prb->oc;
    cd.mb = prb->mb;
    cd.ic = prb->oc;
    cd.id = is_3d ? prb->od : 1;
    cd.ih = is_2d ? prb->oh : 1;
    cd.iw = prb->ow;
    cd.oc = prb->oc;
    cd.od = is_3d ? div_up(cd.id, stride) : 1;
    cd.oh = is_2d ? div_up(cd.ih, stride) : 1;
    cd.ow = div_up(cd.iw, stride);
    cd.kd = is_3d ? 3 : 1;
    cd.kh = is_2d ? 3 : 1;
    cd.kw = 3;
    cd.sd = is_3d ? stride : 1;
    cd.sh = is_2d ? stride : 1;
    cd.sw = stride;
    cd.pd = is_3d;
    cd.ph = is_2d;
    cd.pw = 1;
    cd.has_groups = true;
    cd.ndims = prb->ndims;
    cd.init_pad_r(false); // is_deconv = false for conv descriptor

    return std::unique_ptr<prb_t>(new prb_t(cd, prb->dir, p_dw_cfg, tag::any,
            tag::any, prb->dtag, alg_t::DIRECT, fusion_attr, prb->mb));
}

void skip_unimplemented_prb(const prb_t *prb, res_t *res) {
    skip_unimplemented_data_type(
            {prb->cfg[SRC].dt, prb->cfg[WEI].dt, prb->cfg[DST].dt}, prb->dir,
            res);
    skip_unimplemented_sum_po(prb->attr, res);

    // GPU does not support depthwise fusion
    if (is_gpu() && prb->attr.post_ops.convolution_index() != -1) {
        res->state = SKIPPED, res->reason = CASE_NOT_SUPPORTED;
        return;
    }
}

int doit(const prb_t *prb, res_t *res) {
    if (bench_mode == LIST) return res->state = LISTED, OK;

    conv_dw_fusion::skip_unimplemented_prb(prb, res);
    if (res->state == SKIPPED) return OK;

    // Original problem with fusion attributes
    benchdnn_dnnl_wrapper_t<dnnl_primitive_t> prim;
    SAFE(init_prim(prim, init_pd, prb, res), WARN);
    if (res->state == SKIPPED || res->state == UNIMPLEMENTED) return OK;

    auto const_pd = query_pd(prim);

    // Check memory requirements only for original problem though it's broken
    // due to quering not by arg md.
    if (check_mem_size(const_pd) != OK) {
        return res->state = SKIPPED, res->reason = NOT_ENOUGH_RAM, OK;
    }

    const auto adjust_alg = [](const_dnnl_primitive_desc_t pd, alg_t &alg) {
        if (alg == alg_t::AUTO) {
            dnnl_convolution_desc_t *temp_conv_desc = {nullptr};
            DNN_SAFE(dnnl_primitive_desc_query(
                             pd, dnnl_query_convolution_d, 0, &temp_conv_desc),
                    CRIT);
            alg = conv::alg_kind2alg(temp_conv_desc->alg_kind);
        }
        return OK;
    };

    alg_t alg = prb->alg;
    adjust_alg(const_pd, alg);
    auto cfg = auto_cfg(alg, prb->cfg);
    prb_t p_new((desc_t)*prb, prb->dir, cfg, prb->stag, prb->wtag, prb->dtag,
            alg, prb->attr, prb->mb);
    prb = &p_new;

    const auto &src_md = prb->dir == BWD_D
            ? query_md(const_pd, DNNL_ARG_DIFF_SRC)
            : query_md(const_pd, DNNL_ARG_SRC);
    const auto &wei_md = prb->dir & FLAG_WEI
            ? query_md(const_pd, DNNL_ARG_DIFF_WEIGHTS)
            : query_md(const_pd, DNNL_ARG_WEIGHTS);
    const auto &bia_md = prb->dir & FLAG_WEI
            ? query_md(const_pd, DNNL_ARG_DIFF_BIAS)
            : query_md(const_pd, DNNL_ARG_BIAS);
    const auto &dst_md = prb->dir & FLAG_BWD
            ? query_md(const_pd, DNNL_ARG_DIFF_DST)
            : query_md(const_pd, DNNL_ARG_DST);
    const auto &fused_wei_md = prb->dir & FLAG_WEI
            ? query_md(
                    const_pd, DNNL_ARG_ATTR_POST_OP_DW | DNNL_ARG_DIFF_WEIGHTS)
            : query_md(const_pd, DNNL_ARG_ATTR_POST_OP_DW | DNNL_ARG_WEIGHTS);
    const auto &fused_bia_md = prb->dir & FLAG_WEI
            ? query_md(const_pd, DNNL_ARG_ATTR_POST_OP_DW | DNNL_ARG_DIFF_BIAS)
            : query_md(const_pd, DNNL_ARG_ATTR_POST_OP_DW | DNNL_ARG_BIAS);
    const auto &scratchpad_md = query_md(const_pd, DNNL_ARG_SCRATCHPAD);

    const auto &test_engine = get_test_engine();
    const auto &ref_engine = get_cpu_engine();

    dnn_mem_t src_dt(src_md, test_engine);
    dnn_mem_t wei_dt(wei_md, test_engine);
    dnn_mem_t bia_dt(bia_md, test_engine);
    dnn_mem_t dst_dt(dst_md, test_engine);
    dnn_mem_t fused_wei_dt(fused_wei_md, test_engine);
    dnn_mem_t fused_bia_dt(fused_bia_md, test_engine);
    dnn_mem_t scratchpad_dt(scratchpad_md, test_engine);

    const auto fp = dnnl_f32;
    dnn_mem_t src_fp(src_md, fp, tag::abx, ref_engine);
    dnn_mem_t wei_fp(wei_md, fp, tag::abx, ref_engine);
    dnn_mem_t bia_fp(bia_md, fp, tag::x, ref_engine);
    dnn_mem_t dst_fp(dst_md, fp, tag::abx, ref_engine);
    dnn_mem_t fused_wei_fp(fused_wei_md, fp, tag::abx, ref_engine);
    dnn_mem_t fused_bia_fp(fused_bia_md, fp, tag::x, ref_engine);

    std::vector<dnn_mem_t> binary_po_dt;
    std::vector<int> binary_po_args;

    // Current filling doesn't work for fused_wei due to relying on prb values,
    // which are different for fused conv. This can be fixed later by relying
    // on md values, rather than prb desc ones.
    // Filling for this problem is done below.
    // TODO: fix this if irritates.

    // Fill first convolution
    std::unique_ptr<prb_t> p0 = get_first_conv_prb(prb);

    benchdnn_dnnl_wrapper_t<dnnl_primitive_t> prim0;
    SAFE(init_prim(prim0, init_pd, p0.get(), res), WARN);
    if (res->state == SKIPPED || res->state == UNIMPLEMENTED) return OK;

    auto const_pd0 = query_pd(prim0);

    alg = p0->alg;
    adjust_alg(const_pd0, alg);
    cfg = auto_cfg(alg, p0->cfg);
    p0.reset(new prb_t((desc_t)*p0, p0->dir, cfg, p0->stag, p0->wtag, p0->dtag,
            alg, p0->attr, p0->mb));

    const auto &src_md0 = p0->dir == BWD_D
            ? query_md(const_pd0, DNNL_ARG_DIFF_SRC)
            : query_md(const_pd0, DNNL_ARG_SRC);
    const auto &wei_md0 = p0->dir & FLAG_WEI
            ? query_md(const_pd0, DNNL_ARG_DIFF_WEIGHTS)
            : query_md(const_pd0, DNNL_ARG_WEIGHTS);
    const auto &bia_md0 = p0->dir & FLAG_WEI
            ? query_md(const_pd0, DNNL_ARG_DIFF_BIAS)
            : query_md(const_pd0, DNNL_ARG_BIAS);
    const auto &dst_md0 = p0->dir & FLAG_BWD
            ? query_md(const_pd0, DNNL_ARG_DIFF_DST)
            : query_md(const_pd0, DNNL_ARG_DST);
    const auto &scratchpad_md0 = query_md(const_pd0, DNNL_ARG_SCRATCHPAD);

    dnn_mem_t src_dt0(src_md0, test_engine);
    dnn_mem_t wei_dt0(wei_md0, test_engine);
    dnn_mem_t bia_dt0(bia_md0, test_engine);
    dnn_mem_t dst_dt0(dst_md0, test_engine);
    dnn_mem_t scratchpad_dt0(scratchpad_md0, test_engine);

    dnn_mem_t src_fp0(src_md0, fp, tag::abx, ref_engine);
    dnn_mem_t wei_fp0(wei_md0, fp, tag::abx, ref_engine);
    dnn_mem_t bia_fp0(bia_md0, fp, tag::x, ref_engine);
    dnn_mem_t dst_fp0(dst_md0, fp, tag::abx, ref_engine);

    std::vector<dnn_mem_t> binary_po_fp0, binary_po_dt0;
    std::vector<int> binary_po_args0;
    SAFE(binary::setup_binary_po(
                 const_pd0, binary_po_args0, binary_po_dt0, binary_po_fp0),
            WARN);

    SAFE(conv::fill_src(p0.get(), src_dt0, src_fp0, res), WARN);
    SAFE(conv::fill_wei(p0.get(), wei_dt0, wei_fp0, res), WARN);
    SAFE(conv::fill_bia(p0.get(), bia_dt0, bia_fp0, res), WARN);
    SAFE(conv::fill_dst(p0.get(), dst_dt0, dst_fp0, res), WARN);

    // Fill next convolution
    std::unique_ptr<prb_t> p1 = get_fused_conv_prb(prb);
    if (!p1) SAFE(FAIL, CRIT);

    benchdnn_dnnl_wrapper_t<dnnl_primitive_t> prim1;
    SAFE(init_prim(prim1, init_pd, p1.get(), res), WARN);
    if (res->state == SKIPPED || res->state == UNIMPLEMENTED) return OK;

    auto const_pd1 = query_pd(prim1);

    alg = p1->alg;
    adjust_alg(const_pd1, alg);
    cfg = auto_cfg(alg, p1->cfg);
    p1.reset(new prb_t((desc_t)*p1, p1->dir, cfg, p1->stag, p1->wtag, p1->dtag,
            alg, p1->attr, p1->mb));

    const auto &src_md1 = prb->dir == BWD_D
            ? query_md(const_pd1, DNNL_ARG_DIFF_SRC)
            : query_md(const_pd1, DNNL_ARG_SRC);
    const auto &wei_md1 = prb->dir & FLAG_WEI
            ? query_md(const_pd1, DNNL_ARG_DIFF_WEIGHTS)
            : query_md(const_pd1, DNNL_ARG_WEIGHTS);

    const auto &bia_md1 = prb->dir & FLAG_WEI
            ? query_md(const_pd1, DNNL_ARG_DIFF_BIAS)
            : query_md(const_pd1, DNNL_ARG_BIAS);
    const auto &dst_md1 = prb->dir & FLAG_BWD
            ? query_md(const_pd1, DNNL_ARG_DIFF_DST)
            : query_md(const_pd1, DNNL_ARG_DST);
    const auto &scratchpad_md1 = query_md(const_pd, DNNL_ARG_SCRATCHPAD);

    dnn_mem_t src_dt1(src_md1, test_engine);
    dnn_mem_t wei_dt1(wei_md1, test_engine);
    dnn_mem_t bia_dt1(bia_md1, test_engine);
    dnn_mem_t dst_dt1(dst_md1, test_engine);
    dnn_mem_t scratchpad_dt1(scratchpad_md1, test_engine);

    dnn_mem_t wei_fp1(wei_md1, fp, tag::abx, ref_engine);
    dnn_mem_t bia_fp1(bia_md1, fp, tag::x, ref_engine);
    dnn_mem_t dst_fp1(dst_md1, fp, tag::abx, ref_engine);

    std::vector<dnn_mem_t> binary_po_fp1, binary_po_dt1;
    std::vector<int> binary_po_args1;
    SAFE(binary::setup_binary_po(
                 const_pd1, binary_po_args1, binary_po_dt1, binary_po_fp1),
            WARN);

    SAFE(conv::fill_wei(p1.get(), wei_dt1, wei_fp1, res), WARN);
    SAFE(conv::fill_bia(p1.get(), bia_dt1, bia_fp1, res), WARN);
    SAFE(conv::fill_dst(p1.get(), dst_dt1, dst_fp1, res), WARN);

    // TODO: fix this if irritates.
    // SAFE(conv::fill_src(prb, src_dt, src_fp, res), WARN);
    // SAFE(conv::fill_wei(prb, wei_dt, wei_fp, res), WARN);
    // SAFE(conv::fill_bia(prb, bia_dt, bia_fp, res), WARN);
    // SAFE(conv::fill_dst(prb, dst_dt, dst_fp, res), WARN);
    // SAFE(conv::fill_wei(prb, fused_wei_dt, fused_wei_fp, res), WARN);
    // SAFE(conv::fill_bia(prb, fused_bia_dt, fused_bia_fp, res), WARN);
    // Work around for the issue above
    SAFE(src_dt.reorder(src_fp0), WARN);
    SAFE(wei_dt.reorder(wei_fp0), WARN);
    if (bia_md.data_type != dnnl_data_type_undef)
        SAFE(bia_dt.reorder(bia_fp0), WARN);
    SAFE(dst_dt.reorder(dst_fp1), WARN);
    SAFE(fused_wei_dt.reorder(wei_fp1), WARN);
    if (fused_bia_md.data_type != dnnl_data_type_undef)
        SAFE(fused_bia_dt.reorder(bia_fp1), WARN);

    args_t args, args0, args1, ref_args;

    if (prb->dir & FLAG_FWD) {
        args0.set(DNNL_ARG_SRC, src_dt0);
        args0.set(DNNL_ARG_WEIGHTS, wei_dt0);
        args0.set(DNNL_ARG_BIAS, bia_dt0);
        args0.set(DNNL_ARG_DST, dst_dt0);
        args0.set(DNNL_ARG_SCRATCHPAD, scratchpad_dt0);
        args0.set(binary_po_args0, binary_po_dt0);

        SAFE(execute_and_wait(prim0, args0), WARN);
        SAFE(src_dt1.reorder(dst_dt0), WARN);

        args1.set(DNNL_ARG_SRC, src_dt1);
        args1.set(DNNL_ARG_WEIGHTS, wei_dt1);
        args1.set(DNNL_ARG_BIAS, bia_dt1);
        args1.set(DNNL_ARG_DST, dst_dt1);
        args1.set(DNNL_ARG_SCRATCHPAD, scratchpad_dt1);
        args1.set(binary_po_args1, binary_po_dt1);

        SAFE(execute_and_wait(prim1, args1), WARN);

        // Reverse engineer binary post-ops indices from second conv and update
        // them in-place to follow fused conv enumaration.
        const int dw_idx = prb->attr.post_ops.convolution_index();
        const auto update_bin_po_args1_indices = [&](size_t i) {
            auto &b = binary_po_args1[i];
            const int orig_idx = b / DNNL_ARG_ATTR_MULTIPLE_POST_OP_BASE - 1;
            b = DNNL_ARG_ATTR_MULTIPLE_POST_OP(orig_idx + dw_idx + 1)
                    | DNNL_ARG_SRC_1;
        };
        for (size_t i = 0; i < binary_po_dt1.size(); ++i)
            update_bin_po_args1_indices(i);

        // As memory is not allowed to be copied, and binary post-op memories
        // are read-only, we move them to main convolution execution and adjust
        // arg indices to follow the library API.

        // Move the content to binary_po_dt from separate convs.
        std::move(binary_po_dt0.begin(), binary_po_dt0.end(),
                std::back_inserter(binary_po_dt));
        std::move(binary_po_dt1.begin(), binary_po_dt1.end(),
                std::back_inserter(binary_po_dt));
        // Move the content to binary_po_args from separate convs.
        std::move(binary_po_args0.begin(), binary_po_args0.end(),
                std::back_inserter(binary_po_args));
        std::move(binary_po_args1.begin(), binary_po_args1.end(),
                std::back_inserter(binary_po_args));

        args.set(DNNL_ARG_SRC, src_dt);
        args.set(DNNL_ARG_WEIGHTS, wei_dt);
        args.set(DNNL_ARG_BIAS, bia_dt);
        args.set(DNNL_ARG_DST, dst_dt);
        args.set(DNNL_ARG_ATTR_POST_OP_DW | DNNL_ARG_WEIGHTS, fused_wei_dt);
        args.set(DNNL_ARG_ATTR_POST_OP_DW | DNNL_ARG_BIAS, fused_bia_dt);
        args.set(DNNL_ARG_SCRATCHPAD, scratchpad_dt);
        args.set(binary_po_args, binary_po_dt);

        SAFE(execute_and_wait(prim, args, res), WARN);

        if (is_bench_mode(CORR)) {
            compare::compare_t cmp;
            cmp.set_data_kind(DST);
            // Used p1 to avoid writing separate compare function. Compare uses
            // prb->cfg which can be u8s8u8 while after fusion it may be u8s8s8,
            // thus, compare() will saturate values which is not correct.
            conv::setup_cmp(cmp, p1.get(), DST, ref_args);

            dnn_mem_t dst_fused(dst_dt, fp, tag::abx, test_engine);
            dnn_mem_t dst_unfused(dst_dt1, fp, tag::abx, test_engine);

            cmp.compare(dst_unfused, dst_fused, prb->attr, res);
        }
    } else {
        assert(!"Backward is not supported");
        SAFE(FAIL, CRIT);
    }

    return measure_perf(res, prim, args);
}

} // namespace conv_dw_fusion
