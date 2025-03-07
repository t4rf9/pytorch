#include <torch/csrc/distributed/c10d/NCCLUtils.hpp>
#include <torch/csrc/distributed/c10d/ProcessGroupNCCL.hpp>
#include <fstream>
#include <mutex>
#include <sstream>

#if defined(__linux__)
#include <fcntl.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <unistd.h>
#endif

#ifdef USE_C10D_NCCL

#include <exception>
#include <map>
#include <stdexcept>
#include <tuple>
#include <unordered_set>
#include <utility>

#include <ATen/cuda/CUDAContext.h>
#include <ATen/cuda/CUDAGraph.h>
#include <c10/core/DeviceType.h>
#include <c10/cuda/CUDAAllocatorConfig.h>
#include <c10/cuda/CUDAGraphsC10Utils.h>
#include <c10/cuda/CUDAGuard.h>
#include <c10/util/CallOnce.h>
#include <c10/util/Exception.h>
#include <c10/util/Logging.h>
#include <c10/util/Optional.h>
#include <c10/util/irange.h>
#include <torch/csrc/cuda/nccl.h>
#include <torch/csrc/distributed/c10d/ParamCommsUtils.hpp>
#include <torch/csrc/distributed/c10d/TraceUtils.h>
#include <torch/csrc/distributed/c10d/Utils.hpp>
#include <torch/torch.h>

namespace c10d {

constexpr const char* const kNCCLAbortedCommStoreKey = "NCCLABORTEDCOMM";

namespace {

#if defined(NCCL_MAJOR) && \
    ((NCCL_MAJOR > 2) || (NCCL_MAJOR == 2) && (NCCL_MINOR >= 10))
#define NCCL_HAS_AVG 1
#endif

// NCCL op mapping
const std::map<ReduceOp::RedOpType, ncclRedOp_t> ncclOp = {
    {ReduceOp::MIN, ncclMin},
    {ReduceOp::MAX, ncclMax},
    {ReduceOp::SUM, ncclSum},
    {ReduceOp::PRODUCT, ncclProd},
#ifdef NCCL_HAS_AVG
    {ReduceOp::AVG, ncclAvg},
#endif
};

// NCCL type typing
std::map<at::ScalarType, ncclDataType_t> ncclDataType = {
    {at::kChar, ncclInt8},
    {at::kByte, ncclUint8},
    {at::kFloat, ncclFloat},
    {at::kDouble, ncclDouble},
    {at::kInt, ncclInt32},
    {at::kLong, ncclInt64},
    {at::kHalf, ncclHalf},
    {at::kBool, ncclUint8},
#if HAS_NCCL_BF16_DATATYPE
    {at::kBFloat16, ncclBfloat16},
#endif
};

// Helper function that gets the data type and issues error if not supported
ncclDataType_t getNcclDataType(at::ScalarType type) {
  auto it = ncclDataType.find(type);
  TORCH_CHECK_WITH(
      TypeError,
      it != ncclDataType.end(),
      "Input tensor data type is not supported for NCCL process group: ",
      type);
  return it->second;
}

#ifdef ENABLE_NCCL_PREMUL_SUM_SUPPORT
template <typename T, ncclDataType_t dataType>
ncclRedOpRAII unpackPreMulSum(
    const ReduceOp& reduceOp,
    const ncclComm_t& comm) {
  const auto* preMulSupplement =
      reinterpret_cast<NCCLPreMulSumSupplement*>(reduceOp.supplement_.get());
  ncclRedOp_t preMulSum;
  bool has_tensor = preMulSupplement->tensor_factor.defined();
  auto residence = has_tensor ? ncclScalarDevice : ncclScalarHostImmediate;
  const T* ptr_factor = has_tensor
      ? preMulSupplement->tensor_factor.const_data_ptr<T>()
      : nullptr;
  T scalar_factor = T(preMulSupplement->double_factor);
  ncclRedOpCreatePreMulSum(
      &preMulSum,
      // https://docs.nvidia.com/deeplearning/nccl/user-guide/docs/api/ops.html#ncclredopcreatepremulsum
      // tells us that the scalar input is strictly a multiplier.
      /*scalar=*/has_tensor ? const_cast<T*>(ptr_factor) : &scalar_factor,
      dataType,
      residence,
      comm);
  return ncclRedOpRAII(preMulSum, comm);
}
#endif

ncclRedOpRAII getNcclReduceOp(
    const ReduceOp& reduceOp,
    at::Tensor& input,
    const ncclDataType_t& dataType,
    const ncclComm_t& comm) {
  try {
    if (input.scalar_type() == at::kBool) {
      if (reduceOp == ReduceOp::SUM) {
        // For bool tensors, map sum to max, which both represent a bitwise or.
        // This is to prevent overflow issues with sum, since we use uint8 to
        // represent a bool (see ncclDataType mapping).
        return ncclMax;
      }
#ifdef NCCL_HAS_AVG
      if (reduceOp == ReduceOp::AVG) {
        C10_THROW_ERROR(
            TypeError, "Cannot use ReduceOp.AVG with boolean inputs");
      }
#endif
    }
    if (reduceOp == ReduceOp::PREMUL_SUM) {
#ifdef ENABLE_NCCL_PREMUL_SUM_SUPPORT
      switch (dataType) {
        case ncclHalf:
          return unpackPreMulSum<at::Half, ncclHalf>(reduceOp, comm);
        case ncclFloat:
          return unpackPreMulSum<float, ncclFloat>(reduceOp, comm);
        case ncclDouble:
          return unpackPreMulSum<double, ncclDouble>(reduceOp, comm);
        default:
          C10_THROW_ERROR(
              TypeError, "PreMulSum Data type must be half, float, or double");
          ncclRedOp_t unused;
          return unused;
      }
#else
      C10_THROW_ERROR(ValueError, "PreMulSum requires NCCL>=2.11.1");
#endif
    }
    return ncclOp.at(reduceOp);
  } catch (const std::out_of_range& e) {
    switch (reduceOp) {
      case ReduceOp::AVG:
        C10_THROW_ERROR(
            ValueError,
            c10::str(
                "AVG requires NCCL 2.10+. The current version is ",
                NCCL_MAJOR,
                ".",
                NCCL_MINOR));
        break;
      case ReduceOp::BAND:
        C10_THROW_ERROR(ValueError, "Cannot use ReduceOp.BAND with NCCL");
        break;
      case ReduceOp::BOR:
        C10_THROW_ERROR(ValueError, "Cannot use ReduceOp.BOR with NCCL");
        break;
      case ReduceOp::BXOR:
        C10_THROW_ERROR(ValueError, "Cannot use ReduceOp.BXOR with NCCL");
        break;
      default:
        C10_THROW_ERROR(ValueError, "Unhandled ReduceOp");
        break;
    }
  }
}

// Get a key string from device
inline std::string getKeyFromDevice(at::Device& device) {
  return std::to_string(device.index());
}

std::string getKeySendRecv(int myRank, int peer) {
  int lowRank = myRank < peer ? myRank : peer;
  int highRank = myRank < peer ? peer : myRank;
  std::string sendRecvPair =
      std::to_string(lowRank) + ":" + std::to_string(highRank);
  return sendRecvPair;
}

// Get device from tensor
inline at::Device getDevice(at::Tensor& tensor) {
  return tensor.device();
}

// [Sync Streams] Helper that lets the input ncclStreams to wait for the current
// stream. NCCL communications run on ncclStreams, but input tensors are
// allocated on different streams (i.e., current streams). Communications on
// ncclStreams cannot start before pending input tensor ops on current streams
// finish. Otherwise, ops on two streams might read/write same tensors
// concurrently.
//
// The synchronization above alone is not enough. We also need to make sure
// input tensors are not freed before their usages on ncclStreams finish. This
// can be achieved by calling c10::cuda::CUDACachingAllocator::recordStream,
// which remembers the usage stream (ncclStream), creates an event on the usage
// stream when GC attempts to free the input tensor, and delays GC until that
// event is done.
void syncStream(
    at::Device& device,
    at::cuda::CUDAEvent& ncclEvent,
    at::cuda::CUDAStream& ncclStream) {
  ncclEvent.record(at::cuda::getCurrentCUDAStream(device.index()));
  ncclEvent.block(ncclStream);
}

// Given a ncclUniqueId, convert it to a string representation that can be put
// in the store.
std::string buildNcclUniqueIdStr(const ncclUniqueId& ncclID) {
  const uint8_t* bytes = reinterpret_cast<const uint8_t*>(&ncclID);
  std::ostringstream oss;
  for (const auto i : c10::irange(NCCL_UNIQUE_ID_BYTES)) {
    oss << std::hex << static_cast<int>(bytes[i]);
  }
  return oss.str();
}

std::string getNcclAbortedCommStoreKey(const std::string ncclIdStr) {
  return std::string(kNCCLAbortedCommStoreKey) + ":" + ncclIdStr;
}

// Returns exception's what() given an exception_ptr instance.
std::string getExceptionMsgFromExceptionPtr(
    const std::exception_ptr& exceptionPtr) {
  TORCH_CHECK(exceptionPtr != nullptr);
  try {
    std::rethrow_exception(exceptionPtr);
  } catch (const std::exception& e) {
    return e.what();
  } catch (...) {
    return "Unknown exception type";
  }
}

inline void errorIfCapturingNonCapturableNCCL(c10::cuda::CaptureStatus status) {
  // parentheses avoid some compiler warnings
  static const uint64_t min_version =
      (((uint64_t)2) << 32) + (((uint64_t)9) << 16) + ((uint64_t)6);
  static const uint64_t cur_version = torch::cuda::nccl::version();
  if (cur_version < min_version) {
    TORCH_CHECK_WITH(
        NotImplementedError,
        status == c10::cuda::CaptureStatus::None,
        "Capturing NCCL collectives is only allowed with NCCL >= 2.9.6");
  }
}

} // namespace

// Map from each communicator to its device index.
// This map is used when register/deregister cache segments from cache
// allocator. See design notes below:
// - Each segment should be registered only to the communicator on the
//   same device.
// - We cannot reuse devNCCLCommMap_ in each ProcessGroup because the key may be
//   ranks rather than device in point-to-point case.
// - This map has also to be maintained as global variable since the register
//   hooks are called outside the scope of any PG, thus we need traverse
//   communicators in all PGs.
static std::unordered_map<std::shared_ptr<NCCLComm>, int> ncclCommDevIdxMap;
static std::mutex ncclCommDevIdxMapMutex;
static bool allocatorHooksAttached = false;
void cacheAllocatorRegisterHook(
    const c10::cuda::CUDACachingAllocator::TraceEntry& te) {
  // Register after SEGMENT_ALLOC
  if (te.action_ !=
      c10::cuda::CUDACachingAllocator::TraceEntry::Action::SEGMENT_ALLOC) {
    return;
  }

  std::lock_guard<std::mutex> lock(ncclCommDevIdxMapMutex);
  for (auto& it : ncclCommDevIdxMap) {
    auto& ncclComm = it.first;
    auto& devIdx = it.second;
    if (te.device_ == devIdx) {
      ncclComm->registerSegment(reinterpret_cast<void*>(te.addr_), te.size_);
    }
  }
}

void cacheAllocatorDeregisterHook(
    const c10::cuda::CUDACachingAllocator::TraceEntry& te) {
  // deregister before SEGMENT_FREE
  if (te.action_ !=
      c10::cuda::CUDACachingAllocator::TraceEntry::Action::SEGMENT_FREE) {
    return;
  }

  std::lock_guard<std::mutex> lock(ncclCommDevIdxMapMutex);
  for (auto& it : ncclCommDevIdxMap) {
    auto& ncclComm = it.first;
    auto& devIdx = it.second;
    if (te.device_ == devIdx) {
      ncclComm->deregisterSegment(reinterpret_cast<void*>(te.addr_));
    }
  }
}

#if defined(IS_NCCL_EXP) && defined(NCCL_COMM_DUMP)
std::string dump_nccl_trace() {
  std::unordered_map<
      std::string /* ncclUniqueID */,
      std::unordered_map<std::string, std::string> /* dump from this comm */>
      ncclDumpMap;
  // dump_nccl_trace is only called from the default PG (uid_=0), but we want to
  // dump from all comms so we need to iterate over ncclCommDevIdxMap, which
  // is static
  std::vector<std::shared_ptr<NCCLComm>> allNCCLComms;
  // within the critical section, we don't want to dump while holding the lock
  // as dump might hang
  ncclCommDevIdxMapMutex.lock();
  for (auto& [ncclComm, _] : ncclCommDevIdxMap) {
    allNCCLComms.push_back(ncclComm);
  }
  ncclCommDevIdxMapMutex.unlock();
  for (auto& ncclComm : allNCCLComms) {
    std::string ncclUniqueIDStr = buildNcclUniqueIdStr(ncclComm->getNcclId());
    ncclDumpMap[ncclUniqueIDStr] = ncclComm->ncclCommDump();
  }
  return NCCLTraceBuffer::get()->dump(ncclDumpMap);
}
#else
std::string dump_nccl_trace() {
  return NCCLTraceBuffer::get()->dump(c10::nullopt);
}
#endif

c10::optional<std::function<std::string()>>& get_cpp_trace_dumper() {
  static c10::optional<std::function<std::string()>> dumper(c10::nullopt);
  return dumper;
}

gil_checker_t& get_gil_checker() {
  static gil_checker_t gil_checker = nullptr;
  return gil_checker;
}

std::future<bool> launchAsyncGilCheck() {
  std::promise<bool> resultPromise;
  std::future<bool> resultFuture = resultPromise.get_future();
  TORCH_CHECK(get_gil_checker(), "Can't check GIL with null GIL checker");
  std::thread workerThread([promise = std::move(resultPromise)]() mutable {
    try {
      auto& gil_checker = get_gil_checker();
      promise.set_value((*gil_checker)());
    } catch (...) {
      promise.set_exception(std::current_exception());
    }
  });

  // Detach the thread to allow it to run independently
  workerThread.detach();

  return resultFuture;
}

// Return CUDA device with ordinal given by input rank.  If we aren't
// bound to a specific device, there is no strict guarantee that this
// heuristic is the correct assignment of ranks to GPUs that Python
// layers use, but in practice it tends to be.  Fortunately we don't
// rely on this for correctness of any tensor operations, just for
// ancillary uses like barriers.
at::Device ProcessGroupNCCL::guessDeviceForRank() const {
  TORCH_CHECK_WITH(ValueError, rank_ >= 0, "Invalid rank ", rank_);
  if (getBoundDeviceId()) {
    return *getBoundDeviceId();
  } else {
    auto numGPUs = at::cuda::getNumGPUs();
    int16_t deviceIdx = static_cast<int16_t>(rank_ % numGPUs);
    return at::Device(at::DeviceType::CUDA, deviceIdx);
  }
}

const int64_t ProcessGroupNCCL::kWatchdogThreadSleepMillis = 100;
constexpr int64_t kSynchronizeBusyWaitMillis = 10;
thread_local uint64_t ProcessGroupNCCL::ncclActiveGroupCounter_ = 0;

std::ostream& operator<<(
    std::ostream& output,
    const ProcessGroupNCCL::WorkNCCL& workNCCL) {
  std::string workInfo;
  workInfo = c10::str(
      "WorkNCCL(",
      "SeqNum=",
      workNCCL.seq_,
      ", OpType=",
      opTypeToString(workNCCL.opType_),
      ", NumelIn=",
      workNCCL.numelIn_,
      ", NumelOut=",
      workNCCL.numelOut_,
      ", Timeout(ms)=",
      workNCCL.opTimeout_.count(),
      ")");
  return output << workInfo;
}

ProcessGroupNCCL::WorkNCCL::WorkNCCL(
    at::Device& device,
    int rank,
    OpType opType,
    uint64_t seq,
    const char* profilingTitle,
    const c10::optional<std::vector<at::Tensor>>& inputs,
    bool desyncDebug,
    bool enableTiming,
    DebugLevel distDebugLevel)
    : Work(rank, opType, profilingTitle, inputs),
      device_(device),
      workStartTime_(std::chrono::steady_clock::now()),
      seq_(seq),
      timingEnabled_(enableTiming),
      distDebugLevel_(distDebugLevel) {
  // Creates the CUDA event wrappers
  // Note: The actual events are lazily created when first recorded to with
  // DEFAULT_FLAGS = cudaEventDisableTiming.
  if (enableTiming) {
    ncclStartEvent_ = std::make_shared<at::cuda::CUDAEvent>(cudaEventDefault);
  }
  ncclEndEvent_ = std::make_shared<at::cuda::CUDAEvent>(
      enableTiming ? cudaEventDefault : cudaEventDisableTiming);
}

ProcessGroupNCCL::WorkNCCL::WorkNCCL(const WorkNCCL& w)
    : Work(w.rank_, w.opType_),
      std::enable_shared_from_this<WorkNCCL>(w),
      device_(w.device_),
      ncclStartEvent_(w.ncclStartEvent_),
      ncclEndEvent_(w.ncclEndEvent_),
      ncclComm_(w.ncclComm_),
      blockingWait_(w.blockingWait_),
      opTimeout_(w.opTimeout_),
      workStartTime_(w.workStartTime_),
      seq_(w.seq_),
      startTraceUpdated_(w.startTraceUpdated_),
      numelIn_(w.numelIn_),
      numelOut_(w.numelOut_),
      store_(w.store_),
      timingEnabled_(w.timingEnabled_),
      trace_id_(w.trace_id_),
      distDebugLevel_(w.distDebugLevel_) {
  exception_ = w.exception_;
}

ProcessGroupNCCL::WorkNCCL::~WorkNCCL() = default;

bool ProcessGroupNCCL::WorkNCCL::isCompleted() {
  checkAndSetException();
  return exception() || finishedGPUExecutionInternal();
}

bool ProcessGroupNCCL::WorkNCCL::isStarted() {
  checkAndSetException();
  return exception() || startedGPUExecutionInternal();
}

bool ProcessGroupNCCL::WorkNCCL::isSuccess() const {
  C10_THROW_ERROR(NotImplementedError, "WorkNCCL::isSuccess() is deprecated");
}

void ProcessGroupNCCL::WorkNCCL::checkAndSetException() {
  if (exception()) {
    // We already have an exception.
    return;
  }

  auto exception_ptr = checkForNCCLErrors();
  std::unique_lock<std::mutex> lock(mutex_);
  exception_ = exception_ptr;
  if (exception_) {
    LOG(INFO) << logPrefix()
              << "found async exception when checking for NCCL errors: "
              << getExceptionMsgFromExceptionPtr(exception_);
  }
}

const std::string& ProcessGroupNCCL::WorkNCCL::logPrefix() const {
  static std::string prefix = c10::str("[Rank ", rank_, "] ");
  return prefix;
}

void ProcessGroupNCCL::WorkNCCL::setException(
    std::exception_ptr exception_ptr) {
  std::unique_lock<std::mutex> lock(mutex_);
  exception_ = exception_ptr;
}

// Helper that checks if the NCCL kernels are completed on the GPUs
bool ProcessGroupNCCL::WorkNCCL::finishedGPUExecution() {
  checkAndSetException();
  return finishedGPUExecutionInternal();
}

bool ProcessGroupNCCL::WorkNCCL::startedGPUExecutionInternal() const {
  // if timing is disabled we won't have allocated start events
  if (!timingEnabled_) {
    return false;
  }
  // Checking the work's corresponding CUDA event's status
  if (!ncclStartEvent_->query()) {
    return false;
  }
  return true;
}

bool ProcessGroupNCCL::WorkNCCL::finishedGPUExecutionInternal() const {
  // Checking the work's corresponding CUDA event's status
  if (!ncclEndEvent_->query()) {
    return false;
  }
  return true;
}

bool ProcessGroupNCCL::WorkNCCL::checkTimeout(
    c10::optional<std::chrono::milliseconds> timeout) {
  auto currentTimepoint = std::chrono::steady_clock::now();
  auto timeElapsed = std::chrono::duration_cast<std::chrono::milliseconds>(
      currentTimepoint - workStartTime_);
  auto workTimeout = timeout ? *timeout : opTimeout_;

  if (timeElapsed < workTimeout)
    return false;

  // Timed out

  // There is already an error, we don't override it
  if (exception())
    return true;

  std::string exceptionMsg = c10::str(
      logPrefix(),
      "Watchdog caught collective operation timeout: ",
      *this,
      " ran for ",
      timeElapsed.count(),
      " milliseconds before timing out.");

  LOG(ERROR) << exceptionMsg;
  std::exception_ptr exception_ptr =
      std::make_exception_ptr(C10_BUILD_ERROR(DistBackendError, exceptionMsg));
  setException(exception_ptr);
  return true;
}

void ProcessGroupNCCL::WorkNCCL::handleException(
    ErrorHandlingMode errorHandling) {
  if (exception_) {
    auto exceptionMsg = c10::str(
        "Some NCCL operations have failed or timed out. Due to the ",
        "asynchronous nature of CUDA kernels, subsequent GPU operations ",
        "might run on corrupted/incomplete data.");
    LOG(ERROR) << logPrefix() << exceptionMsg;
    C10_LOG_API_USAGE_ONCE("ProcessGroupNCCL.WorkNCCL.handleException");

    if (SHOULD_TEAR_DOWN(errorHandling)) {
      auto tearDownMsg = c10::str(
          "To avoid data inconsistency, we are taking the entire process down.");
      LOG(ERROR) << logPrefix() << tearDownMsg;
      std::rethrow_exception(exception_);
    }
  }
}

void ProcessGroupNCCL::WorkNCCL::synchronize() {
  // Call Synchronize without a timeout. We use this method to avoid adding a
  // timeout argument to the public synchronize API.
  synchronizeInternal(kNoTimeout);
}

void ProcessGroupNCCL::WorkNCCL::synchronizeStream() {
  auto currentStream = at::cuda::getCurrentCUDAStream(device_.index());
  // Block the current stream on the NCCL stream
  ncclEndEvent_->block(currentStream);

  if (avoidRecordStreams_) {
    stashed_for_allocator_safety_->clear();
  }
}

// Waiting on the work's corresponding CUDA events
void ProcessGroupNCCL::WorkNCCL::synchronizeInternal(
    std::chrono::milliseconds timeout) {
  synchronizeStream();

  // In case of blocking, wait for the operation to complete.
  if (blockingWait_) {
    while (!isCompleted()) {
      bool timedOut = checkTimeout(
          timeout == kNoTimeout ? c10::nullopt : c10::make_optional(timeout));
      // Explicitly abort ncclComms here before throwing this timed out
      // exception to users.
      // If throwing timed out excepiton without aborting nccl communicators
      // here, it was observed that CUDA GPU will have 100% utilization and
      // can not run new events successfully.
      if (timedOut) {
        std::string exceptionMsg = c10::str(
            logPrefix(),
            "Work ",
            (*this),
            " timed out in blocking wait (TORCH_NCCL_BLOCKING_WAIT=1).");
        LOG(ERROR) << exceptionMsg;
        break;
      }
      // Yield
      std::this_thread::sleep_for(
          std::chrono::milliseconds(kSynchronizeBusyWaitMillis));
    }
    // exception() includes timeout and error during blocking wait
    if (exception()) {
      // Abort NCCL communicators
      abort();
      // Throw exception (from main thread here)
      handleException(TearDown);
    }
  }

  // Device synchronize only after we've completed timeout checks.
  if (barrierTensor_.defined()) {
    // If we use the work to do barrier, we should block here
    // `dist.barrier()` only requires all CPU processes to enter this
    // function, hence we only need to make sure the dummy all-reduce has
    // completed. So we would only need to sync the **current stream** back to
    // host, and do not need to synchronize the entire device (which may have
    // kernels running on other streams).
    // Using `cudaStreamSynchronize` instead of `cudaDeviceSynchronize` can:
    // - lower chance of hang;
    // - CurrentCUDAStream is usually the context of the next operation in
    // Python, thus blocking current stream would already block the next
    // compute kernel;
    // - achieve better barrier performance.
    auto currentStream = at::cuda::getCurrentCUDAStream(device_.index());
    AT_CUDA_CHECK(cudaStreamSynchronize(currentStream));
  }
}

// Same as calling synchronize().
bool ProcessGroupNCCL::WorkNCCL::wait(std::chrono::milliseconds timeout) {
  RECORD_PARAM_COMMS(
      static_cast<int>(this->seq_), // seq
      0, // process group ptr
      rank_, // rank
      "wait", // colName
      0, // inNelems
      0, // outNelems
      at::kByte, // dType
      std::vector<int64_t>(), // inSplitSizes
      std::vector<int64_t>(), // outSplitSizes
      -1,
      -1,
      static_cast<int>(1)); // number of device?
  synchronizeInternal(timeout);
  // TODO(kwen2501): this should be moved to c10d tests, to qualify a NCCL
  // upgrade. Once a NCCL version is qualified, this code should not be needed
  // at runtime.
#ifdef PGNCCL_ENABLE_HASH
  if (distDebugLevel_ >= DebugLevel::Detail) {
    auto numel = getTensorsNumel(*outputs_);
    auto hashValue = hashTensors(*outputs_);
    PRINT_COLLECTIVE_HASH_SIGNATURE(
        "output", opTypeToString(opType_), numel, hashValue);
  }
#endif
  // Always return true, because abort API is not implemented.
  return true;
}

void ProcessGroupNCCL::WorkNCCL::abort() {
  // Abort all communicators of this work
  ncclComm_->ncclCommAbort();

  ncclCommDevIdxMapMutex.lock();
  ncclCommDevIdxMap.erase(ncclComm_);
  ncclCommDevIdxMapMutex.unlock();
}

static std::atomic<size_t> process_group_id = 0;

constexpr const char* MULTI_DEVICE_ERROR_MSG =
    "Expecting one tensor only but got multiple. You are probably using multiple "
    "devices under one thread. The support for such usage has been deprecated. "
    "For details, please refer to "
    "https://pytorch.org/docs/stable/distributed.html#multi-gpu-collective-functions. "
    "ProcessGroupNCCL continues supporting multi-process and multi-thread modes.";

ProcessGroupNCCL::ProcessGroupNCCL(
    const c10::intrusive_ptr<Store>& store,
    int rank,
    int size,
    c10::intrusive_ptr<Options> options)
    : Backend(rank, size),
      store_(store),
      options_(options),
      ncclCommCounter_(0),
      traceKeyStart_(getTraceStartKey("NCCL", rank)),
      traceKeyEnd_(getTraceEndKey("NCCL", rank)),
      terminateProcessGroup_(false),
      terminateHeartbeatMonitorThread_(false),
      collectiveDebugInfoMode_(false),
      uid_(process_group_id++),
      intraNodeComm_(initIntraNodeComm()) {
  TORCH_CHECK_WITH(
      ValueError,
      at::cuda::getNumGPUs() != 0,
      "ProcessGroupNCCL is only supported with GPUs, no GPUs found!");
  logPrefix_ = createLogPrefix();
  blockingWait_ = getCvarBool(TORCH_NCCL_BLOCKING_WAIT, false);
  abortInDestroyProcessGroup_ =
      getCvarBool(TORCH_NCCL_ABORT_IN_DESTROY_PG, false);
  asyncErrorHandling_ = static_cast<ErrorHandlingMode>(
      getCvarInt(TORCH_NCCL_ASYNC_ERROR_HANDLING, 3 /*SkipCleanUp*/));
  desyncDebug_ = getCvarBool(TORCH_NCCL_DESYNC_DEBUG, false) ||
      (dist_debug_level_ >= DebugLevel::Detail);
  dumpOnTimeout_ = getCvarBool(TORCH_NCCL_DUMP_ON_TIMEOUT, false) ||
      (dist_debug_level_ >= DebugLevel::Detail);
  heartbeat_ = 1ULL;
  monitorThreadEnabled_.store(getCvarBool(TORCH_NCCL_ENABLE_MONITORING, true));
  heartbeatTimeoutInSec_ =
      getCvarInt(TORCH_NCCL_HEARTBEAT_TIMEOUT_SEC, 60 * 10 /*10 Mins*/);
  waitTimeoutDumpInMilSec_ =
      getCvarInt(TORCH_NCCL_WAIT_TIMEOUT_DUMP_MILSEC, 60 * 1000 /*60 Sec*/);
  coordCheckIntervalMilSec_ = getCvarInt(TORCH_NCCL_COORD_CHECK_MILSEC, 1000);
  ncclTraceBufferSize_ = getCvarInt(TORCH_NCCL_TRACE_BUFFER_SIZE, 0);
  NCCLTraceBuffer::get()->record_pg_ranks(uid_, groupRanks());
  enableCollecticeHashDebug_ = (dist_debug_level_ >= DebugLevel::Detail);
  // store_ usually is wrapped with PrefixStore and the prefix is different
  // across different ProcessGroupNCCL(PG) instances. We need to get the
  // underlying non-PrefixStore for sharing global information shared across
  // different PGs.
  PrefixStore* prefixStore = dynamic_cast<PrefixStore*>(store_.get());
  globalStore_ =
      prefixStore ? prefixStore->getUnderlyingNonPrefixStore() : store_;
#ifdef ENABLE_NCCL_ERROR_CHECKING
  enableTiming_.store(
      getCvarBool(TORCH_NCCL_ENABLE_TIMING, false) || desyncDebug_);
#endif
  avoidRecordStreams_ = getCvarBool(TORCH_NCCL_AVOID_RECORD_STREAMS, false);
#ifdef NCCL_HAS_COMM_REGISTER
  useTensorRegisterAllocatorHook_ =
      getCvarBool(TORCH_NCCL_USE_TENSOR_REGISTER_ALLOCATOR_HOOK, false);
  if (c10::cuda::CUDACachingAllocator::CUDAAllocatorConfig::
          expandable_segments()) {
    useTensorRegisterAllocatorHook_ = false;
    LOG(INFO)
        << logPrefix()
        << "disables TORCH_NCCL_USE_TENSOR_REGISTER_ALLOCATOR_HOOK because it is not compatible with CUDA allocator expandable segments mode.";
  }
#endif

  if (blockingWait_) {
    if (asyncErrorHandling_ != NoHandling || desyncDebug_) {
      LOG(INFO)
          << logPrefix() << "TORCH_NCCL_BLOCKING_WAIT and "
          << "TORCH_NCCL_ASYNC_ERROR_HANDLING|TORCH_NCCL_DESYNC_DEBUG"
          << "should not both be enabled. "
          << "Only TORCH_NCCL_BLOCKING_WAIT is being used in this process.";
      asyncErrorHandling_ = NoHandling;
      desyncDebug_ = false;
    }
  } else {
    if (desyncDebug_ && asyncErrorHandling_ == NoHandling) {
      LOG(INFO)
          << logPrefix()
          << "TORCH_NCCL_DESYNC_DEBUG and TORCH_NCCL_ASYNC_ERROR_HANDLING "
          << "must both be enabled. "
          << "Enabling TORCH_NCCL_ASYNC_ERROR_HANDLING.";
      asyncErrorHandling_ = SkipCleanUp;
    }
  }

#ifdef ENABLE_NCCL_ERROR_CHECKING
  ncclCommWatchdogThread_ =
      std::thread(&ProcessGroupNCCL::ncclCommWatchdog, this);
#endif

  init();
  const std::string OFF = "OFF";
  std::string torch_distributed_debug =
      getCvarString({"TORCH_DISTRIBUTED_DEBUG"}, OFF.c_str());
  LOG(INFO) << logPrefix() << "ProcessGroupNCCL initialization options: "
            << "NCCL version: " << getNcclVersion() << ", size: " << size
            << ", global rank: " << globalRank()
            << ", TORCH_NCCL_ASYNC_ERROR_HANDLING: " << asyncErrorHandling_
            << ", TORCH_NCCL_DUMP_ON_TIMEOUT: " << dumpOnTimeout_
            << ", TORCH_NCCL_WAIT_TIMEOUT_DUMP_MILSEC: "
            << waitTimeoutDumpInMilSec_
            << ", TORCH_NCCL_DESYNC_DEBUG: " << desyncDebug_
            << ", TORCH_NCCL_ENABLE_TIMING: " << enableTiming_.load()
            << ", TORCH_NCCL_BLOCKING_WAIT: " << blockingWait_
            << ", TIMEOUT(ms): " << options_->timeout.count()
            << ", USE_HIGH_PRIORITY_STREAM: "
            << options_->is_high_priority_stream
            << ", SPLIT_FROM: " << options_->split_from
            << ", SPLIT_COLOR: " << options_->split_color
            << ", TORCH_DISTRIBUTED_DEBUG: " << torch_distributed_debug
#ifdef NCCL_HAS_COMM_REGISTER
            << ", TORCH_NCCL_USE_TENSOR_REGISTER_ALLOCATOR_HOOK: "
            << useTensorRegisterAllocatorHook_
#endif
            << ", TORCH_NCCL_ENABLE_MONITORING: "
            << monitorThreadEnabled_.load()
            << ", TORCH_NCCL_HEARTBEAT_TIMEOUT_SEC: " << heartbeatTimeoutInSec_
            << ", TORCH_NCCL_TRACE_BUFFER_SIZE: " << ncclTraceBufferSize_
            << ", TORCH_NCCL_COORD_CHECK_MILSEC: " << coordCheckIntervalMilSec_
            << ", ID=" << this->getID();

  if (options_->global_ranks_in_group.empty()) {
    this->globalRankStart = 0;
  } else {
    this->globalRankStart = options_->global_ranks_in_group[0];
  }

  if (options_->global_ranks_in_group.empty()) {
    this->globalRankStride = 1;
  } else if (options_->global_ranks_in_group.size() == 1) {
    this->globalRankStride = 0;
  } else {
    bool ranksAreStrided = true;
    int startRank = options_->global_ranks_in_group[0];
    int stride =
        options_->global_ranks_in_group[1] - options_->global_ranks_in_group[0];
    for (std::vector<uint64_t>::size_type i = 0;
         i < options_->global_ranks_in_group.size();
         i++) {
      if (options_->global_ranks_in_group[i] != startRank + i * stride) {
        ranksAreStrided = false;
        break;
      }
    }

    if (ranksAreStrided) {
      this->globalRankStride = options_->global_ranks_in_group[1] -
          options_->global_ranks_in_group[0];
    } else {
      this->globalRankStride = -1;
    }
  }

  RECORD_PARAM_COMMS(
      0, // seq
      this->getID(),
      rank, // rank
      "init", // colName
      0, // inNelems
      0, // outNelems
      at::kByte, // dType
      std::vector<int64_t>(), // inSplitSizes
      std::vector<int64_t>(), // outSplitSizes
      globalRankStart, // globalRankStart
      globalRankStride, // globalRankStride
      size_); // worldSize

  // Attach hooks to cache allocator to trigger the hooks whenever a traced
  // action is called. In the following hooks, we register a newly allocated
  // segment when SEGMENT_ALLOC action occurs, and deregister a segment when
  // SEGMENT_FREE action occurs.
  // We attach hooks only once at the first PG creation.
  // Attaching hooks fails if CUDACachingAllocator is not initialized, so
  // lazyInitCUDA is called (and is a no-op if CUDA is already initialized).
  if (useTensorRegisterAllocatorHook_ && !allocatorHooksAttached) {
    at::globalContext().lazyInitCUDA();
    c10::cuda::CUDACachingAllocator::attachAllocatorTraceTracker(
        &cacheAllocatorRegisterHook);
    c10::cuda::CUDACachingAllocator::attachAllocatorTraceTracker(
        &cacheAllocatorDeregisterHook);
    allocatorHooksAttached = true;
  }
}

void ProcessGroupNCCL::eagerConnectSingleDevice(at::Device device) {
  const auto key = getKeyFromDevice(device);
  LOG(INFO) << logPrefix() << "Eagerly connecting nccl backend with device "
            << device;
  getNCCLComm(key, device, OpType::ALLREDUCE);
}

void ProcessGroupNCCL::performNocolorSplit(at::Device device) {
  // If our backend doesn't support splitting, this is a no-op for
  // ranks not in the new subgroup (and ranks that would be in it will
  // just use a new communicator rather than split).
#ifdef NCCL_HAS_COMM_SPLIT
  const auto key = getKeyFromDevice(device);
  LOG(INFO) << logPrefix() << "Performing nocolor split on backend device "
            << device << ", key " << key << ", i am " << this;
  auto comm = getNCCLComm(key, device, OpType::ALLREDUCE);
  NCCLComm::split(comm.get(), NCCL_SPLIT_NOCOLOR, rank_, options_->config);
#endif
}

c10::intrusive_ptr<intra_node_comm::IntraNodeComm> ProcessGroupNCCL::
    initIntraNodeComm() {
  return intra_node_comm::IntraNodeComm::rendezvous(
      store_, std::to_string(uid_), rank_, size_);
}

void ProcessGroupNCCL::setSequenceNumberForGroup() {
} // NCCL just starts sequence numbers at 0.

uint64_t ProcessGroupNCCL::getSequenceNumberForGroup() {
  return seq_;
}

void ProcessGroupNCCL::registerOnCompletionHook(
    std::function<void(std::shared_ptr<WorkInfo>)>&& hook) {
  TORCH_CHECK_WITH(
      DistBackendError,
      onCompletionHook_ == nullptr,
      "ProcessGroupNCCL OnCompletion hook already registered");

  TORCH_CHECK_WITH(
      ValueError,
      enableTiming_.load(),
      "ProcessGroupNCCL OnCompletion hook requires recording start and end "
      "events which require setting TORCH_NCCL_ENABLE_TIMING environment variable. "
      "This is only available for NCCL version >= 2.4.");
  onCompletionHook_ = std::move(hook);
  onCompletionHookThread_ = std::thread(&ProcessGroupNCCL::runHookLoop, this);
}

// must release GIL when calling this method
void ProcessGroupNCCL::waitForPendingWorks() {
  // Reasoning about hook completion:
  // 1. waitForPendingWorks should be called after user code has finished
  // calling
  //    all collectives. This means, when we got here, all of the collectives
  //    are either in workMetaList_ or has been erased from workMetaList_.
  // 2. The watchdog thread grabs both locks to move Work object from the
  //    workMetaList_ to the completedWorkList_, and the hook thread only erases
  //    a Work object after the hook is returned. Therefore, after user code
  //    calls a collective, its Work object is either in workMetaList_ or in
  //    completedWorkList_ before it finishes.
  // 3. We have three threads and two locks.
  //      a. main thread (this function) grabs two locks atomically
  //      b. watchdog thread (watchdogHandler function) always grabs
  //      workMetaListMutex_
  //         first and then grabs completedWorkListMutex_.
  //      c. hook thread (runHookLoop function) only grabs
  //      completedWorkListMutex_. Therefore, locks are always acquired in the
  //      same order and hence no deadlocks.
  while (true) {
    {
      std::lock(workMetaListMutex_, completedWorkListMutex_);
      std::lock_guard<std::mutex> lockWork(workMetaListMutex_, std::adopt_lock);
      std::lock_guard<std::mutex> lockHook(
          completedWorkListMutex_, std::adopt_lock);

      if (workMetaList_.empty() && completedWorkList_.empty()) {
        return;
      }
    }

    std::this_thread::sleep_for(
        std::chrono::milliseconds(kWatchdogThreadSleepMillis));
  }
}

void ProcessGroupNCCL::enableCollectivesTiming() {
  enableTiming_.store(true);
}

void ProcessGroupNCCL::waitForFutureOrTimeout(
    std::future<bool>& fut,
    const std::chrono::milliseconds& timeOutMilSec,
    const std::string& futDescription,
    bool throwException) {
  std::string errorMsg;
  TORCH_CHECK(fut.valid(), "Expected a valid future");
  std::future_status status = fut.wait_for(timeOutMilSec);
  if (status == std::future_status::ready) {
    // Calling .get() will re-raise any exception from the future, and we don't
    // care about the retval
    try {
      bool result = fut.get();
      if (result) {
        LOG(INFO) << logPrefix()
                  << "future is successfully executed for: " << futDescription;
      }
    } catch (const std::exception& e) {
      errorMsg = c10::str(
          logPrefix(),
          "Exception thrown when waitng for future ",
          futDescription,
          ": ",
          e.what());
      LOG(ERROR) << errorMsg;
    } catch (...) {
      errorMsg = c10::str(
          logPrefix(),
          "Unknown exception thrown when waitng for future ",
          futDescription);
      LOG(ERROR) << errorMsg;
    }
  } else {
    errorMsg = c10::str(
        logPrefix(),
        "Future for ",
        futDescription,
        " timed out after ",
        timeOutMilSec.count(),
        " ms");
    LOG(ERROR) << errorMsg;
  }
  if (throwException && !errorMsg.empty()) {
    C10_THROW_ERROR(DistBackendError, errorMsg);
  }
}

void ProcessGroupNCCL::abortCommsFromMap(
    std::unordered_map<std::string, std::shared_ptr<NCCLComm>>& ncclCommsMap,
    c10::optional<std::string> abortReason) {
  // The process may control multiple devices, loop through the communicators on
  // each device
  for (auto& it : ncclCommsMap) {
    auto& devName = it.first;
    auto& ncclComm = it.second;

    LOG(INFO) << logPrefix() << "ProcessGroupNCCL destroying ncclComm_ "
              << ncclComm->ncclComm_ << " on CUDA device: " << devName;
    ncclComm->ncclCommAbort(abortReason);
    // Note that we don't remove the aborted communicators from the
    // cache. The reason is that if we do remove the communicator
    // from the cache, it is possible that a new collective operation
    // calls `ncclCommInitRank` to create a new communicator whereas
    // other ranks might have failed/timed out and didn't enter
    // `ncclCommInitRank`. As a result, when there is a failure on
    // a communicator the application receives an exception and its
    // their responsibility to destroy the process group and recreate
    // it to recover from errors.

    c10::StreamId streamId = -1;
    if (ncclStreams_.find(devName) != ncclStreams_.end()) {
      auto stream = ncclStreams_.at(devName);
      streamId = stream.id();
    }

    LOG(INFO) << logPrefix() << "ProcessGroupNCCL destroyed "
              << " communicator on CUDA device: " << devName
              << " with stream: " << streamId;
  }
}

// Abort all communicators on this rank
bool ProcessGroupNCCL::abort(c10::optional<std::string> abortReason) {
  // Remove record from global ncclCommDevIdxMapMutex before aboarting,
  // so that a new cache segment would not register to already aborded
  // communicators. Note that ncclCommDevIdxMap is a global container which may
  // contain other PG's communicators, thus we need to only erase communicators
  // for the current PG.
  ncclCommDevIdxMapMutex.lock();
  for (auto& it : devNCCLCommMap_) {
    auto& ncclComm = it.second;
    ncclCommDevIdxMap.erase(ncclComm);
  }
  ncclCommDevIdxMapMutex.unlock();

  std::lock_guard<std::mutex> lock(mutex_);
  abortCommsFromMap(devNCCLCommMap_, abortReason);
  abortCommsFromMap(inInitializationCommMap_, abortReason);
  return true;
}

void ProcessGroupNCCL::shutdown() {
  // Don't join threads here since the purpose of this method is to abort all
  // communicators and signal the threads to exit. Joining on the threads could
  // potentially block and hence avoid it in this method.
  terminateProcessGroup_.store(true);
  workMetaListCV_.notify_one();

  std::string abortReason = c10::str("Process Group shutdown on rank ", rank_);
  // lauch abort asynchrounously and wait for it to complete or timeout
  LOG(INFO) << logPrefix()
            << "Launching ProcessGroupNCCL abort asynchrounously.";
  std::future<bool> fut = std::async(std::launch::async, [this, abortReason]() {
    return this->abort(abortReason);
  });

  waitForFutureOrTimeout(fut, options_->timeout, "ProcessGroup abort", true);
  LOG(INFO) << logPrefix() << "ProcessGroupNCCL aborts successfully.";

  // We need to wait for abort to finish before we can safely shut down
  // heartbeat monitoring thread.
  terminateHeartbeatMonitorThread_.store(true);
  monitorWakeUpCV_.notify_one();
}

ProcessGroupNCCL::~ProcessGroupNCCL() {
  LOG(INFO) << logPrefix() << "ProcessGroupNCCL destructor entered.";

  if (!terminateProcessGroup_.load()) {
    // Only if TORCH_NCCL_ABORT_IN_DESTROY_PG is enabled, terminateProcessGroup_
    // will be set to true through destroy_process_group
    if (abortInDestroyProcessGroup_) {
      LOG(WARNING) << c10::str(
          "WARNING: process group has NOT been destroyed before it is being destructed. ",
          "On normal program exit, the application should call destroy_process_group to ",
          "ensure that any pending NCCL data transfers have finished in this process. "
          "In rare cases this process can exit before this point and block the progress of "
          "another member of the process group. This constraint has always been present, "
          " but this warning has only been added since PyTorch 2.3");
    }
    // If user haven't explicitly destroy/shutdown process group, destructor
    // needs to do so
    shutdown();
  }

  // Wait for all threads to finish before returning
#ifdef ENABLE_NCCL_ERROR_CHECKING
  if (ncclCommWatchdogThread_.joinable()) {
    ncclCommWatchdogThread_.join();
    LOG(INFO) << logPrefix() << "ProcessGroupNCCL watchdog thread joined.";
  }
  if (ncclHeartbeatMonitorThread_.joinable()) {
    ncclHeartbeatMonitorThread_.join();
    LOG(INFO) << logPrefix()
              << "ProcessGroupNCCL heart beat monitor thread joined.";
  }
#endif
  if (onCompletionHookThread_.joinable()) {
    onCompletionHookThread_.join();
    LOG(INFO) << logPrefix()
              << "ProcessGroupNCCL onCompletionHookThread thread joined.";
  }
}

bool ProcessGroupNCCL::dumpDebuggingInfo() {
  // Serialize all calls to this function to avoid corrupting data, but allow
  // multiple calls in one runtime. User is responsible for preserving the
  // output file from an earlier call before a later call overwrites it.
  static std::mutex writeDebugInfoMutex;
  std::lock_guard<std::mutex> lock(writeDebugInfoMutex);
  LOG(ERROR) << logPrefix() << "ProcessGroupNCCL preparing to dump debug info.";
  if (ncclTraceBufferSize_ > 0) {
    // We dump nccl trace into local disk by default and users can register
    // their customized writer by inheriting `DebugInfoWriter` via
    // `registerDebugInfoWriter`.
    auto ncclTrace = dump_nccl_trace();
    DebugInfoWriter& writer = DebugInfoWriter::getWriter(globalRank());
    writer.write(ncclTrace);
    return true;
  }
  return false;
}

void ProcessGroupNCCL::terminateProcess(std::string errMsg) {
  // Logging with `FATAL`, after errMsg printed, it calls `std::abort()`
  // to terminate the program execution.
  LOG(FATAL) << logPrefix() << errMsg;
}

int computeDeltaMS(
    std::chrono::time_point<std::chrono::steady_clock> start,
    std::chrono::time_point<std::chrono::steady_clock> end) {
  return std::chrono::duration_cast<std::chrono::milliseconds>(end - start)
      .count();
}

void ProcessGroupNCCL::heartbeatMonitor() {
  uint64_t heartBeatCounter = 0ULL;
  std::string errorMsg;
  std::string exitMsg;
  bool checkTimeoutSignal = (dumpOnTimeout_ && uid_ == 0);
  int monitorPollInterval = checkTimeoutSignal ? coordCheckIntervalMilSec_
                                               : heartbeatTimeoutInSec_ * 1000;
  auto lastTimePollStore = std::chrono::steady_clock::now();
  auto lastTimeHeartBeatCheck = std::chrono::steady_clock::now();
  while (true) {
    // This won't have any lock since this lock is only used here.
    // Please be aware that mutex `monitorMutex_` should not be used
    // somewhere else to avoid the deadlock.
    std::unique_lock<std::mutex> lock(monitorMutex_);
    if (monitorWakeUpCV_.wait_for(
            lock, std::chrono::milliseconds(monitorPollInterval), [&] {
              return terminateHeartbeatMonitorThread_.load();
            })) {
      // For the normal complete or user interception, monitorWakeUpCV_
      // will get notified, we early return and exit heartbeatMonitor.
      return;
    }
    auto currentTime = std::chrono::steady_clock::now();

    // We put extra functionality in the thread for the default PG (aka, uid_=0)
    // because the signal is same across different PGs. We only need to run
    // once per process to avoid duplicate things performed in too many separate
    // threads. For example, we check a global flag on the TCPStore periodically
    // to see if any PG on any rank observed a timeout and signaled peers to
    // dump debugging info, and we avoid hammering the TCPStore from all PGs on
    // the same rank.
    if (checkTimeoutSignal) {
      // We poll store to see if some ranks have flagged a timeout when
      // we haven't polled for `heartbeat_timeout` seconds and there haven't
      // any work added or removed for `watchdog_timeout` seconds.
      if (computeDeltaMS(lastWorkListUpdateTime_, currentTime) >=
              kWatchdogThreadSleepMillis &&
          computeDeltaMS(lastTimePollStore, currentTime) >=
              coordCheckIntervalMilSec_) {
        lastTimePollStore = currentTime;
        if (globalStore_->check({std::string(TIMEOUT_DUMP)})) {
          errorMsg = c10::str(
              logPrefix(),
              "Received a global timeout from another rank and will ",
              "start to dump the debug info. ",
              "Last enqueued NCCL work: ",
              lastEnqueuedSeq_,
              ", last completed NCCL work: ",
              lastCompletedSeq_,
              ".");
          exitMsg = c10::str(
              "ProcessGroupNCCL's watchdog detected a collective timeout and notified current rank. ",
              "This is most likely caused by incorrect usages of collectives, e.g., wrong ",
              "sizes used across ranks, the order of collectives is not same for all ranks ",
              "or the scheduled collective, for some reason, didn't run. Additionally, ",
              "this can be caused by GIL deadlock or other reasons such as network errors or ",
              "bugs in the communications library (e.g. NCCL), etc. We tried our best to ",
              "dump the debug info into the storage to help you debug the issue.");
          break;
        }
      }
    }

    if (computeDeltaMS(lastTimeHeartBeatCheck, currentTime) >=
        heartbeatTimeoutInSec_ * 1000) {
      // Check the heart beat of watchdog thread.
      lastTimeHeartBeatCheck = currentTime;
      auto heartbeat = heartbeat_.load();
      if (heartbeat != heartBeatCounter) {
        heartBeatCounter = heartbeat;
      } else {
        // No heartbeat increase detected and timeout.
        errorMsg = c10::str(
            logPrefix(),
            "Heartbeat monitor timed out! Process will be terminated after dumping debug info.",
            " workMetaList_.size()=",
            workMetaList_.size());
        exitMsg = c10::str(
            "ProcessGroupNCCL's watchdog got stuck for ",
            heartbeatTimeoutInSec_,
            " seconds without making progress in monitoring enqueued collectives. ",
            "This typically indicates a NCCL/CUDA API hang blocking the watchdog, ",
            "and could be triggered by another thread holding the GIL inside a ",
            "CUDA api, or other deadlock-prone behaviors.",
            "If you suspect the watchdog is not actually stuck and a longer timeout would help, ",
            "you can either increase the timeout (TORCH_NCCL_HEARTBEAT_TIMEOUT_SEC) to a larger value "
            "or disable the heartbeat monitor (TORCH_NCCL_ENABLE_MONITORING=0)."
            "If either of aforementioned helps, feel free to file an issue to PyTorch about the short timeout "
            "or false positive abort; otherwise, please attempt to debug the hang. "
            "workMetaList_.size() = ",
            workMetaList_.size(),
            "");
        break;
      }
    }
  }
  LOG(ERROR) << errorMsg;

  auto& cpp_dumper = get_cpp_trace_dumper();
  if (cpp_dumper.has_value()) {
    LOG(INFO) << "Dumping c++ stacktraces: " << cpp_dumper.value()();
  }

  // Store debug info to storage if no other thread does it. (By default to
  // local disk)
  std::future<bool> asyncDebugDump = std::async(
      std::launch::async, [this]() { return this->dumpDebuggingInfo(); });

  // wait for the dump until timeout
  waitForFutureOrTimeout(
      asyncDebugDump,
      std::chrono::milliseconds(waitTimeoutDumpInMilSec_),
      "Flight recorder dump in heartbeatMonitor");

  if (get_gil_checker() != nullptr) {
    auto fut = launchAsyncGilCheck();
    auto kGilCheckTimeout = std::chrono::milliseconds(300);
    auto futStatus = fut.wait_for(kGilCheckTimeout);
    if (futStatus != std::future_status::ready) {
      TORCH_CHECK(
          futStatus != std::future_status::deferred,
          "Expected the future to have been launched eagerly.");
      LOG(ERROR)
          << "Could not acquire GIL within 300 ms on exit, possible GIL induced hang";
    }
    LOG(INFO) << "Could acquire GIL on exit";
  } else {
    LOG(INFO)
        << "GIL checker was not registered, perhaps this is a no-python build?";
  }

  // There are two possible cases for the watchdog thread exit:
  // Case one: desync report runs quickly, and it follows the step:
  // collective timeout -> desync -> exception handling -> destructors
  // -> set terminateHeartbeatMonitorThread_ -> notify monitorWakeUpCV_.
  // So the code either early returns above or will skip the sleep below.
  // Case two: desync might be slow or get stuck. Or we get stuck in
  // destructors, we will sleep for some time before calling std::abort() to
  // kill the whole process.
  if ((terminateProcessGroup_.load() || collectiveDebugInfoMode_.load()) &&
      !terminateHeartbeatMonitorThread_.load()) {
    // Leave another two mins for desync report generation or process group
    // destroy.
    std::this_thread::sleep_for(std::chrono::seconds(heartbeatTimeoutInSec_));
  }

  // At this point, we either already sleep for another `heartbeatTimeoutInSec_`
  // or the thread has finished. Because we don't want to block the monitor
  // thread, so We mark the thread detach and the dump of debug info becomes
  // "best effort". If the process exit normally, marking it detach also makes
  // sense because we don't really care about dumping the debug info.

  // We already log completion inside the thread, so it may not be necessary to
  // check the return value here.  We mainly use a future so we can exit early
  // if done.

  if (!terminateHeartbeatMonitorThread_.load()) {
    // Create a error message reported from MonitorThread, so
    // we throw exception and make the whole process to be killed.
    // TODO(fduwjj): After having a hang debug wiki, we need to update the wiki
    // url here.
    const auto finalExitMsg = c10::str(logPrefix(), exitMsg);
    if (monitorThreadEnabled_.load()) {
      terminateProcess(finalExitMsg);
    } else {
      LOG(ERROR)
          << "PGNCCL Monitor Thread is disabled, but would have killed this job:\n"
          << finalExitMsg;
    }
  }
}

void ProcessGroupNCCL::ncclCommWatchdog() {
  try {
    VLOG(2) << logPrefix() << "Process group watchdog thread started!";
    ncclHeartbeatMonitorThread_ =
        std::thread(&ProcessGroupNCCL::heartbeatMonitor, this);
    watchdogHandler();
    VLOG(2) << logPrefix()
            << "Process group watchdog thread terminated normally";
  } catch (std::exception& e) {
    if (std::string(e.what()).find("driver shutting down") !=
        std::string::npos) {
      LOG(INFO)
          << logPrefix()
          << "main process destroyed cuda before watchdog loop exited, terminating watchdog."
          << " (Watchdog caught exception: " << e.what();

    } else {
      // Append error message reported from watchdogHandler
      const auto exitMsg = c10::str(
          logPrefix(),
          "Process group watchdog thread terminated with exception: ",
          e.what());
      LOG(ERROR) << exitMsg;
      // TODO(whc) clean up the rethrow - why is it stored in a class var and
      // rethrown?
      watchDogException_ =
          std::make_exception_ptr(C10_BUILD_ERROR(DistBackendError, exitMsg));
      std::rethrow_exception(watchDogException_);
    }
  } catch (...) {
    const auto exitMsg = c10::str(
        logPrefix(),
        "Process group watchdog thread terminated with exception: unknown");
    LOG(ERROR) << exitMsg;
    watchDogException_ =
        std::make_exception_ptr(C10_BUILD_ERROR(DistBackendError, exitMsg));
    std::rethrow_exception(watchDogException_);
  }
}

void ProcessGroupNCCL::logWorkStart(WorkNCCL& work) {
  if (work.startTraceUpdated_)
    return;

  if (terminateProcessGroup_.load() || storeError_)
    return;

  work.startTraceUpdated_ = true;
  storeError_ = !c10d::traceUpdate(
      store_, traceKeyStart_, work.seq_, opTypeToString(work.opType_));
}

void ProcessGroupNCCL::logWorkEnd(WorkNCCL& work) {
  if (terminateProcessGroup_.load() || storeError_)
    return;

  // In case the start of the work hasn't been logged
  if (!work.startTraceUpdated_) {
    logWorkStart(work);
  }

  storeError_ = !c10d::traceUpdate(
      store_, traceKeyEnd_, work.seq_, opTypeToString(work.opType_));
}

std::string ProcessGroupNCCL::getNCCLWatchdogDebugInfo() {
  return retrieveDesyncReport(store_, "NCCL", rank_, size_);
}

#if defined(__linux__)
struct DumpPipe {
  DumpPipe(int rank) {
    std::string fileStem =
        getCvarString({"TORCH_NCCL_DEBUG_INFO_PIPE_FILE"}, "");
    if (fileStem.empty() ||
        getCvarInt({"TORCH_NCCL_TRACE_BUFFER_SIZE"}, 0) <= 0) {
      return;
    }
    TORCH_CHECK(!fileStem.empty(), "TORCH_NCCL_DEBUG_INFO_TEMP_FILE is empty");
    std::string filename = c10::str(fileStem, rank, ".pipe");
    TORCH_CHECK(
        unlink(filename.c_str()) != -1 || errno == ENOENT,
        "Error removing existing named pipe ",
        filename);
    TORCH_CHECK(
        mkfifo(filename.c_str(), 0666) != -1,
        "Error creating named pipe ",
        filename);
    fd_ = open(filename.c_str(), O_RDONLY | O_NONBLOCK);
    LOG(INFO) << "Pipe file " << filename
              << " has been opened, write to it to trigger NCCL Debug Dump.";
    TORCH_CHECK(fd_ != -1, "Error opening named pipe ", filename);
  }
  bool shouldDump() {
    if (fd_ == -1) {
      return false;
    }
    char buf[128];
    // non-blocking from O_NONBLOCK above.
    // Ignore EINTR because we already will poll this
    // again later.
    ssize_t bytesRead = read(fd_, &buf, 128);
    return bytesRead > 0;
  }
  ~DumpPipe() {
    if (fd_ != -1) {
      close(fd_);
    }
  }

 private:
  int fd_ = -1;
};
#else
struct DumpPipe {
  DumpPipe(int rank) {}
  bool shouldDump() {
    return false;
  }
};
#endif

std::string ProcessGroupNCCL::createLogPrefix() const {
  return c10::str("[PG ", uid_, " Rank ", rank_, "] ");
}

const std::string& ProcessGroupNCCL::logPrefix() const {
  return logPrefix_;
}

const int& ProcessGroupNCCL::globalRank() const {
  static int globalRank = rank_;
  return globalRank;
}

const std::vector<uint64_t>& ProcessGroupNCCL::groupRanks() const {
  if (options_->global_ranks_in_group.empty() && uid_ == 0) {
    static std::vector<uint64_t> globalRanks(size_);
    std::iota(globalRanks.begin(), globalRanks.end(), 0);
    return globalRanks;
  }
  return options_->global_ranks_in_group;
}

void ProcessGroupNCCL::watchdogHandler() {
  bool done = false;
  lastWorkListUpdateTime_ = std::chrono::steady_clock::now();
  c10::optional<std::future<bool>> optAsyncDebugDump;

  std::list<ProcessGroupNCCL::WorkNCCL> completedWorkList;

  c10::optional<DumpPipe> dumpPipe = c10::nullopt;
  if (uid_ == 0) {
    // DumpPipe is one per-trainer process, and its convenient to name them
    // after 'global' ranks in the system, So we assume processgroup (uid)==0 is
    // the global PG and has globally unique rank ids across trainers.
    dumpPipe.emplace(rank_);
  }
  while (!done || !terminateProcessGroup_.load()) {
    std::unique_lock<std::mutex> lock(workMetaListMutex_);
    // We busy-poll the work vector every kWatchdogThreadSleepMillis
    // milliseconds as long as the atomic is True.
    workMetaListCV_.wait_for(
        lock,
        std::chrono::milliseconds(kWatchdogThreadSleepMillis),
        [&]() -> bool { return terminateProcessGroup_.load(); });
    // Bump up heart beat by one.
    heartbeat_++;

// Some versions of GLOG support less-spammy version of LOG_EVERY_MS
// in which case we don't want to spam the logs.
#ifdef LOG_EVERY_MS
    // Log the progress of this PG periodically
    C10_LOG_EVERY_MS(INFO, kWorkStatusUpdatePeriodMs) << c10::str(
        logPrefix(),
        "NCCL Work update periodically: ",
        "last enqueued NCCL work: ",
        lastEnqueuedSeq_,
        ", last completed NCCL work: ",
        lastCompletedSeq_,
        ".");
#endif

    for (auto it = workMetaList_.begin(); it != workMetaList_.end();
         /* no increment */) {
      auto& work = *it;
      // When terminateProcessGroup_ is true, communicators have already been
      // aborted, So cannot check exception based on them. But watchdog needs to
      // finish the check for the works that have already been enqueued to
      // workMetaList_
      if (!terminateProcessGroup_.load()) {
        work.checkAndSetException();
      }
      bool timedOut = work.checkTimeout();

      // If work hits an exception (either an error or timeout)
      if (work.exception()) {
        if (SHOULD_CLEAN_UP(asyncErrorHandling_)) {
          // Abort work and corresponding communicators
          work.abort();
          // PG level abort, which would abort all other communicators on this
          // rank
          abort();
        }

        // Report desync state in case of timeout
        if (timedOut) {
          LOG(ERROR) << c10::str(
              logPrefix(),
              "Timeout at NCCL work: ",
              work.seq_,
              ", last enqueued NCCL work: ",
              lastEnqueuedSeq_,
              ", last completed NCCL work: ",
              lastCompletedSeq_,
              ".");
          try {
            if (desyncDebug_ || dumpOnTimeout_) {
              // Set shutdown mode, so the heartbeat monitor thread will not
              // abort process immediately.
              collectiveDebugInfoMode_.store(true);
              std::vector<uint8_t> vec(1);
              globalStore_->set(std::string(TIMEOUT_DUMP), vec);
            }

            if (dumpOnTimeout_ && !optAsyncDebugDump) {
              // Store debug info to storage. (By default to local disk)
              optAsyncDebugDump = std::async(std::launch::async, [this]() {
                return this->dumpDebuggingInfo();
              });
              // wait for the dump until timeout
              waitForFutureOrTimeout(
                  *optAsyncDebugDump,
                  std::chrono::milliseconds(waitTimeoutDumpInMilSec_),
                  "Flight recorder dump in watchdog");
              // This sleep is used to give additional time for other ranks to
              // receive the dump signal and then dump flight records, the same
              // sleep time used in the heartbeatmonitor thread
              // TODO, we should probably dump from a single thread in the
              // future
              std::this_thread::sleep_for(
                  std::chrono::seconds(heartbeatTimeoutInSec_));
            }

            if (desyncDebug_) {
              auto desyncMsg = getNCCLWatchdogDebugInfo();
              LOG(ERROR) << logPrefix() << desyncMsg;
            }
          } catch (const std::exception& e) {
            LOG(ERROR) << logPrefix()
                       << "Failed to retrieve TORCH_NCCL_DESYNC_DEBUG report. "
                       << " Please file an issue. Error: " << e.what();
          } catch (...) {
            LOG(ERROR)
                << logPrefix()
                << "Failed to rerieve TORCH_NCCL_DESYNC_DEBUG report with unknown error."
                << " Please file an issue.";
          }
        }
        // Throw exception
        work.handleException(asyncErrorHandling_);
      }

      // Work status logging for desync debug
      if (desyncDebug_) {
        if (work.isStarted()) {
          logWorkStart(work);
        }
        if (work.isCompleted()) {
          logWorkEnd(work);
        }
      }

      // Clean up completed work
      if (work.isCompleted()) {
        lastCompletedSeq_ = work.seq_;
        NCCLTraceBuffer::get()->retire_id(work.trace_id_, true);
        if (onCompletionHook_) {
          // Move Work object to completedWorkList_ to be consumed by the hook
          // thread
          {
            const std::lock_guard<std::mutex> lock(completedWorkListMutex_);
            completedWorkList_.splice(
                completedWorkList_.end(), workMetaList_, it++);
          }
          completedWorkListCV_.notify_one();
        } else {
          it = workMetaList_.erase(it);
          lastWorkListUpdateTime_ = std::chrono::steady_clock::now();
        }
        at::cuda::CUDAGraph::dec_pending_event_queries();
      } else {
        // Increment the iterator if the current WorkNCCL object is not
        // completed.
        ++it;
      }
      // Increment heartbeat after each work processed,
      // in case processing is slowed down (but not hung) by cuda api contention
      heartbeat_++;
    }
    // process a request to dump the trace. only PG uid 0 will respond to dump
    // requests, but this is fine since all PG's feed into the same flight
    // recorder and dump.
    if (dumpPipe.has_value() && dumpPipe->shouldDump()) {
      std::future<bool> fut = std::async(
          std::launch::async, [this]() { return this->dumpDebuggingInfo(); });
      // best effort dump, watchdog is not waiting for the dump here
    }
    done = workMetaList_.empty();
  }
}

void ProcessGroupNCCL::runHookLoop() {
  bool done = false;
  while (!done || !terminateProcessGroup_.load()) {
    std::unique_lock<std::mutex> lock(completedWorkListMutex_);
    // We busy-poll the work vector every kWatchdogThreadSleepMillis
    // milliseconds as long as the atomic is True.
    completedWorkListCV_.wait_for(
        lock,
        std::chrono::milliseconds(kWatchdogThreadSleepMillis),
        [&]() -> bool {
          return !completedWorkList_.empty() || terminateProcessGroup_.load();
        });

    try {
      for (auto it = completedWorkList_.begin(); it != completedWorkList_.end();
           /* no increment */) {
        const WorkNCCL& work = *it;
        // Hook might grab GIL, unlock first to prevent deadlock
        lock.unlock();

        auto timeStarted =
            std::chrono::system_clock::now() +
            std::chrono::duration_cast<std::chrono::system_clock::duration>(
                work.workStartTime_ - std::chrono::steady_clock::now());
        onCompletionHook_(std::make_shared<WorkInfo>(
            work.retrieveOpType(), // OpType
            work.getSequencenumber(), // seq
            timeStarted, // timeStarted
            std::chrono::system_clock::now(), // timeFinished
            std::chrono::duration<float, std::milli>(
                work.getDuration()) // activeDuration
            ));

        lock.lock();
        it = completedWorkList_.erase(it);
      }
    } catch (std::exception& e) {
      if (std::string(e.what()).find("driver shutting down") !=
          std::string::npos) {
        LOG(INFO)
            << logPrefix()
            << "main process destroyed cuda before runHookLoop exited, terminating runHookLoop."
            << " (runHookLoop caught exception: " << e.what();

      } else {
        // PythonOnCompletionHook has already extracted Python exception message
        // and wrapped it with a cpp one. So we no longer need to acquire GIL
        // here.
        const auto errorStr = c10::str(
            "Caught exception on rank ",
            rank_,
            " while running onCompletion hook for ProcessGroupNCCL: ",
            e.what(),
            ". Aborting all communicators.");

        // No need to call abort() on WorkNCCL here as that collective has
        // already finished successfully at this point. We just need to abort
        // the process Abort all NCCL Communicators on this ProcessGroupNCCL
        // instance.
        abort(errorStr);
      }
    }

    // Lock is still acquired at this point
    done = completedWorkList_.empty();
  }
}

std::exception_ptr ProcessGroupNCCL::WorkNCCL::checkForNCCLErrors() {
  return checkForNCCLErrorsInternal(ncclComm_);
}

std::exception_ptr ProcessGroupNCCL::checkForNCCLErrors(
    std::shared_ptr<NCCLComm>& ncclComm) {
  return checkForNCCLErrorsInternal(ncclComm);
}

std::exception_ptr ProcessGroupNCCL::checkForNCCLErrorsInternal(
    std::shared_ptr<NCCLComm>& ncclComm) {
  // Prioritize commFailureReason over checkForNcclError() result if
  // commFailureReason is set.
  auto commFailureReason = ncclComm->getNcclCommFailureReason();
  if (commFailureReason != c10::nullopt) {
    return std::make_exception_ptr(C10_BUILD_ERROR(
        DistBackendError,
        c10::str(
            "NCCL communicator encountered error set by ProcessGroupNCCL: ",
            *commFailureReason)));
  }
  ncclResult_t ncclAsyncErr = ncclComm->checkForNcclError();
  // When nonblocking mode is enabled by TORCH_NCCL_USE_COMM_NONBLOCKING,
  // ncclInProgress could be returned when there are pending NCCL calls.
  // In this case, no exception should be thrown
#ifdef NCCL_HAS_COMM_NONBLOCKING
  // ncclInProgress is defined only if NCCL_HAS_COMM_NONBLOCKING is defined
  if (ncclAsyncErr != ncclSuccess && ncclAsyncErr != ncclInProgress) {
#else
  if (ncclAsyncErr != ncclSuccess) {
#endif
    return std::make_exception_ptr(C10_BUILD_ERROR(
        DistBackendError,
        "NCCL error: " + ncclGetErrorWithVersion(ncclAsyncErr) + "\n" +
            getNcclErrorDetailStr(ncclAsyncErr)));
  }

  return nullptr;
}

void ProcessGroupNCCL::broadcastUniqueNCCLID(
    ncclUniqueId* ncclID,
    bool isSingleP2POp,
    const std::string& p2pKey,
    int p2pRank) {
  // For collective operations:
  // For every NCCL communicator that we create we need to broadcast
  // a unique ID from rank 0 to all other ranks. This broadcast is
  // done by rank 0 setting a key in the store and all other ranks
  // retrieving the contents of that key. A single process group
  // may create multiple NCCL communicators, so we use a sequence
  // number to differentiate between them.
  // For single point-to-point operations:
  // The sequence number will only be increased on 2 out of all the
  // processes in a Process Group. So all following collective
  // operations will see different sequence numbers which will cause
  // runtime errors. To avoid that, use the src:target pair instead
  // of sequence number for p2p communications.

  std::string storeKey;
  if (!isSingleP2POp) {
    storeKey = std::to_string(ncclCommCounter_++);
  } else {
    storeKey = p2pKey;
  }
  if (rank_ == 0 || (isSingleP2POp && p2pRank == 0)) {
    auto vec = std::vector<uint8_t>(
        reinterpret_cast<uint8_t*>(ncclID),
        reinterpret_cast<uint8_t*>(ncclID) + NCCL_UNIQUE_ID_BYTES);
    store_->set(storeKey, vec);
  } else {
    try {
      auto vec = store_->get(storeKey);
      TORCH_CHECK_WITH(
          DistBackendError,
          vec.size() == NCCL_UNIQUE_ID_BYTES,
          "Invalid size for ncclUniqueId");
      std::memcpy(ncclID, vec.data(), vec.size());
    } catch (const std::exception& e) {
      std::string exceptionMsg = c10::str(
          "[",
          rank_,
          "] is setting up NCCL communicator and "
          "retrieving ncclUniqueId from [0] via c10d key-value store by key '",
          storeKey,
          "', but store->get('",
          storeKey,
          "') got error: ");
      C10_THROW_ERROR(
          DistBackendError,
          exceptionMsg + e.what() +
              ". This may indicate a possible application crash on rank 0 or a network set up issue.");
    } catch (...) {
      C10_THROW_ERROR(
          DistBackendError,
          c10::str(
              "Unknown exception while [",
              rank_,
              "] is setting up NCCL communicator and "
              "retrieving ncclUniqueId from [0] via c10d key-value store by key '",
              storeKey,
              "'",
              ". This may indicate a possible application crash on rank 0 or a network set up issue."));
    }
  }
}

void ProcessGroupNCCL::destroyNCCLComms(const std::string& devNCCLCommMapKey) {
  std::lock_guard<std::mutex> lock(mutex_);
  if (devNCCLCommMap_.find(devNCCLCommMapKey) == devNCCLCommMap_.end()) {
    TORCH_INTERNAL_ASSERT(
        false,
        "Expected to find key ",
        devNCCLCommMapKey,
        " in NCCL communicator map.");
  }
  std::shared_ptr<NCCLComm>& ncclComm = devNCCLCommMap_[devNCCLCommMapKey];
  // ncclCommDestroy(comm->getNcclComm()) results in segfault when PG is being
  // destroyed, so using ncclCommAbort here.
  ncclComm->ncclCommAbort();
  // Remove communicators from the cache.
  devNCCLCommMap_.erase(devNCCLCommMapKey);
  // Clear used device indices.
  usedDeviceIdxs_.clear();

  ncclCommDevIdxMapMutex.lock();
  ncclCommDevIdxMap.erase(ncclComm);
  ncclCommDevIdxMapMutex.unlock();
}

std::shared_ptr<NCCLComm> ProcessGroupNCCL::getNCCLComm(
    const std::string& deviceKey,
    at::Device& device,
    OpType opType,
    int p2pRank,
    bool isSendRecvSelf) {
  // Sanity check
  if (deviceKey.empty()) {
    C10_THROW_ERROR(
        DistBackendError,
        "Not able to create/get the NCCL Communicator since "
        "the GPU devices are not known");
  }
  if (bound_device_id_) {
    if (*bound_device_id_ != device) {
      LOG(ERROR) << logPrefix() << "Tensor found on device " << device
                 << " but backend constrained to " << *bound_device_id_;
      C10_THROW_ERROR(
          DistBackendError,
          "Attempt to perform collective on tensor not on device passed to init_process_group");
    }
  }

  usedDeviceIdxs_.insert(device.index());

  {
    std::lock_guard<std::mutex> lock(mutex_);
    if (devNCCLCommMap_.find(deviceKey) != devNCCLCommMap_.end()) {
      // Reuse the cached communicator if there is one.
      return devNCCLCommMap_[deviceKey];
    }
  }

  // NCCL communicator not cached, create a new entry
  std::shared_ptr<NCCLComm> ncclComm;

  // Create the unique NCCL ID and broadcast it
  ncclUniqueId ncclID;

  // For batch_isend_irecv, ncclGroupStart() would be called upfront
  bool batchP2P = ncclActiveGroupCounter_ > 0;
  bool singleP2POp = isP2POp(opType, batchP2P);
  // For point-to-point communication, lower rank of the two will get unique id.
  if (rank_ == 0 || (singleP2POp && p2pRank == 0)) {
    C10D_NCCL_CHECK(ncclGetUniqueId(&ncclID), c10::nullopt);
  }

  // For point-to-point communication on the same process, don't need broadcast.
  if (!isSendRecvSelf) {
    // Broadcast so that each process can have a unique NCCL ID
    broadcastUniqueNCCLID(&ncclID, singleP2POp, deviceKey, p2pRank);
  }

  at::cuda::OptionalCUDAGuard gpuGuard;

  // [Group Start/End Note] This is used to ensure that nccl communicator will
  // be created before communication primitives are called. Let's look at this
  // example: Using the batch_isend_irecv to send a tensor to a target process.
  // On the sender side, the corresponding underlying NCCL calls will look like
  //   ncclGroupStart() // This is in batch_isend_irecv
  //   ncclGroupStart() // This is [Note 1]
  //   ncclCommInitRank() // Inside NCCLComm::create
  //   ncclSend()
  //   ncclGroupEnd() // This is [Note 2]
  //   ncclGroupEnd() // This is in batch_isend_irecv
  // With this pattern, the nccl communicator will be created in the last
  // ncclGroupEnd which means when ncclSend is processed, the passed
  // communicator argument is NULL which will lead to runtime error. So we need
  // to "close" all active nccl groups to ensure nccl communicator is actually
  // created before encountering any communication calls. This is why we need
  // the following for loop.
  for (const auto i : c10::irange(ncclActiveGroupCounter_)) {
    (void)i;
    // comms have not been initiated yet, so can only check in blocking-way
    C10D_NCCL_CHECK(ncclGroupEnd(), c10::nullopt);
  }

  // [Note 1] Create the NCCL communicators for each GPU
  C10D_NCCL_CHECK(ncclGroupStart(), c10::nullopt);

  // GPU world size and GPU rank
  int numRanks, rank;

  if (!singleP2POp) {
    // Collective, all-to-all, or batch P2P
    numRanks = getSize();
    rank = getRank();
  } else if (isSendRecvSelf) {
    // Same process send and recv.
    numRanks = 1;
    rank = 0;
  } else {
    // For single point-to-point operation, there are only 2 processes
    // involved so the GPU rank is either 0 or 1.
    numRanks = 2;
    rank = p2pRank;
  }
  // Get the device index
  auto deviceIndex = device.index();
  gpuGuard.set_index(deviceIndex);
#ifdef NCCL_HAS_COMM_SPLIT
  if (options_->split_from) {
    TORCH_CHECK(
        options_->split_color != 0,
        "Must specify a non-zero color when splitting");
    // Find a valid, healthy communicator to split from if possible.
    std::lock_guard<std::mutex> lock(options_->split_from->mutex_);
    auto& other_comms = options_->split_from->devNCCLCommMap_;
    auto dit = other_comms.find(deviceKey);
    if (dit != other_comms.end()) {
      auto& parentComm = dit->second;
      if (parentComm != nullptr && !parentComm->isAborted()) {
        ncclComm = NCCLComm::split(
            parentComm.get(), options_->split_color, rank, options_->config);
      }
    }
  }
#endif

  // To simplify conditioonal nesting, just create the ncclComms[i]
  // entry if it hasn't been yet rather than untangling the
  // conditions that might have resulted in a split above.
  if (!ncclComm) {
#ifdef NCCL_HAS_COMM_NONBLOCKING
    ncclComm = NCCLComm::create(numRanks, rank, ncclID, options_->config);
#else
    ncclComm = NCCLComm::create(numRanks, rank, ncclID);
#endif
  }

  // Creates the NCCL streams
  auto streamVal =
      at::cuda::getStreamFromPool(options_->is_high_priority_stream);

  {
    std::lock_guard<std::mutex> lock(mutex_);
    inInitializationCommMap_.emplace(deviceKey, ncclComm);
  }

  // [Note 2 ]
#ifndef NCCL_HAS_COMM_NONBLOCKING
  C10D_NCCL_CHECK(ncclGroupEnd(), c10::nullopt);
#else
  if (nccl_use_nonblocking()) {
    // If we use nonblocking mode, allow communicators to be
    // uninitialized/ncclInProgress until the first communication
    C10D_NCCL_CHECK_NONBLOCKING(ncclGroupEnd(), c10::nullopt);
  } else {
    C10D_NCCL_CHECK(ncclGroupEnd(), c10::nullopt);
  }
#endif

  LOG(INFO) << logPrefix() << "ProcessGroupNCCL created ncclComm_ "
            << ncclComm->ncclComm_ << " on CUDA device: " << deviceIndex;

  // At this point NCCL should have been initialized, hence we can accurately
  // get the env value even if NCCL sets it by reading from nccl.conf file
  LOG(INFO) << logPrefix()
            << "NCCL_DEBUG: " << getCvarString({"NCCL_DEBUG"}, "N/A");

  // See [Group Start/End Note]
  for (const auto i : c10::irange(ncclActiveGroupCounter_)) {
    (void)i;
    C10D_NCCL_CHECK(ncclGroupStart(), c10::nullopt);
  }

  ncclStreams_.emplace(deviceKey, std::move(streamVal));

  // Note: these events are created with the (default) cudaEventDisableTiming
  // flag This flag provides the best performance when used with
  // cudaStreamWaitEvent() and cudaEventQuery(). Since we here don't measure the
  // performance using cudaEvent, this should be set.
  // TODO(kwen2501): is ncclEvents_ used anywhere else?
  ncclEvents_.emplace(deviceKey, at::cuda::CUDAEvent(cudaEventDisableTiming));

  // Record the communicators based on ncclUniqueId.
  ncclIdToCommMap_.emplace(buildNcclUniqueIdStr(ncclID), ncclComm);

  // Move the NCCL resource to cache
  auto it = inInitializationCommMap_.find(deviceKey);
  // A previous thread could've already removed devicesKey from
  // inInitializationCommMap_ and added it to devNCCLCommMap_
  if (it != inInitializationCommMap_.end()) {
    devNCCLCommMap_.emplace(deviceKey, std::move(it->second));
    inInitializationCommMap_.erase(deviceKey);

    // Now ncclComms are fully initialized.
    // Register all active CUDA memory segments in cache allocator to
    // the new NCCL communicators
    if (useTensorRegisterAllocatorHook_) {
      auto snapshot = c10::cuda::CUDACachingAllocator::snapshot();
      // Register the segment to a new NCCL communicator if on the same device
      for (const auto& segmentInfo : snapshot.segments) {
        TORCH_INTERNAL_ASSERT(
            segmentInfo.device == device.index(),
            "Mismatch between CUDA memory segment device and current device");
        ncclComm->registerSegment(
            reinterpret_cast<void*>(segmentInfo.address),
            segmentInfo.total_size);
      }

      // Record the mapping between ncclComm and device index so that later
      // register hook can register a newly allocated segment to communicators
      // on the same device.
      // NOTE: we need remove the communicator from this map when it is
      // destroyed, otherwise may register onto an invalid communicator.
      ncclCommDevIdxMapMutex.lock();
      ncclCommDevIdxMap.emplace(ncclComm, device.index());
      ncclCommDevIdxMapMutex.unlock();
    }
  }

  it = devNCCLCommMap_.find(deviceKey);
  TORCH_INTERNAL_ASSERT(
      it != devNCCLCommMap_.end(), "Communicators not populated in cache!");

  return it->second;
}

uint64_t ProcessGroupNCCL::getCommSplitCounter() const {
  uint64_t ret = 0;
  for (const auto& i : ncclIdToCommMap_) {
    auto& ncclComm = i.second;
    ret += ncclComm->getCommSplitCounter();
  }
  return ret;
}

namespace {

// Check validity of tensor
void check_gpu_single_tensor(
    const at::Tensor& tensor,
    const bool p2p = false // whether operation is a P2P operation
) {
  if (!tensor.is_cuda() || tensor.is_sparse()) {
    C10_THROW_ERROR(ValueError, "Tensors must be CUDA and dense");
  }
  // Skip the following requirements for P2P operations
  if (!tensor.is_contiguous(tensor.suggest_memory_format())) {
    if (p2p) {
      TORCH_WARN_ONCE(
          "Detected non-contiguous tensor in P2P operations. It is user "
          "responsibility to guarantee that source and destination tensors have "
          "the same contiguity format.");
    } else {
      C10_THROW_ERROR(ValueError, "Tensors must be contiguous");
    }
  }
}

// Checks that all `tensors' have the same type and shape and reside on the same
// GPU.
// TODO: test_c10d_nccl.py should consider adding tests for the error conditions
// here, ie, that deliberately pass invalid tensors and check the right
// exception is thrown. The "Expected list of tensors on the same device"
// condition may be a challenge because the test would need to pass tensors on
// different devices in the same process.
int64_t check_gpu_tensors_same_device(const std::vector<at::Tensor>& tensors) {
  if (tensors.size() == 0) {
    C10_THROW_ERROR(ValueError, "Tensor list must be nonempty");
  }

  const auto& first = tensors.front();

  int64_t total_numel = 0;
  for (const auto& t : tensors) {
    if (!t.is_cuda() || t.is_sparse()) {
      C10_THROW_ERROR(ValueError, "Tensors must be CUDA and dense");
    }
    if (t.scalar_type() != first.scalar_type()) {
      C10_THROW_ERROR(TypeError, "Tensors must have identical type");
    }
    if (!t.is_non_overlapping_and_dense()) {
      C10_THROW_ERROR(ValueError, "Tensors must be non-overlapping and dense");
    }
    // If we're in this function, the user called a _coalesced collective
    // on a set of tensors with potentially different sizes and strides.
    // Therefore, we don't check for matching sizes and strides,
    // but we do double-check tensors are on the same device.
    TORCH_CHECK_WITH(
        ValueError,
        t.get_device() == tensors[0].get_device(),
        "Expected list of tensors on the same device");
    total_numel += t.numel();
  }

  return total_numel;
}

bool check_same_size(const std::vector<at::Tensor>& input_tensors) {
  for (const auto& input_tensor : input_tensors) {
    if (!input_tensors[0].is_same_size(input_tensor)) {
      return false;
    }
  }
  return true;
}

} // namespace

c10::intrusive_ptr<ProcessGroupNCCL::WorkNCCL> ProcessGroupNCCL::initWork(
    at::Device& device,
    int rank,
    OpType opType,
    const char* profilingTitle,
    const std::vector<at::Tensor>& inputs,
    const std::vector<at::Tensor>& outputs, // TODO(kwen2501): necessary?
    bool record) {
  auto r = c10::make_intrusive<ProcessGroupNCCL::WorkNCCL>(
      device,
      rank,
      opType,
      seq_,
      profilingTitle,
      profilingTitle != nullptr ? c10::optional<std::vector<at::Tensor>>(inputs)
                                : c10::nullopt,
      desyncDebug_,
      enableTiming_.load(),
      dist_debug_level_);
  if (record) {
    // Ideally record every work that we enqueue, rather than every work we
    // create.
    // - at the time of this PR we do not currently enqueue every created work
    // - but it is unsafe to steal refs to start/end cuda events from Works that
    //   may go out of scope before flight recorder has retired them,
    //   so we must ensure that any work that is initialized via initWork will
    //   be enqueued
    // - initially, moved record() into workEnqueue(), but found that makes it
    //   hard to get access to profilingTitle,
    //   inputs, and outputs for metadata recording, and we don't want to attach
    //   these objects to the Work becuase it has implications for keeping those
    //   tensors alive longer and adds overhead when copying Work objects
    //   between threads
    r->trace_id_ = NCCLTraceBuffer::get()->record(
        uid_,
        seq_,
        op_id_,
        profilingTitle ? profilingTitle : "",
        inputs,
        outputs,
        r->ncclStartEvent_.get(),
        r->ncclEndEvent_.get());
  }
  return r;
}

// TODO(kwen2501): deprecate
std::vector<at::Tensor> ProcessGroupNCCL::WorkNCCL::result() {
  return *outputs_;
}

c10::intrusive_ptr<c10::ivalue::Future> ProcessGroupNCCL::WorkNCCL::
    getFuture() {
  return future_;
}

float ProcessGroupNCCL::WorkNCCL::getDuration() const {
  TORCH_CHECK(timingEnabled_, "getDuration only works if timing was enabled");
  TORCH_CHECK(
      ncclStartEvent_,
      "getDuration only works if ncclStartEvents_ is populated, true if timing enabled");
  TORCH_CHECK(
      ncclEndEvent_,
      "getDuration only works if ncclEndEvents_ is populated, which should always be true");
  return ncclStartEvent_->elapsed_time(*ncclEndEvent_);
}

uint64_t ProcessGroupNCCL::WorkNCCL::getSequencenumber() const {
  return seq_;
}

void ProcessGroupNCCL::workEnqueue(
    c10::intrusive_ptr<ProcessGroupNCCL::WorkNCCL> work) {
  if (!terminateProcessGroup_.load()) {
    std::lock_guard<std::mutex> lock(workMetaListMutex_);
    // Avoid view tensors to be processed in cleanup thread.
    // View tensors' destruction invokes autograd_meta, which
    // needs to be destructed in user thread. Otherwise will
    // get deadlock. Here we enqueue work without outputs_.
    workMetaList_.emplace_back(*work);
    lastEnqueuedSeq_ = work->seq_;
    lastWorkListUpdateTime_ = std::chrono::steady_clock::now();
  }
}

ProcessGroupNCCL::Options::Options(bool is_high_priority_stream)
    : Backend::Options(NCCL_BACKEND_NAME, kProcessGroupNCCLDefaultTimeout),
      is_high_priority_stream(is_high_priority_stream) {}

static constexpr int CoalActive = 0x01, CoalColl = 0x02, CoalP2P = 0x04;

void ProcessGroupNCCL::startCoalescing() {
  coalescedDevices_.clear();
  coalescedComms_.clear();
  coalescing_state_ |= CoalActive;
  groupStart();
  // Other collective ops bump seq_ before creating a work. Thus, if coalesced
  // ops bump seq_ only after initing a work they will collide with (reuse) the
  // seq_ of the last non-coalesced collective.  Previously, seq_ was bumped
  // inside endCoalescing, but before initWork. Since we now record individual
  // ops from a coalesce group into the flight recorder, we want to have the
  // same seq_ for those ops and its 'endCoalescing' op. Hence we bump during
  // start, which has one minor downside- we burn a seq_ if someone ever does a
  // 'start' and 'end' coalescing region without doing an operation inbetween.
  seq_++;

  // Don't bump op_id_ here, becuase startCoalescing isn't a logical operation.
  // Bump it for each logical op inside the coalescing group.
}

// `optype` is for specifying a composite optype, such as ALLGATHER and
// REDUCE_SCATTER
c10::intrusive_ptr<Work> ProcessGroupNCCL::endCoalescing(OpType optype) {
  if (coalescedComms_.size() == 0) {
    // There is no actual work being coalesced, return here
    groupEnd();
    coalescing_state_ = 0;
    return nullptr;
  }

  // `coalescedComms_` should have same set of comms across collectives
  auto comm = coalescedComms_[0];
  // `coalescedDevices_` should have same set of devices across collectives
  auto device = coalescedDevices_[0];

  // `getKeyFromDevice` is how we get keys for both collectives and batch P2P
  const auto key = getKeyFromDevice(device);
  auto ncclStream = ncclStreams_.at(key);

  // Create Work object
  c10::cuda::CaptureStatus capture_status =
      c10::cuda::currentStreamCaptureStatusMayInitCtx();
  bool enqueue =
      (coalescing_state_) && capture_status == c10::cuda::CaptureStatus::None;
  auto work =
      initWork(device, rank_, optype, "nccl:coalesced", {}, {}, enqueue);
  work->ncclComm_ = comm;
  work->blockingWait_ = blockingWait_;
  work->avoidRecordStreams_ = avoidRecordStreams_;
  work->opTimeout_ = options_->timeout;
  work->store_ = store_;

  // Record start before ncclGroupEnd
  if (work->timingEnabled_) {
    work->ncclStartEvent_->record(ncclStream);
  }

  if (nccl_use_nonblocking()) {
    groupEndNonblocking(comm);
  } else {
    groupEnd();
  }

  // Record end after ncclGroupEnd
  // TODO(eqy): is this still necessary if avoidRecordStreams_ is set?
  work->ncclEndEvent_->record(ncclStream);

  if (avoidRecordStreams_) {
    // other functions expect an initialized ptr if avoidRecordStreams_ is set
    work->stashed_for_allocator_safety_ =
        std::make_shared<std::vector<at::Tensor>>();
  }

  // Notify graphs before we check the capture status preemptively
  at::cuda::CUDAGraph::inc_pending_event_queries();

  if (enqueue) {
    workEnqueue(work);
  } else {
    at::cuda::CUDAGraph::dec_pending_event_queries();
  }

  coalescing_state_ = 0;
  return work;
}

c10::intrusive_ptr<Work> ProcessGroupNCCL::endCoalescing() {
  // Default OpType to COALESCED if not specified
  return endCoalescing(OpType::COALESCED);
}

template <typename Fn, typename PreProcess, typename PostProcess>
c10::intrusive_ptr<Work> ProcessGroupNCCL::collective(
    at::Tensor& input,
    at::Tensor& output,
    Fn fn,
    PreProcess pre,
    PostProcess post,
    OpType opType,
    const char* profilingTitle,
    bool avoidRecordStreams) {
  // Environment setting by the user may add onto collective call's option
  avoidRecordStreams |= avoidRecordStreams_;
  c10::cuda::CaptureStatus capture_status =
      c10::cuda::currentStreamCaptureStatusMayInitCtx();
  errorIfCapturingNonCapturableNCCL(capture_status);

  // Bump collective counter
  seq_++;
  op_id_++;

  auto device = getDevice(input);
  const auto key = getKeyFromDevice(device);
  auto ncclComm = getNCCLComm(key, device, opType);

  if (coalescing_state_ & CoalActive) {
    coalescing_state_ |= CoalColl;
    coalescedDevices_.push_back(device);
    coalescedComms_.push_back(ncclComm);
  }

  // Used many times below, so we stash the unordered_map lookup
  auto ncclStream = ncclStreams_.at(key);

  // First let NCCL streams wait for input tensors allocation streams
  syncStream(device, ncclEvents_[key], ncclStream);

  std::vector<at::Tensor> inputs{input};
  std::vector<at::Tensor> outputs{output};

  bool enqueue =
      !coalescing_state_ && capture_status == c10::cuda::CaptureStatus::None;
  auto work =
      initWork(device, rank_, opType, profilingTitle, inputs, outputs, enqueue);

  // Store references to outputs to be used by WorkNCCL::result and operator<<.
  work->outputs_ =
      std::make_shared<std::vector<at::Tensor>>(std::move(outputs));

  if (avoidRecordStreams) {
    work->stashed_for_allocator_safety_ =
        std::make_shared<std::vector<at::Tensor>>();
    work->stashed_for_allocator_safety_->push_back(input);
  }

  at::cuda::OptionalCUDAGuard gpuGuard;

  // Start event should only be recorded before the ncclGroupStart()
  if (work->timingEnabled_) {
    work->ncclStartEvent_->record(ncclStream);
  }

  pre(ncclStream, work);

  ncclComm_t comm = ncclComm->getNcclComm();

  // Both `inputs' and `outputs' are created on a worker stream and used in
  // different ncclStreams.  Hence, both must record the ncclStream to
  // prevent being freed before the collective finishes.
  //
  // We only record `inputs' here, and leave recording `outputs' to `fn' for
  // operations where `inputs' and `outputs' are not the same.
  //
  // See [Sync Streams].
  if (!avoidRecordStreams) {
    if (!input.is_sparse()) {
      c10::cuda::CUDACachingAllocator::recordStream(
          input.storage().data_ptr(), ncclStream);
    } else {
      // for sparse input case record streams on both index and value
      // tensors
      c10::cuda::CUDACachingAllocator::recordStream(
          input.values().storage().data_ptr(), ncclStream);
      c10::cuda::CUDACachingAllocator::recordStream(
          input.indices().storage().data_ptr(), ncclStream);
    }
  }
#ifndef NCCL_HAS_COMM_NONBLOCKING
  C10D_NCCL_CHECK(
      fn(input, output, comm, ncclStream),
      ncclComm->getNcclCommFailureReason());
#else
  C10D_NCCL_CHECK_TIMEOUT(
      fn(input, output, comm, ncclStream),
      comm,
      ncclComm->getNcclCommFailureReason());
#endif

  post(ncclStream, work);

  // End event should only be recorded after the ncclGroupEnd()
  if (!coalescing_state_) {
    work->ncclEndEvent_->record(ncclStream);
  }
  work->ncclComm_ = ncclComm;

  {
    c10::cuda::CUDAMultiStreamGuard streamGuard(ncclStream);
    std::vector<at::Device> devices{device};
    work->future_ = c10::make_intrusive<at::ivalue::Future>(
        c10::ListType::create(c10::TensorType::get()), devices);

    // Add a callback that runs profiling end callbacks. wrapCallback() in CUDA
    // future blocks the stream this callback runs on the corresponding
    // ncclEndEvents_ ensuring appropriate synchronization.
    if (work->recordFunctionEndCallback_) {
      work->future_->addCallback(
          [work](at::ivalue::Future& /* unused */) {
            work->recordFunctionEndCallback_();
          },
          // uses_future = false allows us to skip synchronization in
          // ivalue::Future, but is only valid as long as the lambda doesn't use
          // the "Future" argument.
          /*uses_future=*/false);
    }
    work->future_->markCompleted(at::IValue(*work->outputs_));
  }

  // Set appropriate work parameters.
  work->blockingWait_ = blockingWait_;
  work->avoidRecordStreams_ = avoidRecordStreams;
  work->opTimeout_ = options_->timeout;
  work->store_ = store_;
  // Record size info for debug. We only record the size on the first device as
  // multi-device per process is deprecated
  work->numelIn_ = input.numel();
  work->numelOut_ = output.numel();

  // Notify graphs before we check the capture status preemptively
  at::cuda::CUDAGraph::inc_pending_event_queries();
  if (enqueue) {
    workEnqueue(work);
  } else {
    at::cuda::CUDAGraph::dec_pending_event_queries();
  }

  return work;
}

template <typename Fn>
c10::intrusive_ptr<Work> ProcessGroupNCCL::collectiveCoalesced(
    std::vector<at::Tensor>& inputs,
    std::vector<at::Tensor>& outputs,
    Fn fn,
    OpType opType,
    const char* profilingTitle,
    bool avoidRecordStreams) {
  // Environment setting by the user may add onto collective call's option
  avoidRecordStreams |= avoidRecordStreams_;
  c10::cuda::CaptureStatus capture_status =
      c10::cuda::currentStreamCaptureStatusMayInitCtx();
  errorIfCapturingNonCapturableNCCL(capture_status);

  // Bump collective counter
  seq_++;
  // For coalescingManager collectives, there is no individual c++ call per
  // collective so there is no flight record and we increment seq_ and op_id_
  // together. Compare this to startCoalesing/endCoalescing flow where we
  // increment seq_ once per group and increment op_id_ once per indvidual
  // operation within the group
  op_id_++;

  // Currently, the API permits one scenario where inputs.size() and
  // outputs.size() are > 0.
  // 1. If the call was a _coalesced call, all inputs must be on the same
  // device.
  //    The group of nccl calls applies the collective separately to each input,
  //    but the group as a whole should be efficient, and might even execute as
  //    a single fused kernel.
  auto device = getDevice(inputs[0]);
  const auto key = getKeyFromDevice(device);
  auto ncclComm = getNCCLComm(key, device, opType);

  // Used many times below, so we stash the unordered_map lookup
  auto ncclStream = ncclStreams_.at(key);

  // First let NCCL streams wait for input tensors allocation streams
  syncStream(device, ncclEvents_[key], ncclStream);

  auto work = initWork(
      device, rank_, opType, nullptr, inputs, outputs, /*record=*/true);

  // Store references to outputs to be used by WorkNCCL::result and operator<<.
  work->outputs_ = std::make_shared<std::vector<at::Tensor>>(outputs);

  if (avoidRecordStreams) {
    work->stashed_for_allocator_safety_ =
        std::make_shared<std::vector<at::Tensor>>(inputs);
  }

  at::cuda::OptionalCUDAGuard gpuGuard;

  // Start event should only be recorded before the ncclGroupStart()
  if (work->timingEnabled_) {
    work->ncclStartEvent_->record(ncclStream);
  }

  ncclComm_t comm = ncclComm->getNcclComm();

// TODO(kwen2501): this should be moved to c10d tests, to qualify a NCCL
// upgrade. Once a NCCL version is qualified, this code should not be needed at
// runtime.
#ifdef PGNCCL_ENABLE_HASH
  if (enableCollecticeHashDebug_.load()) {
    auto numel = getTensorsNumel(inputs);
    auto hashValue = hashTensors(inputs);
    PRINT_COLLECTIVE_HASH_SIGNATURE(
        "input", opTypeToString(opType), numel, hashValue);
  }
#endif

  {
    torch::cuda::nccl::AutoNcclGroup nccl_group_guard(
        comm, nccl_use_nonblocking());
    for (const auto i : c10::irange(inputs.size())) {
      // Both `inputs' and `outputs' are created on a worker stream and used in
      // different ncclStreams.  Hence, both must record the ncclStream to
      // prevent being freed before the collective finishes.
      //
      // We only record `inputs' here, and leave recording `outputs' to `fn' for
      // operations where `inputs' and `outputs' are not the same.
      //
      // See [Sync Streams].
      if (!avoidRecordStreams) {
        if (!inputs[i].is_sparse()) {
          c10::cuda::CUDACachingAllocator::recordStream(
              inputs[i].storage().data_ptr(), ncclStream);
        } else {
          // for sparse input case record streams on both index and value
          // tensors
          c10::cuda::CUDACachingAllocator::recordStream(
              inputs[i].values().storage().data_ptr(), ncclStream);
          c10::cuda::CUDACachingAllocator::recordStream(
              inputs[i].indices().storage().data_ptr(), ncclStream);
        }
      }
#ifndef NCCL_HAS_COMM_NONBLOCKING
      C10D_NCCL_CHECK(
          fn(inputs[i], outputs[i], comm, ncclStream),
          ncclComm->getNcclCommFailureReason());
#else
      C10D_NCCL_CHECK_TIMEOUT(
          fn(inputs[i], outputs[i], comm, ncclStream),
          comm,
          ncclComm->getNcclCommFailureReason());
#endif
    }
  }

  // End event should only be recorded after the ncclGroupEnd()
  if (!coalescing_state_) {
    work->ncclEndEvent_->record(ncclStream);
  }
  work->ncclComm_ = ncclComm;

  {
    c10::cuda::CUDAMultiStreamGuard streamGuard(ncclStream);
    std::vector<at::Device> devices{device};
    work->future_ = c10::make_intrusive<at::ivalue::Future>(
        c10::ListType::create(c10::TensorType::get()), devices);

    // Add a callback that runs profiling end callbacks. wrapCallback() in CUDA
    // future blocks the stream this callback runs on the corresponding
    // ncclEndEvents_ ensuring appropriate synchronization.
    if (work->recordFunctionEndCallback_) {
      work->future_->addCallback(
          [work](at::ivalue::Future& /* unused */) {
            work->recordFunctionEndCallback_();
          },
          // uses_future = false allows us to skip synchronization in
          // ivalue::Future, but is only valid as long as the lambda doesn't use
          // the "Future" argument.
          /*uses_future=*/false);
    }
    work->future_->markCompleted(at::IValue(*work->outputs_));
  }

  // Set appropriate work parameters.
  work->blockingWait_ = blockingWait_;
  work->avoidRecordStreams_ = avoidRecordStreams;
  work->opTimeout_ = options_->timeout;
  work->store_ = store_;
  // Record size info for debug. We only record the size on the first device as
  // multi-device per process is deprecated
  work->numelIn_ = inputs[0].numel();
  work->numelOut_ = outputs[0].numel();

  // Notify graphs before we check the capture status preemptively
  at::cuda::CUDAGraph::inc_pending_event_queries();

  if (!coalescing_state_ && capture_status == c10::cuda::CaptureStatus::None) {
    workEnqueue(work);
  } else {
    at::cuda::CUDAGraph::dec_pending_event_queries();
  }

  return work;
}

template <typename Fn, typename PreProcess, typename PostProcess>
c10::intrusive_ptr<Work> ProcessGroupNCCL::pointToPoint(
    at::Tensor& tensor,
    Fn fn,
    int peer,
    OpType opType,
    PreProcess pre,
    PostProcess post,
    const char* profilingTitle) {
  // avoidRecordStreams_ note:
  // send, recv, and irecv should be ok with avoidRecordStreams,
  // However, for isend, I don't think the API requires the user
  // to wait() on the returned handle, so ProcessGroupNCCL can't know
  // when it's safe to release the input back to the allocator,
  // and the present call has no way to know it's not an isend.
  // Therefore, we warn and fall back to the typical recordStream logic:
  if (avoidRecordStreams_) {
    TORCH_WARN_ONCE(
        "TORCH_NCCL_AVOID_RECORD_STREAMS=1 has no effect for point-to-point "
        "collectives.");
  }

  auto device = getDevice(tensor);
  std::string key;
  int p2pRank = 0, p2pTargetRank = 0;
  bool isSendRecvSelf = false;
  // For batch_isend_irecv, ncclGroupStart() would be called upfront
  bool batchP2P = ncclActiveGroupCounter_ > 0;
  if (batchP2P) {
    // For batch P2P, we need to treat it like a collective when selecting
    // communicator, because other ranks can call into this batch other than my
    // rank and my peer
    key = getKeyFromDevice(device);
    p2pRank = rank_;
    p2pTargetRank = peer;
  } else {
    // For single P2P, preserve the old two-rank behavior (to avoid perf diff)
    key = getKeySendRecv(rank_, peer);
    p2pRank = rank_ <= peer ? 0 : 1;
    isSendRecvSelf = rank_ == peer;
    p2pTargetRank = isSendRecvSelf ? 0 : 1 - p2pRank;

    if (!coalescing_state_) {
      // Bump sequence number. Don't do so if it's a batch P2P, it will be
      // bumped in `endCoalescing`.
      seq_++;
    }
  }

  // Bump the logical operation counter regardless of whether this op is
  // coalesced or individual
  op_id_++;

  auto ncclComm = getNCCLComm(key, device, opType, p2pRank, isSendRecvSelf);

  if (coalescing_state_ & CoalActive) {
    coalescing_state_ |= CoalP2P;
    coalescedDevices_.push_back(device);
    coalescedComms_.push_back(ncclComm);
  }

  // Used many times below, so we stash the unordered_map lookup
  auto ncclStream = ncclStreams_.at(key);
  // First let NCCL streams wait for input tensors allocation streams
  syncStream(device, ncclEvents_[key], ncclStream);

  // Work itself will create the CUDA events on all GPUs of tensors
  c10::intrusive_ptr<ProcessGroupNCCL::WorkNCCL> work;
  if (coalescing_state_) {
    // When coalescing, we record events per op that lack timing/state
    // information becuase there is no 'work' associated with them, and then
    // later in endCoalescing we record a 'coalesced' Work which has
    // timing/state updates via watchdog thread, but lacks op metadata such as
    // input/output sizes and profilingTitle per-op in the group.
    auto trace_id = NCCLTraceBuffer::get()->record(
        uid_,
        seq_,
        op_id_,
        profilingTitle,
        {tensor},
        {tensor},
        nullptr,
        nullptr);
    // TODO(whc) if we want to make the per-p2p-op flightrecorder entries get
    // their timings/states updated by proxy when the Work obj representing the
    // coalesce group gets its update, we could accumulate these trace_ids
    // together and ask FlightRecorder to take the update from one Work and
    // apply it to multiple entries
    (void)trace_id;
  } else {
    // Store references to outputs to be used by WorkNCCL::result and
    // operator<<. Note that these outputs are only valid for recv(), as send()
    // does not modify the inputs but we still create these outputs for use
    // cases such as profiling.

    work = initWork(
        device, rank_, opType, profilingTitle, {tensor}, {}, /*record=*/false);
    // This bypasses something in Work() that crashes if {tensor} is given as
    // output, not sure what
    work->outputs_ = std::make_shared<std::vector<at::Tensor>>();
    work->outputs_->push_back(tensor);
    // TODO(whc) becuase we don't pass output {tensor} to initWork, we tell
    // initWork to not record, and then we manually call record passing all the
    // information it wants.
    work->trace_id_ = NCCLTraceBuffer::get()->record(
        uid_,
        seq_,
        op_id_,
        profilingTitle,
        {tensor},
        {tensor},
        work->ncclStartEvent_.get(),
        work->ncclEndEvent_.get());
  }

  // is gpuGuard needed for the if block below, or can i swap them
  at::cuda::OptionalCUDAGuard gpuGuard;

  if (!coalescing_state_) {
    // Start event should only be recorded before the ncclGroupStart()
    if (work->timingEnabled_) {
      work->ncclStartEvent_->record(ncclStream);
    }

    pre(ncclStream, work);
  }

  // Both send tensor and recv tensor are created on a worker stream and used
  // in different ncclStreams.  Hence, both must record the ncclStream to
  // prevent being freed before the collective finishes.
  //
  // See [Sync Streams].
  c10::cuda::CUDACachingAllocator::recordStream(
      tensor.storage().data_ptr(), ncclStream);

  // This part seems common to both p2p and coalesced-p2p usage?
  ncclComm_t comm_ = ncclComm->getNcclComm();

#ifndef NCCL_HAS_COMM_NONBLOCKING
  C10D_NCCL_CHECK(
      fn(tensor, comm_, ncclStream, p2pTargetRank),
      ncclComm->getNcclCommFailureReason());
#else
  C10D_NCCL_CHECK_TIMEOUT(
      fn(tensor, comm_, ncclStream, p2pTargetRank),
      ncclComm->getNcclComm(),
      ncclComm->getNcclCommFailureReason());
#endif

  if (!coalescing_state_) {
    post(ncclStream);

    // End event should only be recorded after the ncclGroupEnd()
    work->ncclEndEvent_->record(ncclStream);
    work->ncclComm_ = ncclComm;
    work->blockingWait_ = blockingWait_;
    work->opTimeout_ = options_->timeout;
    work->store_ = store_;
    // Record size info for debug. We only record the size on the first device
    // as multi-device per process is deprecated
    work->numelIn_ = work->numelOut_ = tensor.numel();

    // Future only needs to be created and marked completed with outputs for
    // recv(), but still create future for use cases such as profiling even for
    // send().
    {
      c10::cuda::CUDAMultiStreamGuard streamGuard(ncclStream);
      std::vector<at::Device> devices{device};
      work->future_ = c10::make_intrusive<at::ivalue::Future>(
          c10::ListType::create(c10::TensorType::get()), devices);
      work->future_->markCompleted(at::IValue(*work->outputs_));
    }

    // Add a callback that runs profiling end callbacks. wrapCallback() in CUDA
    // future blocks the stream this callback runs on the corresponding
    // ncclEndEvents_ ensuring appropriate synchronization.
    if (work->recordFunctionEndCallback_) {
      work->future_->addCallback(
          [work](at::ivalue::Future& /* unused */) {
            work->recordFunctionEndCallback_();
          },
          // uses_future = false allows us to skip synchronization in
          // ivalue::Future, but is only valid as long as the lambda doesn't use
          // the "Future" argument.
          /*uses_future=*/false);
    }
  }

  // Enqueue P2P op so that it can be cancelled by NCCL watchdog
  c10::cuda::CaptureStatus capture_status =
      c10::cuda::currentStreamCaptureStatusMayInitCtx();

  // Notify graphs before we check the capture status preemptively
  at::cuda::CUDAGraph::inc_pending_event_queries();

  if (!coalescing_state_ && capture_status == c10::cuda::CaptureStatus::None) {
    workEnqueue(work);
    return work;
  } else {
    at::cuda::CUDAGraph::dec_pending_event_queries();
    return nullptr;
  }
}

template <typename Fn>
c10::intrusive_ptr<Work> ProcessGroupNCCL::collective(
    at::Tensor& input,
    at::Tensor& output,
    Fn fn,
    OpType opType,
    const char* profilingTitle,
    bool avoidRecordStreams) {
  return collective(
      input,
      output,
      fn,
      [](at::cuda::CUDAStream&,
         c10::intrusive_ptr<ProcessGroupNCCL::WorkNCCL>& work) {},
      [](at::cuda::CUDAStream&,
         c10::intrusive_ptr<ProcessGroupNCCL::WorkNCCL>& work) {},
      opType,
      profilingTitle,
      avoidRecordStreams);
}

template <typename Fn>
c10::intrusive_ptr<Work> ProcessGroupNCCL::pointToPoint(
    at::Tensor& tensor,
    Fn fn,
    int peer,
    OpType opType,
    const char* profilingTitle) {
  return pointToPoint(
      tensor,
      fn,
      peer,
      opType,
      [](at::cuda::CUDAStream&,
         c10::intrusive_ptr<ProcessGroupNCCL::WorkNCCL>& work) {},
      [](at::cuda::CUDAStream&) {},
      profilingTitle);
}

c10::intrusive_ptr<Work> ProcessGroupNCCL::allreduce_sparse(
    std::vector<at::Tensor>& tensors,
    const AllreduceOptions& opts) {
  TORCH_CHECK(tensors.size() == 1, MULTI_DEVICE_ERROR_MSG);
  auto tensor = tensors.back();
#ifdef IS_NCCL_EXP
  tensor = tensor.coalesce();
  at::Tensor outputTensor =
      torch::zeros(tensor.sizes(), tensor.options().layout(torch::kStrided));
  auto work = collective(
      tensor,
      outputTensor,
      [&](at::Tensor& input,
          at::Tensor& output,
          ncclComm_t comm,
          at::cuda::CUDAStream& stream) {
        auto ncclDataType = getNcclDataType(input.scalar_type());
        auto ncclReduceOp =
            getNcclReduceOp(opts.reduceOp, input, ncclDataType, comm);

        size_t num_elements = output.numel();
        auto indices = input.indices();
        auto sizes = input.sizes();
        int colSize = sizes[1];
        auto rows = indices[0];
        size_t blockCount = rows.sizes()[0];
        auto recvIndices = indices[0] * colSize;

        // prevent output and recvIndices from being freed
        c10::cuda::CUDACachingAllocator::recordStream(
            output.storage().data_ptr(), stream);
        c10::cuda::CUDACachingAllocator::recordStream(
            recvIndices.storage().data_ptr(), stream);
        auto result = ncclAllReduceSparseBlock(
            input._values().data_ptr(), // sendbuff
            recvIndices.data_ptr<int64_t>(), // recv_indices
            blockCount, // block_count
            colSize, // block_length
            output.data_ptr(), // recvbuff
            output.numel(), // recv_count
            ncclDataType,
            ncclReduceOp,
            comm,
            stream.stream());
        return result;
      },
      [](at::cuda::CUDAStream& ncclStream,
         c10::intrusive_ptr<ProcessGroupNCCL::WorkNCCL>& work) {},
      [&](at::cuda::CUDAStream& ncclStream,
          c10::intrusive_ptr<ProcessGroupNCCL::WorkNCCL>& work) {
        // Convert output tensors to sparse and back into tensors.
        at::cuda::CUDAStreamGuard guard(ncclStream);
        if (opts.sparseIndices.has_value()) {
          tensor = at::sparse_coo_tensor(
              opts.sparseIndices.value(), outputTensor, tensor.sizes());
        } else {
          tensor = outputTensor.to_sparse();
        }
      },
      OpType::_ALLREDUCE_SPARSE,
      "nccl:all_reduce_sparse");
  return work;
#else
  // If the nccl branch is not "exp" then we just error
  C10_THROW_ERROR(
      Error,
      "allreduce_sparse is only available in the NCCL experimental branch.");
#endif
}

c10::intrusive_ptr<Work> ProcessGroupNCCL::allreduce_impl(
    at::Tensor& tensor,
    const AllreduceOptions& opts) {
  return collective(
      tensor,
      tensor,
      [&](at::Tensor& input,
          at::Tensor& output,
          ncclComm_t comm,
          at::cuda::CUDAStream& stream) {
        auto ncclDataType = getNcclDataType(input.scalar_type());
        auto ncclReduceOp =
            getNcclReduceOp(opts.reduceOp, input, ncclDataType, comm);
        return ncclAllReduce(
            input.data_ptr(),
            output.data_ptr(),
            input.numel(),
            ncclDataType,
            ncclReduceOp,
            comm,
            stream.stream());
      },
      OpType::ALLREDUCE,
      "nccl:all_reduce");
}

c10::intrusive_ptr<Work> ProcessGroupNCCL::allreduce(
    std::vector<at::Tensor>& tensors,
    const AllreduceOptions& opts) {
  TORCH_CHECK(tensors.size() == 1, MULTI_DEVICE_ERROR_MSG);
  auto tensor = tensors.back();
  check_gpu_single_tensor(tensor);

  if (intraNodeComm_ != nullptr && opts.reduceOp == ReduceOp::SUM) {
    using namespace intra_node_comm;
    auto algo = intraNodeComm_->selectAllReduceAlgo(tensor);
    if (algo != intra_node_comm::AllReduceAlgo::NONE) {
      intraNodeComm_->allReduce(tensor, algo);
      return c10::make_intrusive<IntraNodeCommWork>();
    }
  }

  // @lint-ignore CLANGTIDY
  RECORD_PARAM_COMMS_DATA(
      static_cast<int>(
          this->getSequenceNumberForGroup() + 1), // seq + 1 to match collective
      this->getID(),
      tensors, // inputTensors
      tensors, // outputTensors
      rank_, // rank
      "allreduce", // colName
      tensor.numel(), // inNelems
      tensor.numel(), // outNelems
      tensor.scalar_type(), // dType
      std::vector<int64_t>(), // inSplitSizes
      std::vector<int64_t>(), // outSplitSizes
      globalRankStart, // globalRankStart
      globalRankStride, // globalRankStride
      this->getSize()); // worldSize

  // avoidRecordStreams_ note: collective() will stash tensors.
  return allreduce_impl(tensor, opts);
}

c10::intrusive_ptr<Work> ProcessGroupNCCL::allreduce_coalesced(
    std::vector<at::Tensor>& tensors,
    const AllreduceCoalescedOptions& opts) {
  auto total_numel = check_gpu_tensors_same_device(tensors);

  // @lint-ignore CLANGTIDY
  RECORD_PARAM_COMMS_DATA(
      static_cast<int>(
          this->getSequenceNumberForGroup() + 1), // seq + 1 to match collective
      this->getID(),
      tensors, // inputTensors
      tensors, // outputTensors
      rank_, // rank
      "allreduce_coalesced", // colName
      total_numel, // inNelems
      total_numel, // outNelems
      tensors[0].scalar_type(), // dType
      // I'm not sure what in,outSplitSizes mean here.
      std::vector<int64_t>(), // inSplitSizes
      std::vector<int64_t>(), // outSplitSizes
      globalRankStart, // globalRankStart
      globalRankStride, // globalRankStride
      this->getSize()); // worldSize

  // avoidRecordStreams_ note: collective() will stash tensors.
  return collectiveCoalesced(
      tensors,
      tensors,
      [&](at::Tensor& input,
          at::Tensor& output,
          ncclComm_t comm,
          at::cuda::CUDAStream& stream) {
        auto ncclDataType = getNcclDataType(input.scalar_type());
        auto ncclReduceOp =
            getNcclReduceOp(opts.reduceOp, input, ncclDataType, comm);
        return ncclAllReduce(
            input.data_ptr(),
            output.data_ptr(),
            input.numel(),
            ncclDataType,
            ncclReduceOp,
            comm,
            stream.stream());
      },
      OpType::COALESCED,
      "nccl:allreduce_coalesced");
}

c10::intrusive_ptr<Work> ProcessGroupNCCL::broadcast(
    std::vector<at::Tensor>& tensors,
    const BroadcastOptions& opts) {
  TORCH_CHECK(tensors.size() == 1, MULTI_DEVICE_ERROR_MSG);
  auto tensor = tensors.back();
  check_gpu_single_tensor(tensor);

  // @lint-ignore CLANGTIDY
  RECORD_PARAM_COMMS_DATA(
      static_cast<int>(
          this->getSequenceNumberForGroup() + 1), // seq + 1 to match collective
      this->getID(),
      tensors, // inputTensors
      tensors, // outputTensors
      opts.rootRank, // root rank
      "broadcast", // colName
      tensor.numel(), // inNelems
      tensor.numel(), // outNelems
      tensor.scalar_type(), // dType
      std::vector<int64_t>(), // inSplitSizes
      std::vector<int64_t>(), // outSplitSizes
      globalRankStart, // globalRankStart
      globalRankStride, // globalRankStride
      this->getSize()); // worldSize

  // avoidRecordStreams_ note: collective() will stash tensors.
  bool avoidRecordStreams = avoidRecordStreams_ || (!opts.asyncOp);

  return collective(
      tensor,
      tensor,
      [&](at::Tensor& input,
          at::Tensor& output,
          ncclComm_t comm,
          at::cuda::CUDAStream& stream) {
        const auto root = opts.rootRank + opts.rootTensor;
        return ncclBcast(
            input.data_ptr(),
            input.numel(),
            getNcclDataType(input.scalar_type()),
            root,
            comm,
            stream.stream());
      },
      OpType::BROADCAST,
      "nccl:broadcast",
      avoidRecordStreams);
}

// _broadcast_oop adds an out-of-place broadcast in PGNCCL
// Custom collectives may be implemented by coalescing broadcast operations
// One use-case is implementing a vector all_gather (all_gather_v)
// where unevenly sized inputs are gathered among participating ranks
// Since all_gather provides an out-of-place API, an all_gather_v
// semantic implemented inside pg_nccl.all_gather also needs to support
// out-of-place, for which an out-of-place broadcast is required to be added
c10::intrusive_ptr<Work> ProcessGroupNCCL::_broadcast_oop(
    at::Tensor& outputTensor,
    at::Tensor& inputTensor,
    const BroadcastOptions& opts) {
  if (outputTensor.numel() != inputTensor.numel()) {
    C10_THROW_ERROR(
        ValueError,
        "Tensor input and output of _broadcast_oop must have the same number of elements ");
  }

  return collective(
      inputTensor,
      outputTensor,
      [&](at::Tensor& input,
          at::Tensor& output,
          ncclComm_t comm,
          at::cuda::CUDAStream& stream) {
        const auto root = opts.rootRank + opts.rootTensor;
        return ncclBroadcast(
            input.data_ptr(),
            output.data_ptr(),
            input.numel(),
            getNcclDataType(input.scalar_type()),
            root,
            comm,
            stream.stream());
      },
      OpType::BROADCAST,
      "nccl:_broadcast_oop");
}

c10::intrusive_ptr<Work> ProcessGroupNCCL::reduce(
    std::vector<at::Tensor>& tensors,
    const ReduceOptions& opts) {
  TORCH_CHECK(tensors.size() == 1, MULTI_DEVICE_ERROR_MSG);
  // @lint-ignore CLANGTIDY
  auto tensor = tensors.back();
  check_gpu_single_tensor(tensor);
  RECORD_PARAM_COMMS_DATA(
      static_cast<int>(
          this->getSequenceNumberForGroup() + 1), // seq + 1 to match collective
      this->getID(),
      tensors, // inputTensors
      tensors, // outputTensors
      opts.rootRank, // root rank
      "reduce", // colName
      tensor.numel(), // inNelems
      tensor.numel(), // outNelems
      tensor.scalar_type(), // dType
      std::vector<int64_t>(), // inSplitSizes
      std::vector<int64_t>(), // outSplitSizes
      globalRankStart, // globalRankStart
      globalRankStride, // globalRankStride
      this->getSize()); // worldSize

  // avoidRecordStreams_ note: collective() will stash tensors.
  return collective(
      tensor,
      tensor,
      [&](at::Tensor& input,
          at::Tensor& output,
          ncclComm_t comm,
          at::cuda::CUDAStream& stream) {
        const auto root = opts.rootRank + opts.rootTensor;
        auto ncclDataType = getNcclDataType(input.scalar_type());
        auto ncclReduceOp =
            getNcclReduceOp(opts.reduceOp, input, ncclDataType, comm);
        return ncclReduce(
            input.data_ptr(),
            output.data_ptr(),
            input.numel(),
            ncclDataType,
            ncclReduceOp,
            root,
            comm,
            stream.stream());
      },
      OpType::REDUCE,
      "nccl:reduce");
}

// _reduce_oop exposes an out-of-place reduce from PGNCCL
// Custom collectives may be implemented by coalescing reduce operations
// One use-case is implementing a vector reduce_scatter (reduce_scatter_v)
// where inputs are reduced and scattered unevenly among participating ranks
// Since reduce_scatter provides an out-of-place API, a reduce_scatter_v
// semantic implemented inside pg_nccl.reduce_scatter also needs to support
// out-of-place, for which an out-of-place reduce is required to be added
c10::intrusive_ptr<Work> ProcessGroupNCCL::_reduce_oop(
    at::Tensor& outputTensor,
    at::Tensor& inputTensor,
    const ReduceOptions& opts) {
  if (outputTensor.numel() != inputTensor.numel()) {
    C10_THROW_ERROR(
        ValueError,
        "Tensor input and output of _reduce_oop must have the same number of elements ");
  }

  return collective(
      inputTensor,
      outputTensor,
      [&](at::Tensor& input,
          at::Tensor& output,
          ncclComm_t comm,
          at::cuda::CUDAStream& stream) {
        const auto root = opts.rootRank + opts.rootTensor;
        const auto ncclDataType = getNcclDataType(input.scalar_type());
        const auto ncclReduceOp =
            getNcclReduceOp(opts.reduceOp, input, ncclDataType, comm);
        return ncclReduce(
            input.data_ptr(),
            output.data_ptr(),
            input.numel(),
            ncclDataType,
            ncclReduceOp,
            (int)root,
            comm,
            stream.stream());
      },
      OpType::REDUCE,
      "nccl:_reduce_oop");
}

c10::intrusive_ptr<Work> ProcessGroupNCCL::allgather(
    std::vector<std::vector<at::Tensor>>& outputTensors,
    std::vector<at::Tensor>& inputTensors,
    const AllgatherOptions& opts) {
  TORCH_CHECK(inputTensors.size() == 1, MULTI_DEVICE_ERROR_MSG);
  // @lint-ignore CLANGTIDY
  auto inputTensor = inputTensors.back();
  check_gpu_single_tensor(inputTensor);
  // @lint-ignore CLANGTIDY
  auto outputTensors_ = outputTensors.back();

  RECORD_PARAM_COMMS_DATA(
      static_cast<int>(
          this->getSequenceNumberForGroup() + 1), // seq + 1 to match collective
      this->getID(),
      inputTensors, // inputTensors
      outputTensors, // outputTensors
      rank_, // rank
      "all_gather", // colName
      inputTensor.numel(), // inNelems
      inputTensor.numel() * // outNelems
          this->getSize(),
      inputTensor.scalar_type(), // dType
      std::vector<int64_t>(), // inSplitSizes
      std::vector<int64_t>(), // outSplitSize
      globalRankStart, // globalRankStart
      globalRankStride, // globalRankStride
      this->getSize()); // worldSize

  bool same_size = check_same_size(outputTensors_);
  if (same_size) {
    // Flatten a vector of tensors into a single, stacked tensor.
    at::Tensor outputFlattened = newLikeFlat(outputTensors_);

    return collective(
        inputTensor,
        outputFlattened,
        [&](at::Tensor& input,
            at::Tensor& output,
            ncclComm_t comm,
            at::cuda::CUDAStream& stream) {
          if (!avoidRecordStreams_) {
            c10::cuda::CUDACachingAllocator::recordStream(
                output.storage().data_ptr(), stream);
          }
          return ncclAllGather(
              input.data_ptr(),
              output.data_ptr(),
              input.numel(),
              getNcclDataType(input.scalar_type()),
              comm,
              stream.stream());
        },
        [](at::cuda::CUDAStream& ncclStream,
           c10::intrusive_ptr<ProcessGroupNCCL::WorkNCCL>& work) {
          // avoidRecordStreams_ note: We actually don't need to stash anything
          // here.
          //  - inputTensors is stashed onto work->stashed_for_allocator_safety_
          //    in collective().
          //  - outputFlattened is stashed onto work->outputs_ in collective().
          //  - User-facing outputTensors should be held by the user until after
          //    waiting on work_, or the call makes no sense.
          // So all participating tensors are accounted for, and won't be
          // released back to their allocation streams until after work_ is
          // waited on.
        },
        [&](at::cuda::CUDAStream& ncclStream,
            c10::intrusive_ptr<ProcessGroupNCCL::WorkNCCL>& work) {
          // Copy the flattened output tensors to the outputs.
          at::cuda::CUDAStreamGuard guard(ncclStream);
          for (const auto j : c10::irange(outputTensors_.size())) {
            // See [Sync Streams].
            if (!avoidRecordStreams_) {
              c10::cuda::CUDACachingAllocator::recordStream(
                  outputTensors_[j].storage().data_ptr(), ncclStream);
            }
            outputTensors_[j].copy_(outputFlattened[j], true);
          }
        },
        OpType::ALLGATHER,
        "nccl:all_gather");
  } else {
    const auto num_reduces = outputTensors_.size();
    startCoalescing();
    for (const int i : c10::irange(num_reduces)) {
      auto& output = outputTensors_[i];
      auto& input = (i == rank_) ? inputTensor : output;
      auto broadcastOpts = BroadcastOptions{
          static_cast<int64_t>(i), static_cast<int64_t>(0), opts.timeout};
      _broadcast_oop(output, input, broadcastOpts);
    }
    auto work = endCoalescing(OpType::ALLGATHER);
    return work;
  }
}

c10::intrusive_ptr<Work> ProcessGroupNCCL::allgather_coalesced(
    std::vector<std::vector<at::Tensor>>& /* unused */,
    std::vector<at::Tensor>& /* unused */,
    const AllgatherOptions& /* unused */) {
  C10_THROW_ERROR(
      NotImplementedError,
      "ProcessGroupNCCL does not support allgather_coalesced");
}

c10::intrusive_ptr<Work> ProcessGroupNCCL::allgather_into_tensor_coalesced(
    std::vector<at::Tensor>& outputs,
    std::vector<at::Tensor>& inputs,
    const AllgatherOptions& opts) {
  return collectiveCoalesced(
      inputs,
      outputs,
      [&](at::Tensor& input,
          at::Tensor& output,
          ncclComm_t comm,
          at::cuda::CUDAStream& stream) {
        return ncclAllGather(
            input.data_ptr(),
            output.data_ptr(),
            input.numel(),
            getNcclDataType(input.scalar_type()),
            comm,
            stream.stream());
      },
      OpType::COALESCED,
      "nccl:all_gather_into_tensor_coalesced");
}

c10::intrusive_ptr<Work> ProcessGroupNCCL::reduce_scatter(
    std::vector<at::Tensor>& outputTensors,
    std::vector<std::vector<at::Tensor>>& inputTensors,
    const ReduceScatterOptions& opts) {
  TORCH_CHECK(outputTensors.size() == 1, MULTI_DEVICE_ERROR_MSG);
  // @lint-ignore CLANGTIDY
  auto outputTensor = outputTensors.back();
  check_gpu_single_tensor(outputTensor);
  // @lint-ignore CLANGTIDY
  auto inputTensors_ = inputTensors.back();

  RECORD_PARAM_COMMS_DATA(
      static_cast<int>(
          this->getSequenceNumberForGroup() + 1), // seq + 1 to match collective
      this->getID(),
      inputTensors, // inputTensors
      outputTensors, // outputTensors
      rank_, // rank
      "reduce_scatter", // colName
      outputTensor.numel() * this->getSize(), // inNelems
      outputTensor.numel(), // outNelems
      outputTensor.scalar_type(), // dType
      std::vector<int64_t>(), // inSplitSizes
      std::vector<int64_t>(), // outSplitSizes
      globalRankStart, // globalRankStart
      globalRankStride, // globalRankStride
      this->getSize()); // worldSize

  bool same_size = check_same_size(inputTensors_);
  if (same_size) {
    // Flatten a vector of tensors into a single, stacked tensor.
    at::Tensor inputFlattened = newLikeFlat(inputTensors_);

    return collective(
        inputFlattened,
        outputTensor,
        [&](at::Tensor& input,
            at::Tensor& output,
            ncclComm_t comm,
            at::cuda::CUDAStream& stream) {
          if (!avoidRecordStreams_) {
            c10::cuda::CUDACachingAllocator::recordStream(
                output.storage().data_ptr(), stream);
          }
          const auto ncclDataType = getNcclDataType(input.scalar_type());
          const auto ncclReduceOp =
              getNcclReduceOp(opts.reduceOp, input, ncclDataType, comm);
          return ncclReduceScatter(
              input.data_ptr(),
              output.data_ptr(),
              output.numel(),
              ncclDataType,
              ncclReduceOp,
              comm,
              stream.stream());
        },
        [&](at::cuda::CUDAStream& ncclStream,
            c10::intrusive_ptr<ProcessGroupNCCL::WorkNCCL>& work) {
          if (avoidRecordStreams_) {
            // We only need to stash inputTensors.
            //  - inputFlattened is stashed onto
            //  work->stashed_for_allocator_safety_
            //    in collective().
            //  - User-facing outputTensors is stashed onto work->outputs_ in
            //  collective(),
            //    and should also be held by the user until after waiting on
            //    work_.
            auto& v = work->stashed_for_allocator_safety_;
            v->insert(v->end(), inputTensors_.begin(), inputTensors_.end());
          }

          // Copy the input tensors to the flattened inputs.
          at::cuda::CUDAStreamGuard guard(ncclStream);
          for (const auto j : c10::irange(inputTensors_.size())) {
            // See [Sync Streams].
            if (!avoidRecordStreams_) {
              c10::cuda::CUDACachingAllocator::recordStream(
                  inputTensors_[j].storage().data_ptr(), ncclStream);
            }
            inputFlattened[j].copy_(inputTensors_[j], true);
          }
        },
        [&](at::cuda::CUDAStream&,
            c10::intrusive_ptr<ProcessGroupNCCL::WorkNCCL>& work) {},
        OpType::REDUCE_SCATTER,
        "nccl:reduce_scatter");
  } else {
    const auto num_reduces = inputTensors_.size();
    startCoalescing();
    for (const int i : c10::irange(num_reduces)) {
      auto& input = inputTensors_[i];
      auto& output = (i == rank_) ? outputTensor : input;
      auto reduceOpts = ReduceOptions{
          opts.reduceOp,
          static_cast<int64_t>(i),
          static_cast<int64_t>(0),
          opts.timeout};
      _reduce_oop(output, input, reduceOpts);
    }
    auto work = endCoalescing(OpType::REDUCE_SCATTER);
    return work;
  }
}

c10::intrusive_ptr<Work> ProcessGroupNCCL::_reduce_scatter_base(
    at::Tensor& outputTensor,
    at::Tensor& inputTensor,
    const ReduceScatterOptions& opts) {
  if (inputTensor.dtype() != outputTensor.dtype()) {
    C10_THROW_ERROR(
        TypeError, "input tensor must be the same type as the output tensor.");
  }

  if (inputTensor.numel() != outputTensor.numel() * size_) {
    C10_THROW_ERROR(
        ValueError,
        "input tensor must be the same size as output size times world size");
  }

  // @lint-ignore CLANGTIDY
  const auto& tensor = outputTensor;
  RECORD_PARAM_COMMS_DATA(
      static_cast<int>(
          this->getSequenceNumberForGroup() + 1), // seq + 1 to match collective
      this->getID(),
      inputTensor, // inputTensor
      outputTensor, // outputTensor
      rank_, // rank
      "_reduce_scatter_base", // colName
      inputTensor.numel(), // inNelems
      tensor.numel(), // outNelems
      tensor.scalar_type(), // dtype
      std::vector<int64_t>(), // inSplitSizes
      std::vector<int64_t>(), // outSplitSizes
      globalRankStart, // globalRankStart
      globalRankStride, // globalRankStride
      this->getSize()); // worldSize

  // avoidRecordStreams_ note: collective() will stash inputs and outputs.
  // Note 2: for asyncOp = false, we don't want to record streams because we
  // know that the NCCL stream will join back to the "current" stream right
  // after this op. So we might just as well keep the stream ownership of the
  // input/output tensors unchanged. The benefit would be that the
  // allocation/free of the tensors would look deterministic to the "current"
  // stream so that the caching allocator can reuse memory pool for this stream
  // in a clever way. This setting is added for libraries like FSDP which uses
  // `reduce_scatter_tensor`.
  bool avoidRecordStreams = avoidRecordStreams_ || (!opts.asyncOp);

  return collective(
      inputTensor,
      outputTensor,
      [&](at::Tensor& input,
          at::Tensor& output,
          ncclComm_t comm,
          at::cuda::CUDAStream& stream) {
        if (!avoidRecordStreams) {
          c10::cuda::CUDACachingAllocator::recordStream(
              output.storage().data_ptr(), stream);
        }
        auto ncclDataType = getNcclDataType(input.scalar_type());
        auto ncclReduceOp =
            getNcclReduceOp(opts.reduceOp, input, ncclDataType, comm);
        return ncclReduceScatter(
            input.data_ptr(),
            output.data_ptr(),
            output.numel(),
            ncclDataType,
            ncclReduceOp,
            comm,
            stream.stream());
      },
      OpType::_REDUCE_SCATTER_BASE,
      "nccl:_reduce_scatter_base",
      avoidRecordStreams);
}

c10::intrusive_ptr<Work> ProcessGroupNCCL::reduce_scatter_tensor_coalesced(
    std::vector<at::Tensor>& outputs,
    std::vector<at::Tensor>& inputs,
    const ReduceScatterOptions& opts) {
  return collectiveCoalesced(
      inputs,
      outputs,
      [&](at::Tensor& input,
          at::Tensor& output,
          ncclComm_t comm,
          at::cuda::CUDAStream& stream) {
        if (!avoidRecordStreams_) {
          c10::cuda::CUDACachingAllocator::recordStream(
              output.storage().data_ptr(), stream);
        }
        auto ncclDataType = getNcclDataType(input.scalar_type());
        auto ncclReduceOp =
            getNcclReduceOp(opts.reduceOp, input, ncclDataType, comm);
        return ncclReduceScatter(
            input.data_ptr(),
            output.data_ptr(),
            output.numel(),
            ncclDataType,
            ncclReduceOp,
            comm,
            stream.stream());
      },
      OpType::COALESCED,
      "nccl:reduce_scatter_tensor_coalesced");
}

c10::intrusive_ptr<Work> ProcessGroupNCCL::barrier(const BarrierOptions& opts) {
  RECORD_PARAM_COMMS(
      static_cast<int>(
          this->getSequenceNumberForGroup() + 1), // seq + 1 to match collective
      this->getID(),
      rank_, // rank
      "barrier", // colName
      0, // inNelems
      0, // outNelems
      at::kByte, // dType
      std::vector<int64_t>(), // inSplitSizes
      std::vector<int64_t>(), // outSplitSizes
      globalRankStart, // globalRankStart
      globalRankStride, // globalRankStride
      this->getSize()); // worldSize

  std::vector<at::Device> devices;

  // Use user defined GPU device ids if provided
  if (!opts.device_ids.empty()) {
    for (auto device : opts.device_ids) {
      devices.emplace_back(at::DeviceType::CUDA, device);
    }
  } else if (usedDeviceIdxs_.empty()) {
    // This means there is not yet a NCCL collective being called
    // Here we have to use the best guesses and will use a single GPU to call
    // allreduce to achieve barrier.
    // In case the multiple processes fall into the same node, we use rank to
    // ensure that each process is on a different GPU
    auto numGPUs = at::cuda::getNumGPUs();
    int16_t deviceIdx = static_cast<int16_t>(rank_ % numGPUs);
    LOG(INFO)
        << logPrefix()
        << c10::str(
               " using GPU ",
               deviceIdx,
               " to perform barrier as devices used by this process are currently unknown. ",
               "This can potentially cause a hang if this rank to GPU mapping is incorrect.",
               "Specify device_ids in barrier() to force use of a particular device.");
    devices.emplace_back(guessDeviceForRank());
  } else {
    for (auto usedDeviceIdx : usedDeviceIdxs_) {
      devices.emplace_back(at::DeviceType::CUDA, usedDeviceIdx);
    }
  }

  // Use one device only
  auto device = devices.back();
  at::Tensor barrierTensor =
      at::empty({1}, at::TensorOptions().device(device).dtype(at::kByte));
  // All reduce to achieve the barrier
  auto work = allreduce_impl(barrierTensor);

  // Work will take over barrierTensors
  auto ncclWork = dynamic_cast<ProcessGroupNCCL::WorkNCCL*>(work.get());
  TORCH_CHECK(ncclWork);
  ncclWork->barrierTensor_ = std::move(barrierTensor);
  return work;
}

c10::intrusive_ptr<Work> ProcessGroupNCCL::alltoall_base(
    at::Tensor& outputTensor,
    at::Tensor& inputTensor,
    std::vector<int64_t>& outputSplitSizes,
    std::vector<int64_t>& inputSplitSizes,
    const AllToAllOptions& /* unused */) {
  check_gpu_single_tensor(outputTensor, true);
  check_gpu_single_tensor(inputTensor, true);
  if (outputSplitSizes.size() == 0 && inputSplitSizes.size() == 0) {
    RECORD_PARAM_COMMS_DATA(
        static_cast<int>(
            this->getSequenceNumberForGroup() +
            1), // seq + 1 to match collective
        this->getID(),
        inputTensor, // inputTensor
        outputTensor, // outputTensor
        rank_, // rank
        "all_to_all", // colName
        inputTensor.numel(), // inNelems
        outputTensor.numel(), // outNelems
        inputTensor.scalar_type(), // dType
        std::vector<int64_t>(), // inSplitSizes
        std::vector<int64_t>(), // outSplitSizes
        globalRankStart, // globalRankStart
        globalRankStride, // globalRankStride
        this->getSize()); // worldSize

    // avoidRecordStreams_ note: collective() will stash inputTensors and
    // outputTensors.
    return collective(
        inputTensor,
        outputTensor,
        [&](at::Tensor& input,
            at::Tensor& output,
            ncclComm_t comm,
            at::cuda::CUDAStream& stream) {
          // See [Sync Streams].
          if (!avoidRecordStreams_) {
            c10::cuda::CUDACachingAllocator::recordStream(
                output.storage().data_ptr(), stream);
          }
          torch::cuda::nccl::all2all_single_equal_split(
              input, output, this->getSize(), comm, stream);
          return ncclSuccess;
        },
        OpType::ALLTOALL_BASE,
        "nccl:all_to_all");
  } else {
    c10d::checkSplitSizes(inputSplitSizes, inputTensor, size_);
    c10d::checkSplitSizes(outputSplitSizes, outputTensor, size_);

    RECORD_PARAM_COMMS_DATA(
        static_cast<int>(
            this->getSequenceNumberForGroup() +
            1), // seq + 1 to match collective
        this->getID(),
        inputTensor, // inputTensor
        outputTensor, // outputTensor
        rank_, // rank
        "all_to_allv", // colName
        inputTensor.numel(), // inNelems
        outputTensor.numel(), // outNelems
        inputTensor.scalar_type(), // dType
        inputSplitSizes, // inSplitSizes
        outputSplitSizes, // outSplitSizes
        globalRankStart, // globalRankStart
        globalRankStride, // globalRankStride
        this->getSize()); // worldSize

    // avoidRecordStreams_ note: collective() will stash inputTensors and
    // outputTensors.
    return collective(
        inputTensor,
        outputTensor,
        [&](at::Tensor& input,
            at::Tensor& output,
            ncclComm_t comm,
            at::cuda::CUDAStream& stream) {
          std::vector<size_t> send_lengths(size_);
          std::vector<size_t> recv_lengths(size_);
          std::vector<size_t> send_offsets(size_);
          std::vector<size_t> recv_offsets(size_);
          c10d::computeLengthsAndOffsets(
              inputSplitSizes, input, &send_lengths, &send_offsets);
          c10d::computeLengthsAndOffsets(
              outputSplitSizes, output, &recv_lengths, &recv_offsets);
          // See [Sync Streams].
          if (!avoidRecordStreams_) {
            c10::cuda::CUDACachingAllocator::recordStream(
                output.storage().data_ptr(), stream);
          }
          torch::cuda::nccl::all2all_single_unequal_split(
              input.data_ptr(),
              send_lengths.data(),
              send_offsets.data(),
              output.data_ptr(),
              recv_lengths.data(),
              recv_offsets.data(),
              input.element_size(),
              input.scalar_type(),
              comm,
              stream);
          return ncclSuccess;
        },
        OpType::ALLTOALL_BASE,
        "nccl:all_to_all");
  }
}

c10::intrusive_ptr<Work> ProcessGroupNCCL::alltoall(
    std::vector<at::Tensor>& outputTensors,
    std::vector<at::Tensor>& inputTensors,
    const AllToAllOptions& /* unused */) {
  std::vector<int64_t> inSplitSizes;
  std::vector<int64_t> outSplitSizes;
  int64_t total_numel = 0;

  auto device = outputTensors[0].device();
  for (const auto r : c10::irange(outputTensors.size())) {
    check_gpu_single_tensor(outputTensors[r], true);
    check_gpu_single_tensor(inputTensors[r], true);
    TORCH_CHECK(
        device == outputTensors[r].device() &&
            device == inputTensors[r].device(),
        "Tensors must be on the same device")
    inSplitSizes.push_back(inputTensors[r].numel());
    outSplitSizes.push_back(outputTensors[r].numel());
    total_numel += inputTensors[r].numel();
  }

  RECORD_PARAM_COMMS_DATA(
      static_cast<int>(
          this->getSequenceNumberForGroup() + 1), // seq + 1 to match collective
      this->getID(),
      inputTensors, // inputTensors
      outputTensors, // outputTensors
      rank_, // rank
      "all_to_all", // colName
      total_numel, // inNelems
      total_numel, // outNelems
      inputTensors.front().scalar_type(), // dType
      inSplitSizes, // inSplitSizes
      outSplitSizes, // outSplitSizes
      globalRankStart, // globalRankStart
      globalRankStride, // globalRankStride
      this->getSize()); // worldSize

  return collective(
      inputTensors[0],
      outputTensors[0],
      [&](at::Tensor& /* unused */,
          at::Tensor& /* unused */,
          ncclComm_t comm,
          at::cuda::CUDAStream& stream) {
        torch::cuda::nccl::all2all(outputTensors, inputTensors, comm, stream);
        return ncclSuccess;
      },
      [&](at::cuda::CUDAStream&,
          c10::intrusive_ptr<ProcessGroupNCCL::WorkNCCL>& work) {
        if (avoidRecordStreams_) {
          // inputTensor0 and outputTensor0 are stashed redundantly by
          // collective(), but that's ok.
          auto& v = work->stashed_for_allocator_safety_;
          v->insert(v->end(), inputTensors.begin(), inputTensors.end());
          v->insert(v->end(), outputTensors.begin(), outputTensors.end());
        }
      },
      [](at::cuda::CUDAStream&,
         c10::intrusive_ptr<ProcessGroupNCCL::WorkNCCL>& work) {},
      OpType::ALLTOALL,
      "nccl:all_to_all");
}

c10::intrusive_ptr<Work> ProcessGroupNCCL::send(
    std::vector<at::Tensor>& tensors,
    int dstRank,
    int /* unused */) {
  TORCH_CHECK(tensors.size() == 1, MULTI_DEVICE_ERROR_MSG);
  // @lint-ignore CLANGTIDY
  auto tensor = tensors.back();
  check_gpu_single_tensor(tensor, true);

  RECORD_PARAM_COMMS_DATA(
      static_cast<int>(
          this->getSequenceNumberForGroup() + 1), // seq + 1 to match collective
      this->getID(),
      tensors, // inputTensors
      tensors, // outputTensors
      dstRank, // dst rank
      "send", // colName
      tensor.numel(), // inNelems
      tensor.numel(), // outNelems
      tensor.scalar_type(), // dType
      std::vector<int64_t>(), // inSplitSizes
      std::vector<int64_t>(), // outSplitSizes
      globalRankStart, // globalRankStart
      globalRankStride, // globalRankStride
      this->getSize()); // worldSize

  auto ret = pointToPoint(
      tensor,
      [&](at::Tensor& input,
          ncclComm_t comm,
          at::cuda::CUDAStream& stream,
          int dst) {
        torch::cuda::nccl::send(input, comm, stream, dst);
        return ncclSuccess;
      },
      dstRank,
      OpType::SEND,
      c10::str("nccl:send ", rank_, "->", dstRank).c_str());
  return ret;
}

c10::intrusive_ptr<Work> ProcessGroupNCCL::recv(
    std::vector<at::Tensor>& tensors,
    int srcRank,
    int /* unused */) {
  TORCH_CHECK(tensors.size() == 1, MULTI_DEVICE_ERROR_MSG);
  // @lint-ignore CLANGTIDY
  auto tensor = tensors.back();
  check_gpu_single_tensor(tensor, true);

  RECORD_PARAM_COMMS_DATA(
      static_cast<int>(
          this->getSequenceNumberForGroup() + 1), // seq + 1 to match collective
      this->getID(),
      tensors, // inputTensors
      tensors, // outputTensors
      srcRank, // src rank
      "recv", // colName
      tensor.numel(), // inNelems
      tensor.numel(), // outNelems
      tensor.scalar_type(), // dType
      std::vector<int64_t>(), // inSplitSizes
      std::vector<int64_t>(), // outSplitSizes
      globalRankStart, // globalRankStart
      globalRankStride, // globalRankStride
      this->getSize()); // worldSize

  auto ret = pointToPoint(
      tensor,
      [&](at::Tensor& output,
          ncclComm_t comm,
          at::cuda::CUDAStream& stream,
          int src) {
        torch::cuda::nccl::recv(output, comm, stream, src);
        return ncclSuccess;
      },
      srcRank,
      OpType::RECV,
      c10::str("nccl:recv ", rank_, "<-", srcRank).c_str());
  return ret;
}

void ProcessGroupNCCL::groupStart() {
  C10D_NCCL_CHECK(ncclGroupStart(), c10::nullopt);
  ++ncclActiveGroupCounter_;
}

void ProcessGroupNCCL::groupEnd() {
  C10D_NCCL_CHECK(ncclGroupEnd(), c10::nullopt);
  --ncclActiveGroupCounter_;
}

void ProcessGroupNCCL::groupEndNonblocking(std::shared_ptr<NCCLComm> comm) {
#ifndef NCCL_HAS_COMM_NONBLOCKING
  C10D_NCCL_CHECK(ncclGroupEnd(), c10::nullopt);
#else
  if (!nccl_use_nonblocking()) {
    C10D_NCCL_CHECK(ncclGroupEnd(), c10::nullopt);
  } else {
    C10D_NCCL_CHECK_TIMEOUT_GROUPEND(ncclGroupEnd(), comm, c10::nullopt);
  }
#endif
  --ncclActiveGroupCounter_;
}

c10::intrusive_ptr<Work> ProcessGroupNCCL::gather(
    std::vector<std::vector<at::Tensor>>& outputTensors,
    std::vector<at::Tensor>& inputTensors,
    const GatherOptions& opts) {
  static auto invalidArgument = [](const std::string& msg) {
    C10_THROW_ERROR(ValueError, "ProcessGroupNCCL::gather: " + msg);
  };

  assertRootRank(invalidArgument, opts.rootRank, size_);

  TORCH_CHECK(inputTensors.size() == 1, MULTI_DEVICE_ERROR_MSG);
  // @lint-ignore CLANGTIDY
  auto inputTensor = inputTensors.back();

  std::vector<at::Tensor> outputs;

  if (getRank() == opts.rootRank) {
    if (outputTensors.size() != 1) {
      std::stringstream ss;
      ss << "requires a single-element output list containing a list with "
         << getSize() << " tensors.";
      invalidArgument(ss.str());
    } else if (outputTensors[0].size() != static_cast<size_t>(getSize())) {
      std::stringstream ss;
      ss << "Incorrect output list size " << outputTensors[0].size()
         << ". Output list size should be " << getSize()
         << ", same as size of the process group.";
      invalidArgument(ss.str());
    }

    const auto& options = inputTensor.options();
    const auto& sizes = inputTensor.sizes();
    assertTypeAndSizesMatch(invalidArgument, outputTensors[0], options, sizes);
    outputs = outputTensors[0];
  } else {
    // if not in the root rank, initialize outputs as empty list
    if (outputTensors.size() != 0) {
      invalidArgument("requires empty output on non-root");
    }
    outputs = {};
    // append a empty tensor to the list, we don't use it but the
    // `collective` template function requires it to invoke its function
    outputs.emplace_back();
  }

  RECORD_PARAM_COMMS_DATA(
      static_cast<int>(
          this->getSequenceNumberForGroup() + 1), // seq + 1 to match collective
      this->getID(),
      inputTensors, // inputTensors
      outputTensors, // outputTensors
      opts.rootRank, // root rank
      "gather", // colName
      inputTensor.numel(), // inNelems
      inputTensor.numel() * this->getSize(), // outNelems
      inputTensor.scalar_type(), // dType
      std::vector<int64_t>(), // inSplitSizes
      std::vector<int64_t>(), // outSplitSize
      globalRankStart, // globalRankStart
      globalRankStride, // globalRankStride
      this->getSize()); // worldSize

  // avoidRecordStreams_ note: collective() will stash inputTensors and
  // outputs, which == outputTensors[0] on the root rank where it matters.
  return collective(
      inputTensor,
      outputs[0], // just to fit the collective interface
      [&](at::Tensor& /* unused */,
          at::Tensor& /* unused */,
          ncclComm_t comm,
          at::cuda::CUDAStream& stream) {
        const auto root = opts.rootRank;
        if (getRank() == root) {
          if (!avoidRecordStreams_) {
            for (auto output : outputs) {
              c10::cuda::CUDACachingAllocator::recordStream(
                  output.storage().data_ptr(), stream);
            }
          }
        }
        torch::cuda::nccl::gather(inputTensor, outputs, comm, stream, root);
        return ncclSuccess;
      },
      OpType::GATHER,
      "nccl:gather");
}

c10::intrusive_ptr<Work> ProcessGroupNCCL::scatter(
    std::vector<at::Tensor>& outputTensors,
    std::vector<std::vector<at::Tensor>>& inputTensors,
    const ScatterOptions& opts) {
  static auto invalidArgument = [](const std::string& msg) {
    C10_THROW_ERROR(ValueError, "ProcessGroupNCCL::scatter: " + msg);
  };

  assertRootRank(invalidArgument, opts.rootRank, size_);

  TORCH_CHECK(outputTensors.size() == 1, MULTI_DEVICE_ERROR_MSG);
  auto outputTensor = outputTensors.back();

  std::vector<at::Tensor> inputs;

  if (getRank() == opts.rootRank) {
    if (inputTensors.size() != 1) {
      std::stringstream ss;
      ss << "requires a single-element input list containing a list with "
         << getSize() << " tensors.";
      invalidArgument(ss.str());
    } else if (inputTensors[0].size() != static_cast<size_t>(getSize())) {
      std::stringstream ss;
      ss << "Incorrect input list size " << inputTensors[0].size()
         << ". Input list size should be " << getSize()
         << ", same as size of the process group.";
      invalidArgument(ss.str());
    }

    const auto& options = outputTensor.options();
    const auto& sizes = outputTensor.sizes();
    assertTypeAndSizesMatch(invalidArgument, inputTensors[0], options, sizes);
    inputs = inputTensors[0];
  } else {
    // if not in the root rank, initialize inputTensors as empty place holder
    // with an empty list
    if (inputTensors.size() != 0) {
      invalidArgument("requires empty input on non-root");
    }
    inputs = {};
    // append a empty tensor to the list, we don't use it but the
    // `collective` template function requires it to invoke its function
    inputs.emplace_back();
  }

  RECORD_PARAM_COMMS_DATA(
      static_cast<int>(
          this->getSequenceNumberForGroup() + 1), // seq + 1 to match collective
      this->getID(),
      inputTensors, // inputTensors
      outputTensors, // outputTensors
      opts.rootRank, // root rank
      "scatter", // colName
      outputTensor.numel() * this->getSize(), // inNelems
      outputTensor.numel(), // outNelems
      outputTensor.scalar_type(), // dType
      std::vector<int64_t>(), // inSplitSizes
      std::vector<int64_t>(), // outSplitSize
      globalRankStart, // globalRankStart
      globalRankStride, // globalRankStride
      this->getSize()); // worldSize

  // avoidRecordStreams_ note: collective() will stash outputTensors and
  // inputs, which == inputTensors[0] on the root rank where it matters.
  bool avoidRecordStreams = avoidRecordStreams_ || (!opts.asyncOp);

  return collective(
      outputTensor,
      inputs[0], // just to fit the collective interface
      [&](at::Tensor& /* unused */,
          at::Tensor& /* unused */,
          ncclComm_t comm,
          at::cuda::CUDAStream& stream) {
        const auto root = opts.rootRank;
        if (getRank() == root) {
          if (!avoidRecordStreams) {
            for (auto input : inputs) {
              c10::cuda::CUDACachingAllocator::recordStream(
                  input.storage().data_ptr(), stream);
            }
          }
        }
        torch::cuda::nccl::scatter(inputs, outputTensor, comm, stream, root);
        return ncclSuccess;
      },
      OpType::SCATTER,
      "nccl:scatter",
      avoidRecordStreams);
}

c10::intrusive_ptr<Work> ProcessGroupNCCL::recvAnysource(
    std::vector<at::Tensor>& /* unused */,
    int /* unused */) {
  C10_THROW_ERROR(
      NotImplementedError, "ProcessGroupNCCL does not support recvAnysource");
}

c10::intrusive_ptr<Work> ProcessGroupNCCL::_allgather_base(
    at::Tensor& output_tensor,
    at::Tensor& input_tensor,
    const AllgatherOptions& opts) {
  check_gpu_single_tensor(input_tensor);
  check_gpu_single_tensor(output_tensor);

  if (input_tensor.dtype() != output_tensor.dtype()) {
    C10_THROW_ERROR(
        TypeError, "output tensor must have the same type as input tensor");
  }

  if (input_tensor.numel() * size_ != output_tensor.numel()) {
    C10_THROW_ERROR(
        ValueError,
        "output tensor size must be equal to world_size times input tensor size");
  }

  RECORD_PARAM_COMMS_DATA(
      static_cast<int>(
          this->getSequenceNumberForGroup() + 1), // seq + 1 to match collective
      this->getID(),
      input_tensor, // inputTensors
      output_tensor, // outputTensors
      rank_, // rank
      "_allgather_base", // colName
      input_tensor.numel(), // inNelems
      output_tensor.numel(), // outNelems
      output_tensor.scalar_type(), // dType
      std::vector<int64_t>(), // inSplitSizes
      std::vector<int64_t>(), // outSplitSize
      globalRankStart, // globalRankStart
      globalRankStride, // globalRankStride
      this->getSize()); // worldSize

  // avoidRecordStreams_ note: collective() will stash inputs and outputs.
  // Note 2: for asyncOp = false, we don't want to record streams because we
  // know that the NCCL stream will join back to the "current" stream right
  // after this op. So we might just as well keep the stream ownership of the
  // input/output tensors unchanged. The benefit would be that the
  // allocation/free of the tensors would look deterministic to the "current"
  // stream so that the caching allocator can reuse memory pool for this stream
  // in a clever way. This setting is added for libraries like FSDP which uses
  // `all_gather_into_tensor`.
  bool avoidRecordStreams = avoidRecordStreams_ || (!opts.asyncOp);

  return collective(
      input_tensor,
      output_tensor,
      [&](at::Tensor& input,
          at::Tensor& output,
          ncclComm_t comm,
          at::cuda::CUDAStream& stream) {
        if (!avoidRecordStreams) {
          c10::cuda::CUDACachingAllocator::recordStream(
              output.storage().data_ptr(), stream);
        }
        return ncclAllGather(
            input.data_ptr(),
            output.data_ptr(),
            input.numel(),
            getNcclDataType(input.scalar_type()),
            comm,
            stream.stream());
      },
      OpType::_ALLGATHER_BASE,
      "nccl:_all_gather_base",
      avoidRecordStreams);
}

} // namespace c10d

#endif // USE_C10D_NCCL
