/**
 * @file        system/thread_sampler.cpp
 * @brief       Wall-clock sampler of the title's threads
 *
 * Every thread_sampler_interval_us the sampler suspends each selected thread,
 * unwinds its host stack and resumes it. Waiting threads are sampled too, so a
 * report shows where a thread's frame time goes (guest sub_XXXXXXXX code, host
 * calls, waits), not just its CPU use. Symbols come from the PDBs next to the
 * executable. Windows x64 only.
 *
 * @copyright   Copyright (c) 2026 ReXGlue fork contributors
 *              All rights reserved.
 *
 * @license     BSD 3-Clause License
 *              See LICENSE file in the project root for full license text.
 */

#include <rex/perf/thread_sampler.h>

#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstdint>
#include <mutex>
#include <string>
#include <string_view>
#include <thread>

#include <rex/cvar.h>
#include <rex/filesystem.h>
#include <rex/logging.h>
#include <rex/platform.h>
#include <rex/thread.h>

#if REX_PLATFORM_WIN32 && (defined(_M_X64) || defined(__x86_64__))
#define REX_THREAD_SAMPLER 1
#include <windows.h>
#include <dbghelp.h>
#include <tlhelp32.h>

#include <algorithm>
#include <cctype>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <map>
#include <set>
#include <unordered_map>
#include <vector>

#include <fmt/chrono.h>
#include <fmt/format.h>
#else
#define REX_THREAD_SAMPLER 0
#endif

REXCVAR_DEFINE_INT32(thread_sampler_request, 0, "Runtime/Profiling",
                     "Thread sampler: change to a new nonzero number to capture "
                     "thread_sampler_seconds now")
    .range(0, INT32_MAX)
    .lifecycle(rex::cvar::Lifecycle::kHotReload);
REXCVAR_DEFINE_INT32(thread_sampler_delay, 0, "Runtime/Profiling",
                     "Thread sampler: capture once this many seconds after start (0: only on "
                     "request)")
    .range(0, 3600)
    .lifecycle(rex::cvar::Lifecycle::kRequiresRestart);
REXCVAR_DEFINE_INT32(thread_sampler_seconds, 20, "Runtime/Profiling",
                     "Thread sampler: length of a capture")
    .range(1, 600)
    .lifecycle(rex::cvar::Lifecycle::kHotReload);
REXCVAR_DEFINE_INT32(thread_sampler_interval_us, 1000, "Runtime/Profiling",
                     "Thread sampler: time between sampling rounds")
    .range(100, 100000)
    .lifecycle(rex::cvar::Lifecycle::kHotReload);
REXCVAR_DEFINE_STRING(thread_sampler_threads, "", "Runtime/Profiling",
                      "Thread sampler: threads whose name contains this; empty: guest threads "
                      "and the thread calling VdSwap; \"*\": every thread")
    .lifecycle(rex::cvar::Lifecycle::kHotReload);
REXCVAR_DEFINE_STRING(thread_sampler_dir, "", "Runtime/Profiling",
                      "Thread sampler: report folder (empty: logs/profile next to the "
                      "executable). Each capture adds sampler-<time> with report.txt and "
                      "stacks.folded (speedscope, flamegraph.pl)")
    .lifecycle(rex::cvar::Lifecycle::kHotReload);

namespace rex::perf {
namespace {

std::atomic<uint32_t> swap_thread{0};
std::atomic<uint64_t> swap_count{0};

#if REX_THREAD_SAMPLER

std::mutex worker_mutex;
std::condition_variable worker_wake;
bool worker_started = false;
bool requested = false;

constexpr int kMaxFrames = 96;
constexpr size_t kTopRows = 40;
constexpr size_t kDetailedThreads = 6;

using Stack = std::vector<uint64_t>;  // innermost first

struct Target {
  DWORD tid = 0;
  HANDLE handle = nullptr;
  uint64_t stack_base = 0;
  std::string name;
  uint64_t cpu_start = 0, cpu_end = 0;  // 100 ns
  uint32_t samples = 0;
  std::map<Stack, uint32_t> stacks;
};

uint64_t ThreadCpu(HANDLE thread) {
  FILETIME created, exited, kernel, user;
  if (!GetThreadTimes(thread, &created, &exited, &kernel, &user))
    return 0;
  auto value = [](FILETIME t) { return (uint64_t(t.dwHighDateTime) << 32) | t.dwLowDateTime; };
  return value(kernel) + value(user);
}

std::string ThreadName(HANDLE thread) {
  // Windows 10 1607+, looked up like SetThreadDescription in threading_win.cpp.
  using GetThreadDescriptionFn = HRESULT(WINAPI*)(HANDLE, PWSTR*);
  static const auto get_description = reinterpret_cast<GetThreadDescriptionFn>(
      GetProcAddress(GetModuleHandleW(L"kernel32.dll"), "GetThreadDescription"));
  std::string name;
  PWSTR wide = nullptr;
  if (get_description && SUCCEEDED(get_description(thread, &wide)) && wide) {
    const int size = WideCharToMultiByte(CP_UTF8, 0, wide, -1, nullptr, 0, nullptr, nullptr);
    if (size > 1) {
      name.resize(size - 1);
      WideCharToMultiByte(CP_UTF8, 0, wide, -1, name.data(), size, nullptr, nullptr);
    }
    LocalFree(wide);
  }
  return name;
}

// XThread::set_name: "<name> (XXXXXXXX)" with the guest handle.
bool IsGuestThreadName(const std::string& name) {
  if (name.size() < 11 || name.back() != ')' || name[name.size() - 10] != '(')
    return false;
  return std::all_of(name.end() - 9, name.end() - 1,
                     [](char c) { return std::isxdigit(uint8_t(c)) != 0; });
}

// NT_TIB::StackBase of a live thread of this process (0 if unknown).
uint64_t ThreadStackBase(HANDLE thread) {
  struct ThreadBasicInformation {
    LONG exit_status;
    PVOID teb;
    PVOID client_id[2];
    ULONG_PTR affinity;
    LONG priority, base_priority;
  };
  using QueryFn = LONG(NTAPI*)(HANDLE, int, PVOID, ULONG, PULONG);
  static const auto query = reinterpret_cast<QueryFn>(
      GetProcAddress(GetModuleHandleW(L"ntdll.dll"), "NtQueryInformationThread"));
  ThreadBasicInformation info{};
  if (!query || query(thread, 0, &info, sizeof(info), nullptr) < 0 || !info.teb)
    return 0;
  return reinterpret_cast<uint64_t>(static_cast<const NT_TIB*>(info.teb)->StackBase);
}

// Runs while the thread is suspended: no allocation, no lock the target may hold.
// Saved registers come from the live stack and can be stale: leave the stack or fault, end the walk.
int Unwind(CONTEXT context, uint64_t stack_base, uint64_t* pcs) {
  const DWORD64 low = context.Rsp;
  const DWORD64 high = stack_base ? stack_base : ~DWORD64(0);
  auto on_stack = [&](DWORD64 p) { return p >= low && p < high; };
  int count = 0;
  __try {
    while (count < kMaxFrames && context.Rip && on_stack(context.Rsp)) {
      pcs[count++] = context.Rip;
      const DWORD64 sp = context.Rsp;
      DWORD64 image = 0;
      PRUNTIME_FUNCTION entry = RtlLookupFunctionEntry(context.Rip, &image, nullptr);
      if (!entry) {
        // A leaf (syscall stubs): the return address is on top. Deeper, unknown
        // code ends the walk.
        if (count > 1)
          break;
        context.Rip = *reinterpret_cast<const DWORD64*>(context.Rsp);
        context.Rsp += 8;
      } else {
        // UNWIND_INFO byte 3, low nibble: frame register (0: none), numbered as Rax..R15.
        const uint8_t frame_register =
            reinterpret_cast<const uint8_t*>(image + entry->UnwindData)[3] & 0x0F;
        if (frame_register && !on_stack((&context.Rax)[frame_register]))
          break;
        PVOID handler_data = nullptr;
        DWORD64 frame = 0;
        RtlVirtualUnwind(UNW_FLAG_NHANDLER, image, context.Rip, entry, &context, &handler_data,
                         &frame, nullptr);
      }
      if (context.Rsp <= sp)
        break;
    }
  } __except (EXCEPTION_EXECUTE_HANDLER) {  // NOLINT
  }
  return count;
}

void Sample(Target& target) {
  uint64_t pcs[kMaxFrames];
  int count = 0;
  if (SuspendThread(target.handle) == DWORD(-1))
    return;
  CONTEXT context{};
  context.ContextFlags = CONTEXT_FULL;
  if (GetThreadContext(target.handle, &context))
    count = Unwind(context, target.stack_base, pcs);
  ResumeThread(target.handle);
  if (count) {
    ++target.samples;
    ++target.stacks[Stack(pcs, pcs + count)];
  }
}

class Symbols {
 public:
  Symbols() {
    // Own DbgHelp session: Tracy may have one on GetCurrentProcess().
    DuplicateHandle(GetCurrentProcess(), GetCurrentProcess(), GetCurrentProcess(), &process_, 0,
                    FALSE, DUPLICATE_SAME_ACCESS);
    SymSetOptions(SYMOPT_UNDNAME | SYMOPT_DEFERRED_LOADS | SYMOPT_FAIL_CRITICAL_ERRORS |
                  SYMOPT_NO_PROMPTS);
    const std::string path = rex::filesystem::GetExecutableFolder().string();
    ready_ = process_ && SymInitialize(process_, path.c_str(), TRUE);
  }
  ~Symbols() {
    if (ready_)
      SymCleanup(process_);
    if (process_)
      CloseHandle(process_);
  }
  Symbols(const Symbols&) = delete;
  Symbols& operator=(const Symbols&) = delete;

  // Function names at a pc, innermost (inlined) first; return addresses look up pc - 1.
  const std::vector<std::string>& Names(uint64_t pc, bool return_address) {
    const uint64_t address = return_address ? pc - 1 : pc;
    auto [it, inserted] = cache_.try_emplace(address);
    if (inserted)
      it->second = Resolve(address);
    return it->second;
  }

 private:
  static std::string Clean(const char* name) {
    std::string_view view(name);
    if (view.starts_with("__imp__"))
      view.remove_prefix(7);
    std::string out(view.substr(0, 160));
    std::replace(out.begin(), out.end(), ';', ':');
    return out;
  }

  std::vector<std::string> Resolve(uint64_t address) {
    std::vector<std::string> names;
    alignas(SYMBOL_INFO) char buffer[sizeof(SYMBOL_INFO) + MAX_SYM_NAME];
    auto* symbol = reinterpret_cast<SYMBOL_INFO*>(buffer);
    auto reset = [&] {
      std::memset(buffer, 0, sizeof(SYMBOL_INFO));
      symbol->SizeOfStruct = sizeof(SYMBOL_INFO);
      symbol->MaxNameLen = MAX_SYM_NAME;
    };
    if (ready_) {
      if (const DWORD inlined = SymAddrIncludeInlineTrace(process_, address)) {
        DWORD context = 0, index = 0;
        if (SymQueryInlineTrace(process_, address, 0, address, address, &context, &index)) {
          for (DWORD i = 0; i < inlined; ++i) {
            reset();
            DWORD64 displacement = 0;
            if (SymFromInlineContext(process_, address, context + i, &displacement, symbol))
              names.push_back(Clean(symbol->Name));
          }
        }
      }
      reset();
      DWORD64 displacement = 0;
      if (SymFromAddr(process_, address, &displacement, symbol)) {
        std::string name = Clean(symbol->Name);
        if (names.empty() || names.back() != name)
          names.push_back(std::move(name));
      }
    }
    if (names.empty()) {
      IMAGEHLP_MODULE64 module{};
      module.SizeOfStruct = sizeof(module);
      if (ready_ && SymGetModuleInfo64(process_, address, &module))
        names.push_back(fmt::format("{}!{:X}", module.ModuleName, address - module.BaseOfImage));
      else
        names.push_back(fmt::format("0x{:X}", address));
    }
    return names;
  }

  HANDLE process_ = nullptr;
  bool ready_ = false;
  std::unordered_map<uint64_t, std::vector<std::string>> cache_;
};

// Recompiled guest functions: sub_XXXXXXXX.
bool IsGuestFunction(const std::string& name) {
  return name.size() == 12 && name.starts_with("sub_") &&
         std::all_of(name.begin() + 4, name.end(),
                     [](char c) { return std::isxdigit(uint8_t(c)) != 0; });
}

// A thread blocked in the kernel shows a syscall stub (Nt*, Zw*) on top.
bool IsWait(const std::string& leaf) {
  return leaf.starts_with("Nt") || leaf.starts_with("Zw");
}

struct Row {
  uint32_t total = 0, wait = 0;
};

void Table(std::ofstream& out, const char* title, const std::unordered_map<std::string, Row>& rows,
           uint32_t samples, double ms_per_sample, uint64_t frames) {
  std::vector<std::pair<std::string, Row>> sorted(rows.begin(), rows.end());
  std::sort(sorted.begin(), sorted.end(),
            [](const auto& a, const auto& b) { return a.second.total > b.second.total; });
  out << fmt::format("\n  {}\n  {:>7} {:>9} {:>9}  {}\n", title, "%", "ms/frame", "wait ms",
                     "function");
  const double per_frame = frames ? ms_per_sample / double(frames) : 0;
  for (size_t i = 0; i < std::min(sorted.size(), kTopRows); ++i) {
    const auto& [name, row] = sorted[i];
    out << fmt::format("  {:7.2f} {:9.3f} {:9.3f}  {}\n", 100.0 * row.total / samples,
                       row.total * per_frame, row.wait * per_frame, name);
  }
}

void Report(std::vector<Target>& targets, double wall_ms, uint64_t rounds, uint64_t frames,
            uint32_t presenter) {
  std::filesystem::path root = REXCVAR_GET(thread_sampler_dir);
  if (root.empty())
    root = rex::filesystem::GetExecutableFolder() / "logs" / "profile";
  const auto stamp = std::chrono::floor<std::chrono::seconds>(std::chrono::system_clock::now());
  const std::filesystem::path dir = root / fmt::format("sampler-{:%Y%m%dT%H%M%S}", stamp);
  std::error_code error;
  std::filesystem::create_directories(dir, error);

  Symbols symbols;
  const double ms_per_sample = rounds ? wall_ms / double(rounds) : 0;
  std::sort(targets.begin(), targets.end(), [&](const Target& a, const Target& b) {
    if ((a.tid == presenter) != (b.tid == presenter))
      return a.tid == presenter;
    return a.cpu_end - a.cpu_start > b.cpu_end - b.cpu_start;
  });

  std::ofstream report(dir / "report.txt");
  std::ofstream folded(dir / "stacks.folded");
  report << fmt::format(
      "Wall {:.0f} ms, {} rounds ({:.3f} ms each), {} guest frames (VdSwap; {:.1f} fps, "
      "{:.2f} ms a frame). ms/frame below: per guest frame.\n"
      "'wait ms': samples blocked in a syscall (Nt*/Zw* on top of the stack).\n",
      wall_ms, rounds, ms_per_sample, frames, frames * 1000.0 / wall_ms,
      frames ? wall_ms / frames : 0.0);

  report << "\nThreads (CPU % of wall, samples blocked in a syscall)\n";
  for (Target& target : targets) {
    uint32_t waits = 0;
    for (const auto& [stack, count] : target.stacks)
      if (IsWait(symbols.Names(stack[0], false).front()))
        waits += count;
    report << fmt::format("  {:6.1f}% cpu {:6.1f}% wait  tid {:6}  {}{}\n",
                          (target.cpu_end - target.cpu_start) / 100.0 / wall_ms,
                          target.samples ? 100.0 * waits / target.samples : 0.0, target.tid,
                          target.name, target.tid == presenter ? "  <- VdSwap" : "");
  }

  size_t detailed = 0;
  for (const Target& target : targets) {
    if (!target.samples)
      continue;
    std::unordered_map<std::string, Row> guest_self, guest_total, leaf;
    for (const auto& [stack, count] : target.stacks) {
      std::vector<std::string> names;  // innermost first, inlined frames expanded
      for (size_t i = 0; i < stack.size(); ++i)
        for (const std::string& name : symbols.Names(stack[i], i != 0))
          names.push_back(name);
      const bool wait = IsWait(names.front());
      auto add = [&](std::unordered_map<std::string, Row>& table, const std::string& name) {
        Row& row = table[name];
        row.total += count;
        row.wait += wait ? count : 0;
      };
      add(leaf, names.front());
      std::set<std::string> seen;
      bool self = false;
      for (const std::string& name : names) {
        if (!IsGuestFunction(name))
          continue;
        if (!self) {
          add(guest_self, name);
          self = true;
        }
        if (seen.insert(name).second)
          add(guest_total, name);
      }
      if (!self)
        add(guest_self, "[no guest frame]");

      std::string line = target.name.empty() ? fmt::format("tid {}", target.tid) : target.name;
      std::replace(line.begin(), line.end(), ';', ':');
      for (auto it = names.rbegin(); it != names.rend(); ++it)
        line += ';' + *it;
      folded << line << ' ' << count << '\n';
    }
    if (detailed++ >= kDetailedThreads && target.tid != presenter)
      continue;
    report << fmt::format("\n=== {} (tid {}){}: {} samples ===\n", target.name, target.tid,
                          target.tid == presenter ? ", VdSwap thread" : "", target.samples);
    Table(report,
          "Guest self (innermost sub_ on the stack: its own code and the host calls it makes)",
          guest_self, target.samples, ms_per_sample, frames);
    Table(report, "Guest inclusive (sub_ anywhere on the stack)", guest_total, target.samples,
          ms_per_sample, frames);
    Table(report, "Leaf (any function on top of the stack)", leaf, target.samples, ms_per_sample,
          frames);
  }
  REXLOG_INFO("Thread sampler: {:.1f} fps over {:.0f} ms, report {}", frames * 1000.0 / wall_ms,
              wall_ms, rex::path_to_utf8(dir / "report.txt"));
}

void Capture() {
  const DWORD self = GetCurrentThreadId();
  const uint32_t presenter = swap_thread.load(std::memory_order_relaxed);
  const std::string filter = REXCVAR_GET(thread_sampler_threads);
  std::vector<Target> targets;
  HANDLE snapshot = CreateToolhelp32Snapshot(TH32CS_SNAPTHREAD, 0);
  if (snapshot == INVALID_HANDLE_VALUE)
    return;
  THREADENTRY32 entry{};
  entry.dwSize = sizeof(entry);
  for (BOOL more = Thread32First(snapshot, &entry); more; more = Thread32Next(snapshot, &entry)) {
    if (entry.th32OwnerProcessID != GetCurrentProcessId() || entry.th32ThreadID == self)
      continue;
    HANDLE handle =
        OpenThread(THREAD_SUSPEND_RESUME | THREAD_GET_CONTEXT | THREAD_QUERY_INFORMATION, FALSE,
                   entry.th32ThreadID);
    if (!handle)
      continue;
    std::string name = ThreadName(handle);
    const bool wanted = filter == "*" ||
                        (filter.empty() ? IsGuestThreadName(name) || entry.th32ThreadID == presenter
                                        : name.find(filter) != std::string::npos);
    if (!wanted) {
      CloseHandle(handle);
      continue;
    }
    Target& target = targets.emplace_back();
    target.tid = entry.th32ThreadID;
    target.handle = handle;
    target.stack_base = ThreadStackBase(handle);
    target.name = std::move(name);
  }
  CloseHandle(snapshot);
  if (targets.empty()) {
    REXLOG_WARN("Thread sampler: no thread matches '{}'", filter);
    return;
  }

  const auto interval = std::chrono::microseconds(REXCVAR_GET(thread_sampler_interval_us));
  const auto length = std::chrono::seconds(REXCVAR_GET(thread_sampler_seconds));
  REXLOG_INFO("Thread sampler: {} threads for {} s", targets.size(), length.count());
  for (Target& target : targets)
    target.cpu_start = ThreadCpu(target.handle);
  const uint64_t frames_start = swap_count.load(std::memory_order_relaxed);
  const auto start = std::chrono::steady_clock::now();
  auto next = start;
  uint64_t rounds = 0;
  while (std::chrono::steady_clock::now() - start < length) {
    for (Target& target : targets)
      Sample(target);
    ++rounds;
    next += interval;
    const auto now = std::chrono::steady_clock::now();
    if (next < now)
      next = now;
    std::this_thread::sleep_until(next);
  }
  const double wall_ms =
      std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - start).count();
  const uint64_t frames = swap_count.load(std::memory_order_relaxed) - frames_start;
  for (Target& target : targets)
    target.cpu_end = ThreadCpu(target.handle);
  Report(targets, wall_ms, rounds, frames, swap_thread.load(std::memory_order_relaxed));
  for (Target& target : targets)
    CloseHandle(target.handle);
}

void Run(std::chrono::steady_clock::time_point scheduled) {
  bool pending_schedule = scheduled != std::chrono::steady_clock::time_point{};
  for (;;) {
    {
      std::unique_lock lock(worker_mutex);
      if (pending_schedule) {
        if (!worker_wake.wait_until(lock, scheduled, [] { return requested; }))
          pending_schedule = false;
      } else {
        worker_wake.wait(lock, [] { return requested; });
      }
      requested = false;
    }
    Capture();
  }
}

void StartWorker(std::unique_lock<std::mutex>&, std::chrono::steady_clock::time_point scheduled) {
  if (worker_started)
    return;
  worker_started = true;
  std::thread(Run, scheduled).detach();
}

#endif  // REX_THREAD_SAMPLER

}  // namespace

void RequestThreadSamplerCapture() {
#if REX_THREAD_SAMPLER
  std::unique_lock lock(worker_mutex);
  requested = true;
  StartWorker(lock, {});
  worker_wake.notify_one();
#else
  REXLOG_WARN("Thread sampler: only on Windows x64");
#endif
}

void StartThreadSampler() {
#if REX_THREAD_SAMPLER
  if (const int delay = REXCVAR_GET(thread_sampler_delay)) {
    std::unique_lock lock(worker_mutex);
    StartWorker(lock, std::chrono::steady_clock::now() + std::chrono::seconds(delay));
  }
#endif
  rex::cvar::RegisterChangeCallback("thread_sampler_request",
                                    [](std::string_view, std::string_view value) {
                                      if (value != "0")
                                        RequestThreadSamplerCapture();
                                    });
  if (REXCVAR_GET(thread_sampler_request))
    RequestThreadSamplerCapture();
}

void NoteGuestSwap() {
  swap_thread.store(rex::thread::current_thread_system_id(), std::memory_order_relaxed);
  swap_count.fetch_add(1, std::memory_order_relaxed);
}

}  // namespace rex::perf
