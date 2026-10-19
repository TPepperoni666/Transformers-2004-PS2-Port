#include "ps2_runtime.h"
#include "games_database.h"
#include "runtime/ps2_settings.h"
#if defined(PS2X_ENABLE_PC_MENU)
#include "ps2_pc_menu.h"
#endif
#if defined(PS2X_ENABLE_DEBUG_UI) || defined(PS2X_ENABLE_PC_MENU)
#include "rlImGui.h"
#endif
#if defined(PS2X_ENABLE_DEBUG_UI) && !defined(PLATFORM_VITA)
#include "ps2_debug_panel.h"
#endif

#ifdef _DEBUG
#include "ps2_log.h"
#endif

#include <iostream>
#include <string>
#include <filesystem>
#include <exception>
#include <algorithm>
#include <cstdlib>

namespace ps2_test
{
    int exitCode(); // TEMP(transformers): src/lib/Kernel/TestScript.cpp; PS2X_SCRIPT verdict, 0 when the harness is inactive
}

#if defined(__ANDROID__)
#include <android/log.h>
#include <unistd.h>
#include <thread>
#include <cstdio>
#include <cstring>
#endif

#if defined(_WIN32)
// GENERIC (hybrid-GPU laptops): ask the NVIDIA Optimus / AMD PowerXpress drivers to run this process's OpenGL (the raylib
// window and presenter) on the high-performance GPU. parallel-gs already picks the discrete GPU for Vulkan; without these
// exports the presenter ran on the integrated GPU (laptop log 12 Oct: Vulkan on RTX 3080, OpenGL on AMD Radeon iGPU, and a
// device-lost crash inside the iGPU's OpenGL driver). Must be exported from the .exe itself, not a DLL. No effect on single-GPU PCs.
extern "C" {
__declspec(dllexport) unsigned long NvOptimusEnablement = 0x00000001;
__declspec(dllexport) int AmdPowerXpressRequestHighPerformance = 1;
}
#endif

namespace
{
#if defined(__ANDROID__)
    int g_logcatPipeFds[2]{-1, -1};
    std::thread g_logcatThread;

    void stopLogcatRedirect()
    {
        std::fflush(stdout);
        std::fflush(stderr);
        close(STDOUT_FILENO);
        close(STDERR_FILENO);
        if (g_logcatPipeFds[1] >= 0)
        {
            close(g_logcatPipeFds[1]);
            g_logcatPipeFds[1] = -1;
        }
        if (g_logcatThread.joinable())
        {
            g_logcatThread.join();
        }
    }

    void redirectStdioToLogcat()
    {
        if (pipe(g_logcatPipeFds) != 0)
        {
            return;
        }

        setvbuf(stdout, nullptr, _IOLBF, 0);
        setvbuf(stderr, nullptr, _IONBF, 0);
        dup2(g_logcatPipeFds[1], STDOUT_FILENO);
        dup2(g_logcatPipeFds[1], STDERR_FILENO);

        g_logcatThread = std::thread([]()
                                     {
                                         FILE *reader = fdopen(g_logcatPipeFds[0], "r");
                                         if (!reader)
                                         {
                                             return;
                                         }
                                         char line[1024];
                                         while (fgets(line, sizeof(line), reader))
                                         {
                                             size_t len = std::strlen(line);
                                             if (len > 0 && line[len - 1] == '\n')
                                             {
                                                 line[len - 1] = '\0';
                                             }
                                             __android_log_write(ANDROID_LOG_INFO, "ps2x", line);
                                         }
                                         fclose(reader);
                                         g_logcatPipeFds[0] = -1;
                                     });
        if (std::atexit(stopLogcatRedirect) != 0)
        {
            close(STDOUT_FILENO);
            close(STDERR_FILENO);
            close(g_logcatPipeFds[1]);
            g_logcatPipeFds[1] = -1;
            if (g_logcatThread.joinable())
            {
                g_logcatThread.join();
            }
        }
    }
#endif

    void setupTerminateLogger() // to help on release build crashs
    {
        std::set_terminate([]()
                           {
                               std::cerr << "[terminate] unhandled exception" << std::endl;
                               const std::exception_ptr ep = std::current_exception();
                               if (ep)
                               {
                                   try
                                   {
                                       std::rethrow_exception(ep);
                                   }
                                   catch (const std::system_error &e)
                                   {
                                       std::cerr << "[terminate] std::system_error code=" << e.code().value()
                                                 << " category=" << e.code().category().name()
                                                 << " message=" << e.what() << std::endl;
                                   }
                                   catch (const std::exception &e)
                                   {
                                       std::cerr << "[terminate] std::exception: " << e.what() << std::endl;
                                   }
                                   catch (...)
                                   {
                                       std::cerr << "[terminate] non-std exception" << std::endl;
                                   }
                               }
                               std::abort(); });
    }

    std::string normalizeGameId(const std::string &folderName)
    {
        std::string result = folderName;

        size_t underscore = result.find('_');
        if (underscore != std::string::npos)
            result[underscore] = '-';

        size_t dot = result.find('.');
        if (dot != std::string::npos)
            result.erase(dot, 1);

        std::ranges::transform(result, result.begin(), [](unsigned char character)
                               { return static_cast<char>(std::toupper(character)); });

        return result;
    }

    std::filesystem::path getExecutablePath(int argc, char *argv[])
    {
        if (argc >= 2 && argv[1] && argv[1][0] != '\0')
        {
            std::cout << "Using argv boot path" << std::endl;
            return std::filesystem::path(argv[1]);
        }
#if defined(PS2X_DEFAULT_BOOT_ELF)
        std::cout << "Using default boot file" << std::endl;
        const std::filesystem::path configuredPath = std::filesystem::path(PS2X_DEFAULT_BOOT_ELF);
#if defined(PLATFORM_VITA)
        return configuredPath;
#endif
        if (configuredPath.is_absolute())
        {
            return configuredPath;
        }
        return (std::filesystem::current_path() / configuredPath).lexically_normal();
#else
        throw std::runtime_error("Unable to determine executable path. Pass the guest ELF as argv[1] or define PS2X_DEFAULT_BOOT_ELF.");
#endif
    }
}

int main(int argc, char *argv[])
{
#if defined(__ANDROID__)
    redirectStdioToLogcat();
#endif
    setupTerminateLogger();

    try
    {
        std::filesystem::path pathObj = getExecutablePath(argc, argv);

        std::error_code settingsError;
        const auto settingsElf = std::filesystem::absolute(pathObj, settingsError);
        ps2x::settings::load(settingsError ? pathObj.parent_path() : settingsElf.parent_path(), argc, argv);
        std::string filePathStr = pathObj.string();
        std::string elfName = pathObj.filename().string();
        std::string normalizedId = normalizeGameId(elfName);

        std::string windowTitle = "PS2-Recomp | ";
        const char *gameName = getGameName(normalizedId);

#if !defined(PLATFORM_VITA)
        if (gameName)
        {
            windowTitle += std::string(gameName) + " | " + elfName;
        }
        else
#endif
        {
            windowTitle += elfName;
        }

        PS2Runtime runtime;
#if (defined(PS2X_ENABLE_DEBUG_UI) || defined(PS2X_ENABLE_PC_MENU)) && !defined(PLATFORM_VITA)
        // GAME(transformers): single ImGui host, independent menu/debugger build switches.
        struct ImGuiHost {
#if defined(PS2X_ENABLE_DEBUG_UI)
            PS2DebugPanel debugPanel;
#endif
#if defined(PS2X_ENABLE_PC_MENU)
            PS2PcMenu pcMenu;
#endif
        } uiHost;
        runtime.setDebugUiCallbacks(
            [](PS2Runtime& rt,void* data) {
                auto* ui=static_cast<ImGuiHost*>(data); rlImGuiSetup(true);
#if defined(PS2X_ENABLE_DEBUG_UI)
                ui->debugPanel.initialize(false);
#endif
#if defined(PS2X_ENABLE_PC_MENU)
                ui->pcMenu.initialize(rt);
#endif
            },
            [](PS2Runtime& rt,void* data) {
                auto* ui=static_cast<ImGuiHost*>(data);
#if defined(PS2X_ENABLE_PC_MENU)
                ui->pcMenu.serviceResolutionTest(rt);
#endif
                if(ps2x::settings::harnessMode() && !std::getenv("PS2X_PCMENU_AUTOTEST")) return;
                bool wantsFrame=false;
#if defined(PS2X_ENABLE_DEBUG_UI)
                ui->debugPanel.handleHotkeys(); wantsFrame |= ui->debugPanel.isVisible();
#endif
#if defined(PS2X_ENABLE_PC_MENU)
                ui->pcMenu.update(rt); wantsFrame |= ui->pcMenu.wantsImGuiFrame();
#endif
                if(!wantsFrame) return;
#if defined(PS2X_ENABLE_PC_MENU)
                ui->pcMenu.beginImGuiFrame();
#else
                rlImGuiBegin();
#endif
#if defined(PS2X_ENABLE_DEBUG_UI)
                ui->debugPanel.drawWindows(rt);
#endif
#if defined(PS2X_ENABLE_PC_MENU)
                ui->pcMenu.draw(rt);
#endif
                rlImGuiEnd();
#if defined(PS2X_ENABLE_PC_MENU)
                ui->pcMenu.afterRender(rt);
#endif
            },
            [](PS2Runtime& rt,void* data) {
                auto* ui=static_cast<ImGuiHost*>(data);
#if defined(PS2X_ENABLE_PC_MENU)
                ui->pcMenu.shutdown();
#endif
#if defined(PS2X_ENABLE_DEBUG_UI)
                ui->debugPanel.shutdown();
#endif
                rlImGuiShutdown();
            }, &uiHost);
#endif
        if (!runtime.initialize(windowTitle.c_str()))
        {
            std::cerr << "Failed to initialize PS2 runtime" << std::endl;
            return 1;
        }

        if (!runtime.loadELF(filePathStr))
        {
            std::cerr << "Failed to load ELF file: " << filePathStr << std::endl;
            return 1;
        }

        runtime.run();
        ps2x::settings::saveIfDirty();

#ifdef _DEBUG
        ps2_log::print_saved_location();
#endif
        std::cout.flush();
        std::cerr.flush();
        std::_Exit(ps2_test::exitCode()); // TEMP(transformers): was std::_Exit(0); see private qualification evidence exit codes
    }
    catch (const std::exception &e)
    {
        std::cerr << "[main] fatal exception: " << e.what() << std::endl;
    }
    catch (...)
    {
        std::cerr << "[main] fatal exception: unknown" << std::endl;
    }

    std::cout.flush();
    std::cerr.flush();
    std::_Exit(1);
}
