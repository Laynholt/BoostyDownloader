#include "Config.h"

#include <algorithm>
#include <system_error>

#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>

namespace {

std::wstring ReadIni(const std::filesystem::path& path, const wchar_t* section, const wchar_t* key, const wchar_t* fallback) {
    std::wstring buffer(8192, L'\0');
    const DWORD size = GetPrivateProfileStringW(section, key, fallback, buffer.data(), static_cast<DWORD>(buffer.size()), path.c_str());
    buffer.resize(size);
    return buffer;
}

int ReadInt(const std::filesystem::path& path, const wchar_t* section, const wchar_t* key, int fallback) {
    return static_cast<int>(GetPrivateProfileIntW(section, key, fallback, path.c_str()));
}

} // namespace

AppConfig ConfigStore::Load(const AppPaths& paths) {
    AppConfig config;
    config.downloadDir = ReadIni(paths.configPath(), L"download", L"dir", (paths.root() / L"downloads").c_str());
    config.cookie = ReadIni(paths.configPath(), L"auth", L"cookie", L"");
    config.authHeader = ReadIni(paths.configPath(), L"auth", L"authorization", L"");
    config.quality = ReadIni(paths.configPath(), L"download", L"quality", L"highest");
    config.container = ReadIni(paths.configPath(), L"download", L"container", L"auto");
    config.ffmpegPath = ReadIni(paths.configPath(), L"tools", L"ffmpeg", L"");
    config.autoUpdateCheck = ReadInt(paths.configPath(), L"app", L"auto_update", 0) != 0;
    config.maxParallelDownloads = std::clamp(ReadInt(paths.configPath(), L"download", L"workers", 3), 1, 16);
    return config;
}

void ConfigStore::Save(const AppPaths& paths, const AppConfig& config) {
    std::error_code ec;
    std::filesystem::create_directories(paths.stuffDir(), ec);
    WritePrivateProfileStringW(L"download", L"dir", config.downloadDir.c_str(), paths.configPath().c_str());
    WritePrivateProfileStringW(L"download", L"workers", std::to_wstring(std::clamp(config.maxParallelDownloads, 1, 16)).c_str(), paths.configPath().c_str());
    WritePrivateProfileStringW(L"download", L"quality", config.quality.c_str(), paths.configPath().c_str());
    WritePrivateProfileStringW(L"download", L"container", config.container.c_str(), paths.configPath().c_str());
    WritePrivateProfileStringW(L"tools", L"ffmpeg", config.ffmpegPath.c_str(), paths.configPath().c_str());
    WritePrivateProfileStringW(L"app", L"auto_update", config.autoUpdateCheck ? L"1" : L"0", paths.configPath().c_str());
    WritePrivateProfileStringW(L"auth", L"cookie", config.cookie.c_str(), paths.configPath().c_str());
    WritePrivateProfileStringW(L"auth", L"authorization", config.authHeader.c_str(), paths.configPath().c_str());
}
