#include <c10/core/ScalarType.h>
#include <c10/core/impl/DeviceGuardImplInterface.h>
#include <c10/macros/Macros.h>
#include "torch_npu/csrc/core/npu/NPUStream.h"

#include "torch_npu/csrc/framework/StorageDescHelper.h"
#include "torch_npu/csrc/core/NPUTensorImpl.h"
#include "third_party/acl/inc/acl/acl_rt.h"
#include "torch_npu/csrc/core/NPUStorageImpl.h"

namespace torch_npu
{
  NPUTensorImpl::NPUTensorImpl(c10::Storage &&storage,
      const c10::intrusive_ptr<c10::StorageImpl> storage_impl, const caffe2::TypeMeta &data_type)
      : c10::TensorImpl(std::move(storage),
                        c10::DispatchKeySet{c10::DispatchKey::PrivateUse1,
                                            c10::DispatchKey::AutogradPrivateUse1},
                        data_type)
  {
    is_non_overlapping_and_dense_ = false;
    _storage_impl = storage_impl;
  }

  void NPUTensorImpl::shallow_copy_from(const c10::intrusive_ptr<TensorImpl> &impl)
  {
    NPUTensorImpl *npu_impl = static_cast<NPUTensorImpl *>(impl.get());
    copy_tensor_metadata(
        npu_impl,
        this,
        version_counter(),
        allow_tensor_metadata_change());
    npu_impl->refresh_numel();
    npu_impl->refresh_contiguous();
  }

  c10::intrusive_ptr<c10::TensorImpl> NPUTensorImpl::shallow_copy_and_detach(
      const c10::VariableVersion &version_counter,
      bool allow_tensor_metadata_change) const
  {
    auto impl = c10::make_intrusive<NPUTensorImpl>(c10::Storage(this->storage()), this->_storage_impl, this->data_type_);
    copy_tensor_metadata(
        this,
        impl.get(),
        version_counter,
        allow_tensor_metadata_change);
    impl->refresh_numel();
    impl->refresh_contiguous();
    return impl;
  }

  c10::intrusive_ptr<c10::TensorImpl> NPUTensorImpl::shallow_copy_and_detach(
      c10::VariableVersion &&version_counter,
      bool allow_tensor_metadata_change) const
  {
    auto impl = c10::make_intrusive<NPUTensorImpl>(c10::Storage(this->storage()), this->_storage_impl, this->data_type_);
    copy_tensor_metadata(
        this,
        impl.get(),
        std::move(version_counter),
        allow_tensor_metadata_change);
    impl->refresh_numel();
    impl->refresh_contiguous();
    return impl;
  }
  NPUTensorImpl::~NPUTensorImpl() {
    this->_storage_impl.reset();
  }
}
