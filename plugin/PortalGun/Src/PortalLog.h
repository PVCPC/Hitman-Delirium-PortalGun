// Every log line of ours also goes to PortalGun.log next to the DLL (the SDK's own log is only on screen).
// PortalGun.cpp includes this after the SDK headers and points `Logger` at it.
#pragma once

#include <chrono>
#include <filesystem>
#include <format>
#include <fstream>
#include <mutex>
#include <string>

#include "Logging.h"

namespace PortalLog {
    inline std::mutex g_Mutex;
    inline std::ofstream g_File;

    inline void Open(const std::filesystem::path& p_Dir) {
        std::scoped_lock s_Lock(g_Mutex);
        g_File.open(p_Dir / "PortalGun.log", std::ios::trunc);
    }

    inline void Write(const char* p_Level, const std::string& p_Line) {
        std::scoped_lock s_Lock(g_Mutex);

        if (g_File.is_open()) {
            const auto s_Now = std::chrono::floor<std::chrono::milliseconds>(std::chrono::system_clock::now());
            g_File << std::format("{:%T} {} {}", s_Now, p_Level, p_Line) << std::endl;
        }
    }

    template <typename... Args>
    void Error(std::format_string<Args...> p_Format, Args&&... p_Args) {
        const std::string s_Line = std::format(p_Format, std::forward<Args>(p_Args)...);
        Write("ERROR", s_Line);
        ::Logger::Error("{}", s_Line);
    }

    template <typename... Args>
    void Warn(std::format_string<Args...> p_Format, Args&&... p_Args) {
        const std::string s_Line = std::format(p_Format, std::forward<Args>(p_Args)...);
        Write("WARN", s_Line);
        ::Logger::Warn("{}", s_Line);
    }

    template <typename... Args>
    void Info(std::format_string<Args...> p_Format, Args&&... p_Args) {
        const std::string s_Line = std::format(p_Format, std::forward<Args>(p_Args)...);
        Write("INFO", s_Line);
    }

    template <typename... Args>
    void Debug(std::format_string<Args...> p_Format, Args&&... p_Args) {
        const std::string s_Line = std::format(p_Format, std::forward<Args>(p_Args)...);
        Write("DEBUG", s_Line);
    }
}
