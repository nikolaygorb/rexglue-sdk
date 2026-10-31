/**
 ******************************************************************************
 * Xenia : Xbox 360 Emulator Research Project                                 *
 ******************************************************************************
 * Copyright 2020 Ben Vanik. All rights reserved.                             *
 * Released under the BSD license - see LICENSE in the root for more details. *
 ******************************************************************************
 *
 * @modified    Tom Clay, 2026 - Adapted for ReXGlue runtime
 */

#include <algorithm>
#include <cstring>

#include <rex/logging.h>
#include <rex/stream.h>
#include <rex/system/util/object_table.h>
#include <rex/system/xobject.h>
#include <rex/system/xthread.h>

namespace rex::system::util {

ObjectTable::ObjectTable() {}

ObjectTable::~ObjectTable() {
  Reset();
}

void ObjectTable::Reset() {
  auto global_lock = global_critical_region_.Acquire();

  // Release all objects.
  for (uint32_t n = 0; n < table_capacity_; n++) {
    ObjectTableEntry& entry = table_[n];
    if (entry.object) {
      entry.object->Release();
    }
  }
  for (uint32_t n = 0; n < host_table_capacity_; n++) {
    ObjectTableEntry& entry = host_table_[n];
    if (entry.object) {
      entry.object->Release();
    }
  }

  table_capacity_ = 0;
  last_free_entry_ = 0;
  free(table_);
  table_ = nullptr;
  host_table_capacity_ = 0;
  last_free_host_entry_ = 0;
  free(host_table_);
  host_table_ = nullptr;
  native_handles_.clear();
}

X_STATUS ObjectTable::FindFreeSlot(uint32_t* out_slot, bool host) {
  ObjectTableEntry* table = host ? host_table_ : table_;
  const uint32_t capacity = host ? host_table_capacity_ : table_capacity_;
  uint32_t& last_free_entry = host ? last_free_host_entry_ : last_free_entry_;
  // Find a free slot.
  uint32_t slot = last_free_entry;
  uint32_t scan_count = 0;
  while (scan_count < capacity) {
    ObjectTableEntry& entry = table[slot];
    if (!entry.object) {
      *out_slot = slot;
      return X_STATUS_SUCCESS;
    }
    scan_count++;
    slot = (slot + 1) % capacity;
    if (slot == 0) {
      // Never allow 0 handles.
      scan_count++;
      slot++;
    }
  }

  // Table out of slots, expand.
  uint32_t new_table_capacity = std::max(16 * 1024u, capacity * 2);
  if (!Resize(new_table_capacity, host)) {
    return X_STATUS_NO_MEMORY;
  }

  // Never allow 0 handles.
  slot = ++last_free_entry;
  *out_slot = slot;

  return X_STATUS_SUCCESS;
}

bool ObjectTable::Resize(uint32_t new_capacity, bool host) {
  ObjectTableEntry*& table = host ? host_table_ : table_;
  uint32_t& capacity = host ? host_table_capacity_ : table_capacity_;
  uint32_t new_size = new_capacity * sizeof(ObjectTableEntry);
  uint32_t old_size = capacity * sizeof(ObjectTableEntry);
  auto new_table = reinterpret_cast<ObjectTableEntry*>(realloc(table, new_size));
  if (!new_table) {
    return false;
  }

  // New entries are empty and untracked.
  for (uint32_t i = capacity; i < new_capacity; ++i) {
    new_table[i] = ObjectTableEntry();
  }

  (host ? last_free_host_entry_ : last_free_entry_) = capacity;
  capacity = new_capacity;
  table = new_table;

  return true;
}

X_STATUS ObjectTable::AddHandle(XObject* object, X_HANDLE* out_handle, bool force_guest_handle) {
  X_STATUS result = X_STATUS_SUCCESS;

  uint32_t handle = 0;
  {
    auto global_lock = global_critical_region_.Acquire();

    // Find a free slot.
    uint32_t slot = 0;
    const bool host = object->is_host_object() && !force_guest_handle;
    result = FindFreeSlot(&slot, host);

    // Stash.
    if (XSUCCEEDED(result)) {
      ObjectTableEntry& entry = host ? host_table_[slot] : table_[slot];
      entry.object = object;
      entry.handle_ref_count = 1;
      entry.guest_open_count = kUntrackedGuestOpens;
      entry.generation = ++next_handle_generation_;
      handle = (host ? XObject::kHandleHostBase : XObject::kHandleBase) + (slot << 2);
      object->handles().push_back(handle);

      // Retain so long as the object is in the table.
      object->Retain();

      REXSYS_NOISY_DEBUG("Added handle:{:08X} for {}", handle, typeid(*object).name());
    }
  }

  if (XSUCCEEDED(result)) {
    if (out_handle) {
      *out_handle = handle;
    }
  }

  return result;
}

X_STATUS ObjectTable::DuplicateHandle(X_HANDLE handle, X_HANDLE* out_handle) {
  auto global_lock = global_critical_region_.Acquire();
  ObjectTableEntry* source = LookupTable(handle);
  if (!source || !source->object || source->guest_open_count == 0) {
    return X_STATUS_INVALID_HANDLE;
  }
  XObject* object = source->object;
  const int32_t guest_open_count = source->guest_open_count;
  X_HANDLE new_handle = 0;
  // The duplicate goes to the guest even when the object is a host one.
  X_STATUS result = AddHandle(object, &new_handle, true);
  if (XSUCCEEDED(result)) {
    // AddHandle may have moved the table: look the new slot up again.
    LookupTable(new_handle)->guest_open_count = guest_open_count >= 0 ? 1 : guest_open_count;
    if (out_handle) {
      *out_handle = new_handle;
    }
  }
  return result;
}

X_STATUS ObjectTable::RetainHandle(X_HANDLE handle, uint64_t* out_generation) {
  auto global_lock = global_critical_region_.Acquire();

  ObjectTableEntry* entry = LookupTable(handle);
  if (!entry || !entry->object) {
    return X_STATUS_INVALID_HANDLE;
  }

  entry->handle_ref_count++;
  if (out_generation) {
    *out_generation = entry->generation;
  }
  return X_STATUS_SUCCESS;
}

X_STATUS ObjectTable::ReleaseHandle(X_HANDLE handle) {
  auto global_lock = global_critical_region_.Acquire();

  ObjectTableEntry* entry = LookupTable(handle);
  if (!entry || !entry->object || entry->handle_ref_count <= 0) {
    return X_STATUS_INVALID_HANDLE;
  }

  if (--entry->handle_ref_count == 0) {
    // No more references. Remove it from the table.
    return RemoveHandle(handle);
  }

  // FIXME: Return a status code telling the caller it wasn't released
  // (but not a failure code)
  return X_STATUS_SUCCESS;
}

X_STATUS ObjectTable::RemoveHandle(X_HANDLE handle) {
  X_STATUS result = X_STATUS_SUCCESS;

  handle = TranslateHandle(handle);
  if (!handle) {
    return X_STATUS_INVALID_HANDLE;
  }

  // The global lock first: the entry pointer stays valid only while it is
  // held (Resize moves the table).
  auto global_lock = global_critical_region_.Acquire();
  ObjectTableEntry* entry = LookupTable(handle);
  if (!entry) {
    return X_STATUS_INVALID_HANDLE;
  }

  if (entry->object) {
    auto object = entry->object;
    entry->object = nullptr;
    assert_zero(entry->handle_ref_count);
    entry->handle_ref_count = 0;
    entry->guest_open_count = kUntrackedGuestOpens;

    // Walk the object's handles and remove this one.
    auto handle_entry = std::find(object->handles().begin(), object->handles().end(), handle);
    if (handle_entry != object->handles().end()) {
      object->handles().erase(handle_entry);
    }
    object->UpdateNativeHandle(handle);
    auto native_handle = native_handles_.find(object->guest_object());
    if (native_handle != native_handles_.end() && native_handle->second == handle) {
      if (object->handles().empty()) {
        native_handles_.erase(native_handle);
      } else {
        native_handle->second = object->handles().front();
      }
    }

    REXSYS_NOISY_DEBUG("Removed handle:{:08X} for {}", handle, typeid(*object).name());

    // The name moves to another of the object's guest handles and goes with its last.
    if (!object->name().empty()) {
      std::lock_guard<std::mutex> name_lock(name_mutex_);
      auto it = name_table_.find(string::string_key_case(object->name()));
      if (it != name_table_.end() && it->second == handle) {
        auto next = std::find_if(object->handles().begin(), object->handles().end(),
                                 [](X_HANDLE h) { return !XObject::is_handle_host_object(h); });
        if (next != object->handles().end()) {
          it->second = *next;
        } else {
          name_table_.erase(it);
        }
      }
    }
    // Release now that the object has been removed from the table.
    object->Release();
  }

  return X_STATUS_SUCCESS;
}

X_STATUS ObjectTable::OpenGuestHandle(X_HANDLE handle, const XObject* object,
                                      bool track_guest_opens, uint64_t expected_generation) {
  auto global_lock = global_critical_region_.Acquire();
  ObjectTableEntry* entry = LookupTable(handle);
  if (!entry || !object || entry->object != object ||
      (expected_generation && entry->generation != expected_generation)) {
    return X_STATUS_INVALID_HANDLE;
  }
  // Once tracked, an object stays tracked even if its type becomes unreadable.
  if (entry->guest_open_count == kUntrackedGuestOpens && track_guest_opens) {
    entry->guest_open_count = 0;
  }
  ++entry->handle_ref_count;
  if (entry->guest_open_count >= 0) {
    ++entry->guest_open_count;
  }
  return X_STATUS_SUCCESS;
}

X_STATUS ObjectTable::ClaimGuestClose(X_HANDLE handle, const XObject* object,
                                      int32_t* out_handle_count, uint64_t* out_generation) {
  auto global_lock = global_critical_region_.Acquire();
  ObjectTableEntry* entry = LookupTable(handle);
  if (!entry || !object || entry->object != object || entry->guest_open_count == 0) {
    return X_STATUS_INVALID_HANDLE;
  }
  *out_generation = entry->generation;
  if (entry->guest_open_count < 0) {
    *out_handle_count = -1;
    return X_STATUS_SUCCESS;
  }
  int32_t count = 0;
  for (X_HANDLE open_handle : object->handles()) {
    const ObjectTableEntry* open_entry = LookupTable(open_handle);
    if (open_entry && open_entry->object == object && open_entry->guest_open_count > 0) {
      count += open_entry->guest_open_count;
    }
  }
  *out_handle_count = count;
  --entry->guest_open_count;
  return X_STATUS_SUCCESS;
}

X_STATUS ObjectTable::ReleaseHandleOf(X_HANDLE handle, const XObject* object,
                                      uint64_t generation) {
  auto global_lock = global_critical_region_.Acquire();
  const ObjectTableEntry* entry = LookupTable(handle);
  if (!entry || !object || entry->object != object || entry->generation != generation) {
    return X_STATUS_INVALID_HANDLE;
  }
  return ReleaseHandle(handle);
}

XObject* ObjectTable::LookupNativeObject(X_HANDLE handle, uint32_t guest_object) {
  auto global_lock = global_critical_region_.Acquire();
  const ObjectTableEntry* entry = LookupTable(handle);
  if (!entry || !entry->object || entry->object->guest_object() != guest_object) {
    return nullptr;
  }
  entry->object->Retain();
  return entry->object;
}

void ObjectTable::AddNativeHandle(uint32_t guest_object, X_HANDLE handle) {
  auto global_lock = global_critical_region_.Acquire();
  native_handles_[guest_object] = handle;
}

X_HANDLE ObjectTable::LookupNativeHandle(uint32_t guest_object) {
  auto global_lock = global_critical_region_.Acquire();
  auto it = native_handles_.find(guest_object);
  return it != native_handles_.end() ? it->second : X_HANDLE(0);
}

std::vector<object_ref<XObject>> ObjectTable::GetAllObjects() {
  auto lock = global_critical_region_.Acquire();
  std::vector<object_ref<XObject>> results;

  for (bool host : {false, true}) {
    ObjectTableEntry* table = host ? host_table_ : table_;
    const uint32_t capacity = host ? host_table_capacity_ : table_capacity_;
    for (uint32_t slot = 0; slot < capacity; slot++) {
      auto& entry = table[slot];
      if (entry.object &&
          std::find(results.begin(), results.end(), entry.object) == results.end()) {
        entry.object->Retain();
        results.push_back(object_ref<XObject>(entry.object));
      }
    }
  }

  return results;
}

void ObjectTable::PurgeAllObjects() {
  auto lock = global_critical_region_.Acquire();
  for (uint32_t slot = 0; slot < table_capacity_; slot++) {
    auto& entry = table_[slot];
    if (entry.object && !entry.object->is_host_object()) {
      entry.handle_ref_count = 0;
      entry.object->Release();

      entry.object = nullptr;
    }
  }
}

ObjectTable::ObjectTableEntry* ObjectTable::LookupTable(X_HANDLE handle) {
  handle = TranslateHandle(handle);
  if (!handle) {
    return nullptr;
  }

  auto global_lock = global_critical_region_.Acquire();

  // Lower 2 bits are ignored.
  const bool host = XObject::is_handle_host_object(handle);
  uint32_t slot = GetHandleSlot(handle, host);
  if (host) {
    if (slot < host_table_capacity_) {
      return &host_table_[slot];
    }
  } else if (slot < table_capacity_) {
    return &table_[slot];
  }

  return nullptr;
}

// Generic lookup
template <>
object_ref<XObject> ObjectTable::LookupObject<XObject>(X_HANDLE handle) {
  auto object = ObjectTable::LookupObject(handle, false);
  auto result = object_ref<XObject>(reinterpret_cast<XObject*>(object));
  return result;
}

XObject* ObjectTable::LookupObject(X_HANDLE handle, bool already_locked) {
  handle = TranslateHandle(handle);
  if (!handle) {
    return nullptr;
  }

  XObject* object = nullptr;
  if (!already_locked) {
    global_critical_region_.mutex().lock();
  }

  // A title-typed slot whose guest opens were all closed is only held by
  // references now: its handle is no longer valid.
  const ObjectTableEntry* entry = LookupTable(handle);
  if (entry && entry->object && entry->guest_open_count != 0) {
    object = entry->object;
  }

  // Retain the object pointer.
  if (object) {
    object->Retain();
  }

  if (!already_locked) {
    global_critical_region_.mutex().unlock();
  }

  return object;
}

void ObjectTable::GetObjectsByType(XObject::Type type, std::vector<object_ref<XObject>>* results) {
  auto global_lock = global_critical_region_.Acquire();
  for (bool host : {false, true}) {
    ObjectTableEntry* table = host ? host_table_ : table_;
    const uint32_t capacity = host ? host_table_capacity_ : table_capacity_;
    for (uint32_t slot = 0; slot < capacity; ++slot) {
      auto& entry = table[slot];
      if (entry.object && entry.object->type() == type) {
        entry.object->Retain();
        results->push_back(object_ref<XObject>(entry.object));
      }
    }
  }
}

X_HANDLE ObjectTable::TranslateHandle(X_HANDLE handle) {
  if (handle == 0xFFFFFFFF) {
    // CurrentProcess
    // assert_always();
    return 0;
  } else if (handle == 0xFFFFFFFE) {
    // CurrentThread
    return XThread::GetCurrentThreadHandle();
  } else {
    return handle;
  }
}

X_STATUS ObjectTable::AddNameMapping(const std::string_view name, X_HANDLE handle) {
  std::lock_guard<std::mutex> name_lock(name_mutex_);
  if (name_table_.count(string::string_key_case(name))) {
    return X_STATUS_OBJECT_NAME_COLLISION;
  }
  name_table_.insert({string::string_key_case::create(name), handle});
  return X_STATUS_SUCCESS;
}

void ObjectTable::RemoveNameMapping(const std::string_view name) {
  // Names are case-insensitive.
  std::lock_guard<std::mutex> name_lock(name_mutex_);
  auto it = name_table_.find(string::string_key_case(name));
  if (it != name_table_.end()) {
    name_table_.erase(it);
  }
}

X_STATUS ObjectTable::GetObjectByName(const std::string_view name, X_HANDLE* out_handle) {
  // Names are case-insensitive.
  // Look up handle under name lock only -- do NOT hold name_mutex_ while
  // acquiring global lock (RemoveHandle takes global -> name ordering).
  X_HANDLE handle;
  {
    std::lock_guard<std::mutex> name_lock(name_mutex_);
    auto it = name_table_.find(string::string_key_case(name));
    if (it == name_table_.end()) {
      *out_handle = X_INVALID_HANDLE_VALUE;
      return X_STATUS_OBJECT_NAME_NOT_FOUND;
    }
    handle = it->second;
  }
  *out_handle = handle;

  // Retain under global lock via normal LookupObject path.
  // The handle may have been removed between releasing name_mutex_ and
  // acquiring global lock -- LookupObject returns nullptr in that case.
  auto obj = LookupObject(handle, false);
  if (obj) {
    // The name's handle, which may not be the object's first.
    RetainHandle(handle);
    obj->Release();
  }

  return X_STATUS_SUCCESS;
}

bool ObjectTable::Save(stream::ByteStream* stream) {
  stream->Write<uint32_t>(table_capacity_);
  for (uint32_t i = 0; i < table_capacity_; i++) {
    auto& entry = table_[i];
    stream->Write<int32_t>(entry.handle_ref_count);
  }

  return true;
}

bool ObjectTable::Restore(stream::ByteStream* stream) {
  Resize(stream->Read<uint32_t>(), false);
  for (uint32_t i = 0; i < table_capacity_; i++) {
    auto& entry = table_[i];
    // entry.object = nullptr;
    entry.handle_ref_count = stream->Read<int32_t>();
  }

  return true;
}

X_STATUS ObjectTable::RestoreHandle(X_HANDLE handle, XObject* object) {
  // Host objects are not saved.
  if (XObject::is_handle_host_object(handle)) {
    return X_STATUS_INVALID_HANDLE;
  }
  uint32_t slot = GetHandleSlot(handle, false);
  assert_true(table_capacity_ > slot);

  if (table_capacity_ > slot) {
    auto& entry = table_[slot];
    entry.object = object;
    entry.generation = ++next_handle_generation_;
    object->Retain();
  }

  return X_STATUS_SUCCESS;
}

}  // namespace rex::system::util
