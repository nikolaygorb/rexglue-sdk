/**
 ******************************************************************************
 * Xenia : Xbox 360 Emulator Research Project                                 *
 ******************************************************************************
 * Copyright 2022 Ben Vanik. All rights reserved.                             *
 * Released under the BSD license - see LICENSE in the root for more details. *
 ******************************************************************************
 *
 * @modified    Tom Clay, 2026 - Adapted for ReXGlue runtime
 */

#include <algorithm>
#include <chrono>
#include <cstddef>
#include <vector>

#include <rex/chrono/clock.h>
#include <rex/stream.h>
#include <rex/system/kernel_state.h>
#include <rex/system/util/string_utils.h>  // For TranslateAnsiStringAddress
#include <rex/system/xobject.h>
// #include <rex/kernel/xboxkrnl/private.h>  // TODO: JIT only
#include <rex/system/xenumerator.h>
#include <rex/system/xevent.h>
#include <rex/system/xfile.h>
#include <rex/system/xmodule.h>
#include <rex/system/xmutant.h>
#include <rex/system/xnotifylistener.h>
#include <rex/system/xsemaphore.h>
#include <rex/system/xsymboliclink.h>
#include <rex/system/xthread.h>

namespace rex::system {

XObject::XObject(Type type) : kernel_state_(nullptr), pointer_ref_count_(1), type_(type) {
  handles_.reserve(10);
}

XObject::XObject(KernelState* kernel_state, Type type, bool host_object)
    : kernel_state_(kernel_state),
      host_object_(host_object),
      type_(type),
      pointer_ref_count_(1),
      guest_object_ptr_(0),
      allocated_guest_object_(false) {
  handles_.reserve(10);

  // TODO: Assert kernel_state != nullptr in this constructor.
  if (kernel_state) {
    kernel_state->object_table()->AddHandle(this, nullptr);
  }
}

XObject::~XObject() {
  // TODO: these are being asserted true... find out why
  // assert_true(handles_.empty());
  // assert_zero(pointer_ref_count_);

  if (allocated_guest_object_) {
    uint32_t ptr = guest_object_ptr_ - sizeof(X_OBJECT_HEADER);
    auto header = memory()->TranslateVirtual<X_OBJECT_HEADER*>(ptr);

    // Free the placeholder type; a kernel type stays.
    if (allocated_object_type_ && header->object_type_ptr) {
      memory()->SystemHeapFree(header->object_type_ptr);
    }

    memory()->SystemHeapFree(ptr);
  }
}

rex::Runtime* XObject::emulator() const {
  return kernel_state_->emulator_;
}
KernelState* XObject::kernel_state() const {
  return kernel_state_;
}
rex::memory::Memory* XObject::memory() const {
  return kernel_state_->memory();
}

XObject::Type XObject::type() const {
  return type_;
}

void XObject::RetainHandle() {
  kernel_state_->object_table()->RetainHandle(handles_[0]);
}

bool XObject::ReleaseHandle() {
  // FIXME: Return true when handle is actually released.
  return kernel_state_->object_table()->ReleaseHandle(handles_[0]) == X_STATUS_SUCCESS;
}

void XObject::Retain() {
  ++pointer_ref_count_;
}

void XObject::Release() {
  if (--pointer_ref_count_ == 0) {
    delete this;
  }
}

X_STATUS XObject::Delete() {
  if (kernel_state_ == nullptr) {
    // Fake return value for api-scanner
    return X_STATUS_SUCCESS;
  } else {
    if (!name_.empty()) {
      kernel_state_->object_table()->RemoveNameMapping(name_);
    }
    return kernel_state_->object_table()->RemoveHandle(handles_[0]);
  }
}

bool XObject::SaveObject(stream::ByteStream* stream) {
  stream->Write<uint32_t>(allocated_guest_object_);
  stream->Write<uint32_t>(guest_object_ptr_);

  stream->Write(uint32_t(handles_.size()));
  stream->Write(&handles_[0], handles_.size() * sizeof(X_HANDLE));

  return true;
}

bool XObject::RestoreObject(stream::ByteStream* stream) {
  allocated_guest_object_ = stream->Read<uint32_t>() > 0;
  guest_object_ptr_ = stream->Read<uint32_t>();

  handles_.resize(stream->Read<uint32_t>());
  stream->Read(&handles_[0], handles_.size() * sizeof(X_HANDLE));

  // Restore our pointer to our handles in the object table.
  for (size_t i = 0; i < handles_.size(); i++) {
    kernel_state_->object_table()->RestoreHandle(handles_[i], this);
  }

  return true;
}

object_ref<XObject> XObject::Restore(KernelState* kernel_state, Type type,
                                     stream::ByteStream* stream) {
  switch (type) {
    case Type::Enumerator:
      break;
    case Type::Event:
      return XEvent::Restore(kernel_state, stream);
    case Type::File:
      return XFile::Restore(kernel_state, stream);
    case Type::IOCompletion:
      break;
    case Type::Module:
      return XModule::Restore(kernel_state, stream);
    case Type::Mutant:
      return XMutant::Restore(kernel_state, stream);
    case Type::NotifyListener:
      return XNotifyListener::Restore(kernel_state, stream);
    case Type::Semaphore:
      return XSemaphore::Restore(kernel_state, stream);
    case Type::Session:
      break;
    case Type::Socket:
      break;
    case Type::SymbolicLink:
      return XSymbolicLink::Restore(kernel_state, stream);
    case Type::Thread:
      return XThread::Restore(kernel_state, stream);
    case Type::Timer:
      break;
    case Type::Undefined:
      break;
  }

  assert_always("No restore handler exists for this object!");
  return nullptr;
}

void XObject::SetAttributes(uint32_t obj_attributes_ptr) {
  if (!obj_attributes_ptr) {
    return;
  }

  auto name = util::TranslateAnsiStringAddress(
      memory(),
      memory::load_and_swap<uint32_t>(memory()->TranslateVirtual(obj_attributes_ptr + 4)));
  if (!name.empty()) {
    set_name(std::string(name));
    // A host object's own handle must never reach the guest.
    auto guest_handle = std::find_if(handles_.begin(), handles_.end(),
                                     [](X_HANDLE h) { return !is_handle_host_object(h); });
    kernel_state_->object_table()->AddNameMapping(
        name_, guest_handle != handles_.end() ? *guest_handle : handles_[0]);
  }
}

uint32_t XObject::TimeoutTicksToMs(int64_t timeout_ticks) {
  if (timeout_ticks > 0) {
    // Absolute time, based on January 1, 1601.
    // TODO(benvanik): convert time to relative time.
    assert_always();
    return 0;
  } else if (timeout_ticks < 0) {
    // Relative time.
    return (uint32_t)(-timeout_ticks / 10000);  // Ticks -> MS
  } else {
    return 0;
  }
}

namespace {

// The rest of a host wait's timeout after it was woken for a kernel APC.
class WaitTimeout {
 public:
  explicit WaitTimeout(std::chrono::milliseconds timeout)
      : timeout_(timeout), start_(std::chrono::steady_clock::now()) {}
  std::chrono::milliseconds Remaining() const {
    if (timeout_ == std::chrono::milliseconds::max()) {
      return timeout_;
    }
    auto elapsed = std::chrono::duration_cast<std::chrono::milliseconds>(
        std::chrono::steady_clock::now() - start_);
    return elapsed < timeout_ ? timeout_ - elapsed : std::chrono::milliseconds(0);
  }

 private:
  std::chrono::milliseconds timeout_;
  std::chrono::steady_clock::time_point start_;
};

// An alertable host wait was woken by a queued APC. A kernel APC runs and the
// wait goes on for the rest of its timeout; only a pending user APC ends it
// (edge 7598c1779). A host thread keeps the old behaviour.
bool ApcEndsWait() {
  if (!XThread::IsInThread()) {
    return true;
  }
  return XThread::GetCurrentThread()->OnAlertableWaitWoken();
}

}  // namespace

X_STATUS XObject::Wait(uint32_t wait_reason, uint32_t processor_mode, uint32_t alertable,
                       uint64_t* opt_timeout) {
  auto wait_handle = GetWaitHandle();
  if (!wait_handle) {
    // Object doesn't support waiting.
    return X_STATUS_SUCCESS;
  }

  auto timeout_ms = opt_timeout ? std::chrono::milliseconds(chrono::Clock::ScaleGuestDurationMillis(
                                      TimeoutTicksToMs(*opt_timeout)))
                                : std::chrono::milliseconds::max();

  XThread::CheckTitleTermination();
  WaitTimeout timeout(timeout_ms);
  rex::thread::WaitResult result;
  do {
    result = rex::thread::Wait(wait_handle, alertable ? true : false, timeout.Remaining());
    XThread::CheckTitleTermination();
  } while (result == rex::thread::WaitResult::kUserCallback && !ApcEndsWait());
  switch (result) {
    case rex::thread::WaitResult::kSuccess:
      WaitCallback();
      return X_STATUS_SUCCESS;
    case rex::thread::WaitResult::kUserCallback:
      // Or X_STATUS_ALERTED?
      return X_STATUS_USER_APC;
    case rex::thread::WaitResult::kTimeout:
      rex::thread::MaybeYield();
      return X_STATUS_TIMEOUT;
    default:
    case rex::thread::WaitResult::kAbandoned:
    case rex::thread::WaitResult::kFailed:
      return X_STATUS_ABANDONED_WAIT_0;
  }
}

X_STATUS XObject::SignalAndWait(XObject* signal_object, XObject* wait_object, uint32_t wait_reason,
                                uint32_t processor_mode, uint32_t alertable,
                                uint64_t* opt_timeout) {
  auto timeout_ms = opt_timeout ? std::chrono::milliseconds(chrono::Clock::ScaleGuestDurationMillis(
                                      TimeoutTicksToMs(*opt_timeout)))
                                : std::chrono::milliseconds::max();

  WaitTimeout timeout(timeout_ms);
  auto result =
      rex::thread::SignalAndWait(signal_object->GetWaitHandle(), wait_object->GetWaitHandle(),
                                 alertable ? true : false, timeout_ms);
  // Woken for a kernel APC: the signal is done, the wait goes on.
  while (result == rex::thread::WaitResult::kUserCallback && !ApcEndsWait()) {
    result = rex::thread::Wait(wait_object->GetWaitHandle(), true, timeout.Remaining());
  }
  switch (result) {
    case rex::thread::WaitResult::kSuccess:
      wait_object->WaitCallback();
      return X_STATUS_SUCCESS;
    case rex::thread::WaitResult::kUserCallback:
      // Or X_STATUS_ALERTED?
      return X_STATUS_USER_APC;
    case rex::thread::WaitResult::kTimeout:
      rex::thread::MaybeYield();
      return X_STATUS_TIMEOUT;
    default:
    case rex::thread::WaitResult::kAbandoned:
    case rex::thread::WaitResult::kFailed:
      return X_STATUS_ABANDONED_WAIT_0;
  }
}

X_STATUS XObject::WaitMultiple(uint32_t count, XObject** objects, uint32_t wait_type,
                               uint32_t wait_reason, uint32_t processor_mode, uint32_t alertable,
                               uint64_t* opt_timeout) {
  std::vector<rex::thread::WaitHandle*> wait_handles(count);
  for (size_t i = 0; i < count; ++i) {
    wait_handles[i] = objects[i]->GetWaitHandle();
    assert_not_null(wait_handles[i]);
  }

  auto timeout_ms = opt_timeout ? std::chrono::milliseconds(chrono::Clock::ScaleGuestDurationMillis(
                                      TimeoutTicksToMs(*opt_timeout)))
                                : std::chrono::milliseconds::max();

  XThread::CheckTitleTermination();
  WaitTimeout timeout(timeout_ms);
  if (wait_type) {
    std::pair<rex::thread::WaitResult, size_t> result;
    do {
      result = rex::thread::WaitAny(wait_handles.data(), wait_handles.size(),
                                   alertable ? true : false, timeout.Remaining());
      XThread::CheckTitleTermination();
    } while (result.first == rex::thread::WaitResult::kUserCallback && !ApcEndsWait());
    switch (result.first) {
      case rex::thread::WaitResult::kSuccess:
        objects[result.second]->WaitCallback();

        return X_STATUS(result.second);
      case rex::thread::WaitResult::kUserCallback:
        // Or X_STATUS_ALERTED?
        return X_STATUS_USER_APC;
      case rex::thread::WaitResult::kTimeout:
        rex::thread::MaybeYield();
        return X_STATUS_TIMEOUT;
      default:
      case rex::thread::WaitResult::kAbandoned:
        return X_STATUS(X_STATUS_ABANDONED_WAIT_0 + result.second);
      case rex::thread::WaitResult::kFailed:
        return X_STATUS_UNSUCCESSFUL;
    }
  } else {
    rex::thread::WaitResult result;
    do {
      result = rex::thread::WaitAll(wait_handles.data(), wait_handles.size(),
                                   alertable ? true : false, timeout.Remaining());
      XThread::CheckTitleTermination();
    } while (result == rex::thread::WaitResult::kUserCallback && !ApcEndsWait());
    switch (result) {
      case rex::thread::WaitResult::kSuccess:
        for (uint32_t i = 0; i < count; i++) {
          objects[i]->WaitCallback();
        }

        return X_STATUS_SUCCESS;
      case rex::thread::WaitResult::kUserCallback:
        // Or X_STATUS_ALERTED?
        return X_STATUS_USER_APC;
      case rex::thread::WaitResult::kTimeout:
        rex::thread::MaybeYield();
        return X_STATUS_TIMEOUT;
      default:
      case rex::thread::WaitResult::kAbandoned:
      case rex::thread::WaitResult::kFailed:
        return X_STATUS_ABANDONED_WAIT_0;
    }
  }
}

uint8_t* XObject::CreateNative(uint32_t size) {
  auto global_lock = rex::thread::global_critical_region::AcquireDirect();

  uint32_t total_size = size + sizeof(X_OBJECT_HEADER);

  auto mem = memory()->SystemHeapAlloc(total_size);
  if (!mem) {
    // Out of memory!
    return nullptr;
  }

  allocated_guest_object_ = true;
  memory()->Zero(mem, total_size);
  SetNativePointer(mem + sizeof(X_OBJECT_HEADER), true);

  auto header = memory()->TranslateVirtual<X_OBJECT_HEADER*>(mem);
  header->pointer_count = 1;
  header->handle_count = 1;
  // The kernel's type for the object, as on the console; a placeholder for
  // the kinds that have none (some kernel method reads a member at 0x14).
  uint32_t object_type = KernelObjectType();
  if (!object_type) {
    object_type = memory()->SystemHeapAlloc(sizeof(X_OBJECT_TYPE));
    allocated_object_type_ = object_type != 0;
  }
  header->object_type_ptr = object_type;
  // Objects without a dispatch header (enumerators) have no room in their body
  // for the handle; ObDereferenceObject finds them through this.
  kernel_state_->object_table()->AddNativeHandle(guest_object_ptr_, handle());

  return memory()->TranslateVirtual(guest_object_ptr_);
}

void XObject::SetNativePointer(uint32_t native_ptr, bool uninitialized) {
  auto global_lock = rex::thread::global_critical_region::AcquireDirect();

  // If hit: We've already setup the native ptr with CreateNative!
  assert_zero(guest_object_ptr_);

  auto header = kernel_state_->memory()->TranslateVirtual<X_DISPATCH_HEADER*>(native_ptr);

  // Memory uninitialized, so don't bother with the check.
  if (!uninitialized) {
    assert_true(!(header->wait_list_blink & 0x1));
  }

  // Stash pointer in struct.
  // FIXME: This assumes the object has a dispatch header (some don't!)
  StashHandle(header, handle());

  guest_object_ptr_ = native_ptr;
}

void XObject::SetNativeType() {
  if (allocated_guest_object_) {
    return;
  }
  // ObOpenObjectByPointer takes an Object Manager object, whose body follows an
  // X_OBJECT_HEADER. Generic dispatch lookups must not inspect this prefix.
  if (guest_object_ptr_ < sizeof(X_OBJECT_HEADER)) {
    return;
  }
  const uint32_t header_ptr = guest_object_ptr_ - sizeof(X_OBJECT_HEADER);
  if (memory()->QueryRangeAccess(header_ptr, sizeof(X_OBJECT_HEADER)) ==
      rex::memory::PageAccess::kNoAccess) {
    return;
  }
  guest_object_type_ptr_.store(
      memory()->TranslateVirtual<X_OBJECT_HEADER*>(header_ptr)->object_type_ptr);
}

uint32_t XObject::KernelObjectType() const {
  size_t offset;
  switch (type_) {
    case Type::Thread:
      offset = offsetof(KernelGuestGlobals, ExThreadObjectType);
      break;
    case Type::Event:
      offset = offsetof(KernelGuestGlobals, ExEventObjectType);
      break;
    case Type::Mutant:
      offset = offsetof(KernelGuestGlobals, ExMutantObjectType);
      break;
    case Type::Semaphore:
      offset = offsetof(KernelGuestGlobals, ExSemaphoreObjectType);
      break;
    case Type::Timer:
      offset = offsetof(KernelGuestGlobals, ExTimerObjectType);
      break;
    case Type::IOCompletion:
      offset = offsetof(KernelGuestGlobals, IoCompletionObjectType);
      break;
    case Type::File:
      offset = offsetof(KernelGuestGlobals, IoFileObjectType);
      break;
    case Type::SymbolicLink:
      offset = offsetof(KernelGuestGlobals, ObSymbolicLinkObjectType);
      break;
    default:
      return 0;
  }
  const uint32_t globals = kernel_state_->GetKernelGuestGlobals();
  return globals ? globals + uint32_t(offset) : 0;
}

void XObject::UpdateNativeHandle(X_HANDLE removed_handle) {
  if (!guest_object_ptr_ ||
      memory()->QueryRangeAccess(guest_object_ptr_, sizeof(X_DISPATCH_HEADER)) !=
          rex::memory::PageAccess::kReadWrite) {
    return;
  }
  auto* header = memory()->TranslateVirtual<X_DISPATCH_HEADER*>(guest_object_ptr_);
  if (header->wait_list_flink != kXObjSignature || header->wait_list_blink != removed_handle) {
    return;
  }
  if (!handles_.empty()) {
    StashHandle(header, handles_.front());
  } else {
    const uint32_t wait_list =
        guest_object_ptr_ + uint32_t(offsetof(X_DISPATCH_HEADER, wait_list_flink));
    header->wait_list_flink = wait_list;
    header->wait_list_blink = wait_list;
  }
}

object_ref<XObject> XObject::GetNativeObject(KernelState* kernel_state, void* native_ptr,
                                             int32_t as_type) {
  assert_not_null(native_ptr);

  // Unfortunately the XDK seems to inline some KeInitialize calls, meaning
  // we never see it and just randomly start getting passed events/timers/etc.
  // Luckily it seems like all other calls (Set/Reset/Wait/etc) are used and
  // we don't have to worry about PPC code poking the struct. Because of that,
  // we init on first use, store our handle in the struct, and dereference it
  // each time.
  // We identify this by setting wait_list_flink to a magic value. When set,
  // wait_list_blink will hold a handle to our object.

  auto global_lock = rex::thread::global_critical_region::AcquireDirect();

  auto header = reinterpret_cast<X_DISPATCH_HEADER*>(native_ptr);

  if (as_type == -1) {
    as_type = header->type;
  }

  if (header->wait_list_flink == kXObjSignature) {
    // Already initialized.
    // TODO: assert if the type of the object != as_type
    // Only the object still at this address: the stashed slot may have been
    // freed and reused (canary 291f0c9be).
    uint32_t handle = header->wait_list_blink;
    auto object = object_ref<XObject>(kernel_state->object_table()->LookupNativeObject(
        handle, kernel_state->memory()->HostToGuestVirtual(native_ptr)));
    if (object) {
      // The only place the guest hands us a raw dispatch header, so the only
      // place a direct write to it can be picked up.
      object->SyncFromGuest();
    }

    // TODO(benvanik): assert nothing has been changed in the struct.
    return object;
  } else {
    // First use, create new.
    // https://www.nirsoft.net/kernel_struct/vista/KOBJECTS.html
    XObject* object = nullptr;
    switch (as_type) {
      case 0:  // EventNotificationObject
      case 1:  // EventSynchronizationObject
      {
        // Host objects: an inlined KeInitializeEvent gets no handle on the
        // console, so a guest slot spent here would shift every later handle
        // the title sees (xenia-edge 672bc8b81).
        auto ev = new XEvent(kernel_state, true);
        ev->InitializeNative(native_ptr, header);
        object = ev;
      } break;
      case 2:  // MutantObject
      {
        auto mutant = new XMutant(kernel_state, true);
        mutant->InitializeNative(native_ptr, header);
        object = mutant;
      } break;
      case 5:  // SemaphoreObject
      {
        auto sem = new XSemaphore(kernel_state, true);
        auto success = sem->InitializeNative(native_ptr, header);
        // Can't report failure to the guest at late initialization:
        assert_true(success);
        object = sem;
      } break;
      case 3:   // ProcessObject
      case 4:   // QueueObject
      case 6:   // ThreadObject
      case 7:   // GateObject
      case 8:   // TimerNotificationObject
      case 9:   // TimerSynchronizationObject
      case 18:  // ApcObject
      case 19:  // DpcObject
      case 20:  // DeviceQueueObject
      case 21:  // EventPairObject
      case 22:  // InterruptObject
      case 23:  // ProfileObject
      case 24:  // ThreadedDpcObject
      default:
        assert_always();
        return NULL;
    }

    // Keep the guest address and stash the handle in the struct. The address
    // is what later lookups check the stashed slot against (canary sets it in
    // InitializeNative).
    // FIXME: This assumes the object contains a dispatch header (some don't!)
    object->SetNativePointer(kernel_state->memory()->HostToGuestVirtual(native_ptr), true);

    return object_ref<XObject>(object);
  }
}

}  // namespace rex::system
