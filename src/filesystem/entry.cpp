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

#include <rex/filesystem.h>
#include <rex/filesystem/device.h>
#include <rex/filesystem/entry.h>
#include <rex/string.h>

namespace rex::filesystem {

Entry::Entry(Device* device, Entry* parent, const std::string_view path)
    : device_(device),
      parent_(parent),
      path_(path),
      attributes_(0),
      size_(0),
      allocation_size_(0),
      create_timestamp_(0),
      access_timestamp_(0),
      write_timestamp_(0) {
  assert_not_null(device);
  absolute_path_ = rex::string::utf8_join_guest_paths(device->mount_path(), path);
  name_ = rex::string::utf8_find_name_from_guest_path(path);
}

Entry::~Entry() = default;

void Entry::Dump(rex::string::StringBuffer* string_buffer, int indent) {
  for (int i = 0; i < indent; ++i) {
    string_buffer->Append(' ');
  }
  string_buffer->Append(name());
  string_buffer->Append('\n');
  for (auto& child : children_) {
    child->Dump(string_buffer, indent + 2);
  }
}

bool Entry::is_read_only() const {
  return device_->is_read_only();
}

Entry* Entry::GetChild(const std::string_view name) {
  auto global_lock = global_critical_region_.Acquire();
  // The size test is exact, not just a hint: the fold is ASCII-only, so any two
  // names it calls equal hold the same bytes per codepoint. It skips the UTF-8
  // decode for nearly every child, and a game directory can hold thousands.
  auto it = std::find_if(children_.cbegin(), children_.cend(), [&](const auto& child) {
    return child->name().size() == name.size() && rex::string::utf8_equal_case(child->name(), name);
  });
  if (it == children_.cend()) {
    return nullptr;
  }
  return (*it).get();
}

Entry* Entry::ResolvePath(const std::string_view path) {
  // Walk the path, one separator at a time.
  Entry* entry = this;
  for (auto& part : rex::string::utf8_split_path(path)) {
    entry = entry->GetChild(part);
    if (!entry) {
      // Not found.
      return nullptr;
    }
  }
  return entry;
}

Entry* Entry::IterateChildren(const rex::filesystem::WildcardEngine& engine,
                              size_t* current_index) {
  auto global_lock = global_critical_region_.Acquire();
  while (*current_index < children_.size()) {
    auto& child = children_[*current_index];
    *current_index = *current_index + 1;
    if (engine.Match(child->name())) {
      return child.get();
    }
  }
  return nullptr;
}

Entry* Entry::CreateEntry(const std::string_view name, uint32_t attributes) {
  auto global_lock = global_critical_region_.Acquire();
  if (is_read_only()) {
    return nullptr;
  }
  if (GetChild(name)) {
    // Already exists.
    return nullptr;
  }
  auto entry = CreateEntryInternal(name, attributes);
  if (!entry) {
    return nullptr;
  }
  children_.push_back(std::move(entry));
  // TODO(benvanik): resort? would break iteration?
  Touch();
  return children_.back().get();
}

bool Entry::Delete(Entry* entry) {
  auto global_lock = global_critical_region_.Acquire();
  if (is_read_only()) {
    return false;
  }
  if (entry->parent() != this) {
    return false;
  }
  if (!DeleteEntryInternal(entry)) {
    return false;
  }
  for (auto it = children_.begin(); it != children_.end(); ++it) {
    if (it->get() == entry) {
      children_.erase(it);
      break;
    }
  }
  Touch();
  return true;
}

bool Entry::Delete() {
  assert_not_null(parent_);
  return parent_->Delete(this);
}

X_STATUS Entry::Rename(const std::filesystem::path& file_path, bool replace_existing) {
  auto global_lock = global_critical_region_.Acquire();
  // Store the string so split path string_views remain valid.
  const std::string path_str = rex::path_to_utf8(file_path);
  std::vector<std::string_view> path_parts = rex::string::utf8_split_path(path_str);
  if (path_parts.size() < 2 || !parent_) {
    return X_STATUS_OBJECT_NAME_INVALID;
  }

  // Drop root path (for example, "game:").
  path_parts.erase(path_parts.begin());

  const std::string guest_path = rex::string::utf8_join_guest_paths(path_parts);
  Entry* new_parent = device_->ResolvePath(rex::string::utf8_find_base_guest_path(guest_path));
  if (!new_parent || !(new_parent->attributes() & kFileAttributeDirectory)) {
    return X_STATUS_OBJECT_PATH_NOT_FOUND;
  }
  const std::string_view name = path_parts.back();

  // Renaming to its own name fails too, even a change of case only.
  Entry* existing = new_parent->GetChild(name);
  if (existing) {
    if (!replace_existing || (existing->attributes() & kFileAttributeDirectory)) {
      return X_STATUS_OBJECT_NAME_COLLISION;
    }
    // TODO: edge also refuses an open target; open handles are not tracked
    // here, so the host rename reports it where the host refuses.
    if (existing == this) {
      return X_STATUS_ACCESS_DENIED;
    }
  }

  X_STATUS status = RenameEntryInternal(new_parent, name);
  if (status != X_STATUS_SUCCESS) {
    return status;
  }

  if (existing) {
    std::erase_if(new_parent->children_,
                  [existing](const auto& child) { return child.get() == existing; });
  }
  if (new_parent != parent_) {
    auto it = std::ranges::find(parent_->children_, this, &std::unique_ptr<Entry>::get);
    new_parent->children_.push_back(std::move(*it));
    parent_->children_.erase(it);
    parent_ = new_parent;
  }
  path_ = rex::string::utf8_join_guest_paths(new_parent->path(), name);
  absolute_path_ = rex::string::utf8_join_guest_paths(device_->mount_path(), path_);
  name_ = name;
  return X_STATUS_SUCCESS;
}

void Entry::Touch() {
  // TODO(benvanik): update timestamps.
}

}  // namespace rex::filesystem
