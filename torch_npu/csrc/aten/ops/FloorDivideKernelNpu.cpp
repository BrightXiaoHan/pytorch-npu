#include "torch_npu/csrc/framework/utils/CalcuOpUtil.h"
#include "torch_npu/csrc/framework/utils/OpAdapter.h"
#include "torch_npu/csrc/aten/NPUNativeFunctions.h"

namespace at_npu {
namespace native {

at::Tensor& floor_divide_out_npu_nocheck(at::Tensor& result, const at::Tensor& self, at::Scalar other) {
  OpCommand cmd;
  cmd.Name("FloorDiv")
        .Input(self)
        .Input(other, self.scalar_type())
        .Output(result)
        .Run();
  return result;
}

at::Tensor& floor_divide_out_scalar_npu(const at::Tensor& self, at::Scalar other, at::Tensor& result) {
  at::Tensor formatCastOfSelf = OpPreparation::CastBackToOriFormat(self);
  auto outputSize = formatCastOfSelf.sizes();
  OpPreparation::CheckOut(
      {self},
      result,
      CalcuOpUtil::GetTensorNpuFormat(self),
      result.scalar_type(),
      outputSize);
  floor_divide_out_npu_nocheck(result, formatCastOfSelf, other);
  return result;
}

at::Tensor& floor_divide_out_npu(const at::Tensor& self, const at::Tensor& other, at::Tensor& result) {
  at::Tensor formatCastOfSelf = OpPreparation::CastBackToOriFormat(self);
  auto outputSize = formatCastOfSelf.sizes();
  OpPreparation::CheckOut({self, other}, result,
                          CalcuOpUtil::GetTensorNpuFormat(self),
                          result.scalar_type(), outputSize);
  // executing the NPU operator
  if (other.dim() == 0) {
    floor_divide_out_npu_nocheck(result, self, other.item());
  } else {
    OpCommand cmd;
    cmd.Name("FloorDiv")
        .Input(self)
        .Input(other)
        .Output(result)
        .Run();
  }
  return result;
}

at::Tensor& check_self_dtype_npu(at::Tensor& self){
  if (self.dtype() == at::ScalarType::Bool ||
      self.dtype() == at::ScalarType::Int) {
    self = NPUNativeFunctions::npu_dtype_cast(self, at::ScalarType::Float);
  }
  return self;
}

std::tuple<at::Tensor, at::Tensor> check_dtype_npu(at::Tensor& self, at::Tensor& other){
  if (self.dtype() == at::ScalarType::Bool ||
      (self.dtype() == at::ScalarType::Int &&
      other.scalar_type() == at::ScalarType::Double)) {
    self = NPUNativeFunctions::npu_dtype_cast(self, at::ScalarType::Float);
  }
  if (other.scalar_type() == at::ScalarType::Double) {
    other = other.to(at::ScalarType::Float);
  }
  if (other.scalar_type() == at::ScalarType::Long) {
    other = other.to(at::ScalarType::Int);
  }
  return std::tie(self, other);
}

at::Tensor& NPUNativeFunctions::floor_divide_out(const at::Tensor& self, const at::Tensor& other, at::Tensor& result) {
  at::Tensor selfCast = self;
  at::Tensor otherCast = other;
  check_dtype_npu(selfCast, otherCast);
  floor_divide_out_npu(selfCast, otherCast, result);
  return result;
}

at::Tensor NPUNativeFunctions::floor_divide(const at::Tensor& self, const at::Tensor& other) {
  at::Tensor selfCast = self;
  at::Tensor otherCast = other;
  check_dtype_npu(selfCast, otherCast);
  bool isSelfWrapped = CalcuOpUtil::IsScalarWrappedToTensor(selfCast);
  at::Tensor outputTensor = isSelfWrapped ? otherCast : selfCast;
  auto outputSize = broadcast_ops_npu_output_size(selfCast, otherCast);
  at::Tensor result = OpPreparation::ApplyTensorWithFormat(
      outputSize,
      outputTensor.options(),
      CalcuOpUtil::GetTensorNpuFormat(selfCast));
  floor_divide_out_npu(selfCast, otherCast, result);
  return result;
}

at::Tensor NPUNativeFunctions::floor_divide(const at::Tensor& self, const at::Scalar& other) {
  at::Tensor selfCast = self;
  check_self_dtype_npu(selfCast);
  at::Tensor result = OpPreparation::ApplyTensor(selfCast);
  floor_divide_out_scalar_npu(selfCast, other, result);
  return result;
}

at::Tensor& NPUNativeFunctions::floor_divide_(at::Tensor& self, const at::Tensor& other) {
  at::Tensor otherCast = other;
  check_dtype_npu(self, otherCast);
  
  at::SmallVector<at::Tensor, N> inputs = {self, otherCast};
  at::SmallVector<at::Tensor, N> outputs = {self};
  CalcuOpUtil::CheckMemoryOverLaps(inputs, outputs);
  if (!NpuUtils::check_match(&self)) {
    at::Tensor contiguousSelf = NpuUtils::format_contiguous(self);
    at::Tensor result = floor_divide_out_npu(contiguousSelf, otherCast, contiguousSelf);
    NpuUtils::format_fresh_view(self, result);
  } else {
    floor_divide_out_npu(self, otherCast, self);
  }
  return self;
}

at::Tensor& NPUNativeFunctions::floor_divide_(at::Tensor& self, const at::Scalar& other) {
  check_self_dtype_npu(self);
  if (!NpuUtils::check_match(&self)) {
    at::Tensor contiguousSelf = NpuUtils::format_contiguous(self);
    floor_divide_out_scalar_npu(contiguousSelf, other, contiguousSelf);
    NpuUtils::format_fresh_view(self, contiguousSelf);
  } else {
    floor_divide_out_scalar_npu(self, other, self);
  }
  return self;
}
} // namespace native
} // at_npu
