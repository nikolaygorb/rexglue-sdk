/**
 * @file        rex/input/mnk/mnk_input_driver.h
 * @brief       Keyboard/mouse input driver - maps MnK to Xbox 360 controller.
 *
 * @copyright   Copyright (c) 2026 Tom Clay <tomc@tctechstuff.com>
 *              All rights reserved.
 *
 * @license     BSD 3-Clause License
 *              See LICENSE file in the project root for full license text.
 */
#pragma once

#include <rex/input/input_driver.h>
#include <rex/ui/window.h>
#include <rex/ui/window_listener.h>

#include <atomic>
#include <chrono>
#include <cstdint>
#include <mutex>
#include <queue>
#include <string>
#include <unordered_set>

namespace rex::input::mnk {

void SetMouseLookActive(bool active);
bool IsMouseLookActive();

class MnkInputDriver final : public InputDriver,
                             public rex::ui::WindowInputListener,
                             public rex::ui::WindowListener {
 public:
  explicit MnkInputDriver(rex::ui::Window* window, size_t window_z_order);
  ~MnkInputDriver() override;

  X_STATUS Setup() override;

  void EnumerateDevices(std::vector<DeviceInfo>& out) override;
  X_RESULT GetDeviceState(DeviceId id, X_INPUT_STATE* out_state) override;
  X_RESULT GetDeviceCapabilities(DeviceId id, uint32_t flags,
                                 X_INPUT_CAPABILITIES* out_caps) override;
  X_RESULT SetDeviceVibration(DeviceId id, X_INPUT_VIBRATION* vibration) override;
  X_RESULT GetDeviceKeystroke(DeviceId id, uint32_t flags,
                              X_INPUT_KEYSTROKE* out_keystroke) override;
  void DiscardKeystrokes() override;

  void OnWindowAvailable(rex::ui::Window* window) override;

  // WindowInputListener
  void OnKeyDown(rex::ui::KeyEvent& e) override;
  void OnKeyUp(rex::ui::KeyEvent& e) override;
  void OnKeyChar(rex::ui::KeyEvent& e) override;
  void OnMouseDown(rex::ui::MouseEvent& e) override;
  void OnMouseUp(rex::ui::MouseEvent& e) override;
  void OnMouseMove(rex::ui::MouseEvent& e) override;

  // WindowListener
  void OnClosing(rex::ui::UIEvent& e) override;
  void OnDestroying(rex::ui::UIEvent& e) override;
  void OnLostFocus(rex::ui::UISetupEvent& e) override;
  void OnGotFocus(rex::ui::UISetupEvent& e) override;

 private:
  bool IsEnabled() const;
  void SetKeyState(uint16_t vk, bool down);
  void EnqueueKeystroke(uint16_t vk_pad, bool down);
  /// Appends to the keystroke queue, evicting the oldest entry once it is full.
  void PushKeystroke(const X_INPUT_KEYSTROKE& ks);
  /// Queues one raw key for the guest, with its unicode and modifier state.
  /// Passthrough only.
  void EnqueueRawKeystroke(const rex::ui::KeyEvent& e, bool down);
  /// Re-evaluates every bind against the current key state and queues a
  /// keystroke for each pad button that changed. Call with state_mutex_ held
  /// after any key or mouse button moves.
  void RefreshBoundKeystrokesLocked();
  /// The packet number for a polled state: it advances only when the state
  /// differs from the last one polled. Call with state_mutex_ held.
  uint32_t PacketNumberForLocked(const X_INPUT_GAMEPAD& gamepad);

  // Called from the guest thread. The rest of the capture path stays on the UI
  // thread, since every Window call in it reaches SDL.
  void QueueMouseCaptureUpdate(bool should_capture);
  void ApplyMouseCaptureFromUIThread();
  void ReleaseMouseCaptureFromUIThread(rex::ui::Window* window);
  void RecenterCursorFromUIThread(int32_t x, int32_t y);
  // Safe to call from any thread.
  void DetachFromWindow();

  // Only the UI thread writes it, so only guest thread access needs the lock.
  rex::ui::Window* attached_window_ = nullptr;

  std::mutex state_mutex_;
  bool key_down_[256] = {};

  // Mouse delta tracking. Fractional because relative motion arrives in
  // fractions of a pixel, and truncating each event drops slow movement.
  // Filled on the UI thread, drained on the guest thread, hence the lock.
  float mouse_dx_ = 0.0f;
  float mouse_dy_ = 0.0f;

  // Guest thread, under state_mutex_: the left stick's ramped axes
  // (mnk_stick_ramp), the low-passed mouse speed in pixels a second
  // (mnk_mouse_speed) and when GetDeviceState was last polled.
  float left_stick_x_ = 0.0f;
  float left_stick_y_ = 0.0f;
  float mouse_rate_x_ = 0.0f;
  float mouse_rate_y_ = 0.0f;
  std::chrono::steady_clock::time_point last_poll_{};

  // UI thread only.
  int32_t prev_mouse_x_ = 0;
  int32_t prev_mouse_y_ = 0;
  bool mouse_captured_ = false;
  // Whether the window has the pointer locked and is reporting relative motion.
  bool relative_mouse_mode_ = false;
  // Cursor visibility to restore on capture release - the window owner may run
  // an auto-hide policy that capture must not permanently override.
  rex::ui::Window::CursorVisibility precapture_cursor_visibility_ =
      rex::ui::Window::CursorVisibility::kVisible;

  // Guest thread to UI thread. The queued flag coalesces the posts.
  std::atomic<bool> mouse_capture_requested_{false};
  std::atomic<bool> mouse_capture_update_queued_{false};

  std::atomic<bool> has_focus_{true};

  // Keystroke queue. In passthrough it carries raw keys; otherwise the
  // VK_PAD_* codes of the bound buttons.
  std::queue<X_INPUT_KEYSTROKE> keystroke_queue_;
  // Which binds were pressed at the last evaluation, to diff against. One bit
  // per entry of the bind table.
  uint32_t bound_pressed_ = 0;
  // Under state_mutex_: raw keys held in passthrough, released for the title
  // when the focus is lost, and keys whose press the title received and whose
  // release it hasn't, so every release it gets pairs with a press.
  std::unordered_set<uint16_t> raw_keys_down_;
  std::unordered_set<uint16_t> delivered_;

  // Under state_mutex_: the last polled state and its packet number, which
  // advances only when the state changes.
  X_INPUT_GAMEPAD last_gamepad_ = {};
  uint32_t packet_number_ = 0;
};

}  // namespace rex::input::mnk
