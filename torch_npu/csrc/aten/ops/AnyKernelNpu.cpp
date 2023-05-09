#include "torch_npu/csrc/framework/utils/CalcuOpUtil.h"
#include "torch_npu/csrc/framework/utils/OpAdapter.h"
#include "torch_npu/csrc/aten/NPUNativeFunctions.h"

namespace at_npu {
namespace native {

inline at::Tensor& any_out_npu_nocheck(
    at::Tensor& result,
    const at::Tensor& self,
    at::SmallVector<int64_t, N> dimList,
    bool keepdim) {
  OpCommand cmd;
  cmd.Name("ReduceAny")
    .Input(self)
    .Input(dimList)
    .Output(result)
    .Attr("keep_dims", keepdim)
    .Run();

   return result;
}

at::Tensor& NPUNativeFunctions::any_out(
    const at::Tensor& self,
    int64_t dim,
    bool keepdim,
    at::Tensor& result) {  
  at::SmallVector<int64_t, N> dimList;
  if (dim == LLONG_MIN) {
    dimList = CalcuOpUtil::GetDimlistForTensor(self);
  } else {
    dimList = {dim};
  }

  // check result for return
  auto outputSize = reduce_ops_npu_output_size(self, dimList, keepdim);
  OpPreparation::CheckOut(
      {self},
      result,
      CalcuOpUtil::GetTensorNpuFormat(self),
      self.scalar_type(),
      outputSize);

  // calculate the output result of the NPU
  any_out_npu_nocheck(result, self, dimList, keepdim);

  return result;
}

at::Tensor NPUNativeFunctions::any(const at::Tensor& self, int64_t dim, bool keepdim) {
  // calculate the output size
  at::IntArrayRef dims(dim);
  auto outputSize = reduce_ops_npu_output_size(self, dims, keepdim);

  // construct the output tensor of the NPU
  at::Tensor result = OpPreparation::ApplyTensorWithFormat(
      outputSize, self.options(), CalcuOpUtil::GetTensorNpuFormat(self));

  // calculate the output result of the NPU  
  if (dim == LLONG_MIN) {
    any_out_npu_nocheck(
        result, self, CalcuOpUtil::GetDimlistForTensor(self), keepdim);
  } else {
    any_out_npu_nocheck(result, self, {dim}, keepdim);
  }

  return result;
}

at::Tensor NPUNativeFunctions::any(const at::Tensor& self) { 
  // when self's dim = 0, convert [1] tensor and reduce it
  if (self.dim() == 0) {
      at::Tensor self_tmp = self;
      self_tmp = NPUNativeFunctions::npu_dtype_cast(OpPreparation::ApplyTensorWithFormat(
          {1}, 
          self.options().dtype(at::ScalarType::Float), 
          CalcuOpUtil::GetTensorNpuFormat(self))
          .fill_(self.item())
          , at::ScalarType::Bool);
      return NPUNativeFunctions::any(self_tmp, 0, false);
  }

  // calculate the output size 
  at::IntArrayRef dims;
  auto outputSize = reduce_ops_npu_output_size(self, dims, false);

  // construct the output tensor of the NPU
  at::Tensor result = OpPreparation::ApplyTensor(self, outputSize);

  // calculate the output result of the NPU
  any_out_npu_nocheck(
      result, self, CalcuOpUtil::GetDimlistForTensor(self), false);

  return result;
}

} // namespace native
} // namespace at_npu