#include "AppPaths.h"

#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>

AppPaths::AppPaths(std::filesystem::path root)
    : m_root(std::move(root)) {
}

const std::filesystem::path& AppPaths::root() const {
    return m_root;
}

std::filesystem::path AppPaths::stuffDir() const {
    return m_root / L"stuff";
}

std::filesystem::path AppPaths::configPath() const {
    return stuffDir() / L"config.ini";
}

std::filesystem::path AppPaths::logPath() const {
    return stuffDir() / L"boosty.log";
}

std::filesystem::path AppPaths::downloadQueuePath() const {
    return stuffDir() / L"download_queue.json";
}

std::filesystem::path AppPaths::webViewDataDir() const {
    return stuffDir() / L"webview2";
}

std::filesystem::path GetExecutableRoot() {
    std::wstring buffer(MAX_PATH, L'\0');
    DWORD size = GetModuleFileNameW(nullptr, buffer.data(), static_cast<DWORD>(buffer.size()));
    while (size == buffer.size()) {
        buffer.resize(buffer.size() * 2);
        size = GetModuleFileNameW(nullptr, buffer.data(), static_cast<DWORD>(buffer.size()));
    }
    buffer.resize(size);
    return std::filesystem::path(buffer).parent_path();
}
