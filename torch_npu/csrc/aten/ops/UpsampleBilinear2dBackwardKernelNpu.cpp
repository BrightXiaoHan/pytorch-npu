#include "torch_npu/csrc/framework/utils/CalcuOpUtil.h"
#include "torch_npu/csrc/framework/utils/OpAdapter.h"
#include "torch_npu/csrc/aten/NPUNativeFunctions.h"

namespace at_npu {
namespace native {
at::Tensor& upsample_bilinear2d_backward_out_npu_nocheck(
    at::Tensor& grad_input,
    const at::Tensor& grad_output,
    at::IntArrayRef output_size,
    at::IntArrayRef input_size,
    bool align_corners,
    c10::optional<double> scales_h,
    c10::optional<double> scales_w) {
  OpCommand cmd;
  at::Tensor original_image = OpPreparation::ApplyTensor(grad_output, input_size);
  bool half_pixel_centers = !align_corners;
  cmd.Name("ResizeBilinearV2Grad")
      .Input(grad_output, "grads", ACL_FORMAT_NCHW)
      .Input(original_image, "original_image", ACL_FORMAT_NCHW)
      .Output(grad_input, "y", ACL_FORMAT_NCHW)
      .Attr("align_corners", align_corners)
      .Attr("half_pixel_centers", half_pixel_centers)
      .Run();
  return grad_input;
}

at::Tensor& NPUNativeFunctions::upsample_bilinear2d_backward_out(
    const at::Tensor& grad_output,
    at::IntArrayRef output_size,
    at::IntArrayRef input_size,
    bool align_corners,
    c10::optional<double> scales_h,
    c10::optional<double> scales_w,
    at::Tensor& grad_input) {
  OpPreparation::CheckOut(
      {grad_output},
      grad_input,
      grad_output,
      input_size);
  if (!NpuUtils::check_match(&grad_input)) {
    at::Tensor result_contiguous = NpuUtils::format_contiguous(grad_input);

    upsample_bilinear2d_backward_out_npu_nocheck(
        result_contiguous,
        grad_output,
        output_size,
        input_size,
        align_corners,
        scales_h,
        scales_w);
    NpuUtils::format_fresh_view(grad_input, result_contiguous);
  } else {
    upsample_bilinear2d_backward_out_npu_nocheck(
        grad_input,
        grad_output,
        output_size,
        input_size,
        align_corners,
        scales_h,
        scales_w);
  }
  return grad_input;
}

at::Tensor NPUNativeFunctions::upsample_bilinear2d_backward(
    const at::Tensor& grad_output,
    at::IntArrayRef output_size,
    at::IntArrayRef input_size,
    bool align_corners,
    c10::optional<double> scales_h,
    c10::optional<double> scales_w) {
  auto outputSize = upsample_bilinear2d_backward_npu_output_size(
      grad_output, output_size, input_size, align_corners, scales_h, scales_w);
  at::Tensor grad_input = OpPreparation::ApplyTensor(grad_output, outputSize);

  upsample_bilinear2d_backward_out_npu_nocheck(
      grad_input,
      grad_output,
      output_size,
      input_size,
      align_corners,
      scales_h,
      scales_w);
  return grad_input;
}

} // namespace native
} // namespace at_npu
