/**
 * @file        launcher/launcher_app.cpp
 * @brief       The title launcher: a window next to the game's exe that edits
 *              the settings files (settings/hardware.toml, mapping.toml) and
 *              starts the game
 *
 * Built per title by rexglue_add_launcher() (cmake/rexglue_helpers.cmake). It
 * runs on the SDK's own window, presenter (D3D12 on Windows, Vulkan elsewhere)
 * and Dear ImGui drawer from rexruntime, the ones the in-game overlays use.
 * What it shows comes from launcher/common.toml (SDK) and the title's
 * launcher/launcher.toml, staged next to the exe. See docs/launcher.md.
 *
 * @copyright   Copyright (c) 2026 ReXGlue fork contributors
 * @license     BSD 3-Clause License
 *              See LICENSE file in the project root for full license text.
 */

// Before any include of imgui.h.
#define IMGUI_DEFINE_MATH_OPERATORS

#include <algorithm>
#include <array>
#include <atomic>
#include <cctype>
#include <cfloat>
#include <chrono>
#include <cmath>
#include <filesystem>
#include <fstream>
#include <map>
#include <memory>
#include <optional>
#include <set>
#include <string>
#include <thread>
#include <vector>

#include <fmt/format.h>
#include <fmt/ranges.h>
#include <imgui.h>

#include <rex/cvar.h>
#include <rex/cvar_file.h>
#include <rex/filesystem.h>
#include <rex/logging.h>
#include <rex/ui/displays.h>
#include <rex/ui/graphics_provider.h>
#include <rex/ui/imgui_dialog.h>
#include <rex/ui/imgui_drawer.h>
#include <rex/ui/immediate_drawer.h>
#include <rex/ui/keybinds.h>
#include <rex/ui/presenter.h>
#include <rex/ui/style.h>
#include <rex/ui/window.h>
#include <rex/ui/window_listener.h>
#include <rex/ui/windowed_app.h>
#include <rex/ui/windowed_app_context.h>
#if REX_HAS_D3D12
#include <rex/ui/d3d12/d3d12_provider.h>
#endif
#if REX_HAS_VULKAN
#include <rex/ui/vulkan/provider.h>
#endif

#include "launcher_bindings.h"
#include "launcher_devices.h"
#include "launcher_manifest.h"
#include "launcher_process.h"
#include "launcher_settings.h"
#include "launcher_widgets.h"

#ifndef REXLAUNCHER_GAME_EXE
#error "REXLAUNCHER_GAME_EXE (the game's file name) is set by rexglue_add_launcher()"
#endif

namespace rex::launcher {

namespace {

using Clock = std::chrono::steady_clock;

constexpr float kSideWidth = 224.0f;
constexpr float kHelpWidth = 256.0f;
constexpr float kFooterHeight = 68.0f;
constexpr float kRowHeight = 46.0f;
constexpr float kLabelWidth = 260.0f;

// The folder with the title's assets/ and settings/: the exe's folder in a
// release, the repository root in a build tree (as the games find it).
std::filesystem::path FindGameRoot(const std::filesystem::path& exe_dir) {
  std::filesystem::path dir = exe_dir;
  for (int i = 0; i < 8 && !dir.empty(); ++i) {
    std::error_code ec;
    if (std::filesystem::is_directory(dir / "assets", ec) ||
        std::filesystem::is_directory(dir / "settings", ec)) {
      return dir;
    }
    const std::filesystem::path parent = dir.parent_path();
    if (parent == dir) {
      break;
    }
    dir = parent;
  }
  return exe_dir;
}

std::string FormatNumber(double value, int decimals) {
  return fmt::format("{:.{}f}", value, std::max(decimals, 0));
}

struct RowState {
  ValueText current;  // what the launcher shows (and Save writes)
  ValueText saved;    // what the file has (or would have: the row's default)
};

class LauncherApp;

class LauncherDialog final : public rex::ui::ImGuiDialog {
 public:
  LauncherDialog(rex::ui::ImGuiDrawer* drawer, LauncherApp& app)
      : ImGuiDialog(drawer), app_(app) {}
  bool WantsContinuousRepaint() const override;

 protected:
  void OnDraw(ImGuiIO& io) override;

 private:
  LauncherApp& app_;
};

class LauncherApp final : public rex::ui::WindowedApp,
                          public rex::ui::WindowListener,
                          public rex::ui::WindowInputListener {
 public:
  static std::unique_ptr<rex::ui::WindowedApp> Create(rex::ui::WindowedAppContext& app_context) {
    return std::unique_ptr<rex::ui::WindowedApp>(new LauncherApp(app_context));
  }

  bool OnInitialize() override;
  void OnDestroy() override;

  void Draw();
  bool WantsRepaint() const;

  // WindowListener
  void OnClosing(rex::ui::UIEvent& e) override;
  void OnResize(rex::ui::UISetupEvent&) override { Wake(); }
  void OnDpiChanged(rex::ui::UISetupEvent&) override { Wake(); }
  void OnGotFocus(rex::ui::UISetupEvent&) override { Wake(); }

  // WindowInputListener: any input wakes the painting up (the window is only
  // painted while something changes). While a key binding is being captured,
  // keys and mouse buttons go to it instead of ImGui.
  void OnKeyDown(rex::ui::KeyEvent& e) override;
  void OnKeyUp(rex::ui::KeyEvent& e) override;
  void OnKeyChar(rex::ui::KeyEvent& e) override;
  void OnMouseDown(rex::ui::MouseEvent& e) override;
  void OnMouseMove(rex::ui::MouseEvent&) override { Wake(); }
  void OnMouseUp(rex::ui::MouseEvent& e) override;
  void OnMouseWheel(rex::ui::MouseEvent& e) override;

 private:
  explicit LauncherApp(rex::ui::WindowedAppContext& app_context)
      : WindowedApp(app_context, "launcher") {}

  bool CreateUI();
  void DestroyUI();
  void SetupFonts(ImFontAtlas* atlas);
  void SetupStyle(ImGuiStyle& style);
  void Wake();
  void ShowMessage(std::string text, bool error);

  void BuildWordmarkOverlays();
  void AddBindingRows();
  std::vector<Choice> ChoicesFor(const Row& row) const;
  void DrawBindings(const ImVec2& origin, const ImVec2& size);
  void DrawBindingRow(size_t index, float x, float width, const std::map<std::string, int>& uses);
  std::string BindingSummary(const bindings::PadElement& element, float width) const;
  void BindCaptured(const std::string& key);
  void StopCapture();

  void LoadSettingsState();
  std::filesystem::path LogsDir() const;
  ValueText ValueOf(const std::string& key) const;
  bool Holds(const Condition& condition) const;
  bool RowVisible(const Row& row) const;
  double NativeScale() const;
  std::string DisplayValue(const Row& row, const ValueText& value) const;
  // An `only` condition of a choice or a row ("" always holds).
  bool Available(const std::string& only) const;
  bool ChoiceVisible(const Choice& choice) const { return Available(choice.only); }
  std::string ChoiceLabel(const std::string& key) const;
  bool PageHasRows(const std::string& page) const;
  size_t ChangedCount() const;
  std::vector<std::string> MissingGameFiles() const;

  void DrawSide(ImDrawList* draw_list, const ImVec2& display);
  void DrawPage(const ImVec2& origin, const ImVec2& size);
  void DrawRow(size_t index, float width);
  void DrawHelp(ImDrawList* draw_list, const ImVec2& origin, const ImVec2& size);
  void DrawFooter(ImDrawList* draw_list, const ImVec2& display);

  bool Save();
  void ResetPage(bool all_pages);
  void RunAction(const std::string& action);
  void Play(bool safe_mode);
  void OnGameExited(int code);

  // Paths.
  std::filesystem::path exe_dir_;
  std::filesystem::path launcher_dir_;
  std::filesystem::path game_root_;
  std::filesystem::path settings_dir_;
  std::vector<std::string> row_status_;  // per row: the first line of its status file
  std::filesystem::path game_exe_;

  Manifest manifest_;
  std::string load_error_;
  Settings settings_;
  std::vector<RowState> states_;
  std::string page_;
  size_t hot_row_ = SIZE_MAX;
  std::string crash_text_;

  std::string message_;
  bool message_error_ = false;
  Clock::time_point message_time_{};
  Clock::time_point last_input_ = Clock::now();

  // UI objects, torn down while the game runs.
  std::unique_ptr<rex::ui::Window> window_;
  std::unique_ptr<rex::ui::GraphicsProvider> provider_;
  std::unique_ptr<rex::ui::Presenter> presenter_;
  std::unique_ptr<rex::ui::ImmediateDrawer> immediate_drawer_;
  std::unique_ptr<rex::ui::ImGuiDrawer> imgui_drawer_;
  std::unique_ptr<LauncherDialog> dialog_;
  widgets::Fonts fonts_;
  std::vector<ImFont*> wordmark_fonts_;
  ImVector<ImWchar> glyph_ranges_;
  float font_density_ = 1.0f;
  std::vector<std::unique_ptr<rex::ui::ImmediateTexture>> overlay_textures_;
  std::vector<widgets::WordmarkOverlay> overlays_;

  std::vector<rex::ui::DisplayInfo> displays_;
  RenderDevices render_devices_;

  // The key bindings screen.
  bool bindings_view_ = false;
  std::string selected_element_ = "a";  // a pad element's id, or "pc" for the title's actions
  size_t capture_row_ = SIZE_MAX;       // the binding waiting for a key
  rex::ui::VirtualKey pending_modifier_ = rex::ui::VirtualKey::kNone;
  bool swallow_mouse_up_ = false;
  std::array<bool, 256> swallowed_keys_{};  // pressed during a capture: their release is ours

  std::thread wait_thread_;
  std::atomic<bool> game_running_{false};
};

bool LauncherDialog::WantsContinuousRepaint() const {
  return app_.WantsRepaint();
}

void LauncherDialog::OnDraw(ImGuiIO& io) {
  (void)io;
  app_.Draw();
}

//------------------------------------------------------------------------------
// Setup and teardown
//------------------------------------------------------------------------------

bool LauncherApp::OnInitialize() {
  exe_dir_ = rex::filesystem::GetExecutableFolder();
  launcher_dir_ = exe_dir_ / "launcher";
  game_root_ = FindGameRoot(exe_dir_);
  settings_dir_ = game_root_ / "settings";
  game_exe_ = exe_dir_ / rex::to_path(REXLAUNCHER_GAME_EXE);

  if (!LoadManifest(launcher_dir_, manifest_, load_error_)) {
    REXLOG_ERROR("Launcher: {}", load_error_);
    manifest_ = Manifest{};
    manifest_.name = "Launcher";
    manifest_.theme = DefaultTheme();
  }
  AddBindingRows();
  displays_ = rex::ui::EnumerateDisplays();
  render_devices_ = EnumerateRenderDevices();
  LoadSettingsState();
  for (const std::string& page : manifest_.pages) {
    if (PageHasRows(page)) {
      page_ = page;
      break;
    }
  }
  return CreateUI();
}

void LauncherApp::OnDestroy() {
  DestroyUI();
  if (wait_thread_.joinable()) {
    wait_thread_.join();
  }
}

bool LauncherApp::CreateUI() {
  window_ = rex::ui::Window::Create(app_context(), manifest_.name + " Launcher");
  if (!window_) {
    return false;
  }
  window_->SetDesiredLogicalSize(1100, 700);
  window_->AddListener(this);
  // Above the ImGui drawer's z-order (64), so input reaches Wake() first.
  window_->AddInputListener(this, 1000);
  if (!window_->Open()) {
    REXLOG_ERROR("Launcher: can't open the window");
    return false;
  }

#if REX_HAS_D3D12
  provider_ = rex::ui::d3d12::D3D12Provider::Create();
#endif
#if REX_HAS_VULKAN
  if (!provider_) {
    provider_ = rex::ui::vulkan::VulkanProvider::Create(/*with_gpu_emulation=*/false,
                                                        /*with_presentation=*/true);
  }
#endif
  if (!provider_) {
    REXLOG_ERROR("Launcher: no graphics provider (D3D12 / Vulkan)");
    return false;
  }
  presenter_ = provider_->CreatePresenter();
  immediate_drawer_ = provider_->CreateImmediateDrawer();
  if (!presenter_ || !immediate_drawer_) {
    REXLOG_ERROR("Launcher: can't create the presenter");
    return false;
  }
  immediate_drawer_->SetPresenter(presenter_.get());

  imgui_drawer_ = std::make_unique<rex::ui::ImGuiDrawer>(
      window_.get(), 64, [this](ImFontAtlas* atlas) { SetupFonts(atlas); },
      [this](ImGuiStyle& style, rex::ui::Style&) { SetupStyle(style); });
  imgui_drawer_->SetPresenterAndImmediateDrawer(presenter_.get(), immediate_drawer_.get());
  window_->SetPresenter(presenter_.get());
  ImGuiIO& io = imgui_drawer_->GetIO();
  io.ConfigFlags |= ImGuiConfigFlags_NavEnableKeyboard;

  dialog_ = std::make_unique<LauncherDialog>(imgui_drawer_.get(), *this);
  BuildWordmarkOverlays();
  Wake();
  return true;
}

void LauncherApp::DestroyUI() {
  StopCapture();
  dialog_.reset();
  overlays_.clear();
  overlay_textures_.clear();
  if (imgui_drawer_) {
    imgui_drawer_->SetPresenterAndImmediateDrawer(nullptr, nullptr);
    imgui_drawer_.reset();
  }
  if (immediate_drawer_) {
    immediate_drawer_->SetPresenter(nullptr);
    immediate_drawer_.reset();
  }
  if (window_) {
    window_->SetPresenter(nullptr);
  }
  presenter_.reset();
  provider_.reset();
  if (window_) {
    window_->RemoveInputListener(this);
    window_->RemoveListener(this);
    window_.reset();
  }
  fonts_ = {};
  wordmark_fonts_.clear();
}

void LauncherApp::SetupFonts(ImFontAtlas* atlas) {
  const float density =
      window_ ? float(window_->GetDpi()) / float(window_->GetMediumDpi()) : 1.0f;
  if (glyph_ranges_.empty()) {
    ImFontGlyphRangesBuilder builder;
    builder.AddRanges(atlas->GetGlyphRangesCyrillic());
    // × ° · … — –
    builder.AddText("\xC3\x97\xC2\xB0\xC2\xB7\xE2\x80\xA6\xE2\x80\x94\xE2\x80\x93");
    builder.BuildRanges(&glyph_ranges_);
  }
  font_density_ = std::max(density, 1.0f);
  std::filesystem::path body = manifest_.theme.body_font;
  std::error_code ec;
  if (body.empty() || !std::filesystem::exists(body, ec)) {
    body = launcher_dir_ / "fonts" / "default.ttf";
  }
  std::filesystem::path display = manifest_.theme.display_font;
  if (display.empty() || !std::filesystem::exists(display, ec)) {
    display = body;
  }
  auto add = [&](const std::filesystem::path& path, float size) -> ImFont* {
    std::error_code exists_ec;
    if (!std::filesystem::exists(path, exists_ec)) {
      return nullptr;
    }
    ImFontConfig config;
    config.OversampleH = 2;
    config.OversampleV = 1;
    config.RasterizerDensity = font_density_;
    const std::string utf8 = rex::path_to_utf8(path);
    return atlas->AddFontFromFileTTF(utf8.c_str(), size, &config, glyph_ranges_.Data);
  };
  fonts_.body = add(body, 15.0f);
  fonts_.note_font = add(body, 13.0f);
  fonts_.title = add(display, 25.0f);
  fonts_.heading = add(display, 18.0f);
  fonts_.play = add(display, 21.0f);
  wordmark_fonts_.clear();
  for (const WordmarkLine& line : manifest_.theme.wordmark) {
    ImFont* font = nullptr;
    if (!line.font_file.empty()) {
      font = add(line.font_file, line.size);
    }
    wordmark_fonts_.push_back(font ? font : add(line.display_font ? display : body, line.size));
  }
  if (!fonts_.body) {
    REXLOG_WARN("Launcher: no font at {}", rex::path_to_utf8(body));
  }
}

void LauncherApp::SetupStyle(ImGuiStyle& style) {
  const Theme& t = manifest_.theme;
  style.WindowPadding = ImVec2(0.0f, 0.0f);
  style.WindowBorderSize = 0.0f;
  style.WindowRounding = 0.0f;
  style.ChildBorderSize = 0.0f;
  style.PopupRounding = 8.0f;
  style.PopupBorderSize = 1.0f;
  style.FrameRounding = 6.0f;
  style.ItemSpacing = ImVec2(10.0f, 8.0f);
  style.ScrollbarSize = 10.0f;
  style.ScrollbarRounding = 5.0f;
  auto color = [](ImU32 c) { return ImGui::ColorConvertU32ToFloat4(c); };
  style.Colors[ImGuiCol_Text] = color(t.text);
  style.Colors[ImGuiCol_TextDisabled] = color(t.dim);
  style.Colors[ImGuiCol_WindowBg] = color(t.bg);
  style.Colors[ImGuiCol_ChildBg] = ImVec4(0, 0, 0, 0);
  style.Colors[ImGuiCol_PopupBg] = color(t.panel);
  style.Colors[ImGuiCol_Border] = color(t.border);
  style.Colors[ImGuiCol_ScrollbarBg] = ImVec4(0, 0, 0, 0);
  style.Colors[ImGuiCol_ScrollbarGrab] = color(t.border);
  style.Colors[ImGuiCol_ScrollbarGrabHovered] = color(t.dim);
  style.Colors[ImGuiCol_ScrollbarGrabActive] = color(t.accent);
  style.Colors[ImGuiCol_NavCursor] = color(t.accent2);
}

void LauncherApp::Wake() {
  last_input_ = Clock::now();
  if (presenter_) {
    presenter_->RequestUIPaintFromUIThread();
  }
}

bool LauncherApp::WantsRepaint() const {
  const auto now = Clock::now();
  if (now - last_input_ < std::chrono::milliseconds(700)) {
    return true;
  }
  return !message_.empty() && now - message_time_ < std::chrono::seconds(5);
}

void LauncherApp::ShowMessage(std::string text, bool error) {
  message_ = std::move(text);
  message_error_ = error;
  message_time_ = Clock::now();
  Wake();
}

void LauncherApp::OnClosing(rex::ui::UIEvent& e) {
  (void)e;
  if (!game_running_) {
    app_context().QuitFromUIThread();
  }
}

//------------------------------------------------------------------------------
// Settings state
//------------------------------------------------------------------------------

void LauncherApp::LoadSettingsState() {
  std::set<std::string> names = {"hardware", "mapping"};
  for (const Row& row : manifest_.rows) {
    if (!row.key.empty()) {
      names.insert(row.file);
    }
  }
  settings_.Load(settings_dir_, std::vector<std::string>(names.begin(), names.end()));
  states_.assign(manifest_.rows.size(), RowState{});
  for (size_t i = 0; i < manifest_.rows.size(); ++i) {
    const Row& row = manifest_.rows[i];
    if (row.key.empty()) {
      continue;
    }
    // Not in the file: the game uses its default there; show the row's
    // player default (or nothing selected) without marking it changed.
    const ValueText value =
        settings_.Get(row.file, row.key).value_or(row.player_default.value_or(""));
    states_[i] = RowState{value, value};
  }
  row_status_.assign(manifest_.rows.size(), std::string{});
  for (size_t i = 0; i < manifest_.rows.size(); ++i) {
    if (manifest_.rows[i].status.empty()) {
      continue;
    }
    std::ifstream file(LogsDir() / rex::to_path(manifest_.rows[i].status));
    std::getline(file, row_status_[i]);
  }
}

// The game's logs: next to its executable, else at the game root.
std::filesystem::path LauncherApp::LogsDir() const {
  std::error_code ec;
  std::filesystem::path logs = exe_dir_ / "logs";
  return std::filesystem::is_directory(logs, ec) ? logs : game_root_ / "logs";
}

ValueText LauncherApp::ValueOf(const std::string& key) const {
  for (size_t i = 0; i < manifest_.rows.size(); ++i) {
    if (manifest_.rows[i].key == key) {
      return states_[i].current;
    }
  }
  for (const char* file : {"hardware", "mapping"}) {
    if (auto value = settings_.Get(file, key)) {
      return *value;
    }
  }
  return {};
}

bool LauncherApp::Holds(const Condition& condition) const {
  if (!condition.any.empty()) {
    return std::any_of(condition.any.begin(), condition.any.end(),
                       [&](const Condition& any) { return Holds(any); });
  }
  return condition.key.empty() || SameValue(ValueOf(condition.key), condition.value);
}

bool LauncherApp::RowVisible(const Row& row) const {
  if (!Available(row.only) || !Holds(row.visible_if) || (row.hide_if.set() && Holds(row.hide_if))) {
    return false;
  }
  if (row.backend.empty()) {
    return true;
  }
  bool available = false;
#if REX_HAS_VULKAN
  available |= row.backend == "vulkan";
#endif
#if REX_HAS_D3D12
  available |= row.backend == "d3d12";
#endif
#if REX_HAS_D3D11
  available |= row.backend == "d3d11";
#endif
  if (!available) {
    return false;
  }
  if (SameValue(ValueOf("gpu_plugin"), QuoteString("native"))) {
    return row.backend == "vulkan";
  }
  const ValueText backend = ValueOf("graphics_backend");
  if (backend.empty() || SameValue(backend, QuoteString("any"))) {
    // Auto may fall back from D3D12 to Vulkan; keep each API's index separate.
    return row.backend == "vulkan" || row.backend == "d3d12";
  }
  return SameValue(backend, QuoteString(row.backend));
}

double LauncherApp::NativeScale() const {
  double scale = ValueNumber(ValueOf("native_resolution_scale")).value_or(0.0);
  if (!(scale > 0.0)) {
    const double whole = ValueNumber(ValueOf("resolution_scale")).value_or(1.0);
    const double x = ValueNumber(ValueOf("draw_resolution_scale_x")).value_or(1.0);
    const double y = ValueNumber(ValueOf("draw_resolution_scale_y")).value_or(1.0);
    scale = std::min(x != 1.0 ? x : whole, y != 1.0 ? y : whole);
  }
  return std::round(std::clamp(scale, 1.0, 4.0) * 4.0) * 0.25;
}

std::string LauncherApp::DisplayValue(const Row& row, const ValueText& value) const {
  if (value.empty()) {
    return "Default";
  }
  if (row.kind == RowKind::kToggle) {
    return (SameValue(value, "true") != row.invert) ? "On" : "Off";
  }
  if (row.kind == RowKind::kChoice || row.kind == RowKind::kSelect) {
    for (const Choice& choice : ChoicesFor(row)) {
      if (SameValue(choice.value, value)) {
        return choice.label;
      }
    }
  }
  if (const auto number = ValueNumber(value); number && row.kind == RowKind::kSlider) {
    if (row.key == "native_resolution_scale" && *number == 0.0) {
      return "Automatic";
    }
    return fmt::format("{:g}{}", *number, row.suffix);
  }
  // Decode strings so bindings and other strings show their actual contents.
  if (value.front() == '"') {
    try {
      return toml::parse("value = " + value)["value"].value_or(std::string{});
    } catch (const toml::parse_error&) {}
  }
  return value;
}

bool LauncherApp::Available(const std::string& only) const {
  if (only.empty()) {
    return true;
  }
  // "a|b": any of the conditions.
  if (const size_t bar = only.find('|'); bar != std::string::npos) {
    return Available(only.substr(0, bar)) || Available(only.substr(bar + 1));
  }
  if (only == "xess:upscaler") {
    // Intel XeSS SR runs on the chosen Vulkan GPU, or with Auto on any.
    const auto& xess = render_devices_.xess;
    const double device = ValueNumber(ValueOf("vulkan_device")).value_or(-1.0);
    if (device >= 0.0) {
      return size_t(device) < xess.size() && xess[size_t(device)];
    }
    return std::any_of(xess.begin(), xess.end(), [](uint8_t runs) { return runs != 0; });
  }
  if (only == "dlss:upscaler" || only == "dlss:frame_generation") {
    // NVIDIA DLSS SR / Frame Generation on the chosen Vulkan GPU, or with Auto on any.
    const auto& dlss = only == "dlss:upscaler" ? render_devices_.dlss : render_devices_.dlss_frame_generation;
    const double device = ValueNumber(ValueOf("vulkan_device")).value_or(-1.0);
    if (device >= 0.0) {
      return size_t(device) < dlss.size() && dlss[size_t(device)];
    }
    return std::any_of(dlss.begin(), dlss.end(), [](uint8_t runs) { return runs != 0; });
  }
#ifdef _WIN32
  if (only == "windows") {
    return true;
  }
  if (only == "linux") {
    return false;
  }
#else
  if (only == "windows") {
    return false;
  }
  if (only == "linux") {
    return true;
  }
#endif
  if (only == "fsr4:upscaler" || only == "fsr4:frame_generation") {
    // AMD FSR 4 offered by the chosen Vulkan GPU, or with Auto by any (the
    // game falls back to FSR 3 on one without it). Generation needs AMD's
    // FSR 4 swapchain too.
    const bool upscaler = only == "fsr4:upscaler";
    auto offers = [upscaler](const rex::ui::vulkan::FsrVersions& versions) {
      using rex::ui::vulkan::FsrHasMajor;
      return upscaler ? FsrHasMajor(versions.upscaler, 4)
                      : FsrHasMajor(versions.frame_generation, 4) &&
                            (versions.swapchain.empty() || FsrHasMajor(versions.swapchain, 4));
    };
    const auto& fsr = render_devices_.fsr;
    const double device = ValueNumber(ValueOf("vulkan_device")).value_or(-1.0);
    if (device >= 0.0) {
      return size_t(device) < fsr.size() && offers(fsr[size_t(device)]);
    }
    return std::any_of(fsr.begin(), fsr.end(), offers);
  }
  if (only.starts_with("file:")) {
    const std::string name = only.substr(5);
    std::error_code ec;
#ifdef _WIN32
    return std::filesystem::exists(exe_dir_ / rex::to_path(name + ".dll"), ec);
#elif defined(__APPLE__)
    return std::filesystem::exists(exe_dir_ / ("lib" + name + ".dylib"), ec);
#else
    return std::filesystem::exists(exe_dir_ / ("lib" + name + ".so"), ec);
#endif
  }
  return true;
}

std::string LauncherApp::ChoiceLabel(const std::string& key) const {
  for (const Row& row : manifest_.rows) {
    if (row.key != key) {
      continue;
    }
    const ValueText value = Holds(row.lock_if) && row.lock_if.set() ? row.lock_value
                                                                            : ValueOf(key);
    for (const Choice& choice : ChoicesFor(row)) {
      if (SameValue(choice.value, value)) {
        return choice.label;
      }
    }
  }
  return {};
}

bool LauncherApp::PageHasRows(const std::string& page) const {
  return std::any_of(manifest_.rows.begin(), manifest_.rows.end(),
                     [&](const Row& row) { return row.page == page && RowVisible(row); });
}

size_t LauncherApp::ChangedCount() const {
  size_t count = 0;
  for (const RowState& state : states_) {
    if (!SameValue(state.current, state.saved)) {
      ++count;
    }
  }
  return count;
}

std::vector<std::string> LauncherApp::MissingGameFiles() const {
  std::vector<std::string> missing;
  for (const std::string& file : manifest_.required_files) {
    std::error_code ec;
    if (!std::filesystem::exists(game_root_ / rex::to_path(file), ec)) {
      missing.push_back(file);
    }
  }
  std::error_code ec;
  if (!std::filesystem::exists(game_exe_, ec)) {
    missing.push_back(REXLAUNCHER_GAME_EXE);
  }
  return missing;
}

//------------------------------------------------------------------------------
// Drawing
//------------------------------------------------------------------------------

void LauncherApp::Draw() {
  const Theme& t = manifest_.theme;
  widgets::SetStyle(&t, &fonts_);
  ImGuiIO& io = ImGui::GetIO();
  const ImVec2 display = io.DisplaySize;

  ImGui::SetNextWindowPos(ImVec2(0.0f, 0.0f));
  ImGui::SetNextWindowSize(display);
  ImGui::PushStyleColor(ImGuiCol_WindowBg, ImVec4(0, 0, 0, 0));
  ImGui::Begin("##launcher", nullptr,
               ImGuiWindowFlags_NoDecoration | ImGuiWindowFlags_NoMove |
                   ImGuiWindowFlags_NoSavedSettings | ImGuiWindowFlags_NoBringToFrontOnFocus |
                   ImGuiWindowFlags_NoScrollWithMouse);
  ImGui::PopStyleColor();
  if (fonts_.body) {
    ImGui::PushFont(fonts_.body, fonts_.body->LegacySize);
  }
  ImDrawList* draw_list = ImGui::GetWindowDrawList();

  // Background: the title's colour, darker towards the bottom.
  draw_list->AddRectFilledMultiColor(ImVec2(0, 0), display, t.bg, t.bg, t.bg2, t.bg2);

  DrawSide(draw_list, display);

  const float body_height = display.y - kFooterHeight;
  const ImVec2 page_origin(kSideWidth, 0.0f);
  if (bindings_view_) {
    DrawBindings(page_origin, ImVec2(std::max(display.x - kSideWidth, 300.0f), body_height));
  } else {
    const ImVec2 page_size(std::max(display.x - kSideWidth - kHelpWidth, 200.0f), body_height);
    DrawPage(page_origin, page_size);
    const ImVec2 help_origin(display.x - kHelpWidth, 0.0f);
    DrawHelp(draw_list, help_origin, ImVec2(kHelpWidth, body_height));
  }

  DrawFooter(draw_list, display);

  if (fonts_.body) {
    ImGui::PopFont();
  }
  ImGui::End();
}

void LauncherApp::DrawSide(ImDrawList* draw_list, const ImVec2& display) {
  const Theme& t = manifest_.theme;
  const float bottom = display.y - kFooterHeight;
  draw_list->AddRectFilled(ImVec2(0, 0), ImVec2(kSideWidth, bottom), t.side);
  draw_list->AddLine(ImVec2(kSideWidth - 0.5f, 0), ImVec2(kSideWidth - 0.5f, bottom), t.border);

  float y = 26.0f;
  if (!manifest_.theme.wordmark.empty()) {
    y += widgets::Wordmark(draw_list, ImVec2(22.0f, y), manifest_.theme.wordmark,
                           wordmark_fonts_, overlays_, t.text);
  } else if (fonts_.title) {
    draw_list->AddText(fonts_.title, fonts_.title->LegacySize, ImVec2(22.0f, y), t.text,
                       manifest_.name.c_str());
    y += fonts_.title->LegacySize + 4.0f;
  }
  if (!manifest_.theme.tagline.empty() && fonts_.note_font) {
    std::string tagline = manifest_.theme.tagline;
    std::transform(tagline.begin(), tagline.end(), tagline.begin(),
                   [](unsigned char c) { return char(std::toupper(c)); });
    draw_list->AddText(fonts_.note_font, fonts_.note_font->LegacySize, ImVec2(23.0f, y + 6.0f), t.dim,
                       tagline.c_str());
    y += fonts_.note_font->LegacySize + 6.0f;
  }
  y += 26.0f;

  ImGui::SetCursorScreenPos(ImVec2(12.0f, y));
  for (const std::string& page : manifest_.pages) {
    if (!PageHasRows(page)) {
      continue;
    }
    ImGui::SetCursorScreenPos(ImVec2(12.0f, ImGui::GetCursorScreenPos().y));
    if (widgets::Tab(page.c_str(), page == page_ && !bindings_view_, kSideWidth - 24.0f)) {
      page_ = page;
      hot_row_ = SIZE_MAX;
      bindings_view_ = false;
      StopCapture();
    }
  }
}

void LauncherApp::DrawPage(const ImVec2& origin, const ImVec2& size) {
  const Theme& t = manifest_.theme;
  ImGui::SetCursorScreenPos(origin);
  ImGui::BeginChild("##page", size, ImGuiChildFlags_None, ImGuiWindowFlags_NoBackground);
  ImDrawList* draw_list = ImGui::GetWindowDrawList();
  // The child's initial cursor includes its scroll offset; the window origin doesn't.
  const ImVec2 content_origin = ImGui::GetCursorScreenPos();
  const float x = content_origin.x + 24.0f;
  float y = content_origin.y + 22.0f;

  if (!load_error_.empty()) {
    draw_list->AddText(fonts_.body ? fonts_.body : ImGui::GetFont(),
                       fonts_.body ? fonts_.body->LegacySize : ImGui::GetFontSize(),
                       ImVec2(x, y), t.warn, ("Can't read the launcher files: " + load_error_).c_str(),
                       nullptr, size.x - 48.0f);
    ImGui::EndChild();
    return;
  }

  if (!crash_text_.empty()) {
    const ImVec2 min(x, y);
    const ImVec2 max(content_origin.x + size.x - 24.0f, y + 64.0f);
    draw_list->AddRectFilled(min, max, widgets::WithAlpha(t.warn, 0.14f), 8.0f);
    draw_list->AddRect(min, max, t.warn, 8.0f);
    draw_list->AddText(ImVec2(min.x + 14.0f, min.y + 11.0f), t.text, crash_text_.c_str());
    ImGui::SetCursorScreenPos(ImVec2(min.x + 14.0f, min.y + 32.0f));
    if (fonts_.note_font) {
      draw_list->AddText(fonts_.note_font, fonts_.note_font->LegacySize, ImVec2(min.x + 14.0f, min.y + 36.0f),
                         t.dim, "Logs are in the logs folder next to the game. Safe mode starts it with safe settings.");
    }
    ImGui::SetCursorScreenPos(ImVec2(max.x - 92.0f, min.y + 14.0f));
    if (widgets::Button("Dismiss", widgets::ButtonStyle::kGhost)) {
      crash_text_.clear();
    }
    y = max.y + 16.0f;
  }

  if (fonts_.title) {
    draw_list->AddText(fonts_.title, fonts_.title->LegacySize, ImVec2(x, y), t.text, page_.c_str());
    y += fonts_.title->LegacySize + 14.0f;
  }

  ImGui::SetCursorScreenPos(ImVec2(x - 10.0f, y));
  const float row_width = size.x - 28.0f;
  for (size_t i = 0; i < manifest_.rows.size(); ++i) {
    if (manifest_.rows[i].page == page_ && RowVisible(manifest_.rows[i])) {
      DrawRow(i, row_width);
    }
  }
  ImGui::Dummy(ImVec2(0.0f, 12.0f));
  ImGui::EndChild();
}

void LauncherApp::DrawRow(size_t index, float width) {
  const Theme& t = manifest_.theme;
  const Row& row = manifest_.rows[index];
  RowState& state = states_[index];
  ImDrawList* draw_list = ImGui::GetWindowDrawList();
  const ImVec2 origin = ImGui::GetCursorScreenPos();
  ImFont* font = fonts_.body ? fonts_.body : ImGui::GetFont();
  const float font_size = font->LegacySize;
  const float label_width = std::min(kLabelWidth, std::max(100.0f, width * 0.5f));
  const float wrap_width = std::max(40.0f, label_width - 30.0f);
  const float label_height =
      font->CalcTextSizeA(font_size, FLT_MAX, wrap_width, row.label.c_str()).y;
  const float row_height = std::max(kRowHeight, label_height + 18.0f);
  const ImVec2 row_max(origin.x + width, origin.y + row_height);

  if (ImGui::IsWindowHovered() && ImGui::IsMouseHoveringRect(origin, row_max)) {
    hot_row_ = index;
  }
  if (hot_row_ == index) {
    draw_list->AddRectFilled(origin, row_max, widgets::WithAlpha(t.panel, 0.7f), 7.0f);
  }

  const bool enabled = !row.disabled && Holds(row.enable_if);
  const bool locked = row.lock_if.set() && Holds(row.lock_if);
  const ValueText shown = locked ? row.lock_value : state.current;

  // Label, with a dot while the value differs from the file's.
  const float text_y = origin.y + (row_height - label_height) * 0.5f;
  if (row.kind != RowKind::kButton && !SameValue(state.current, state.saved)) {
    draw_list->AddCircleFilled(ImVec2(origin.x + 8.0f, origin.y + row_height * 0.5f), 3.0f,
                               t.accent);
  }
  draw_list->AddText(font, font_size, ImVec2(origin.x + 16.0f, text_y), enabled ? t.text : t.dim,
                     row.label.c_str(), nullptr, wrap_width);

  ImGui::PushID(int(index));
  const float control_x = origin.x + label_width;
  const bool disabled = !enabled || locked;
  bool focused = false;
  switch (row.kind) {
    case RowKind::kToggle: {
      ImGui::SetCursorScreenPos(ImVec2(control_x, origin.y + (row_height - 24.0f) * 0.5f));
      bool on = SameValue(shown, "true") != row.invert;
      if (widgets::Toggle("##value", on, disabled)) {
        state.current = (on != row.invert) ? "true" : "false";
      }
      focused = ImGui::IsItemFocused();
      break;
    }
    case RowKind::kChoice:
    case RowKind::kSelect: {
      const std::vector<Choice> choices = ChoicesFor(row);
      std::vector<size_t> visible;
      std::vector<std::string> labels;
      int selected = -1;
      for (size_t i = 0; i < choices.size(); ++i) {
        if (!ChoiceVisible(choices[i])) {
          continue;
        }
        if (SameValue(choices[i].value, shown)) {
          selected = int(visible.size());
        }
        visible.push_back(i);
        labels.push_back(choices[i].label);
      }
      float segments_width = 6.0f + 2.0f * float(labels.empty() ? 0 : labels.size() - 1);
      for (const std::string& label : labels) {
        segments_width += font->CalcTextSizeA(font_size, FLT_MAX, 0.0f, label.c_str()).x + 26.0f;
      }
      ImGui::SetCursorScreenPos(ImVec2(control_x, origin.y + (row_height - 32.0f) * 0.5f));
      const bool changed =
          row.kind == RowKind::kChoice && segments_width <= ImGui::GetContentRegionAvail().x - 12.0f
              ? widgets::Segmented("##value", labels, {}, selected, disabled)
              : widgets::Select("##value", labels, selected, disabled);
      if (changed && selected >= 0) {
        state.current = choices[visible[size_t(selected)]].value;
      }
      focused = ImGui::IsItemFocused();
      break;
    }
    case RowKind::kSlider: {
      ImGui::SetCursorScreenPos(ImVec2(control_x, origin.y + (row_height - 26.0f) * 0.5f));
      double value = ValueNumber(shown).value_or(row.min);
      if (row.key == "native_resolution_scale") {
        value = NativeScale();
      }
      const std::string text = FormatNumber(value, row.decimals) + row.suffix;
      if (widgets::Slider("##value", value, row.min, row.max, row.step, text, disabled)) {
        if (row.integer) {
          state.current = std::to_string(std::llround(value));
        } else {
          std::string number = FormatNumber(value, std::max(row.decimals, 1));
          state.current = std::move(number);
        }
      }
      focused = ImGui::IsItemFocused();
      break;
    }
    case RowKind::kButton: {
      ImGui::SetCursorScreenPos(ImVec2(control_x, origin.y + (row_height - 38.0f) * 0.5f));
      if (widgets::Button(row.button_text.c_str(), widgets::ButtonStyle::kGhost, disabled)) {
        RunAction(row.action);
      }
      focused = ImGui::IsItemFocused();
      break;
    }
    case RowKind::kKeybind: {
      // A title's action put on a page: its keys, edited on the bindings screen.
      ImGui::SetCursorScreenPos(ImVec2(control_x, origin.y + (row_height - 38.0f) * 0.5f));
      std::string keys;
      for (const std::string& key : bindings::KeyList(shown)) {
        keys += (keys.empty() ? "" : "  \xC2\xB7  ") + bindings::PrettyKey(key);
      }
      if (widgets::Button(keys.empty() ? "not bound" : keys.c_str(), widgets::ButtonStyle::kGhost,
                          disabled)) {
        RunAction("bindings");
        selected_element_ = "pc";
      }
      focused = ImGui::IsItemFocused();
      break;
    }
  }
  if (focused) {
    hot_row_ = index;
  }
  ImGui::PopID();

  // One layout item for the row.
  ImGui::SetCursorScreenPos(origin);
  ImGui::Dummy(ImVec2(width, row_height));
  ImGui::SetCursorScreenPos(ImVec2(origin.x, origin.y + row_height + 2.0f));
}

void LauncherApp::DrawHelp(ImDrawList* draw_list, const ImVec2& origin, const ImVec2& size) {
  const Theme& t = manifest_.theme;
  draw_list->AddRectFilled(origin, origin + size, widgets::WithAlpha(t.side, 0.85f));
  draw_list->AddLine(ImVec2(origin.x + 0.5f, origin.y), ImVec2(origin.x + 0.5f, origin.y + size.y),
                     t.border);
  const Row* row = nullptr;
  if (hot_row_ < manifest_.rows.size() && manifest_.rows[hot_row_].page == page_ &&
      RowVisible(manifest_.rows[hot_row_])) {
    row = &manifest_.rows[hot_row_];
  }
  ImFont* note_font = fonts_.note_font ? fonts_.note_font : ImGui::GetFont();
  const float changes_height = std::min(std::clamp(size.y * 0.4f, 160.0f, 260.0f), size.y * 0.5f);
  const float changes_top = origin.y + size.y - changes_height;
  ImGui::SetCursorScreenPos(origin + ImVec2(20.0f, 20.0f));
  ImGui::BeginChild("##setting_help", ImVec2(size.x - 40.0f, size.y - changes_height - 32.0f),
                    ImGuiChildFlags_None, ImGuiWindowFlags_NoBackground);
  ImGui::PushFont(note_font, note_font->LegacySize);
  ImGui::PushStyleColor(ImGuiCol_Text, ImGui::ColorConvertU32ToFloat4(t.dim));
  if (!row) {
    ImGui::TextWrapped("Point at a setting to see what it does.");
  } else {
    if (fonts_.heading) {
      ImGui::PushFont(fonts_.heading, fonts_.heading->LegacySize);
    }
    ImGui::PushStyleColor(ImGuiCol_Text, ImGui::ColorConvertU32ToFloat4(t.text));
    ImGui::TextWrapped("%s", row->label.c_str());
    ImGui::PopStyleColor();
    if (fonts_.heading) {
      ImGui::PopFont();
    }
    ImGui::Spacing();
    if (!row->help.empty()) {
      ImGui::TextWrapped("%s", row->help.c_str());
      ImGui::Spacing();
    }
    if (!row->key.empty()) {
      const size_t index = size_t(row - manifest_.rows.data());
      ImGui::PushStyleColor(ImGuiCol_Text, ImGui::ColorConvertU32ToFloat4(t.accent2));
      ImGui::TextWrapped("%s = %s", row->key.c_str(), states_[index].current.c_str());
      ImGui::PopStyleColor();
      ImGui::TextWrapped("settings/%s.toml", row->file.c_str());
    }
    if (const size_t index = size_t(row - manifest_.rows.data());
        index < row_status_.size() && !row_status_[index].empty()) {
      ImGui::Spacing();
      ImGui::PushStyleColor(ImGuiCol_Text, ImGui::ColorConvertU32ToFloat4(t.text));
      ImGui::TextWrapped("Last run: %s", row_status_[index].c_str());
      ImGui::PopStyleColor();
    }
    if (row->restart) {
      ImGui::Spacing();
      ImGui::TextColored(ImGui::ColorConvertU32ToFloat4(t.warn), "Restart needed");
    }
  }
  ImGui::PopStyleColor();
  ImGui::PopFont();
  ImGui::EndChild();

  // One list across all pages, derived from the same saved/current pairs as
  // Save. Reverting a value removes it; a successful save clears it.
  draw_list->AddLine(ImVec2(origin.x + 20.0f, changes_top),
                     ImVec2(origin.x + size.x - 20.0f, changes_top), t.border);
  ImGui::SetCursorScreenPos(ImVec2(origin.x + 20.0f, changes_top + 14.0f));
  ImGui::BeginChild("##pending_changes", ImVec2(size.x - 40.0f, changes_height - 26.0f),
                    ImGuiChildFlags_None, ImGuiWindowFlags_NoBackground);
  ImGui::PushFont(note_font, note_font->LegacySize);
  ImGui::TextColored(ImGui::ColorConvertU32ToFloat4(t.text), "Unsaved changes (%zu)",
                     ChangedCount());
  ImGui::Spacing();
  if (ChangedCount() == 0) {
    ImGui::TextColored(ImGui::ColorConvertU32ToFloat4(t.dim), "No unsaved changes.");
  }
  for (size_t i = 0; i < manifest_.rows.size(); ++i) {
    const Row& changed_row = manifest_.rows[i];
    if (changed_row.key.empty() || SameValue(states_[i].current, states_[i].saved)) {
      continue;
    }
    std::string label = changed_row.label;
    if (changed_row.key == "native_resolution_scale") {
      label += " (Native)";
    } else if (changed_row.key == "resolution_scale") {
      label += " (Emulated)";
    }
    ImGui::PushStyleColor(ImGuiCol_Text, ImGui::ColorConvertU32ToFloat4(t.text));
    ImGui::TextWrapped("%s", label.c_str());
    ImGui::PopStyleColor();
    const std::string before = DisplayValue(changed_row, states_[i].saved);
    const std::string after = DisplayValue(changed_row, states_[i].current);
    ImGui::PushStyleColor(ImGuiCol_Text, ImGui::ColorConvertU32ToFloat4(t.accent2));
    ImGui::TextWrapped("%s -> %s", before.c_str(), after.c_str());
    ImGui::PopStyleColor();
    ImGui::Spacing();
  }
  ImGui::PopFont();
  ImGui::EndChild();
}

void LauncherApp::DrawFooter(ImDrawList* draw_list, const ImVec2& display) {
  const Theme& t = manifest_.theme;
  const float top = display.y - kFooterHeight;
  draw_list->AddRectFilled(ImVec2(0, top), display, t.side);
  draw_list->AddLine(ImVec2(0, top + 0.5f), ImVec2(display.x, top + 0.5f), t.border);

  // Status on the left: a message, missing game files, or the setup in short.
  ImFont* note_font = fonts_.note_font ? fonts_.note_font : ImGui::GetFont();
  std::string status;
  ImU32 status_color = t.dim;
  ImU32 dot = t.ok;
  const auto missing = MissingGameFiles();
  if (!message_.empty() && Clock::now() - message_time_ < std::chrono::seconds(5)) {
    status = message_;
    status_color = message_error_ ? t.warn : t.text;
    dot = message_error_ ? t.warn : t.ok;
  } else if (!missing.empty()) {
    status = "Game files missing: " + missing.front();
    status_color = t.warn;
    dot = t.warn;
  } else {
    std::vector<std::string> parts = {"Game files found"};
    for (const char* key : {"gpu_plugin", "graphics_backend"}) {
      std::string label = ChoiceLabel(key);
      if (!label.empty()) {
        parts.push_back(std::move(label));
      }
    }
    if (SameValue(ValueOf("gpu_plugin"), QuoteString("native"))) {
      parts.push_back(fmt::format("{:g}x", NativeScale()));
    } else if (auto scale = ValueNumber(ValueOf("resolution_scale"))) {
      parts.push_back(fmt::format("{:g}x", *scale));
    }
    status = fmt::format("{}", fmt::join(parts, "  \xC2\xB7  "));
  }
  const float status_y = top + (kFooterHeight - note_font->LegacySize) * 0.5f;
  draw_list->AddCircleFilled(ImVec2(22.0f, top + kFooterHeight * 0.5f), 4.0f, dot);
  draw_list->AddText(note_font, note_font->LegacySize, ImVec2(34.0f, status_y), status_color,
                     status.c_str());

  // Buttons on the right, laid out from the right edge.
  ImFont* body = fonts_.body ? fonts_.body : ImGui::GetFont();
  ImFont* play_font = fonts_.play ? fonts_.play : body;
  const size_t changed = ChangedCount();
  const std::string save_label = changed ? fmt::format("Save ({})", changed) : "Save";
  const float play_width =
      play_font->CalcTextSizeA(play_font->LegacySize, FLT_MAX, 0.0f, "PLAY").x + 76.0f;
  const float save_width =
      body->CalcTextSizeA(body->LegacySize, FLT_MAX, 0.0f, save_label.c_str()).x + 34.0f;
  const float safe_width = body->CalcTextSizeA(body->LegacySize, FLT_MAX, 0.0f, "Safe mode").x + 34.0f;
  float x = display.x - 20.0f - play_width;
  const bool can_play = missing.empty() && load_error_.empty();

  ImGui::SetCursorScreenPos(ImVec2(x, top + (kFooterHeight - 46.0f) * 0.5f));
  if (widgets::Button("PLAY", widgets::ButtonStyle::kPlay, !can_play)) {
    Play(false);
  }
  x -= 12.0f + save_width;
  ImGui::SetCursorScreenPos(ImVec2(x, top + (kFooterHeight - 38.0f) * 0.5f));
  if (widgets::Button(save_label.c_str(), widgets::ButtonStyle::kGhost, changed == 0)) {
    Save();
  }
  x -= 10.0f + safe_width;
  ImGui::SetCursorScreenPos(ImVec2(x, top + (kFooterHeight - 38.0f) * 0.5f));
  if (widgets::Button("Safe mode", widgets::ButtonStyle::kGhost, !can_play)) {
    Play(true);
  }
}

//------------------------------------------------------------------------------
// System lists, wordmark textures
//------------------------------------------------------------------------------

std::vector<Choice> LauncherApp::ChoicesFor(const Row& row) const {
  if (row.source.empty()) {
    return row.choices;
  }
  auto size_text = [](int width, int height) {
    return fmt::format("{} \xC3\x97 {}", width, height);
  };
  std::vector<Choice> choices;
  if (row.source == "displays") {
    // The monitor setting: 0 the default, then the displays as the game counts them.
    choices.push_back(Choice{"0", "Default", ""});
    for (size_t i = 0; i < displays_.size(); ++i) {
      const rex::ui::DisplayInfo& display = displays_[i];
      std::string label =
          fmt::format("{}: {}", i + 1, display.name.empty() ? "Display" : display.name);
      if (display.desktop.width > 0) {
        label += fmt::format(" ({})", size_text(display.desktop.width, display.desktop.height));
      }
      choices.push_back(Choice{std::to_string(i + 1), std::move(label), ""});
    }
  } else if (row.source == "display_modes") {
    // The full-screen sizes of the chosen monitor (the main one for Default).
    choices.push_back(Choice{QuoteString(""), "Default", ""});
    const int monitor = int(ValueNumber(ValueOf("monitor")).value_or(0.0));
    const rex::ui::DisplayInfo* display = nullptr;
    if (monitor >= 1 && size_t(monitor) <= displays_.size()) {
      display = &displays_[size_t(monitor) - 1];
    } else {
      for (const rex::ui::DisplayInfo& candidate : displays_) {
        if (candidate.primary) {
          display = &candidate;
          break;
        }
      }
    }
    if (!display && !displays_.empty()) {
      display = &displays_.front();
    }
    if (display) {
      for (const rex::ui::DisplayMode& mode : display->modes) {
        if (mode.width < 640 || mode.height < 480) {
          continue;
        }
        choices.push_back(Choice{QuoteString(fmt::format("{}x{}", mode.width, mode.height)),
                                 size_text(mode.width, mode.height), ""});
      }
    }
  } else if (row.source == "vulkan_devices") {
    choices = render_devices_.vulkan;
  } else if (row.source == "dxgi_devices") {
    choices = render_devices_.dxgi;
  } else {
    choices = row.choices;
  }
  // A value the list doesn't have (written by hand, another monitor) stays a choice.
  const size_t index = size_t(&row - manifest_.rows.data());
  if (index < states_.size()) {
    const ValueText& current = states_[index].current;
    if (!current.empty() &&
        std::none_of(choices.begin(), choices.end(),
                     [&](const Choice& choice) { return SameValue(choice.value, current); })) {
      std::string label = current;
      if (label.size() >= 2 && label.front() == '"') {
        label = label.substr(1, label.size() - 2);
      }
      choices.push_back(Choice{current, std::move(label), ""});
    }
  }
  return choices;
}

void LauncherApp::BuildWordmarkOverlays() {
  overlays_.clear();
  overlay_textures_.clear();
  if (!immediate_drawer_ || !imgui_drawer_) {
    return;
  }
  imgui_drawer_->GetIO();  // the drawer's ImGui context, with its built font atlas
  const std::vector<WordmarkLine>& lines = manifest_.theme.wordmark;
  for (size_t i = 0; i < lines.size(); ++i) {
    widgets::WordmarkOverlay overlay;
    ImFont* font = i < wordmark_fonts_.size() ? wordmark_fonts_[i] : nullptr;
    int width = 0;
    int height = 0;
    float margin = 0.0f;
    const std::vector<uint8_t> pixels =
        widgets::BuildWordmarkTexture(font, lines[i], font_density_, width, height, margin);
    if (!pixels.empty()) {
      std::unique_ptr<rex::ui::ImmediateTexture> texture = immediate_drawer_->CreateTexture(
          uint32_t(width), uint32_t(height), rex::ui::ImmediateTextureFilter::kLinear, false,
          pixels.data());
      if (texture) {
        overlay.texture = reinterpret_cast<ImTextureID>(texture.get());
        overlay.size = ImVec2(float(width) / font_density_, float(height) / font_density_);
        overlay.margin = margin;
        overlay_textures_.push_back(std::move(texture));
      }
    }
    overlays_.push_back(overlay);
  }
}

//------------------------------------------------------------------------------
// Key bindings
//------------------------------------------------------------------------------

void LauncherApp::AddBindingRows() {
  for (const bindings::PadElement& element : bindings::PadElements()) {
    for (const bindings::PadKey& pad_key : element.keys) {
      Row row;
      row.key = std::string("keybind_") + pad_key.suffix;
      if (std::any_of(manifest_.rows.begin(), manifest_.rows.end(),
                      [&](const Row& existing) { return existing.key == row.key; })) {
        continue;
      }
      row.page = kBindingsPage;
      row.file = "mapping";
      row.kind = RowKind::kKeybind;
      row.label = element.keys.size() > 1 ? pad_key.label : element.label;
      // The default is the mouse and keyboard driver's own (rexruntime).
      if (const rex::cvar::FlagEntry* info = rex::cvar::GetFlagInfo(row.key)) {
        row.player_default = QuoteString(info->default_value);
      }
      manifest_.rows.push_back(std::move(row));
    }
  }
}

std::string LauncherApp::BindingSummary(const bindings::PadElement& element, float width) const {
  const bool several = element.keys.size() > 1;
  std::vector<std::string> parts;
  for (const bindings::PadKey& pad_key : element.keys) {
    const std::string key = std::string("keybind_") + pad_key.suffix;
    for (size_t i = 0; i < manifest_.rows.size(); ++i) {
      if (manifest_.rows[i].key != key) {
        continue;
      }
      const std::vector<std::string> keys = bindings::KeyList(states_[i].current);
      if (several) {
        parts.push_back(keys.empty() ? "\xC2\xB7" : bindings::PrettyKey(keys.front()));
      } else {
        for (const std::string& k : keys) {
          parts.push_back(bindings::PrettyKey(k));
        }
      }
    }
  }
  std::string text = fmt::format("{}", fmt::join(parts, several ? " " : " \xC2\xB7 "));
  if (text.empty()) {
    text = "not bound";
  }
  ImFont* font = fonts_.body ? fonts_.body : ImGui::GetFont();
  auto fits = [&](const std::string& s) {
    return font->CalcTextSizeA(font->LegacySize, FLT_MAX, 0.0f, s.c_str()).x <= width;
  };
  if (!fits(text)) {
    while (text.size() > 1 && !fits(text + "\xE2\x80\xA6")) {
      // One UTF-8 character off the end.
      while (!text.empty() && (static_cast<unsigned char>(text.back()) & 0xC0) == 0x80) {
        text.pop_back();
      }
      if (!text.empty()) {
        text.pop_back();
      }
    }
    text += "\xE2\x80\xA6";
  }
  return text;
}

void LauncherApp::DrawBindings(const ImVec2& origin, const ImVec2& size) {
  const Theme& t = manifest_.theme;
  ImGui::SetCursorScreenPos(origin);
  ImGui::BeginChild("##bindings", size, ImGuiChildFlags_None, ImGuiWindowFlags_NoBackground);
  ImDrawList* draw_list = ImGui::GetWindowDrawList();
  const ImVec2 content_origin = ImGui::GetCursorScreenPos();
  ImFont* body = fonts_.body ? fonts_.body : ImGui::GetFont();
  ImFont* note_font = fonts_.note_font ? fonts_.note_font : body;
  const float x = content_origin.x + 24.0f;
  const float right = content_origin.x + size.x - 24.0f;
  float y = content_origin.y + 18.0f;
  auto text_width = [&](ImFont* font, const char* text) {
    return font->CalcTextSizeA(font->LegacySize, FLT_MAX, 0.0f, text).x;
  };

  // Header: back to Controls, the title, the title's own actions, reset.
  const char* back_label = "Back to Controls";
  ImGui::SetCursorScreenPos(ImVec2(x, y));
  if (widgets::Button(back_label, widgets::ButtonStyle::kGhost)) {
    bindings_view_ = false;
    StopCapture();
  }
  if (fonts_.title) {
    draw_list->AddText(fonts_.title, fonts_.title->LegacySize,
                       ImVec2(x + text_width(body, back_label) + 34.0f + 18.0f,
                              y + (38.0f - fonts_.title->LegacySize) * 0.5f),
                       t.text, "Key bindings");
  }
  const bool has_actions =
      std::any_of(manifest_.rows.begin(), manifest_.rows.end(), [](const Row& row) {
        return row.kind == RowKind::kKeybind && !row.key.starts_with("keybind_");
      });
  float button_x = right - (text_width(body, "Reset bindings") + 34.0f);
  ImGui::SetCursorScreenPos(ImVec2(button_x, y));
  if (widgets::Button("Reset bindings", widgets::ButtonStyle::kGhost)) {
    StopCapture();
    for (size_t i = 0; i < manifest_.rows.size(); ++i) {
      const Row& row = manifest_.rows[i];
      if (row.kind == RowKind::kKeybind && row.player_default && !row.disabled) {
        states_[i].current = *row.player_default;
      }
    }
    ShowMessage("Key bindings back to the defaults, not saved yet", false);
  }
  if (has_actions) {
    button_x -= text_width(body, "PC actions") + 34.0f + 10.0f;
    ImGui::SetCursorScreenPos(ImVec2(button_x, y));
    if (widgets::Button("PC actions", selected_element_ == "pc" ? widgets::ButtonStyle::kPrimary
                                                                : widgets::ButtonStyle::kGhost)) {
      selected_element_ = "pc";
      StopCapture();
    }
  }
  y += 52.0f;

  // The pad and its callouts.
  constexpr float kEditorHeight = 236.0f;
  const float available_width = size.x - 48.0f;
  const float available_height = std::max(size.y - (y - content_origin.y) - kEditorHeight, 120.0f);
  const float scale = std::clamp(std::min(available_width / bindings::kCanvasWidth,
                                          available_height / bindings::kCanvasHeight),
                                 0.6f, 1.25f);
  const ImVec2 canvas(x + (available_width - bindings::kCanvasWidth * scale) * 0.5f, y);
  const ImVec2 callout_size(bindings::kCalloutWidth * scale, 38.0f);
  for (const bindings::PadElement& element : bindings::PadElements()) {
    const ImVec2 box = bindings::CalloutPosition(canvas, scale, element);
    const ImVec2 from(element.right ? box.x : box.x + callout_size.x, box.y + callout_size.y * 0.5f);
    const bool on = selected_element_ == element.id;
    draw_list->AddLine(from, bindings::PadPoint(canvas, scale, element.anchor),
                       on ? t.accent : t.border, on ? 2.0f : 1.0f);
  }
  bindings::DrawPad(draw_list, canvas, scale, selected_element_, t, body);
  for (const bindings::PadElement& element : bindings::PadElements()) {
    const bool on = selected_element_ == element.id;
    draw_list->AddCircleFilled(bindings::PadPoint(canvas, scale, element.anchor), 3.5f,
                               on ? t.accent : t.border);
    const std::string summary = BindingSummary(element, callout_size.x - 20.0f);
    if (widgets::Callout(element.id, bindings::CalloutPosition(canvas, scale, element), callout_size,
                         element.label, summary.c_str(), on)) {
      selected_element_ = element.id;
      StopCapture();
    }
  }
  y = canvas.y + bindings::kCanvasHeight * scale + 12.0f;

  // The selected button's keys, or the title's actions.
  std::map<std::string, int> uses;
  for (size_t i = 0; i < manifest_.rows.size(); ++i) {
    if (manifest_.rows[i].kind == RowKind::kKeybind) {
      for (const std::string& key : bindings::KeyList(states_[i].current)) {
        ++uses[key];
      }
    }
  }
  std::string heading;
  std::vector<size_t> rows_here;
  if (selected_element_ == "pc") {
    heading = "PC actions";
    for (size_t i = 0; i < manifest_.rows.size(); ++i) {
      const Row& row = manifest_.rows[i];
      if (row.kind == RowKind::kKeybind && !row.key.starts_with("keybind_")) {
        rows_here.push_back(i);
      }
    }
  } else {
    for (const bindings::PadElement& element : bindings::PadElements()) {
      if (selected_element_ != element.id) {
        continue;
      }
      heading = element.label;
      for (const bindings::PadKey& pad_key : element.keys) {
        const std::string key = std::string("keybind_") + pad_key.suffix;
        for (size_t i = 0; i < manifest_.rows.size(); ++i) {
          if (manifest_.rows[i].key == key) {
            rows_here.push_back(i);
          }
        }
      }
    }
  }
  const float strip_height = 46.0f + float(rows_here.size()) * 36.0f + 28.0f;
  draw_list->AddRectFilled(ImVec2(x, y), ImVec2(right, y + strip_height),
                           widgets::WithAlpha(t.side, 0.9f), 8.0f);
  draw_list->AddRect(ImVec2(x, y), ImVec2(right, y + strip_height), t.border, 8.0f);
  if (fonts_.heading) {
    draw_list->AddText(fonts_.heading, fonts_.heading->LegacySize, ImVec2(x + 18.0f, y + 12.0f),
                       t.text, heading.c_str());
  }
  float row_y = y + 44.0f;
  for (size_t index : rows_here) {
    ImGui::SetCursorScreenPos(ImVec2(x + 18.0f, row_y));
    DrawBindingRow(index, x + 18.0f, right - x - 36.0f, uses);
    row_y += 36.0f;
  }
  const char* note =
      capture_row_ != SIZE_MAX
          ? "Esc cancels  \xC2\xB7  Delete clears this row  \xC2\xB7  a mouse button or the wheel "
            "binds itself"
          : "Pick a button on the pad, then + Add. Several keys per button; Shift, Ctrl and Alt "
            "combinations work. Saved to settings/mapping.toml.";
  draw_list->AddText(note_font, note_font->LegacySize, ImVec2(x + 18.0f, row_y + 2.0f), t.dim, note);

  ImGui::SetCursorScreenPos(ImVec2(x, y + strip_height + 8.0f));
  ImGui::Dummy(ImVec2(0.0f, 0.0f));
  ImGui::EndChild();
}

void LauncherApp::DrawBindingRow(size_t index, float x, float width,
                                 const std::map<std::string, int>& uses) {
  const Theme& t = manifest_.theme;
  const Row& row = manifest_.rows[index];
  RowState& state = states_[index];
  ImDrawList* draw_list = ImGui::GetWindowDrawList();
  ImFont* body = fonts_.body ? fonts_.body : ImGui::GetFont();
  ImFont* note_font = fonts_.note_font ? fonts_.note_font : body;
  const ImVec2 origin = ImGui::GetCursorScreenPos();
  constexpr float kHeight = 28.0f;

  if (!SameValue(state.current, state.saved)) {
    draw_list->AddCircleFilled(ImVec2(x - 8.0f, origin.y + kHeight * 0.5f), 3.0f, t.accent);
  }
  draw_list->AddText(body, body->LegacySize,
                     ImVec2(x, origin.y + (kHeight - body->LegacySize) * 0.5f), t.dim,
                     row.label.c_str());

  std::vector<std::string> keys = bindings::KeyList(state.current);
  int remove = -1;
  ImGui::PushID(int(index));
  ImGui::SetCursorScreenPos(ImVec2(x + 130.0f, origin.y));
  ImGui::BeginDisabled(row.disabled);
  for (size_t j = 0; j < keys.size(); ++j) {
    ImGui::PushID(int(j));
    auto it = uses.find(keys[j]);
    const bool warn = it != uses.end() && it->second > 1;
    if (widgets::KeyChip("##key", bindings::PrettyKey(keys[j]).c_str(), warn)) {
      remove = int(j);
    }
    ImGui::PopID();
    ImGui::SameLine(0.0f, 6.0f);
  }
  if (keys.size() < 4) {
    if (widgets::CaptureButton("##add", capture_row_ == index)) {
      if (capture_row_ == index) {
        StopCapture();
      } else {
        capture_row_ = index;
        pending_modifier_ = rex::ui::VirtualKey::kNone;
      }
    }
  }
  const float controls_end = ImGui::GetItemRectMax().x;
  ImGui::EndDisabled();
  ImGui::PopID();
  if (remove >= 0) {
    keys.erase(keys.begin() + remove);
    state.current = bindings::KeyListValue(keys);
  }

  // The line as it goes to the file, where there is room for it.
  const std::string line = row.key + " = " + state.current;
  const float line_width =
      note_font->CalcTextSizeA(note_font->LegacySize, FLT_MAX, 0.0f, line.c_str()).x;
  if (x + width - line_width > controls_end + 16.0f) {
    draw_list->AddText(note_font, note_font->LegacySize,
                       ImVec2(x + width - line_width, origin.y + (kHeight - note_font->LegacySize) * 0.5f),
                       t.accent2, line.c_str());
  }
  ImGui::SetCursorScreenPos(origin);
  ImGui::Dummy(ImVec2(width, kHeight));
}

void LauncherApp::StopCapture() {
  capture_row_ = SIZE_MAX;
  pending_modifier_ = rex::ui::VirtualKey::kNone;
}

void LauncherApp::BindCaptured(const std::string& key) {
  if (capture_row_ >= states_.size() || key.empty()) {
    StopCapture();
    return;
  }
  std::vector<std::string> keys = bindings::KeyList(states_[capture_row_].current);
  if (std::find(keys.begin(), keys.end(), key) == keys.end() && keys.size() < 4) {
    keys.push_back(key);
  }
  states_[capture_row_].current = bindings::KeyListValue(keys);
  StopCapture();
  Wake();
}

void LauncherApp::OnKeyDown(rex::ui::KeyEvent& e) {
  Wake();
  if (capture_row_ == SIZE_MAX) {
    return;
  }
  e.set_handled(true);
  using rex::ui::VirtualKey;
  const VirtualKey key = e.virtual_key();
  // Its release is ours too (and only then: a key held from before the
  // capture, Enter on "+ Add", is released to ImGui).
  swallowed_keys_[size_t(key) & 0xFF] = true;
  if (key == VirtualKey::kEscape) {
    StopCapture();
    return;
  }
  if (key == VirtualKey::kDelete) {
    states_[capture_row_].current = bindings::KeyListValue({});
    StopCapture();
    return;
  }
  if (key == VirtualKey::kShift || key == VirtualKey::kControl || key == VirtualKey::kMenu) {
    // Alone it binds when let go; with another key it is a prefix.
    pending_modifier_ = key;
    return;
  }
  const std::string name = rex::ui::VirtualKeyToString(key);
  if (name.empty()) {
    return;  // a key the bindings have no name for
  }
  std::string prefix;
  if (e.is_ctrl_pressed()) {
    prefix += "Ctrl+";
  }
  if (e.is_alt_pressed()) {
    prefix += "Alt+";
  }
  if (e.is_shift_pressed()) {
    prefix += "Shift+";
  }
  BindCaptured(prefix + name);
}

void LauncherApp::OnKeyUp(rex::ui::KeyEvent& e) {
  Wake();
  bool& swallowed = swallowed_keys_[size_t(e.virtual_key()) & 0xFF];
  if (!swallowed) {
    return;
  }
  swallowed = false;
  e.set_handled(true);
  if (capture_row_ != SIZE_MAX && pending_modifier_ != rex::ui::VirtualKey::kNone &&
      e.virtual_key() == pending_modifier_) {
    BindCaptured(rex::ui::VirtualKeyToString(pending_modifier_));
  }
}

void LauncherApp::OnKeyChar(rex::ui::KeyEvent& e) {
  Wake();
  if (capture_row_ != SIZE_MAX) {
    e.set_handled(true);
  }
}

void LauncherApp::OnMouseDown(rex::ui::MouseEvent& e) {
  Wake();
  if (capture_row_ == SIZE_MAX) {
    return;
  }
  using rex::ui::VirtualKey;
  VirtualKey key = VirtualKey::kNone;
  switch (e.button()) {
    case rex::ui::MouseEvent::Button::kLeft:
      key = VirtualKey::kLButton;
      break;
    case rex::ui::MouseEvent::Button::kRight:
      key = VirtualKey::kRButton;
      break;
    case rex::ui::MouseEvent::Button::kMiddle:
      key = VirtualKey::kMButton;
      break;
    case rex::ui::MouseEvent::Button::kX1:
      key = VirtualKey::kXButton1;
      break;
    case rex::ui::MouseEvent::Button::kX2:
      key = VirtualKey::kXButton2;
      break;
    default:
      return;
  }
  // The press and its release are the binding's, not ImGui's.
  e.set_handled(true);
  swallow_mouse_up_ = true;
  BindCaptured(rex::ui::VirtualKeyToString(key));
}

void LauncherApp::OnMouseUp(rex::ui::MouseEvent& e) {
  Wake();
  if (swallow_mouse_up_) {
    swallow_mouse_up_ = false;
    e.set_handled(true);
  }
}

void LauncherApp::OnMouseWheel(rex::ui::MouseEvent& e) {
  Wake();
  if (capture_row_ == SIZE_MAX || e.scroll_y() == 0) {
    return;
  }
  e.set_handled(true);
  BindCaptured(e.scroll_y() > 0 ? "WheelUp" : "WheelDown");
}

//------------------------------------------------------------------------------
// Actions
//------------------------------------------------------------------------------

bool LauncherApp::Save() {
  std::map<std::string, std::vector<rex::cvar::ConfigEdit>> edits;
  std::map<std::string, std::vector<size_t>> rows_of_file;
  for (size_t i = 0; i < manifest_.rows.size(); ++i) {
    const Row& row = manifest_.rows[i];
    if (row.key.empty() || SameValue(states_[i].current, states_[i].saved)) {
      continue;
    }
    edits[row.file].push_back(rex::cvar::ConfigEdit{row.key, states_[i].current});
    rows_of_file[row.file].push_back(i);
  }
  size_t saved = 0;
  for (auto& [file, file_edits] : edits) {
    std::string error;
    if (!settings_.Write(file, file_edits, error)) {
      ShowMessage(error, true);
      return false;
    }
    for (size_t i : rows_of_file[file]) {
      states_[i].saved = states_[i].current;
    }
    saved += file_edits.size();
  }
  if (saved) {
    ShowMessage(fmt::format("Saved {} setting(s), comments kept", saved), false);
  }
  return true;
}

void LauncherApp::ResetPage(bool all_pages) {
  size_t count = 0;
  for (size_t i = 0; i < manifest_.rows.size(); ++i) {
    const Row& row = manifest_.rows[i];
    if ((all_pages || row.page == page_) && row.player_default && !row.disabled &&
        !SameValue(states_[i].current, *row.player_default)) {
      states_[i].current = *row.player_default;
      ++count;
    }
  }
  ShowMessage(count ? fmt::format("{} setting(s) back to the defaults, not saved yet", count)
                    : "Already at the defaults",
              false);
}

void LauncherApp::RunAction(const std::string& action) {
  if (action == "bindings") {
    bindings_view_ = true;
    StopCapture();
  } else if (action == "open_settings") {
    OpenFolder(settings_dir_);
  } else if (action == "open_logs") {
    std::error_code ec;
    const std::filesystem::path logs = LogsDir();
    if (std::filesystem::is_directory(logs, ec)) {
      OpenFolder(logs);
    } else {
      ShowMessage("No logs yet: the game writes them when it runs", false);
    }
  } else if (action == "reset_page") {
    ResetPage(false);
  } else if (action == "reset_all") {
    ResetPage(true);
  } else {
    ShowMessage("This button isn't available yet", false);
  }
}

void LauncherApp::Play(bool safe_mode) {
  if (game_running_) {
    return;
  }
  // Safe mode is a one-off: the files keep the player's settings.
  if (!safe_mode && !Save()) {
    return;
  }
  std::vector<std::string> args;
  if (safe_mode) {
    args = manifest_.safe_mode_args;
  }
  auto process = std::make_shared<GameProcess>();
  std::string error;
  if (!process->Start(game_exe_, args, exe_dir_, error)) {
    ShowMessage(error, true);
    return;
  }
  REXLOG_INFO("Launcher: started {}{}", REXLAUNCHER_GAME_EXE, safe_mode ? " (safe mode)" : "");
  game_running_ = true;
  crash_text_.clear();
  // While the game runs the launcher has no window and no GPU device. Torn
  // down outside the paint this is called from.
  app_context().CallInUIThreadDeferred([this] { DestroyUI(); });
  wait_thread_ = std::thread([this, process] {
    const int code = process->Wait();
    app_context().CallInUIThread([this, code] { OnGameExited(code); });
  });
}

void LauncherApp::OnGameExited(int code) {
  if (wait_thread_.joinable()) {
    wait_thread_.join();
  }
  game_running_ = false;
  REXLOG_INFO("Launcher: the game exited with {}", DescribeExitCode(code));
  if (code == 0) {
    // Done: the launcher goes with the game.
    app_context().QuitFromUIThread();
    return;
  }
  crash_text_ = fmt::format("The game stopped with code {}.", DescribeExitCode(code));
  // The game may have saved settings itself (the F4 overlay); a monitor may
  // have come or gone.
  displays_ = rex::ui::EnumerateDisplays();
  LoadSettingsState();
  if (!CreateUI()) {
    app_context().QuitFromUIThread();
  }
}

}  // namespace

}  // namespace rex::launcher

REX_DEFINE_APP(launcher, rex::launcher::LauncherApp::Create)
