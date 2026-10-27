#pragma once
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

#include <string>
#include <unordered_map>
#include <vector>

#include <rex/string/key.h>
#include <rex/system/xobject.h>
#include <rex/system/xtypes.h>
#include <rex/thread/mutex.h>

namespace rex::stream {
class ByteStream;
}  // namespace rex::stream

namespace rex::system::util {

class ObjectTable {
 public:
  ObjectTable();
  ~ObjectTable();

  void Reset();

  // force_guest_handle puts a host object's new handle in the guest table, for
  // the paths that hand one to the guest.
  X_STATUS AddHandle(XObject* object, X_HANDLE* out_handle, bool force_guest_handle = false);
  X_STATUS DuplicateHandle(X_HANDLE orig, X_HANDLE* out_handle);
  // Optionally captures the slot generation for a later checked release.
  X_STATUS RetainHandle(X_HANDLE handle, uint64_t* out_generation = nullptr);
  X_STATUS ReleaseHandle(X_HANDLE handle);
  X_STATUS RemoveHandle(X_HANDLE handle);

  // Guest opens (ObOpenObjectByPointer, NtClose) of objects whose type a title
  // defined itself are counted apart from the references held on the slot, so
  // NtClose can call the type's close procedure with the open count. The
  // optional generation rejects a slot recycled during a close callback.
  X_STATUS OpenGuestHandle(X_HANDLE handle, const XObject* object, bool track_guest_opens = true,
                           uint64_t expected_generation = 0);
  // Claims an open before guest code runs and reports the total open count
  // before it. Untracked slots report -1 and are released without a callback.
  X_STATUS ClaimGuestClose(X_HANDLE handle, const XObject* object, int32_t* out_handle_count,
                           uint64_t* out_generation);
  // The generation keeps a close callback from releasing a recycled slot.
  X_STATUS ReleaseHandleOf(X_HANDLE handle, const XObject* object, uint64_t generation);
  // The object in a slot stashed in a guest dispatch header, only if it is
  // still the object at that guest address. Retained.
  XObject* LookupNativeObject(X_HANDLE handle, uint32_t guest_object);
  // Handles of objects whose guest body XObject::CreateNative allocated, by
  // body address: some (enumerators) have no dispatch header to stash it in,
  // and the console's header in front of them has no room for it.
  void AddNativeHandle(uint32_t guest_object, X_HANDLE handle);
  X_HANDLE LookupNativeHandle(uint32_t guest_object);

  bool Save(stream::ByteStream* stream);
  bool Restore(stream::ByteStream* stream);

  // Restores a XObject reference with a handle. Mainly for internal use - do
  // not use.
  X_STATUS RestoreHandle(X_HANDLE handle, XObject* object);

  template <typename T>
  object_ref<T> LookupObject(X_HANDLE handle) {
    auto object = LookupObject(handle, false);
    if (object) {
      if (object->type() != T::kObjectType) {
        object->Release();
        return object_ref<T>();
      }
    }
    auto result = object_ref<T>(reinterpret_cast<T*>(object));
    return result;
  }

  X_STATUS AddNameMapping(const std::string_view name, X_HANDLE handle);
  void RemoveNameMapping(const std::string_view name);
  X_STATUS GetObjectByName(const std::string_view name, X_HANDLE* out_handle);
  template <typename T>
  std::vector<object_ref<T>> GetObjectsByType(XObject::Type type) {
    std::vector<object_ref<T>> results;
    GetObjectsByType(type, reinterpret_cast<std::vector<object_ref<XObject>>*>(&results));
    return results;
  }

  template <typename T>
  std::vector<object_ref<T>> GetObjectsByType() {
    std::vector<object_ref<T>> results;
    GetObjectsByType(T::kObjectType, reinterpret_cast<std::vector<object_ref<XObject>>*>(&results));
    return results;
  }

  std::vector<object_ref<XObject>> GetAllObjects();
  void PurgeAllObjects();  // Purges the object table of all guest objects

 private:
  static constexpr int32_t kUntrackedGuestOpens = -1;
  struct ObjectTableEntry {
    int handle_ref_count = 0;
    // Guest opens of a title-typed object; kUntrackedGuestOpens for others.
    int32_t guest_open_count = kUntrackedGuestOpens;
    uint64_t generation = 0;
    XObject* object = nullptr;
  };

  ObjectTableEntry* LookupTable(X_HANDLE handle);
  XObject* LookupObject(X_HANDLE handle, bool already_locked);
  void GetObjectsByType(XObject::Type type, std::vector<object_ref<XObject>>* results);

  X_HANDLE TranslateHandle(X_HANDLE handle);
  static constexpr uint32_t GetHandleSlot(X_HANDLE handle, bool host) {
    return (handle - (host ? XObject::kHandleHostBase : XObject::kHandleBase)) >> 2;
  }
  X_STATUS FindFreeSlot(uint32_t* out_slot, bool host);
  bool Resize(uint32_t new_capacity, bool host);

  rex::thread::global_critical_region global_critical_region_;
  std::mutex name_mutex_;
  uint32_t table_capacity_ = 0;
  ObjectTableEntry* table_ = nullptr;
  uint32_t last_free_entry_ = 0;
  uint32_t host_table_capacity_ = 0;
  ObjectTableEntry* host_table_ = nullptr;
  uint32_t last_free_host_entry_ = 0;
  uint64_t next_handle_generation_ = 0;
  std::unordered_map<uint32_t, X_HANDLE> native_handles_;
  std::unordered_map<string::string_key_case, X_HANDLE> name_table_;
};

// Generic lookup
template <>
object_ref<XObject> ObjectTable::LookupObject<XObject>(X_HANDLE handle);

}  // namespace rex::system::util
