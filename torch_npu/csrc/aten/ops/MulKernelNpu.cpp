#include "torch_npu/csrc/core/npu/register/OptionsManager.h"
#include "torch_npu/csrc/framework/utils/OpAdapter.h"
#include "torch_npu/csrc/framework/utils/CalcuOpUtil.h"
#include "torch_npu/csrc/aten/NPUNativeFunctions.h"

namespace at_npu
{
  namespace native
  {

    at::Tensor mul_dest_output(const at::Tensor &self, const at::Tensor &other)
    {
      bool isSelfWrapped = CalcuOpUtil::IsScalarWrappedToTensor(self);
      return isSelfWrapped ? other : self;
    }

    at::Tensor &muls_out_npu(at::Tensor &result, const at::Tensor &self, const at::Scalar other)
    {
      auto unified_result = OpPreparation::binary_op_check(result, self, other, true);
      if (!other.isFloatingPoint()) {
        unified_result.common_type = self.scalar_type();
        if (self.scalar_type() == at::kBool) {
          unified_result.common_type = other.type();
        }
      }

      OpCommand cmd;
      cmd.Name("Mul")
          .Expect(unified_result)
          .Input(self)
          .Input(other, self.scalar_type())
          .Output(result)
          .Run();

      return result;
    }

    at::Tensor &mul_out_npu_nocheck(at::Tensor &result, const at::Tensor &self, const at::Tensor &other)
    {
      if (other.dim() == 0 && !torch_npu::utils::is_npu(other))
      {
        muls_out_npu(result, self, other.item());
      }
      else if (self.dim() == 0 && !torch_npu::utils::is_npu(self))
      {
        muls_out_npu(result, other, self.item());
      }
      else
      {
        auto unified_result = OpPreparation::binary_op_check(result, self, other, true);
        OpCommand cmd;
        cmd.Name("Mul")
            .Expect(unified_result)
            .Input(self)
            .Input(other)
            .Output(result)
            .Run();
      }

      return result;
    }

    at::Tensor &NPUNativeFunctions::mul_out(const at::Tensor &self, const at::Tensor &other, at::Tensor &result)
    {
      // calculate the output size
      auto outputSize = broadcast_ops_npu_output_size(self, other);
      OpPreparation::CheckOut(
          {self},
          result,
          CalcuOpUtil::GetTensorNpuFormat(result),
          self.scalar_type(),
          outputSize);
      mul_out_npu_nocheck(result, self, other);

      return result;
    }

    at::Tensor NPUNativeFunctions::mul(const at::Tensor &self, const at::Tensor &other)
    {
      at::Tensor selfCast = self;
      at::Tensor otherCast = other;
      if (self.dtype() == c10::ScalarType::Bool && other.dtype() == c10::ScalarType::Bool)
      {
        selfCast = NPUNativeFunctions::npu_dtype_cast(self, at::kFloat);
        otherCast = NPUNativeFunctions::npu_dtype_cast(other, at::kFloat);
      }

      // calculate the output size
      at::Tensor outputTensor = mul_dest_output(selfCast, otherCast);
      auto outputSize = broadcast_ops_npu_output_size(selfCast, otherCast);

      // construct the output tensor of the NPU
      at::Tensor result = OpPreparation::ApplyTensorWithFormat(
          outputSize,
          outputTensor.options(),
          CalcuOpUtil::GetTensorNpuFormat(outputTensor));

      // calculate the output result of the NPU
      mul_out_npu_nocheck(result, selfCast, otherCast);

      if (self.dtype() == c10::ScalarType::Bool && other.dtype() == c10::ScalarType::Bool)
      {
        result = NPUNativeFunctions::npu_dtype_cast(result, at::kBool);
      }

      return result;
    }

    at::Tensor NPUNativeFunctions::mul(const at::Tensor &self, const at::Scalar& other)
    {
      at::Tensor result = OpPreparation::ApplyTensor(self);

      // calculate the output result of the NPU
      muls_out_npu(result, self, other);

      return result;
    }

    at::Tensor &NPUNativeFunctions::mul_(at::Tensor &self, const at::Tensor &other)
    {
      TORCH_CHECK(torch_npu::utils::is_npu(self), "Input1 must be NPU-Tensor");

      c10::SmallVector<at::Tensor, N> inputs = {self, other};
      c10::SmallVector<at::Tensor, N> outputs = {self};
      CalcuOpUtil::CheckMemoryOverLaps(inputs, outputs);

      at::Tensor self_dtype_cast = 
          (self.scalar_type() == at::kBool) ? NPUNativeFunctions::npu_dtype_cast(self, at::kFloat) : self;
      at::Tensor other_dtype_cast = 
          (other.scalar_type() == at::kBool) ? NPUNativeFunctions::npu_dtype_cast(other, at::kFloat) : other;
      if (!NpuUtils::check_match(&self_dtype_cast)) {
        at::Tensor contiguous_self = NpuUtils::format_contiguous(self_dtype_cast);
        at::Tensor result = mul_out_npu_nocheck(contiguous_self, contiguous_self, other_dtype_cast);
        NpuUtils::format_fresh_view(self_dtype_cast, result);
      } else {
        mul_out_npu_nocheck(self_dtype_cast, self_dtype_cast, other_dtype_cast);
      }
      if (self_dtype_cast.scalar_type() != self.scalar_type()) {
        self_dtype_cast = NPUNativeFunctions::npu_dtype_cast(self_dtype_cast, self.scalar_type());
        self.copy_(self_dtype_cast);
      } else {
        self = self_dtype_cast;
      }

      return self;
    }

    at::Tensor &NPUNativeFunctions::mul_(at::Tensor &self, const at::Scalar& other)
    {
      if (!NpuUtils::check_match(&self))
      {
        at::Tensor contiguous_self = NpuUtils::format_contiguous(self);
        at::Tensor result = muls_out_npu(contiguous_self, contiguous_self, other);
        NpuUtils::format_fresh_view(self, result);
      }
      else
      {
        muls_out_npu(self, self, other);
      }

      return self;
    }

  } // namespace native
} // namespace at_npu
