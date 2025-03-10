/*******************************************************************************
* Copyright 2021-2022 Intel Corporation
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

#ifndef GPU_OCL_GENERIC_REORDER_HPP
#define GPU_OCL_GENERIC_REORDER_HPP

#include "common/c_types_map.hpp"
#include "common/memory.hpp"
#include "common/primitive.hpp"
#include "common/utils.hpp"
#include "gpu/gpu_primitive.hpp"
#include "gpu/gpu_reorder_pd.hpp"
#include "gpu/gpu_resource.hpp"
#include "gpu/ocl/ocl_utils.hpp"
#include "gpu/primitive_conf.hpp"

namespace dnnl {
namespace impl {
namespace gpu {
namespace ocl {

struct generic_reorder_t : public gpu_primitive_t {
    using gpu_primitive_t::gpu_primitive_t;
    struct pd_t : public gpu_reorder_pd_t {
        using gpu_reorder_pd_t::gpu_reorder_pd_t;

        DECLARE_COMMON_PD_T("ocl:generic:any", generic_reorder_t);

        status_t init(
                engine_t *engine, engine_t *src_engine, engine_t *dst_engine) {
            bool ok = src_engine == dst_engine
                    && src_engine->kind() == engine_kind::gpu && attr_ok()
                    && extra_ok();
            if (!ok) return status::unimplemented;

            if (memory_desc_wrapper(src_md()).has_runtime_dims_or_strides())
                return status::unimplemented;

            auto *compute_engine = utils::downcast<compute::compute_engine_t *>(
                    dst_engine->kind() == engine_kind::gpu ? dst_engine
                                                           : src_engine);

            ok = ok
                    && compute_engine->mayiuse(
                            compute::device_ext_t::intel_subgroups)
                    && IMPLICATION(
                            utils::one_of(data_type::f16, src_md()->data_type,
                                    dst_md()->data_type),
                            compute_engine->mayiuse(
                                    compute::device_ext_t::khr_fp16)
                                    && compute_engine->mayiuse(
                                            compute::device_ext_t::
                                                    intel_subgroups_short));

            if (!ok) return status::unimplemented;

            status_t status = init_conf(engine);
            if (status != status::success) return status;
            init_scratchpad();

            return status::success;
        }

        status_t init_conf(engine_t *engine);
        void init_scratchpad();
        status_t init_kernel_ctx(compute::kernel_ctx_t &kernel_ctx) const;

        reorder_conf_t conf;

    private:
        DECLARE_GPU_REORDER_CREATE();
    };

    status_t init(engine_t *engine) override {
        compute::kernel_ctx_t kernel_ctx;

        auto status = pd()->init_kernel_ctx(kernel_ctx);
        if (status != status::success) return status;

        const auto &conf = pd()->conf;
        if (conf.nelems == 0) return status::success;

        create_kernel(engine, &kernel_, "generic_reorder", kernel_ctx);
        if (!kernel_) return status::runtime_error;
        return status::success;
    }

    status_t execute(const exec_ctx_t &ctx) const override;

private:
    const pd_t *pd() const { return (const pd_t *)primitive_t::pd().get(); }
    compute::kernel_t kernel_;
};

} // namespace ocl
} // namespace gpu
} // namespace impl
} // namespace dnnl

#endif
