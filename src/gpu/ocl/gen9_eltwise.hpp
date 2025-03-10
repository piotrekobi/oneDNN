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

#ifndef GPU_OCL_GEN9_ELTWISE_HPP
#define GPU_OCL_GEN9_ELTWISE_HPP

#include "common/c_types_map.hpp"
#include "common/primitive.hpp"
#include "gpu/compute/compute.hpp"
#include "gpu/gpu_eltwise_pd.hpp"
#include "gpu/gpu_primitive.hpp"
#include "gpu/gpu_resource.hpp"
#include "gpu/ocl/ocl_stream.hpp"
#include "gpu/ocl/ocl_utils.hpp"
#include "gpu/primitive_conf.hpp"

namespace dnnl {
namespace impl {
namespace gpu {
namespace ocl {

struct gen9_eltwise_fwd_t : public gpu_primitive_t {
    using gpu_primitive_t::gpu_primitive_t;
    struct pd_t : public gpu_eltwise_fwd_pd_t {
        using gpu_eltwise_fwd_pd_t::gpu_eltwise_fwd_pd_t;

        DECLARE_COMMON_PD_T("ocl:gen9:any", gen9_eltwise_fwd_t);

        status_t init(engine_t *engine) {
            auto *compute_engine
                    = utils::downcast<compute::compute_engine_t *>(engine);

            using namespace alg_kind;
            bool ok = true
                    && utils::one_of(desc()->prop_kind,
                            prop_kind::forward_training,
                            prop_kind::forward_inference)
                    && utils::one_of(desc()->alg_kind, eltwise_relu,
                            eltwise_linear, eltwise_bounded_relu, eltwise_abs,
                            eltwise_tanh, eltwise_elu, eltwise_square,
                            eltwise_sqrt, eltwise_soft_relu,
                            eltwise_soft_relu_v2, eltwise_logistic,
                            eltwise_logsigmoid, eltwise_mish, eltwise_exp,
                            eltwise_gelu_tanh, eltwise_hardswish, eltwise_swish,
                            eltwise_log, eltwise_clip, eltwise_clip_v2,
                            eltwise_pow, eltwise_gelu_erf, eltwise_round,
                            eltwise_relu_use_dst_for_bwd,
                            eltwise_logistic_use_dst_for_bwd,
                            eltwise_tanh_use_dst_for_bwd,
                            eltwise_elu_use_dst_for_bwd,
                            eltwise_sqrt_use_dst_for_bwd,
                            eltwise_exp_use_dst_for_bwd,
                            eltwise_clip_v2_use_dst_for_bwd)
                    && utils::one_of(desc()->data_desc.data_type,
                            data_type::f32, data_type::f16, data_type::bf16,
                            data_type::s32, data_type::s8, data_type::u8)
                    && attr()->has_default_values()
                    && IMPLICATION(
                            desc()->data_desc.data_type == data_type::f16,
                            compute_engine->mayiuse(
                                    compute::device_ext_t::khr_fp16))
                    && compute_engine->mayiuse_sub_group(16);
            if (!ok) return status::unimplemented;

            return init_conf(engine);
        }

        status_t init_conf(engine_t *engine);
        status_t init_kernel_ctx(compute::kernel_ctx_t &kernel_ctx) const;

        eltwise_conf_t conf;
        offsets_t off;
    };

    status_t init(engine_t *engine) override {
        compute::kernel_ctx_t kernel_ctx;

        status_t status = pd()->init_kernel_ctx(kernel_ctx);
        if (status != status::success) return status;

        create_kernel(engine, &kernel_, "gen9_eltwise_fwd", kernel_ctx);
        if (!kernel_) return status::runtime_error;

        return status::success;
    }

    virtual status_t execute(const exec_ctx_t &ctx) const override {
        return execute_forward_dense(ctx);
    }

private:
    status_t execute_forward_dense(const exec_ctx_t &ctx) const;
    const pd_t *pd() const { return (const pd_t *)primitive_t::pd().get(); }
    compute::kernel_t kernel_;
};

struct gen9_eltwise_bwd_t : public gpu_primitive_t {
    using gpu_primitive_t::gpu_primitive_t;
    struct pd_t : public gpu_eltwise_bwd_pd_t {
        pd_t(const eltwise_desc_t *adesc, const primitive_attr_t *attr,
                const eltwise_fwd_pd_t *hint_fwd_pd)
            : gpu_eltwise_bwd_pd_t(adesc, attr, hint_fwd_pd) {}

        DECLARE_COMMON_PD_T("ocl:gen9:any", gen9_eltwise_bwd_t);

        status_t init(engine_t *engine) {
            using namespace prop_kind;
            using namespace utils;
            assert(engine->kind() == engine_kind::gpu);

            using namespace alg_kind;
            bool ok = desc()->prop_kind == backward_data
                    && utils::one_of(desc()->alg_kind, eltwise_relu,
                            eltwise_linear, eltwise_bounded_relu, eltwise_abs,
                            eltwise_tanh, eltwise_elu, eltwise_square,
                            eltwise_sqrt, eltwise_soft_relu,
                            eltwise_soft_relu_v2, eltwise_logsigmoid,
                            eltwise_mish, eltwise_logistic, eltwise_exp,
                            eltwise_hardswish, eltwise_gelu_tanh, eltwise_swish,
                            eltwise_log, eltwise_clip, eltwise_clip_v2,
                            eltwise_pow, eltwise_gelu_erf,
                            eltwise_relu_use_dst_for_bwd,
                            eltwise_logistic_use_dst_for_bwd,
                            eltwise_tanh_use_dst_for_bwd,
                            eltwise_elu_use_dst_for_bwd,
                            eltwise_sqrt_use_dst_for_bwd,
                            eltwise_exp_use_dst_for_bwd,
                            eltwise_clip_v2_use_dst_for_bwd)
                    && utils::one_of(desc()->data_desc.data_type,
                            data_type::f32, data_type::bf16)
                    && set_default_formats_common()
                    && attr()->has_default_values();
            if (!ok) return status::unimplemented;

            return init_conf(engine);
        }

        status_t init_conf(engine_t *engine);
        status_t init_kernel_ctx(compute::kernel_ctx_t &kernel_ctx) const;

        eltwise_conf_t conf;
        offsets_t off;
        bool use_dense;
    };

    status_t init(engine_t *engine) override {
        compute::kernel_ctx_t kernel_ctx;

        status_t status = pd()->init_kernel_ctx(kernel_ctx);
        if (status != status::success) return status;

        create_kernel(engine, &kernel_, "gen9_eltwise_bwd", kernel_ctx);
        if (!kernel_) return status::runtime_error;

        return status::success;
    }

    virtual status_t execute(const exec_ctx_t &ctx) const override {
        return execute_backward_dense(ctx);
    }

private:
    status_t execute_backward_dense(const exec_ctx_t &ctx) const;
    const pd_t *pd() const { return (const pd_t *)primitive_t::pd().get(); }
    compute::kernel_t kernel_;
};

} // namespace ocl
} // namespace gpu
} // namespace impl
} // namespace dnnl

#endif
