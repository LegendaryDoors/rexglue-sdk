/**
 * @file        ui/windowed_app_main_sdl.cpp
 * @brief       Entry point for windowed applications (SDL3 windowing)
 *
 * @copyright   Copyright (c) 2026 Tom Clay <tomc@tctechstuff.com>
 *              All rights reserved.
 *
 * @license     BSD 3-Clause License
 *              See LICENSE file in the project root for full license text.
 */

#include <algorithm>
#include <cstdio>
#include <cstdlib>
#include <map>
#include <memory>
#include <string>
#include <vector>

#include <rex/cvar.h>
#include <rex/logging.h>
#include <rex/platform.h>
#include <rex/system.h>
#include <rex/ui/windowed_app.h>
#include <rex/ui/windowed_app_context_sdl.h>

#if REX_PLATFORM_WIN32
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#include <objbase.h>
#include <shellapi.h>
#endif

namespace {

#if defined(__x86_64__) || defined(_M_X64)
// The build assumes the instruction set it was compiled for. On an older
// processor the first such instruction crashes with no message.
#if defined(__clang__) || defined(__GNUC__)
__attribute__((target("no-avx2,no-avx,no-bmi2,no-bmi,no-fma")))
#endif
static const char* MissingCpuFeature() {
#if REX_PLATFORM_WIN32
  // The system call rather than the compiler builtin: clang's builtin needs
  // compiler-rt, which its Windows driver does not link.
#if defined(__AVX2__)
  if (!IsProcessorFeaturePresent(PF_AVX2_INSTRUCTIONS_AVAILABLE)) return "AVX2 (Intel from 2013 or AMD from 2015)";
#endif
  if (!IsProcessorFeaturePresent(PF_SSE4_2_INSTRUCTIONS_AVAILABLE)) return "SSE4.2 (Intel from 2008 or AMD from 2011)";
#elif defined(__clang__) || defined(__GNUC__)
  __builtin_cpu_init();
#if defined(__AVX2__)
  if (!__builtin_cpu_supports("avx2")) return "AVX2 (Intel from 2013 or AMD from 2015)";
#endif
#if defined(__SSE4_2__)
  if (!__builtin_cpu_supports("sse4.2")) return "SSE4.2 (Intel from 2008 or AMD from 2011)";
#endif
#if defined(__SSE4_1__)
  if (!__builtin_cpu_supports("sse4.1")) return "SSE4.1 (Intel from 2007 or AMD from 2011)";
#endif
#endif
  return nullptr;
}
#endif

int RunWindowedApp(int argc, char** argv) {
#if defined(__x86_64__) || defined(_M_X64)
  if (const char* missing = MissingCpuFeature()) {
    const std::string message =
        std::string("This build needs a processor with ") + missing + ", which this one does not have.";
    std::fprintf(stderr, "%s\n", message.c_str());
    rex::ShowSimpleMessageBox(rex::SimpleMessageBoxType::Error, message);
    return 1;
  }
#endif
  auto remaining = rex::cvar::Init(argc, argv);
  rex::cvar::ApplyEnvironment();
  rex::InitLoggingEarly();

  int result;
  {
    rex::ui::SDLWindowedAppContext app_context;
    if (!app_context.Initialize()) {
      return EXIT_FAILURE;
    }

#if REX_PLATFORM_WIN32
    // Apartment-threaded COM for shell dialogs.
    if (FAILED(CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED))) {
      return EXIT_FAILURE;
    }
#endif

    std::unique_ptr<rex::ui::WindowedApp> app = rex::ui::GetWindowedAppCreator()(app_context);

    // Match remaining positional args to the app's expected options.
    const auto& option_names = app->GetPositionalOptions();
    std::map<std::string, std::string> parsed;
    size_t count = std::min(remaining.size(), option_names.size());
    for (size_t i = 0; i < count; ++i) {
      parsed[option_names[i]] = remaining[i];
    }
    app->SetParsedArguments(std::move(parsed));

    result = app->OnInitialize() ? app_context.RunMainMessageLoop() : EXIT_FAILURE;

    app->InvokeOnDestroy();
  }

#if REX_PLATFORM_WIN32
  CoUninitialize();
#endif

  return result;
}

#if REX_PLATFORM_WIN32
// Convert wide argv from CommandLineToArgvW to UTF-8 for cvar::Init.
std::vector<std::string> WideArgsToUtf8(int argc, wchar_t** wargv) {
  std::vector<std::string> args;
  args.reserve(static_cast<size_t>(argc));
  for (int i = 0; i < argc; ++i) {
    std::wstring wide(wargv[i]);
    if (wide.empty()) {
      args.emplace_back();
      continue;
    }
    int size = WideCharToMultiByte(CP_UTF8, 0, wide.data(), static_cast<int>(wide.size()), nullptr,
                                   0, nullptr, nullptr);
    std::string utf8(static_cast<size_t>(size), '\0');
    WideCharToMultiByte(CP_UTF8, 0, wide.data(), static_cast<int>(wide.size()), utf8.data(), size,
                        nullptr, nullptr);
    args.push_back(std::move(utf8));
  }
  return args;
}
#endif

}  // namespace

#if REX_PLATFORM_WIN32

int WINAPI wWinMain(HINSTANCE hinstance, HINSTANCE hinstance_prev, LPWSTR command_line,
                    int show_cmd) {
  (void)hinstance;
  (void)hinstance_prev;
  (void)command_line;
  (void)show_cmd;

  int wargc = 0;
  wchar_t** wargv = CommandLineToArgvW(GetCommandLineW(), &wargc);
  auto utf8_args = WideArgsToUtf8(wargc, wargv);
  LocalFree(wargv);

  std::vector<char*> argv_ptrs;
  argv_ptrs.reserve(utf8_args.size());
  for (auto& s : utf8_args) {
    argv_ptrs.push_back(s.data());
  }
  return RunWindowedApp(static_cast<int>(argv_ptrs.size()), argv_ptrs.data());
}

#else

int main(int argc, char* argv[]) {
  return RunWindowedApp(argc, argv);
}

#endif
