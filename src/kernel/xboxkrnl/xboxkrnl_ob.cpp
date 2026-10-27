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

// Disable warnings about unused parameters for kernel functions
#pragma GCC diagnostic ignored "-Wunused-parameter"

#include <cstddef>
#include <cstring>
#include <utility>
#include <vector>

#include <rex/assert.h>
#include <rex/kernel/xboxkrnl/ob.h>
#include <rex/kernel/xboxkrnl/private.h>
#include <rex/logging.h>
#include <rex/hook.h>
#include <rex/types.h>
#include <rex/system/kernel_state.h>
#include <rex/system/util/string_utils.h>
#include <rex/system/xevent.h>
#include <rex/system/xobject.h>
#include <rex/system/xsemaphore.h>
#include <rex/system/xthread.h>
#include <rex/system/xtypes.h>

namespace rex::kernel::xboxkrnl {
using namespace rex::system;

namespace {

// In front of the X_OBJECT_HEADER of a named object.
struct X_OBJECT_HEADER_NAME_INFO {
  rex::be<uint32_t> next_in_directory;
  rex::be<uint32_t> object_directory;
  X_ANSI_STRING name;
};
static_assert_size(X_OBJECT_HEADER_NAME_INFO, 0x10);

// The ten X_OBJECT_TYPEs the kernel defines itself sit together at the start
// of its guest globals, from ExThreadObjectType to ObSymbolicLinkObjectType.
constexpr uint32_t kKernelObjectTypesBegin =
    static_cast<uint32_t>(offsetof(KernelGuestGlobals, ExThreadObjectType));
constexpr uint32_t kKernelObjectTypesEnd = static_cast<uint32_t>(
    offsetof(KernelGuestGlobals, ObSymbolicLinkObjectType) + sizeof(X_OBJECT_TYPE));
static_assert(kKernelObjectTypesEnd - kKernelObjectTypesBegin == 10 * sizeof(X_OBJECT_TYPE),
              "the kernel's object types are no longer one block");

// An object type a title defined itself, as read from its guest memory.
struct TitleObjectType {
  uint32_t type_ptr;
  uint32_t close_proc;
  uint32_t delete_proc;
};

// Reads a title-defined type recorded by ObOpenObjectByPointer. Dispatch
// objects created directly in guest memory need not have an Object Manager
// header; closing them must not speculate about the bytes before their body.
bool ReadTitleObjectType(const XObject& object, TitleObjectType* out_type) {
  const uint32_t type_ptr = object.guest_object_type();
  if (!type_ptr || REX_KERNEL_MEMORY()->QueryRangeAccess(type_ptr, sizeof(X_OBJECT_TYPE)) ==
                       rex::memory::PageAccess::kNoAccess) {
    return false;
  }
  const auto* type = REX_KERNEL_MEMORY()->TranslateVirtual<const X_OBJECT_TYPE*>(type_ptr);
  const uint32_t globals = REX_KERNEL_STATE()->GetKernelGuestGlobals();
  const bool kernel_type =
      type_ptr >= globals + kKernelObjectTypesBegin && type_ptr < globals + kKernelObjectTypesEnd;
  if (!type->constructor || kernel_type) {
    return false;
  }
  *out_type = {type_ptr, type->unk_08, type->unk_0C};
  return true;
}

bool HasTitleObjectType(const XObject& object) {
  TitleObjectType type;
  return ReadTitleObjectType(object, &type);
}

// Only call aligned guest functions the recompiled modules have. The type
// metadata may remain readable after its callback address becomes invalid.
bool IsGuestFunction(uint32_t address) {
  return address && !(address & 3) &&
         REX_KERNEL_STATE()->function_dispatcher()->GetFunction(address) != nullptr;
}

// Splits the first component off an object path, skipping one leading
// separator (xenia-edge 20b97ca61).
void ObSplitName(const X_ANSI_STRING& input, X_ANSI_STRING* leading, X_ANSI_STRING* remaining) {
  const uint32_t pointer = input.pointer;
  const uint16_t length = input.length;
  const char* text = REX_KERNEL_MEMORY()->TranslateVirtual<const char*>(pointer);
  const uint16_t start = (length && text[0] == '\\') ? 1 : 0;
  uint16_t end = start;
  while (end < length && text[end] != '\\') {
    ++end;
  }
  leading->pointer = pointer + start;
  leading->length = uint16_t(end - start);
  leading->maximum_length = uint16_t(end - start);
  const uint16_t rest = end < length ? uint16_t(end + 1) : end;
  remaining->pointer = pointer + rest;
  remaining->length = uint16_t(length - rest);
  remaining->maximum_length = uint16_t(length - rest);
}

// Guest objects whose last reference was dropped on this thread; NtClose and
// ObDereferenceObject run their type's delete and free procedures afterwards,
// outside the object table's lock (xenia-edge db3cbc2c4).
struct PendingGuestObjectDelete {
  uint32_t body;
  uint32_t type_ptr;
};
thread_local std::vector<PendingGuestObjectDelete> pending_guest_object_deletes;

// An object of a type the title defined itself, inserted with ObInsertObject.
// It has no dispatch header to keep a handle in: the object table names it and
// finds it by its body.
class XGuestObject : public XObject {
 public:
  XGuestObject(KernelState* kernel_state, uint32_t body, uint32_t type_ptr)
      : XObject(kernel_state, Type::Undefined) {
    SetGuestBody(body, type_ptr);
  }
  ~XGuestObject() override {
    // A host thread can't run guest code; at teardown the memory goes anyway.
    if (XThread::IsInThread()) {
      pending_guest_object_deletes.push_back({guest_object(), guest_object_type()});
    }
  }
};

// Calls the type's allocate procedure; returns the allocation or 0.
uint32_t AllocateObjectMemory(const X_OBJECT_TYPE& type, uint32_t size, uint32_t pool) {
  if (!IsGuestFunction(type.constructor) || !XThread::IsInThread()) {
    return 0;
  }
  uint64_t args[] = {size, uint32_t(type.pool_tag), pool};
  return static_cast<uint32_t>(REX_KERNEL_STATE()->function_dispatcher()->Execute(
      XThread::GetCurrentThread()->thread_state(), type.constructor, args, 3));
}

// Runs the delete and free procedures for the guest objects this thread
// released.
void RunPendingGuestObjectDeletes() {
  // The procedures may release more objects: take the queue first, a nested
  // call runs those.
  std::vector<PendingGuestObjectDelete> pending = std::move(pending_guest_object_deletes);
  pending_guest_object_deletes.clear();
  if (pending.empty() || !XThread::IsInThread()) {
    return;
  }
  auto* thread_state = XThread::GetCurrentThread()->thread_state();
  auto* dispatcher = REX_KERNEL_STATE()->function_dispatcher();
  for (const PendingGuestObjectDelete& object : pending) {
    const auto* type = REX_KERNEL_MEMORY()->TranslateVirtual<const X_OBJECT_TYPE*>(object.type_ptr);
    const uint32_t delete_proc = type->unk_0C;
    const uint32_t free_proc = type->destructor;
    if (IsGuestFunction(delete_proc)) {
      uint64_t args[] = {object.body};
      dispatcher->Execute(thread_state, delete_proc, args, 1);
    }
    // A named object's allocation starts with its name info.
    uint32_t allocation = object.body - uint32_t(sizeof(X_OBJECT_HEADER));
    if (REX_KERNEL_MEMORY()->TranslateVirtual<X_OBJECT_HEADER*>(allocation)->flags &
        kObjectHeaderNamedObject) {
      allocation -= uint32_t(sizeof(X_OBJECT_HEADER_NAME_INFO));
    }
    if (IsGuestFunction(free_proc)) {
      uint64_t args[] = {allocation};
      dispatcher->Execute(thread_state, free_proc, args, 1);
    }
  }
}

}  // namespace

u32 KeGetCurrentProcessType_entry();  // xboxkrnl_threading.cpp

// A title creates an object of a type it defined itself: memory from the
// type's allocate procedure, with the Object Manager header (and the name) in
// front of the body (canary 275454089).
u32 ObCreateObject_entry(ppc_ptr_t<X_OBJECT_TYPE> object_type,
                         ppc_ptr_t<X_OBJECT_ATTRIBUTES> attributes, u32 object_size,
                         mapped_u32 out_object) {
  *out_object = 0;
  uint16_t flags = 0;
  uint32_t pool = 2;
  auto title_pool = [&]() {
    if (KeGetCurrentProcessType_entry() == X_PROCTYPE_USER) {
      pool = 1;
      flags = kObjectHeaderTitleObject;
    }
  };
  if (!attributes) {
    title_pool();
  } else if (attributes->attributes & 0x1000) {
    pool = 1;
    flags = kObjectHeaderTitleObject;
  } else if (!(attributes->attributes & 0x2000)) {
    title_pool();
  }
  const uint32_t type_ptr = object_type.guest_address();

  if (!attributes || !attributes->name_ptr) {
    uint32_t allocation =
        AllocateObjectMemory(*object_type, object_size + sizeof(X_OBJECT_HEADER), pool);
    if (!allocation) {
      return X_STATUS_INSUFFICIENT_RESOURCES;
    }
    auto* header = REX_KERNEL_MEMORY()->TranslateVirtual<X_OBJECT_HEADER*>(allocation);
    header->pointer_count = 1;
    header->handle_count = 0;
    header->object_type_ptr = type_ptr;
    header->flags = flags;
    *out_object = allocation + sizeof(X_OBJECT_HEADER);
    return X_STATUS_SUCCESS;
  }

  // The object's name is the last component of the path.
  X_ANSI_STRING remaining =
      *REX_KERNEL_MEMORY()->TranslateVirtual<X_ANSI_STRING*>(attributes->name_ptr);
  X_ANSI_STRING name;
  name.reset();
  while (remaining.length) {
    ObSplitName(remaining, &name, &remaining);
    if (remaining.length &&
        *REX_KERNEL_MEMORY()->TranslateVirtual<char*>(remaining.pointer) == '\\') {
      return X_STATUS_OBJECT_NAME_INVALID;
    }
  }
  if (!name.length) {
    return X_STATUS_OBJECT_NAME_INVALID;
  }

  // The name info, header, body and name string in one allocation.
  const uint32_t aligned_size = (object_size + 3) & ~3u;
  uint32_t allocation = AllocateObjectMemory(
      *object_type,
      name.length + aligned_size + uint32_t(sizeof(X_OBJECT_HEADER_NAME_INFO)) +
          uint32_t(sizeof(X_OBJECT_HEADER)),
      pool);
  if (!allocation) {
    return X_STATUS_INSUFFICIENT_RESOURCES;
  }
  const uint32_t header_ptr = allocation + sizeof(X_OBJECT_HEADER_NAME_INFO);
  const uint32_t body_ptr = header_ptr + sizeof(X_OBJECT_HEADER);
  const uint32_t name_ptr = body_ptr + aligned_size;
  auto* name_info =
      REX_KERNEL_MEMORY()->TranslateVirtual<X_OBJECT_HEADER_NAME_INFO*>(allocation);
  name_info->next_in_directory = 0;
  name_info->object_directory = 0;
  name_info->name.pointer = name_ptr;
  name_info->name.length = name.length;
  name_info->name.maximum_length = name.length;
  std::memcpy(REX_KERNEL_MEMORY()->TranslateVirtual(name_ptr),
              REX_KERNEL_MEMORY()->TranslateVirtual(name.pointer), name.length);
  auto* header = REX_KERNEL_MEMORY()->TranslateVirtual<X_OBJECT_HEADER*>(header_ptr);
  header->pointer_count = 1;
  header->handle_count = 0;
  header->object_type_ptr = type_ptr;
  header->flags = uint16_t((flags & 0xFFFE) | kObjectHeaderNamedObject);
  *out_object = body_ptr;
  return X_STATUS_SUCCESS;
}

u32 ObOpenObjectByName_entry(mapped_void obj_attributes_ptr, mapped_void object_type_ptr, u32 unk,
                             mapped_u32 handle_ptr) {
  // r3 = ptr to info?
  //   +0 = -4
  //   +4 = name ptr
  //   +8 = 0
  // r4 = ExEventObjectType | ExSemaphoreObjectType | ExTimerObjectType
  // r5 = 0
  // r6 = out_ptr (handle?)

  if (!obj_attributes_ptr) {
    return X_STATUS_INVALID_PARAMETER;
  }

  auto obj_attributes = REX_KERNEL_MEMORY()->TranslateVirtual<X_OBJECT_ATTRIBUTES*>(
      obj_attributes_ptr.guest_address());
  assert_true(obj_attributes->name_ptr != 0);
  auto name = util::TranslateAnsiStringAddress(REX_KERNEL_MEMORY(), obj_attributes->name_ptr);

  X_HANDLE handle = X_INVALID_HANDLE_VALUE;
  X_STATUS result = REX_KERNEL_OBJECTS()->GetObjectByName(name, &handle);
  if (XSUCCEEDED(result)) {
    *handle_ptr = handle;
  }

  return result;
}

// Gives an object created with ObCreateObject a handle (its creation reference)
// and registers its name; the caller gets |pointer_bias| more references,
// which it drops with ObDereferenceObject (xenia-edge db3cbc2c4).
u32 ObInsertObject_entry(u32 object, ppc_ptr_t<X_OBJECT_ATTRIBUTES> attributes, u32 pointer_bias,
                         mapped_u32 out_handle) {
  if (!object || !out_handle || object < sizeof(X_OBJECT_HEADER)) {
    return X_STATUS_INVALID_PARAMETER;
  }
  const uint32_t header_ptr = object - uint32_t(sizeof(X_OBJECT_HEADER));
  if (REX_KERNEL_MEMORY()->QueryRangeAccess(header_ptr, sizeof(X_OBJECT_HEADER)) ==
      rex::memory::PageAccess::kNoAccess) {
    return X_STATUS_INVALID_PARAMETER;
  }
  const uint32_t type_ptr =
      REX_KERNEL_MEMORY()->TranslateVirtual<X_OBJECT_HEADER*>(header_ptr)->object_type_ptr;
  auto global_lock = rex::thread::global_critical_region::AcquireDirect();
  auto guest_object =
      object_ref<XGuestObject>(new XGuestObject(REX_KERNEL_STATE(), object, type_ptr));
  auto* object_table = REX_KERNEL_OBJECTS();
  const X_HANDLE handle = guest_object->handle();
  object_table->AddNativeHandle(object, handle);
  // The handle is the title's first open of it: counted for the close
  // procedure (the extra slot reference OpenGuestHandle takes is dropped).
  if (XSUCCEEDED(object_table->OpenGuestHandle(handle, guest_object.get(),
                                               HasTitleObjectType(*guest_object)))) {
    object_table->ReleaseHandle(handle);
  }
  for (uint32_t i = 0; i < pointer_bias; ++i) {
    guest_object->RetainHandle();
  }
  guest_object->SetAttributes(attributes.guest_address());
  *out_handle = handle;
  return X_STATUS_SUCCESS;
}

u32 ObOpenObjectByPointer_entry(mapped_void object_ptr, mapped_u32 out_handle_ptr) {
  *out_handle_ptr = 0;
  auto global_lock = rex::thread::global_critical_region::AcquireDirect();
  auto object = XObject::GetNativeObject<XObject>(REX_KERNEL_STATE(), object_ptr);
  if (!object) {
    return X_STATUS_UNSUCCESSFUL;
  }

  // Keep the guest type for ObReferenceObjectByHandle and NtClose (canary
  // 04f85416e).
  object->SetNativeType();

  auto* object_table = REX_KERNEL_OBJECTS();
  const bool title_type = HasTitleObjectType(*object);
  // A host object's own handle is not in the title's numbering and must never
  // reach the guest: it gets a guest one, which holds this open (xenia-edge
  // 672bc8b81).
  if (object->is_host_object()) {
    X_HANDLE guest_handle = X_INVALID_HANDLE_VALUE;
    X_STATUS result = object_table->AddHandle(object.get(), &guest_handle, true);
    if (XFAILED(result)) {
      return result;
    }
    // Counted for the close procedure of a title-defined type (the extra slot
    // reference OpenGuestHandle takes is dropped).
    if (title_type &&
        XSUCCEEDED(object_table->OpenGuestHandle(guest_handle, object.get(), true))) {
      object_table->ReleaseHandle(guest_handle);
    }
    *out_handle_ptr = guest_handle;
    return X_STATUS_SUCCESS;
  }

  // Opened until NtClose; counted apart for a title-defined type.
  const X_STATUS result = object_table->OpenGuestHandle(object->handle(), object.get(), title_type);
  if (XFAILED(result)) {
    return result;
  }
  *out_handle_ptr = object->handle();
  return X_STATUS_SUCCESS;
}

u32 ObLookupThreadByThreadId_entry(u32 thread_id, mapped_u32 out_object_ptr) {
  auto thread = REX_KERNEL_STATE()->GetThreadByID(thread_id);
  if (!thread) {
    return X_STATUS_NOT_FOUND;
  }

  // Retain the object. Will be released in ObDereferenceObject.
  thread->RetainHandle();
  *out_object_ptr = thread->guest_object();
  return X_STATUS_SUCCESS;
}

u32 ObReferenceObjectByHandle_entry(u32 handle, u32 object_type_ptr, mapped_u32 out_object_ptr) {
  REXKRNL_IMPORT_TRACE("ObReferenceObjectByHandle", "handle={:#x} type={:#x}", (uint32_t)handle,
                       (uint32_t)object_type_ptr);

  object_ref<XObject> object;

  // Handle pseudo-handles.
  uint32_t handle_val = static_cast<uint32_t>(handle);
  if (handle_val == 0xFFFFFFFE) {
    // CurrentThread pseudo-handle.
    auto thread = XThread::GetCurrentThread();
    if (!thread) {
      return X_STATUS_INVALID_HANDLE;
    }
    object = retain_object(static_cast<XObject*>(thread));
  } else {
    object = REX_KERNEL_OBJECTS()->LookupObject<XObject>(handle);
  }

  if (!object) {
    return X_STATUS_INVALID_HANDLE;
  }

  uint32_t native_ptr = object->guest_object();

  // Type check: the guest type recorded when the object was opened by pointer
  // (a type the title defined itself, canary 04f85416e), else the kernel's type
  // for the host object. Only a plausible title type counts: a dispatch object
  // the title initialised inline has no Object Manager header, so what was read
  // in front of it is not a type.
  if (object_type_ptr) {
    uint32_t globals_base = REX_KERNEL_STATE()->GetKernelGuestGlobals();
    uint32_t expected_type = HasTitleObjectType(*object) ? object->guest_object_type() : 0;
    switch (expected_type ? XObject::Type::Undefined : object->type()) {
      case XObject::Type::Thread:
        expected_type = globals_base + offsetof(KernelGuestGlobals, ExThreadObjectType);
        break;
      case XObject::Type::Event:
        expected_type = globals_base + offsetof(KernelGuestGlobals, ExEventObjectType);
        break;
      case XObject::Type::Mutant:
        expected_type = globals_base + offsetof(KernelGuestGlobals, ExMutantObjectType);
        break;
      case XObject::Type::Semaphore:
        expected_type = globals_base + offsetof(KernelGuestGlobals, ExSemaphoreObjectType);
        break;
      case XObject::Type::Timer:
        expected_type = globals_base + offsetof(KernelGuestGlobals, ExTimerObjectType);
        break;
      default:
        break;
    }
    if (expected_type && object_type_ptr != expected_type) {
      return X_STATUS_OBJECT_TYPE_MISMATCH;
    }
  }

  // Caller takes the reference.
  // It's released in ObDereferenceObject.
  object->RetainHandle();
  if (out_object_ptr.guest_address()) {
    *out_object_ptr = native_ptr;
  }
  REXKRNL_IMPORT_RESULT("ObReferenceObjectByHandle", "0x0 obj={:#x}", native_ptr);
  return X_STATUS_SUCCESS;
}

u32 ObReferenceObjectByName_entry(mapped_string name, u32 attributes, u32 object_type_ptr,
                                  mapped_void parse_context, mapped_u32 out_object_ptr) {
  X_HANDLE handle = X_INVALID_HANDLE_VALUE;
  X_STATUS result = REX_KERNEL_OBJECTS()->GetObjectByName(name.value(), &handle);
  if (XSUCCEEDED(result)) {
    return ObReferenceObjectByHandle_entry(handle, object_type_ptr, out_object_ptr);
  }

  return result;
}

// The object behind a guest object pointer, as ObReferenceObject and
// ObDereferenceObject receive it.
static object_ref<XObject> LookupObjectByNativePointer(uint32_t native_ptr) {
  // The object table knows the handles of objects XObject::CreateNative
  // allocated. Some have no dispatch header (enumerators), so the handle stashed
  // in the body is not reliable for those: it may be overwritten by the object's
  // own fields.
  object_ref<XObject> object;
  if (X_HANDLE handle = REX_KERNEL_OBJECTS()->LookupNativeHandle(native_ptr)) {
    // Also a slot whose guest opens are all closed: it still holds references
    // to drop.
    object = object_ref<XObject>(REX_KERNEL_OBJECTS()->LookupNativeObject(handle, native_ptr));
    if (object) {
      return object;
    }
  }
  if (!object) {
    object = XObject::GetNativeObject<XObject>(REX_KERNEL_STATE(),
                                               REX_KERNEL_MEMORY()->TranslateVirtual(native_ptr));
  }
  return object;
}

u32 ObDereferenceObject_entry(u32 native_ptr) {
  REXKRNL_IMPORT_TRACE("ObDereferenceObject", "ptr={:#x}", (uint32_t)native_ptr);
  // Check if a dummy value from ObReferenceObjectByHandle.
  if (native_ptr == 0xDEADF00D) {
    return 0;
  }

  if (auto object = LookupObjectByNativePointer(native_ptr)) {
    object->ReleaseHandle();
  }
  RunPendingGuestObjectDeletes();

  return 0;
}

void ObReferenceObject_entry(u32 native_ptr) {
  // Check if a dummy value from ObReferenceObjectByHandle.
  if (!native_ptr || native_ptr == 0xDEADF00D) {
    return;
  }

  if (auto object = LookupObjectByNativePointer(native_ptr)) {
    object->RetainHandle();
  } else {
    REXKRNL_WARN("Unregistered guest object provided to ObReferenceObject {:08X}",
                 uint32_t(native_ptr));
  }
}

u32 ObLookupAnyThreadByThreadId_entry(u32 thread_id, mapped_u32 out_object_ptr) {
  return ObLookupThreadByThreadId_entry(thread_id, out_object_ptr);
}

u32 ObCreateSymbolicLink_entry(ppc_ptr_t<X_ANSI_STRING> path_ptr,
                               ppc_ptr_t<X_ANSI_STRING> target_ptr) {
  auto path = xeObSymbolicLinkName(util::TranslateAnsiPath(REX_KERNEL_MEMORY(), path_ptr));
  auto target = rex::string::utf8_canonicalize_guest_path(
      util::TranslateAnsiPath(REX_KERNEL_MEMORY(), target_ptr));

  // A link that is already registered is not an error (4D5307DC expects success).
  std::string existing_target;
  if (REX_KERNEL_FS()->FindSymbolicLink(path, existing_target)) {
    return X_STATUS_SUCCESS;
  }

  if (!REX_KERNEL_FS()->RegisterSymbolicLink(path, target)) {
    return X_STATUS_UNSUCCESSFUL;
  }

  return X_STATUS_SUCCESS;
}

u32 ObDeleteSymbolicLink_entry(ppc_ptr_t<X_ANSI_STRING> path_ptr) {
  auto path = xeObSymbolicLinkName(util::TranslateAnsiPath(REX_KERNEL_MEMORY(), path_ptr));
  if (!REX_KERNEL_FS()->UnregisterSymbolicLink(path)) {
    return X_STATUS_UNSUCCESSFUL;
  }

  return X_STATUS_SUCCESS;
}

static uint32_t NtClose(uint32_t handle) {
  auto* object_table = REX_KERNEL_OBJECTS();
  object_ref<XObject> object;
  TitleObjectType title_type = {};
  int32_t handle_count = 0;
  uint64_t generation = 0;
  {
    auto global_lock = rex::thread::global_critical_region::AcquireDirect();
    object = object_table->LookupObject<XObject>(handle);
    if (!object) {
      return X_STATUS_INVALID_HANDLE;
    }
    const X_STATUS result =
        object_table->ClaimGuestClose(handle, object.get(), &handle_count, &generation);
    if (XFAILED(result)) {
      return result;
    }
    // Kernel types keep reference-based closing.
    if (handle_count < 0) {
      return object_table->ReleaseHandleOf(handle, object.get(), generation);
    }
    // The open has been consumed even if the type is now unreadable.
    if (!ReadTitleObjectType(*object, &title_type)) {
      REXKRNL_DEBUG("NtClose({:08X}): guest object type is unavailable", handle);
    }
  }

  // A type the title defined itself: its close procedure runs with the open
  // count before this close (canary 291f0c9be).
  if (title_type.close_proc) {
    if (!IsGuestFunction(title_type.close_proc)) {
      REXKRNL_DEBUG("Object type {:08X}: close procedure {:08X} is not guest code",
                    title_type.type_ptr, title_type.close_proc);
    } else if (XThread::IsInThread()) {
      auto* thread = XThread::GetCurrentThread();
      thread->EnterCriticalRegion();
      uint64_t args[] = {object->guest_object(), static_cast<uint32_t>(handle_count)};
      REX_KERNEL_STATE()->function_dispatcher()->Execute(thread->thread_state(),
                                                         title_type.close_proc, args, 2);
      thread->LeaveCriticalRegion();
    } else {
      REXKRNL_DEBUG("NtClose({:08X}): guest close procedure skipped on a host thread", handle);
    }
  }
  if (title_type.delete_proc && !dynamic_cast<XGuestObject*>(object.get())) {
    REXKRNL_DEBUG("Object type {:08X}: delete procedure {:08X} runs only for inserted objects",
                  title_type.type_ptr, title_type.delete_proc);
  }
  // A reentrant callback may have released the slot or caused it to be reused.
  object_table->ReleaseHandleOf(handle, object.get(), generation);
  object.reset();
  RunPendingGuestObjectDeletes();
  return X_STATUS_SUCCESS;
}

u32 NtDuplicateObject_entry(u32 handle, mapped_u32 new_handle_ptr, u32 options) {
  // NOTE: new_handle_ptr may be zero.
  // NOTE: this function seems to be used to get the current thread handle
  //       (passed handle=-2).
  // A close-source request reopens the source slot after closing its guest
  // open (canary 291f0c9be). Other requests create a separate handle to the
  // same object. Most games use it to get real handles to the current thread.

  X_HANDLE new_handle = X_INVALID_HANDLE_VALUE;
  auto* object_table = REX_KERNEL_OBJECTS();
  X_STATUS result;
  if (options & 1 /* DUPLICATE_CLOSE_SOURCE */) {
    const X_HANDLE source_handle =
        handle == 0xFFFFFFFE ? XThread::GetCurrentThreadHandle() : uint32_t(handle);
    object_ref<XObject> object;
    uint64_t generation = 0;
    {
      auto global_lock = rex::thread::global_critical_region::AcquireDirect();
      object = object_table->LookupObject<XObject>(source_handle);
      // A slot reference keeps the object alive across NtClose without adding
      // a guest open to the close callback's count.
      result = object ? object_table->RetainHandle(source_handle, &generation)
                      : X_STATUS_INVALID_HANDLE;
    }
    if (XSUCCEEDED(result)) {
      result = NtClose(source_handle);
      auto global_lock = rex::thread::global_critical_region::AcquireDirect();
      if (XSUCCEEDED(result)) {
        result = object_table->OpenGuestHandle(source_handle, object.get(),
                                               HasTitleObjectType(*object), generation);
        if (XSUCCEEDED(result)) {
          new_handle = source_handle;
        }
      }
      object_table->ReleaseHandleOf(source_handle, object.get(), generation);
    }
  } else {
    result = object_table->DuplicateHandle(handle, &new_handle);
  }

  if (new_handle_ptr) {
    *new_handle_ptr = new_handle;
  }

  return result;
}

u32 NtClose_entry(u32 handle) {
  REXKRNL_IMPORT_TRACE("NtClose", "handle={:#x}", (uint32_t)handle);
  auto result = NtClose(handle);
  REXKRNL_IMPORT_RESULT("NtClose", "{:#x}", result);
  return result;
}

u32 NtQueryEvent_entry(u32 handle, mapped_u32 out_struc) {
  X_STATUS result = X_STATUS_SUCCESS;

  auto ev = REX_KERNEL_OBJECTS()->LookupObject<XEvent>(handle);
  if (ev) {
    uint32_t type_tmp, state_tmp;
    ev->Query(&type_tmp, &state_tmp);
    out_struc[0] = type_tmp;
    out_struc[1] = state_tmp;
  } else {
    result = X_STATUS_INVALID_HANDLE;
  }

  return result;
}

}  // namespace rex::kernel::xboxkrnl

REX_EXPORT(__imp__ObCreateObject, rex::kernel::xboxkrnl::ObCreateObject_entry)
REX_EXPORT(__imp__ObInsertObject, rex::kernel::xboxkrnl::ObInsertObject_entry)
REX_EXPORT(__imp__ObOpenObjectByName, rex::kernel::xboxkrnl::ObOpenObjectByName_entry)
REX_EXPORT(__imp__ObOpenObjectByPointer, rex::kernel::xboxkrnl::ObOpenObjectByPointer_entry)
REX_EXPORT(__imp__ObLookupThreadByThreadId, rex::kernel::xboxkrnl::ObLookupThreadByThreadId_entry)
REX_EXPORT(__imp__ObReferenceObjectByHandle, rex::kernel::xboxkrnl::ObReferenceObjectByHandle_entry)
REX_EXPORT(__imp__ObReferenceObjectByName, rex::kernel::xboxkrnl::ObReferenceObjectByName_entry)
REX_EXPORT(__imp__ObDereferenceObject, rex::kernel::xboxkrnl::ObDereferenceObject_entry)
REX_EXPORT(__imp__ObReferenceObject, rex::kernel::xboxkrnl::ObReferenceObject_entry)
REX_EXPORT(__imp__ObLookupAnyThreadByThreadId,
           rex::kernel::xboxkrnl::ObLookupAnyThreadByThreadId_entry)
REX_EXPORT(__imp__ObCreateSymbolicLink, rex::kernel::xboxkrnl::ObCreateSymbolicLink_entry)
REX_EXPORT(__imp__ObDeleteSymbolicLink, rex::kernel::xboxkrnl::ObDeleteSymbolicLink_entry)
REX_EXPORT(__imp__NtDuplicateObject, rex::kernel::xboxkrnl::NtDuplicateObject_entry)
REX_EXPORT(__imp__NtClose, rex::kernel::xboxkrnl::NtClose_entry)
REX_EXPORT(__imp__NtQueryEvent, rex::kernel::xboxkrnl::NtQueryEvent_entry)

REX_EXPORT_STUB(__imp__ObGetWaitableObject);
REX_EXPORT_STUB(__imp__ObIsTitleObject);
REX_EXPORT_STUB(__imp__ObMakeTemporaryObject);
REX_EXPORT_STUB(__imp__ObTranslateSymbolicLink);
REX_EXPORT_STUB(__imp__NtCreateDirectoryObject);
REX_EXPORT_STUB(__imp__NtCreateSymbolicLinkObject);
REX_EXPORT_STUB(__imp__NtMakeTemporaryObject);
REX_EXPORT_STUB(__imp__NtOpenDirectoryObject);
REX_EXPORT_STUB(__imp__NtQueryDirectoryObject);
REX_EXPORT_STUB(__imp__NtQueryIoCompletion);
REX_EXPORT_STUB(__imp__NtQueryMutant);
REX_EXPORT_STUB(__imp__NtQuerySemaphore);
REX_EXPORT_STUB(__imp__NtQueryTimer);
