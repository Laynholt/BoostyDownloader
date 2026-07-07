#pragma once

#include "AppPaths.h"

#include <filesystem>
#include <mutex>
#include <string>

class Logger {
public:
    explicit Logger(const AppPaths& paths);
    void Info(const std::wstring& message);
    void Error(const std::wstring& message);
    std::wstring ReadAll() const;

private:
    void Append(const std::wstring& level, const std::wstring& message);

    std::filesystem::path m_path;
    mutable std::mutex m_mutex;
};
