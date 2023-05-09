#include "torch_npu/csrc/framework/utils/KernelNpuOutputSize.h"
#include "torch_npu/csrc/framework/utils/NpuUtils.h"
#include "torch_npu/csrc/framework/utils/CalcuOpUtil.h"
#include "torch_npu/csrc/framework/utils/OpAdapter.h"
#include "torch_npu/csrc/aten/NPUNativeFunctions.h"

namespace at_npu {
namespace native {

at::Tensor& NPUNativeFunctions::erfinv_out(const at::Tensor& self, at::Tensor& result)
{
    OpCommand cmd;
    cmd.Name("Erfinv")
       .Input(self)
       .Output(result)
       .Run();

  return result;
}

at::Tensor NPUNativeFunctions::erfinv(const at::Tensor &self) {
  auto output_size = self.sizes();
  auto output_t = OpPreparation::ApplyTensor(self, output_size);
  NPUNativeFunctions::erfinv_out(self, output_t);

  return output_t;
}


at::Tensor& NPUNativeFunctions::erfinv_(at::Tensor& self)
{
  if (!NpuUtils::check_match(&self)) {
    at::Tensor contiguousSelf = NpuUtils::format_contiguous(self);
    at::Tensor result = NPUNativeFunctions::erfinv_out(contiguousSelf, contiguousSelf);
    NpuUtils::format_fresh_view(self, result);
  } else {
    NPUNativeFunctions::erfinv_out(self, self);
  }
  return self;
}

}  // namespace native
}  // namespace at_npu