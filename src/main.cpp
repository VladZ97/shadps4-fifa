// SPDX-FileCopyrightText: Copyright 2025-2026 shadPS4 Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#include <filesystem>
#include <iostream>
#include <memory>
#include <optional>
#include <span>
#include <vector>
#include <CLI/CLI.hpp>
#include <SDL3/SDL_messagebox.h>
#include <fmt/format.h>
#include <fmt/ranges.h>

#include "common/arch.h"
#include "common/key_manager.h"
#include "common/logging/log.h"
#include "common/memory_patcher.h"
#include "common/path_util.h"
#include "core/debugger.h"
#include "core/emulator_settings.h"
#include "core/emulator_state.h"
#include "core/file_sys/fs.h"
#include "core/ipc/ipc.h"
#include "core/user_settings.h"
#include "emulator.h"
#include "imgui/big_picture/big_picture.h"

#ifdef _WIN32
#include <windows.h>
#elif defined(__APPLE__)
#include <sys/sysctl.h>
#endif

#ifdef __APPLE__
#include <cstdlib>
#include <cstring>
#include <AudioToolbox/AudioToolbox.h>
#include <objc/message.h>
#include <objc/runtime.h>

// Local change (0.5.1-f): with the split renderer the picture is in the GPU host's window and
// this process has none, so macOS treats it as an idle background app (App Nap): every thread
// was clamped to utility priority (20, below all normal work on the machine; measured with
// ps -M during a match) and its timers were coalesced. A user-initiated, latency-critical
// activity for the life of the process opts out. SHAD_APP_NAP=1 keeps the old behaviour.
static void KeepProcessInteractive() {
    if (const char* v = std::getenv("SHAD_APP_NAP"); v && v[0] == '1') {
        return;
    }
    const auto cls = [](const char* name) { return reinterpret_cast<id>(objc_getClass(name)); };
    const id info = reinterpret_cast<id (*)(id, SEL)>(objc_msgSend)(
        cls("NSProcessInfo"), sel_registerName("processInfo"));
    const id reason = reinterpret_cast<id (*)(id, SEL, const char*)>(objc_msgSend)(
        cls("NSString"), sel_registerName("stringWithUTF8String:"), "Game running");
    if (!info || !reason) {
        return;
    }
    // NSActivityUserInitiated (0x00FFFFFF, includes idle system sleep disabled) |
    // NSActivityLatencyCritical (0xFF00000000).
    const unsigned long long options = 0x00FFFFFFULL | 0xFF00000000ULL;
    const id activity = reinterpret_cast<id (*)(id, SEL, unsigned long long, id)>(objc_msgSend)(
        info, sel_registerName("beginActivityWithOptions:reason:"), options, reason);
    if (activity) {
        // Never ended: the activity lasts as long as the process.
        reinterpret_cast<id (*)(id, SEL)>(objc_msgSend)(activity, sel_registerName("retain"));
    }
}

// Local change (0.5.1-i): with the split renderer the GPU host is the full-screen game, so macOS
// Game Mode treats this process, which runs the game itself, as a background app and clamps every
// thread to priority 4 (task role "darwinbg application") until the game first plays sound. A
// first launch spent 46 to 55 s instead of 7 s preparing the game's code, and each later boot
// was slowed the same way up to the first sound. A process with a running audio output is
// exempt, so a silent output queue runs for the life of the process. It is separate from the
// game's own audio device and does not change its format. SHAD_BOOT_AUDIO=0 turns it off.
static void SilentQueueCallback(void*, AudioQueueRef queue, AudioQueueBufferRef buffer) {
    std::memset(buffer->mAudioData, 0, buffer->mAudioDataBytesCapacity);
    buffer->mAudioDataByteSize = buffer->mAudioDataBytesCapacity;
    AudioQueueEnqueueBuffer(queue, buffer, 0, nullptr);
}

static void KeepAudioActive() {
    if (const char* v = std::getenv("SHAD_BOOT_AUDIO"); v && v[0] == '0') {
        return;
    }
    AudioStreamBasicDescription format{};
    format.mSampleRate = 48000.0;
    format.mFormatID = kAudioFormatLinearPCM;
    format.mFormatFlags = kAudioFormatFlagIsFloat | kAudioFormatFlagIsPacked;
    format.mChannelsPerFrame = 2;
    format.mBitsPerChannel = 32;
    format.mBytesPerFrame = 8;
    format.mBytesPerPacket = 8;
    format.mFramesPerPacket = 1;
    AudioQueueRef queue = nullptr;
    // No run loop given: the callback runs on the queue's own thread.
    if (AudioQueueNewOutput(&format, SilentQueueCallback, nullptr, nullptr, nullptr, 0, &queue) !=
        noErr) {
        return;
    }
    constexpr UInt32 BufferBytes = 24000 * 8; // half a second each
    for (int i = 0; i < 3; ++i) {
        AudioQueueBufferRef buffer = nullptr;
        if (AudioQueueAllocateBuffer(queue, BufferBytes, &buffer) != noErr) {
            AudioQueueDispose(queue, true);
            return;
        }
        SilentQueueCallback(nullptr, queue, buffer);
    }
    if (AudioQueueStart(queue, nullptr) != noErr) {
        AudioQueueDispose(queue, true);
    }
    // Never stopped: the queue lasts as long as the process.
}
#endif

int main(int argc, char* argv[]) {
#ifdef __APPLE__
    KeepProcessInteractive();
    KeepAudioActive();
#endif
#ifdef _WIN32
    SetConsoleOutputCP(CP_UTF8);
#endif

#if defined(__APPLE__) && defined(ARCH_X86_64)
    // KosmicKrisp only supports Apple Silicon. Check that we are not running on an Intel Mac.
    int sysctl_ret = 0;
    size_t sysctl_size = sizeof(sysctl_ret);
    sysctlbyname("sysctl.proc_translated", &sysctl_ret, &sysctl_size, nullptr, 0);
    if (sysctl_ret != 1) {
        SDL_ShowSimpleMessageBox(SDL_MESSAGEBOX_INFORMATION, "shadPS4",
                                 "shadPS4 only supports Apple Silicon Macs.", nullptr);
        std::cout << "shadPS4 only supports Apple Silicon Macs." << std::endl;
        return -1;
    }
#endif

    CLI::App app{"shadPS4 Emulator CLI"};

    // ---- CLI state ----
    std::optional<std::string> gamePath;
    std::vector<std::string> gameArgs;
    std::optional<std::filesystem::path> overrideRoot;
    std::optional<int> waitPid;
    bool waitForDebugger = false;
    bool userfaultfd = false;

    std::optional<std::string> fullscreenStr;
    bool ignoreGamePatch = false;
    bool showFps = false;
    bool configClean = false;
    bool configGlobal = false;
    bool bigPicture = false;
    bool sameProcess = false;
    bool append_log{};

    std::optional<std::filesystem::path> addGameFolder;
    std::optional<std::filesystem::path> setAddonFolder;
    std::optional<std::string> patchFile;

    std::vector<std::pair<std::filesystem::path, std::string>> mounts;
    static std::vector<std::string> env_vars;

    // ---- Options ----
    app.add_option("guest_arg", gamePath, "Game path or ID"); // positional
    app.add_option("-g,--game", gamePath, "Game path or ID");
    app.add_option("-p,--patch", patchFile, "Patch file to apply");
    app.add_flag("-i,--ignore-game-patch", ignoreGamePatch,
                 "Disable automatic loading of game patches");

    app.add_flag("-b,--big-picture", bigPicture, "Start in Big Picture Mode");
    app.add_flag("--same-process", sameProcess,
                 "Launch the game in the same process when using Big Picture Mode");

    app.add_option("-f,--fullscreen", fullscreenStr, "Fullscreen mode (true|false)");

    app.add_option("--override-root", overrideRoot)->check(CLI::ExistingDirectory);

    app.add_flag("--wait-for-debugger", waitForDebugger);
    app.add_option("--wait-for-pid", waitPid);
#ifdef __linux__
    app.add_flag("--userfaultfd", userfaultfd,
                 "Enable userfaultfd for tracking memory (Linux only)");
#endif

    app.add_flag("--show-fps", showFps);
    app.add_flag("--config-clean", configClean);
    app.add_flag("--config-global", configGlobal);
    app.add_flag("--log-append", append_log);

    app.add_option("--add-game-folder", addGameFolder)->check(CLI::ExistingDirectory);
    app.add_option("--set-addon-folder", setAddonFolder)->check(CLI::ExistingDirectory);
    app.add_option("--mount", mounts, "Mount source to destination");
    app.add_option("-e,--env", env_vars, "Environment variables to pass to the guest");

    // ---- Capture args after `--` verbatim ----
    app.allow_extras();

    // ---- No-args behavior ----
    if (argc == 1) {
        SDL_ShowSimpleMessageBox(SDL_MESSAGEBOX_INFORMATION, "shadPS4",
                                 "This is a CLI application. Please use the '-b' flag for Big "
                                 "Picture mode, or QTLauncher for a standalone GUI:\n"
                                 "https://github.com/shadps4-emu/shadps4-qtlauncher/releases",
                                 nullptr);
        std::cout << app.help();
        return -1;
    }

    try {
        bool double_dash_found = false;
        int double_dash_index;
        for (int i = 0; i < argc; i++) {
            if (double_dash_found) {
                gameArgs.emplace_back(argv[i]);
            }
            if (!double_dash_found && std::string(argv[i]) == "--") {
                double_dash_found = true;
                double_dash_index = i;
            }
        }

        // If the -- arg is present, only parse args before it
        if (double_dash_found) {
            app.parse(double_dash_index, argv);
        } else {
            app.parse(argc, argv);
        }
    } catch (const CLI::ParseError& e) {
        return app.exit(e);
    }

    if (waitPid)
        Core::Debugger::WaitForPid(*waitPid);

    // Initialize main log with default config
    Common::Log::Setup("shadps4.log");

    LOG_INFO(Debug, "Run: {}", fmt::join(std::span(argv, argc), " "));

    IPC::Instance().Init();

    // Initialize key manager
    KeyManager::GetInstance()->LoadFromFile();

    // Load configurations
    EmulatorSettings.Load();
    UserSettings.Load();

    if (bigPicture) {
        BigPictureMode::Launch(argv[0], sameProcess);
        return 0;
    }

    // ---- Utility commands ----
    if (addGameFolder) {
        EmulatorSettings.AddGameInstallDir(*addGameFolder);
        EmulatorSettings.Save();
        LOG_INFO(Config, "Game folder successfully saved.");
        return 0;
    }

    if (setAddonFolder) {
        EmulatorSettings.SetAddonInstallDir(*setAddonFolder);
        EmulatorSettings.Save();
        LOG_INFO(Config, "Addon folder successfully saved.");
        return 0;
    }

    if (!gamePath.has_value()) {
        if (!gameArgs.empty()) {
            gamePath = gameArgs.front();
            gameArgs.erase(gameArgs.begin());
        } else {
            LOG_ERROR(Debug, "Please provide a game path or ID.");
            return 1;
        }
    }

    // ---- Apply flags ----
    if (patchFile)
        MemoryPatcher::patch_file = *patchFile;

    if (ignoreGamePatch)
        Core::FileSys::MntPoints::ignore_game_patches = true;

    if (fullscreenStr) {
        if (*fullscreenStr == "true") {
            EmulatorSettings.SetFullScreen(true);
        } else if (*fullscreenStr == "false") {
            EmulatorSettings.SetFullScreen(false);
        } else {
            LOG_ERROR(Debug, "Invalid argument for --fullscreen (use true|false)");
            return 1;
        }
    }

    if (showFps)
        EmulatorSettings.SetShowFpsCounter(true);

    if (configClean)
        EmulatorSettings.SetConfigMode(ConfigMode::Clean);

    if (configGlobal)
        EmulatorSettings.SetConfigMode(ConfigMode::Global);

    if (userfaultfd) {
        EmulatorSettings.SetUserfaultfdTracking(true);
    }

    // ---- Resolve game path or ID ----
    std::filesystem::path ebootPath(*gamePath);
    const auto archive_component_exists = [](const std::filesystem::path& p) -> bool {
        std::filesystem::path accum;
        for (const auto& comp : p) {
            accum /= comp;
            if (comp.extension() == ".zar") {
                return std::filesystem::is_regular_file(accum);
            }
        }
        return false;
    };
    if (!std::filesystem::exists(ebootPath) && !archive_component_exists(ebootPath)) {
        bool found = false;
        for (const auto& installDir : EmulatorSettings.GetGameInstallDirs()) {
            if (auto foundPath = Common::FS::FindGameByID(installDir, *gamePath, 0)) {
                ebootPath = *foundPath;
                found = true;
                break;
            }
        }
        if (!found) {
            for (const auto& installDir : EmulatorSettings.GetGameInstallDirs()) {
                if (auto foundPath = Common::FS::FindGameByID(installDir, *gamePath, 5)) {
                    ebootPath = *foundPath;
                    found = true;
                    break;
                }
            }
        }
        if (!found) {
            LOG_ERROR(Debug, "Game ID or file path not found: {}", *gamePath);
            return 1;
        }
    }

    auto* emulator = Common::Singleton<Core::Emulator>::Instance();
    emulator->executableName = argv[0];
    emulator->waitForDebuggerBeforeRun = waitForDebugger;
    emulator->Run(ebootPath, gameArgs, overrideRoot, mounts, env_vars, append_log);

    return 0;
}
