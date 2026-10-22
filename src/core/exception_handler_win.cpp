/**
 ******************************************************************************
 * Xenia : Xbox 360 Emulator Research Project                                 *
 ******************************************************************************
 * Copyright 2015 Ben Vanik. All rights reserved.                             *
 * Released under the BSD license - see LICENSE in the root for more details. *
 ******************************************************************************
 *
 * @modified    Tom Clay, 2026 - Adapted for ReXGlue runtime
 */

#include <rex/exception_handler.h>

#if REX_PLATFORM_WIN32

#include "platform_win.h"

#include <atomic>
#include <csignal>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <exception>
#include <string>
#include <typeinfo>

#include <rex/assert.h>
#include <rex/math.h>

namespace rex::arch {

// Handle of the added VectoredExceptionHandler.
void* veh_handle_ = nullptr;
// Handle of the added VectoredContinueHandler.
void* vch_handle_ = nullptr;

// This can be as large as needed, but isn't often needed.
// As we will be sometimes firing many exceptions we want to avoid having to
// scan the table too much or invoke many custom handlers.
constexpr size_t kMaxHandlerCount = 8;

// All custom handlers, left-aligned and null terminated.
// Executed in order.
std::pair<ExceptionHandler::Handler, void*> handlers_[kMaxHandlerCount];

LONG CALLBACK ExceptionHandlerCallback(PEXCEPTION_POINTERS ex_info) {
  // Visual Studio SetThreadName.
  if (ex_info->ExceptionRecord->ExceptionCode == 0x406D1388) {
    return EXCEPTION_CONTINUE_SEARCH;
  }

  HostThreadContext thread_context;
  thread_context.rip = ex_info->ContextRecord->Rip;
  thread_context.eflags = ex_info->ContextRecord->EFlags;
  std::memcpy(thread_context.int_registers, &ex_info->ContextRecord->Rax,
              sizeof(thread_context.int_registers));
  std::memcpy(thread_context.xmm_registers, &ex_info->ContextRecord->Xmm0,
              sizeof(thread_context.xmm_registers));

  // https://msdn.microsoft.com/en-us/library/ms679331(v=vs.85).aspx
  // https://msdn.microsoft.com/en-us/library/aa363082(v=vs.85).aspx
  Exception ex;
  switch (ex_info->ExceptionRecord->ExceptionCode) {
    case STATUS_ILLEGAL_INSTRUCTION:
      ex.InitializeIllegalInstruction(&thread_context);
      break;
    case STATUS_ACCESS_VIOLATION: {
      Exception::AccessViolationOperation access_violation_operation;
      switch (ex_info->ExceptionRecord->ExceptionInformation[0]) {
        case 0:
          access_violation_operation = Exception::AccessViolationOperation::kRead;
          break;
        case 1:
          access_violation_operation = Exception::AccessViolationOperation::kWrite;
          break;
        default:
          access_violation_operation = Exception::AccessViolationOperation::kUnknown;
          break;
      }
      ex.InitializeAccessViolation(&thread_context,
                                   ex_info->ExceptionRecord->ExceptionInformation[1],
                                   access_violation_operation);
    } break;
    default:
      // Unknown/unhandled type.
      return EXCEPTION_CONTINUE_SEARCH;
  }

  for (size_t i = 0; i < rex::countof(handlers_) && handlers_[i].first; ++i) {
    if (handlers_[i].first(&ex, handlers_[i].second)) {
      // Exception handled.
      ex_info->ContextRecord->Rip = thread_context.rip;
      ex_info->ContextRecord->EFlags = thread_context.eflags;
      uint32_t modified_register_index;
      uint16_t modified_int_registers_remaining = ex.modified_int_registers();
      while (rex::bit_scan_forward(modified_int_registers_remaining, &modified_register_index)) {
        modified_int_registers_remaining &= ~(UINT16_C(1) << modified_register_index);
        (&ex_info->ContextRecord->Rax)[modified_register_index] =
            thread_context.int_registers[modified_register_index];
      }
      uint16_t modified_xmm_registers_remaining = ex.modified_xmm_registers();
      while (rex::bit_scan_forward(modified_xmm_registers_remaining, &modified_register_index)) {
        modified_xmm_registers_remaining &= ~(UINT16_C(1) << modified_register_index);
        std::memcpy(&ex_info->ContextRecord->Xmm0 + modified_register_index,
                    &thread_context.xmm_registers[modified_register_index], sizeof(vec128_t));
      }
      return EXCEPTION_CONTINUE_EXECUTION;
    }
  }
  return EXCEPTION_CONTINUE_SEARCH;
}

// Last-chance crash report (upstream PR #440, with changes for the ReXGlue
// fork). The handler above only claims the faults guest memory emulation
// needs; anything else ended the process with nothing written down. Now an
// unhandled exception, std::terminate and abort() append a report to
// rexglue-crash.txt beside the executable: what happened, the host registers
// and a backtrace as module+RVA, to symbolize with llvm-symbolizer against the
// title's PDB. A file rather than the log, as the logger may be what broke;
// appended, so the next run never overwrites a crash.

namespace {

std::atomic<bool> crash_report_written{false};

std::FILE* OpenCrashReport() {
  wchar_t path[MAX_PATH];
  DWORD length = GetModuleFileNameW(nullptr, path, MAX_PATH);
  if (length && length < MAX_PATH) {
    wchar_t* slash = std::wcsrchr(path, L'\\');
    std::FILE* file = nullptr;
    if (slash &&
        !wcscpy_s(slash + 1, MAX_PATH - size_t(slash + 1 - path), L"rexglue-crash.txt") &&
        !_wfopen_s(&file, path, L"a") && file) {
      return file;
    }
  }
  // The executable's folder may be read-only: the working directory then.
  std::FILE* file = nullptr;
  return !fopen_s(&file, "rexglue-crash.txt", "a") ? file : nullptr;
}

void WriteFrame(std::FILE* file, uint32_t index, uint64_t address) {
  HMODULE module = nullptr;
  char name[MAX_PATH] = "?";
  if (GetModuleHandleExA(
          GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
          reinterpret_cast<LPCSTR>(address), &module) &&
      module) {
    char full[MAX_PATH];
    if (GetModuleFileNameA(module, full, MAX_PATH)) {
      const char* slash = std::strrchr(full, '\\');
      std::snprintf(name, sizeof(name), "%s", slash ? slash + 1 : full);
    }
  }
  std::fprintf(file, "    %2u  %-28s +0x%llX\n", index, name,
               static_cast<unsigned long long>(address - reinterpret_cast<uintptr_t>(module)));
}

// The faulting thread's stack, unwound from its context (the frames of the
// report itself aren't in it). Every function of the recompiled code has
// unwind data; a frame without (a leaf) returns through the top of its stack.
void WriteBacktraceFromContext(std::FILE* file, const CONTEXT& fault_context) {
  ULONG_PTR stack_low = 0, stack_high = 0;
  GetCurrentThreadStackLimits(&stack_low, &stack_high);
  CONTEXT context = fault_context;
  std::fprintf(file, "  backtrace (module+RVA, for llvm-symbolizer):\n");
  for (uint32_t i = 0; i < 64 && context.Rip; ++i) {
    WriteFrame(file, i, context.Rip);
    DWORD64 image_base = 0;
    PRUNTIME_FUNCTION function = RtlLookupFunctionEntry(context.Rip, &image_base, nullptr);
    const DWORD64 rsp = context.Rsp;
    if (function) {
      void* handler_data = nullptr;
      DWORD64 establisher_frame = 0;
      RtlVirtualUnwind(UNW_FLAG_NHANDLER, image_base, context.Rip, function, &context,
                       &handler_data, &establisher_frame, nullptr);
    } else {
      if (rsp < stack_low || rsp + sizeof(DWORD64) > stack_high) {
        break;
      }
      context.Rip = *reinterpret_cast<const DWORD64*>(rsp);
      context.Rsp = rsp + sizeof(DWORD64);
    }
    // A broken stack: stop rather than loop or walk off it.
    if (context.Rsp <= rsp || context.Rsp < stack_low || context.Rsp > stack_high) {
      break;
    }
  }
}

void WriteBacktraceHere(std::FILE* file) {
  void* frames[62];
  USHORT count = CaptureStackBackTrace(1, USHORT(rex::countof(frames)), frames, nullptr);
  std::fprintf(file, "  backtrace (module+RVA, for llvm-symbolizer; includes the report's frames):\n");
  for (USHORT i = 0; i < count; ++i) {
    WriteFrame(file, i, reinterpret_cast<uintptr_t>(frames[i]));
  }
}

void WriteCrashReport(const char* what, const char* detail, const CONTEXT* context) {
  // Once: a crash while reporting, or abort() after terminate, adds nothing.
  if (crash_report_written.exchange(true)) {
    return;
  }
  std::FILE* file = OpenCrashReport();
  if (!file) {
    return;
  }
  SYSTEMTIME time;
  GetLocalTime(&time);
  std::fprintf(file, "\n=== %04u-%02u-%02u %02u:%02u:%02u  pid %lu  thread %lu ===\n", time.wYear,
               time.wMonth, time.wDay, time.wHour, time.wMinute, time.wSecond,
               GetCurrentProcessId(), GetCurrentThreadId());
  std::fprintf(file, "%s%s%s\n", what, detail && *detail ? ": " : "", detail ? detail : "");
  if (context) {
    const DWORD64* gpr = &context->Rax;
    static const char* const kGprNames[16] = {"rax", "rcx", "rdx", "rbx", "rsp", "rbp",
                                              "rsi", "rdi", "r8",  "r9",  "r10", "r11",
                                              "r12", "r13", "r14", "r15"};
    std::fprintf(file, "  rip=%016llX eflags=%08lX\n",
                 static_cast<unsigned long long>(context->Rip), context->EFlags);
    for (uint32_t i = 0; i < 16; ++i) {
      std::fprintf(file, "  %-3s=%016llX%s", kGprNames[i], static_cast<unsigned long long>(gpr[i]),
                   (i & 3) == 3 ? "\n" : "");
    }
    WriteBacktraceFromContext(file, *context);
  } else {
    WriteBacktraceHere(file);
  }
  std::fflush(file);
  std::fclose(file);
}

// The type and message of the exception in flight, if any.
std::string DescribeCurrentException() {
  std::exception_ptr exception = std::current_exception();
  if (!exception) {
    return "no exception in flight";
  }
  try {
    std::rethrow_exception(exception);
  } catch (const std::exception& e) {
    return std::string(typeid(e).name()) + " -- " + e.what();
  } catch (...) {
    return "an exception not derived from std::exception";
  }
}

std::terminate_handler previous_terminate_handler = nullptr;

void TerminateHandler() {
  WriteCrashReport("std::terminate called", DescribeCurrentException().c_str(), nullptr);
  // Another handler may log more (the guest thread's, xthread.cpp).
  if (previous_terminate_handler) {
    previous_terminate_handler();
  }
  std::_Exit(3);
}

// std::set_terminate is per thread in the Microsoft C runtime, so on other
// threads an exception nobody catches reaches abort(), which raises SIGABRT
// (process-wide) before failing fast past the unhandled exception filter.
void AbortSignalHandler(int) {
  WriteCrashReport("abort() called", DescribeCurrentException().c_str(), nullptr);
  std::_Exit(3);
}

LONG WINAPI CrashUnhandledExceptionFilter(EXCEPTION_POINTERS* ex_info) {
  const EXCEPTION_RECORD& record = *ex_info->ExceptionRecord;
  char detail[192];
  if (record.ExceptionCode == STATUS_ACCESS_VIOLATION && record.NumberParameters >= 2) {
    const ULONG_PTR operation = record.ExceptionInformation[0];
    std::snprintf(detail, sizeof(detail), "access violation at %p: %s of %p",
                  record.ExceptionAddress,
                  operation == 0 ? "read" : operation == 1 ? "write" : operation == 8 ? "execute"
                                                                                   : "access",
                  reinterpret_cast<void*>(record.ExceptionInformation[1]));
  } else {
    std::snprintf(detail, sizeof(detail), "code 0x%08lX at %p", record.ExceptionCode,
                  record.ExceptionAddress);
  }
  WriteCrashReport("unhandled exception", detail, ex_info->ContextRecord);
  return EXCEPTION_CONTINUE_SEARCH;
}

}  // namespace

void ExceptionHandler::Install(Handler fn, void* data) {
  if (!veh_handle_) {
    veh_handle_ = AddVectoredExceptionHandler(1, ExceptionHandlerCallback);
    previous_terminate_handler = std::set_terminate(TerminateHandler);
    std::signal(SIGABRT, AbortSignalHandler);
    SetUnhandledExceptionFilter(CrashUnhandledExceptionFilter);

    if (IsDebuggerPresent()) {
      // TODO(benvanik): do we need a continue handler if a debugger is
      // attached?
      // vch_handle_ = AddVectoredContinueHandler(1, ExceptionHandlerCallback);
    }
  }

  for (size_t i = 0; i < rex::countof(handlers_); ++i) {
    if (!handlers_[i].first) {
      handlers_[i].first = fn;
      handlers_[i].second = data;
      return;
    }
  }
  assert_always("Too many exception handlers installed");
}

void ExceptionHandler::Uninstall(Handler fn, void* data) {
  for (size_t i = 0; i < rex::countof(handlers_); ++i) {
    if (handlers_[i].first == fn && handlers_[i].second == data) {
      for (; i < rex::countof(handlers_) - 1; ++i) {
        handlers_[i] = handlers_[i + 1];
      }
      handlers_[i].first = nullptr;
      handlers_[i].second = nullptr;
      break;
    }
  }

  bool has_any = false;
  for (size_t i = 0; i < rex::countof(handlers_); ++i) {
    if (handlers_[i].first) {
      has_any = true;
      break;
    }
  }
  if (!has_any) {
    if (veh_handle_) {
      RemoveVectoredExceptionHandler(veh_handle_);
      veh_handle_ = nullptr;
    }
    if (vch_handle_) {
      RemoveVectoredContinueHandler(vch_handle_);
      vch_handle_ = nullptr;
    }
  }
}

}  // namespace rex::arch

#endif  // REX_PLATFORM_WIN32
