#include "torch_npu/csrc/framework/utils/CalcuOpUtil.h"
#include "torch_npu/csrc/framework/utils/OpAdapter.h"
#include "torch_npu/csrc/aten/NPUNativeFunctions.h"
#include "torch_npu/csrc/core/npu/NpuVariables.h"

namespace at_npu {
namespace native {

const int FLOAT_STATUS_OP_DIMS_SIZE = 8;

bool NPUNativeFunctions::_amp_foreach_non_finite_check(at::TensorList scaled_grads) {
    TORCH_WARN_ONCE("Non finite check on NPU device!");

    auto options = at::TensorOptions(c10::DeviceType::PrivateUse1).dtype(at::kFloat);
    at::Tensor float_status = at::zeros({FLOAT_STATUS_OP_DIMS_SIZE}, options);
    auto ans = NPUNativeFunctions::npu_get_float_status(float_status);

    auto result = ans[0].item().to<bool>();

    if(result) {
        auto ans_clear = NPUNativeFunctions::npu_clear_float_status(float_status);
    }
    
    return result;
}

void NPUNativeFunctions::_amp_foreach_non_finite_check_and_unscale_(at::TensorList scaled_grads,
                                                                    at::Tensor& found_inf,
                                                                    const at::Tensor& inv_scale) {
    TORCH_WARN_ONCE("Non finite check and unscale on NPU device!");
    TORCH_CHECK(torch_npu::utils::is_npu(inv_scale), "inv_scale must be NPU-Tensor");
    TORCH_CHECK(inv_scale.numel() == 1, "inv_scale must be a 1-element tensor");
    TORCH_CHECK(inv_scale.scalar_type() == at::ScalarType::Float, "inv_scale must be a float tensor");

    if (scaled_grads.empty()) {
        return;
    }

    bool is_finite = true;
    if (c10_npu::IsSupportInfNan()) {
        for (const auto& scaled_grad : scaled_grads) {
          auto res = NPUNativeFunctions::sum(scaled_grad, at::ScalarType::Float);
          float cpu_sum = res.item().toFloat();
          if (!std::isfinite(cpu_sum)) {
            is_finite = false;
            break;
          }
        }
    } else {
        is_finite = !NPUNativeFunctions::_amp_foreach_non_finite_check(scaled_grads);
    }

    if (is_finite) {
        auto expected_device = scaled_grads[0].device();
        auto expected_dtype = scaled_grads[0].dtype();
        for (const auto& t : scaled_grads) {
            TORCH_CHECK(torch_npu::utils::is_npu(t), "one of scaled_grads was not a NPU tensor.");
            TORCH_CHECK(t.device() == expected_device, "scaled_grads must be on the same device.");
            TORCH_CHECK(t.dtype() == expected_dtype, "scaled_grads must have the same dtype.");
            TORCH_CHECK(t.layout() == at::kStrided, "one of scaled_grads was not a strided tensor.");

            t.mul_(inv_scale);
        }
    } else {
        found_inf.add_(1);
    }
}
}
}

