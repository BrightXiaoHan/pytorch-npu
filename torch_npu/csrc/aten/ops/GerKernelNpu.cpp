#include "torch_npu/csrc/framework/utils/OpAdapter.h"
#include "torch_npu/csrc/aten/NPUNativeFunctions.h"

namespace at_npu {
namespace native {

c10::SmallVector<int64_t, SIZE> ger_npu_output_size(
    const at::Tensor& self,
    const at::Tensor& vec2) {
  int64_t outputsize_0 = self.size(0);
  int64_t outputsize_1 = vec2.size(0);
  c10::SmallVector<int64_t, SIZE> outputsize = {outputsize_0, outputsize_1};

  return outputsize;
}

at::Tensor& ger_out_npu_nocheck(const at::Tensor& self , const at::Tensor& vec2, at::Tensor& result) {
  OpCommand cmd;
  cmd.Name("Ger")
      .Input(self)
      .Input(vec2)
      .Output(result)
      .Run();

  return result;
}

at::Tensor& NPUNativeFunctions::ger_out(const at::Tensor& self , const at::Tensor& vec2, at::Tensor& result) {
  // check shape
  TORCH_CHECK(
      self.dim() == 1, "Input1 must have only1 dims."); 
  TORCH_CHECK(
      vec2.dim() == 1, "Input2 must have only1 dims.");

  // calculate the output size
  auto outputSize = ger_npu_output_size(self, vec2);

  OpPreparation::CheckOut(
      {self},
      result,
      self,
      outputSize);

  OpPipeWithDefinedOut pipe;
  return pipe.Func([&self, &vec2](at::Tensor& result){ger_out_npu_nocheck(self, vec2, result);})
      .Call(result);
}

at::Tensor NPUNativeFunctions::ger(const at::Tensor& self, const at::Tensor& vec2) {
  // check shape
  TORCH_CHECK(
      self.dim() == 1, "Input1 must have only1 dims."); 
  TORCH_CHECK(
      vec2.dim() == 1, "Input2 must have only1 dims.");

  // calculate the output size
  auto outputSize = ger_npu_output_size(self, vec2);

  // construct the output Tensor of the NPU 
  at::Tensor result = OpPreparation::ApplyTensor(self, outputSize);

  // calculate the output result of the NPU
  ger_out_npu_nocheck(self, vec2, result);

  return result;
}
} // namespace native
} // namespace at_npu