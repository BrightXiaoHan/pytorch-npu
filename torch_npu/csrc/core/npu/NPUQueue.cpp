#include "torch_npu/csrc/core/npu/NPUQueue.h"
#include "torch_npu/csrc/core/npu/NPUStream.h"
#include "torch_npu/csrc/core/npu/npu_log.h"
#include "torch_npu/csrc/framework/utils/NpuUtils.h"

#ifndef BUILD_LIBTORCH
#include <Python.h>
#endif

#include <unistd.h>
#include <sys/time.h>
#include <sys/eventfd.h>
#include <sys/prctl.h>
#include <third_party/acl/inc/acl/acl_rt.h>

namespace c10_npu {

struct timeval delay = {0, 1};

namespace {

class CallBackManager {
public:
  CallBackManager() {}
  ~CallBackManager() {}
  void SetExec(const ACL_EXEC_FUNC& func) {
    this->execFunc = func;
  }

  void SetCopy(const ACL_COPY_FUNC& func) {
    this->copyFunc = func;
  }

  void SetRelease(const ACL_RELEASE_FUNC& func) {
    this->releaseFunc = func;
  }

  void SetCopyReleaseParam(const ACL_COPY_RELEASE_PARM_FUNC& func) {
    this->copyReleaseParamFunc = func;
  }

  void SetReleaseParam(const ACL_RELEASE_PARAM_FUNC& func) {
    this->releaseParamFunc = func;
  }

  void SetNew(const ACL_NEW_FUNC& func) {
    this->newFunc = func;
  }

  void SetDelete(const ACL_DELETE_FUNC& func) {
    this->deleteFunc = func;
  }

  int Call(void* head, int offset) {
    TORCH_CHECK(this->execFunc, "Failed to find execution function.");
    auto dstPtr = (uint8_t*)head + sizePerParams * offset;
    return this->execFunc(dstPtr);
  }

  void Copy(void* dstHead, int offset, void* src) {
    TORCH_CHECK(this->copyFunc, "Failed to find copy function.");
    auto dstPtr = (uint8_t*)dstHead + sizePerParams * offset;
    return this->copyFunc(dstPtr, src);
  }

  void Release(void* head, int offset, ReleaseQueue& releaseQueue) {
    TORCH_CHECK(this->releaseFunc, "Failed to find release function.");
    auto ptr = (uint8_t*)head +  sizePerParams * offset;
    return this->releaseFunc(ptr, releaseQueue);
  }

  void CopyRealseParam(void* dstHead, int offset, void* src) {
    TORCH_CHECK(this->copyReleaseParamFunc, "Failed to find copy release params function.");
    auto dstPtr = (uint8_t*)dstHead + sizePerParams * offset;
    return this->copyReleaseParamFunc(dstPtr, src);
  }

  void ReleaseParam(void* head, int offset) {
    TORCH_CHECK(this->releaseParamFunc, "Failed to find release params function.");
    auto ptr = (uint8_t*)head +  sizePerParams * offset;
    return this->releaseParamFunc(ptr);
  }

  void* Init(int capacity) {
    TORCH_CHECK(this->newFunc, "Failed to find new function.");
    void* ptr = this->newFunc(capacity, sizePerParams); // not check as CUDA
    return ptr;
  }

  void DeInit(void* ptr) {
    if (ptr != nullptr) {
      TORCH_CHECK(this->deleteFunc, "Failed to find delete function.");
      this->deleteFunc(ptr);
      ptr = nullptr;
    }
  }
private:
  int sizePerParams = 0;
  ACL_EXEC_FUNC execFunc = nullptr;
  ACL_COPY_FUNC copyFunc = nullptr;
  ACL_RELEASE_FUNC releaseFunc = nullptr;
  ACL_NEW_FUNC newFunc = nullptr;
  ACL_DELETE_FUNC deleteFunc = nullptr;
  ACL_COPY_RELEASE_PARM_FUNC copyReleaseParamFunc = nullptr;
  ACL_RELEASE_PARAM_FUNC releaseParamFunc = nullptr;
}; // class CallBackManager

CallBackManager& manager() {
  static CallBackManager instance;
  return instance;
}

CallBackManager& releaseManager() {
  static CallBackManager releaseinstance;
  return releaseinstance;
}
} // namespace

namespace register_queue_cb {
NPUCallBackRegisterBuilder::NPUCallBackRegisterBuilder(const ACL_EXEC_FUNC& execFunc,
    const ACL_COPY_FUNC& copyFunc, const ACL_RELEASE_FUNC& releaseFunc,
    const ACL_NEW_FUNC& newFunc, const ACL_DELETE_FUNC& deleteFunc,
    const ACL_COPY_RELEASE_PARM_FUNC& copyReleaseParamF, const ACL_RELEASE_PARAM_FUNC& releaseParamF) {
  manager().SetExec(execFunc);
  manager().SetCopy(copyFunc);
  manager().SetRelease(releaseFunc);
  manager().SetNew(newFunc);
  manager().SetDelete(deleteFunc);
  releaseManager().SetCopyReleaseParam(copyReleaseParamF);
  releaseManager().SetReleaseParam(releaseParamF);
  releaseManager().SetNew(newFunc);
  releaseManager().SetDelete(deleteFunc);
}
} // namespace register_queue_cb


// If the capacity is too large, when the queue is full,
// a large amount of device memory is occupied at the same time;
// if the capacity is too small, and the main thread is fast enough,
// it does not make full use of concurrent design capabilities.
static constexpr size_t kQueueCapacity = 4096;

RepoStatus Repository::GetStatus() const {
  if (initialized == false) {
    ASCEND_LOGE("Task queue is not initialized, shouldn't call GetStatus(). !!");
  }

  return repo_status.load();
}

void Repository::SetStatus(RepoStatus desired) {
  if (initialized == false) {
    ASCEND_LOGE("Task queue is not initialized, shouldn't call SetStatus(). !!");
    return;
  }

  repo_status = desired;
}

void Repository::ChangeStatus(RepoStatus expected, RepoStatus desired) {
  if (initialized == false) {
    ASCEND_LOGE("Task queue is not initialized, shouldn't call ChangeStatus(). !!");
    return;
  }

  repo_status.compare_exchange_strong(expected, desired);
}

NPUStatus Repository::MakeSureQueueEmpty() {
  if (initialized == false) {
    ASCEND_LOGE("Task queue is not initialized, shouldn't call MakeSureQueueEmpty(). !!");
    return FAILED;
  }

  // While waiting for ACL thread to launch tasks,
  // the current thread should not hold GIL.
  // When the operator compilation is triggered in the ACL thread,
  // the TE module attempts to obtain the GIL.
  // If the current thread does not release the GIL, a deadlock will
  // occur.
#ifndef BUILD_LIBTORCH
  PyThreadState *gilState = nullptr;
  if (PyGILState_Check()) {
    gilState = PyEval_SaveThread();
  }
#endif

  if (consumer.joinable()) {
    ssize_t s;
    uint64_t u = 1;
    while (!IsEmptyQueue()) {
      std::lock_guard<std::mutex> lock(mu_empty);
      need_empty = true;
      __sync_synchronize();
      if (!IsEmptyQueue()) { // double-check, very important idea
        s = eventfd_read(efd_empty, &u);
        if (s != 0) {
          if (errno == EINTR) {
            continue;
          }
          ASCEND_LOGE("eventfd_read failed. s=%zd, errno=%s.", s, strerror(errno));
#ifndef BUILD_LIBTORCH
          // Get the GIL
          if (gilState) {
            PyEval_RestoreThread(gilState);
          }
#endif
          return INTERNEL_ERROR;
        }
      }
      need_empty = false;
    }
  }

#ifndef BUILD_LIBTORCH
  // Get the GIL
  if (gilState) {
    PyEval_RestoreThread(gilState);
  }
#endif

  return SUCCESS;
}

bool Repository::WriteQueue(void* cur_paras) {
  std::lock_guard<std::mutex> lock(mu_enqueue);
  if (IsFullQueue()) {
    return false;
  }

  __sync_synchronize();
  manager().Copy(datas, write_idx.idx, cur_paras);
  __sync_synchronize();

  write_idx.idx = (write_idx.idx + 1) & (kQueueCapacity - 1);
  return true;
}

bool Repository::ReadQueue() {
  if (IsEmptyQueue()) {
    return false;
  }

  __sync_synchronize();
#ifndef BUILD_LIBTORCH
  at_npu::native::NpuUtils::ProfReportMarkDataToNpuProfiler(2, datas, read_idx.idx);
  auto ret = manager().Call(datas, read_idx.idx);
  at_npu::native::NpuUtils::ProfReportMarkDataToNpuProfiler(3, datas, read_idx.idx);
#else
  auto ret = manager().Call(datas, read_idx.idx);
#endif
  if (ret != 0) {
    ASCEND_LOGE("---Thread---%llu: device = %d, write_idx = %u, read_idx = %u, status = %d, ret = %d",
                std::this_thread::get_id(), device_idx, write_idx.idx, read_idx.idx, GetStatus(), ret);
    while (!IsEmptyQueue()) { // ignore other tasks
      manager().Release(datas, read_idx.idx, releaseQueue);
      read_idx.idx = (read_idx.idx + 1) & (kQueueCapacity - 1);
    }
    ReleaseResource();
    throw std::runtime_error("ASCEND kernel errors might be asynchronously reported at some other API call, "\
                             "so the stacktrace below is not the root cause of the problem.\n" \
                             "For getting the stacktrace of OP in PyTorch, consider passing ASCEND_LAUNCH_BLOCKING=1.");
  }

  manager().Release(datas, read_idx.idx, releaseQueue);
  __sync_synchronize();

  read_idx.idx = (read_idx.idx + 1) & (kQueueCapacity - 1);

  return true;
}

void Repository::Enqueue(void* cur_paras) {
  if (initialized == false) {
    ASCEND_LOGE("Task queue is not initialized, shouldn't call Enqueue(). !!");
    return;
  }
  if (GetStatus() != RUN && GetStatus() != INIT) {
    ASCEND_LOGE("Task queue thread is exit, cann't call Enqueue(). !!");
    return;
  }
  bool ret = false;
  ssize_t s;
  uint64_t u = 1;

  SetWriteWorking(true);
  while (ret == false) {
    ret = WriteQueue(cur_paras);
    if (ret == false) {
      SetWriteWorking(false);
      __sync_synchronize();
      if (IsFullQueue()) {
#ifndef BUILD_LIBTORCH
        // double check the current thread hold a Gil lock
        if (PyGILState_Check()) {
          Py_BEGIN_ALLOW_THREADS s = eventfd_read(efd_write, &u);
          Py_END_ALLOW_THREADS
        } else {
          s = eventfd_read(efd_write, &u);
        }
#else
        s = eventfd_read(efd_write, &u);
#endif
        if (s != 0) {
          if (errno == EINTR) {
            continue;
          }
          ASCEND_LOGE("waiting dequeue failed. s=%zd, errno=%s.", s, strerror(errno));
          return;
        }
        SetWriteWorking(true);
      }
      continue;
    }
    __sync_synchronize();
    while (!IsReadWorking()) {
      s = eventfd_write(efd_read, u);
      if (s != 0) {
        if (errno == EINTR) {
          continue;
        }
        ASCEND_LOGE("notify consumer failed!! s=%zd, errno=%s", s, strerror(errno));
        return;
      }
      break;
    }
  }
  SetWriteWorking(false);
}

void Repository::Dequeue() {
  if (initialized == false) {
    ASCEND_LOGE("Task queue is not initialized, shouldn't call Dequeue(). !!");
    return;
  }

  bool ret = false;
  bool notify_empty = false;
  ssize_t s;
  uint64_t u = 1;

  SetReadWorking(true);
  while (ret == false && GetStatus() != RepoStatus::CAN_EXIT) {
    ret = ReadQueue();
    if (ret == false) {
      if (GetStatus() == RepoStatus::NEED_EXIT) {
        ChangeStatus(NEED_EXIT, CAN_EXIT);
        break;
      }

      SetReadWorking(false);
      __sync_synchronize();
      if (IsEmptyQueue()) {
        s = eventfd_read(efd_read, &u);
        if (s != 0) {
          if (errno == EINTR) {
            continue;
          }
          ASCEND_LOGE("waiting enqueue failed. s=%zd, errno=%s.", s, strerror(errno));
          return;
        }
        SetReadWorking(true);
      }
      continue;
    }
    __sync_synchronize();
    notify_empty = need_empty &&
        IsEmptyQueue(); // need_empty && (ret == false || IsEmptyQueue());
    while (notify_empty) {
      s = eventfd_write(efd_empty, u);
      if (s != 0) {
        if (errno == EINTR) {
          continue;
        }
        ASCEND_LOGE("notify make_sure failed. s=%zd, errno=%s.", s, strerror(errno));
        return;
      }
      break;
    }
    __sync_synchronize();
    while (!IsWriteWorking()) {
      s = eventfd_write(efd_write, u);
      if (s != 0) {
        if (errno == EINTR) {
          continue;
        }
        ASCEND_LOGE("notify producer failed. s=%zd, errno=%s.", s, strerror(errno));
        return;
      }
      break;
    }
  }
  SetReadWorking(false);
}

void Repository::ReleaseResource() {
  manager().DeInit(datas);
  if (efd_read > 0) {
    close(efd_read);
    efd_read = -1;
  }
  if (efd_write > 0) {
    close(efd_write);
    efd_write = -1;
  }
  if (efd_empty > 0) {
    close(efd_empty);
    efd_empty = -1;
  }
}

Repository::~Repository() {
  if (initialized) {
    if (consumer.joinable()) {
      SetStatus(NEED_EXIT);
      (void)eventfd_write(efd_read, 1); // escape wait
      consumer.join();
    }
    eventfd_write(efd_empty, 1);
    ReleaseResource();
  }
}

bool Repository::IsFullQueue() const {
  return ((write_idx.idx + 1) & (kQueueCapacity - 1)) == read_idx.idx;
}

bool Repository::CheckInit() const {
  return initialized;
}

void StartConsume(Repository* repo, c10::DeviceIndex device_id) {
  if (prctl(PR_SET_NAME, ("ACL_thread")) != 0) {
    ASCEND_LOGE("set thread name failed!");
  }

  aclError ret = aclrtSetDevice(device_id);
  if (ret != 0) {
    C10_NPU_SHOW_ERR_MSG();
    ASCEND_LOGE("***Thread*%d: set device (%d): ret = %d", std::this_thread::get_id(), device_id, ret);
  }

  while (repo->GetStatus() != RepoStatus::CAN_EXIT) {
    repo->Dequeue();
  }
  return;
}

void Repository::InitRepo(c10::DeviceIndex device_id) {
  if (datas == nullptr) {
    datas = manager().Init(kQueueCapacity);
    ASCEND_LOGI("TaskQueue is enable");
  }

  efd_read = eventfd(0, 0);
  efd_write = eventfd(0, 0);
  efd_empty = eventfd(0, 0);

  initialized = true;
  SetStatus(INIT);
  device_idx = device_id;
  std::thread cur_consumer(StartConsume, this, device_id);
  consumer = std::move(cur_consumer);

  releaseQueue.InitReleaseQueue();
}

static constexpr size_t kReleaseQueueCapacity = 8192;
bool ReleaseQueue::WriteToReleaseQueue(void* cur_paras)
{
  if (IsFullQueue()) {
    return false;
  }
  __sync_synchronize();
  releaseManager().CopyRealseParam(datas, write_idx.idx, cur_paras);

  __sync_synchronize();
  write_idx.idx = (write_idx.idx + 1) & (kReleaseQueueCapacity - 1);
  return true;
}

void ReleaseQueue::PushToReleaseQueue(void* cur_paras) {
  if (initialized == false) {
    ASCEND_LOGE("Release queue is not initialized, shouldn't call PushToReleaseQueue(). !!");
    return;
  }

  bool ret = false;
  while (ret == false) {
    ret = WriteToReleaseQueue(cur_paras);
    if (ret == true) {
      break;
    }
  }
}

bool ReleaseQueue::ReadFromReleaseQueue() {
  if (IsEmptyQueue()) {
    return false;
  }

  __sync_synchronize();
  releaseManager().ReleaseParam(datas, read_idx.idx);

  __sync_synchronize();
  read_idx.idx = (read_idx.idx + 1) & (kReleaseQueueCapacity - 1);

  return true;
}

void ReleaseQueue::PopFromReleaseQueue() {
  if (initialized == false) {
    ASCEND_LOGE("Release queue is not initialized, shouldn't call PopFromReleaseQueue(). !!");
    return;
  }

  bool ret = false;
  while ((ret == false) && (GetStatus() != RepoStatus::CAN_EXIT)) {
    ret = ReadFromReleaseQueue();
    if (ret == false) {
      if (GetStatus() == RepoStatus::NEED_EXIT) {
        ChangeStatus(NEED_EXIT, CAN_EXIT);
        break;
      }
      delay.tv_usec = 1;
      select(0, nullptr, nullptr, nullptr, &delay);
    }
  }
}

void StartRelease(ReleaseQueue* releaseQue) {
  if (prctl(PR_SET_NAME, ("Release_thread")) != 0) {
    ASCEND_LOGE("set thread name failed!");
  }

  while (releaseQue->GetStatus() != RepoStatus::CAN_EXIT) {
    releaseQue->PopFromReleaseQueue();
  }
  return;
}

void ReleaseQueue::InitReleaseQueue() {
  if (datas == nullptr) {
    datas = releaseManager().Init(kReleaseQueueCapacity);
  }

  initialized = true;
  SetStatus(INIT);
  std::thread cur_releaser(StartRelease, this);
  releaser = std::move(cur_releaser);
}

ReleaseQueue::~ReleaseQueue() {
  if (initialized) {
    if (releaser.joinable()) {
      SetStatus(NEED_EXIT);
      releaser.join();
    }
  }
  releaseManager().DeInit(datas);
}

bool ReleaseQueue::IsFullQueue() const {
  return ((write_idx.idx + 1) % kReleaseQueueCapacity) == read_idx.idx;
}

RepoStatus ReleaseQueue::GetStatus() const {
  if (initialized == false) {
    ASCEND_LOGE("Release queue is not initialized, shouldn't call GetStatus(). !!");
  }

  return repo_status.load();
}

void ReleaseQueue::SetStatus(RepoStatus desired) {
  if (initialized == false) {
    ASCEND_LOGE("Release queue is not initialized, shouldn't call SetStatus(). !!");
    return;
  }

  repo_status = desired;
}

void ReleaseQueue::ChangeStatus(RepoStatus expected, RepoStatus desired) {
  if (initialized == false) {
    ASCEND_LOGE("Release queue is not initialized, shouldn't call ChangeStatus(). !!");
    return;
  }

  repo_status.compare_exchange_strong(expected, desired);
}
} // namespace c10_npu
