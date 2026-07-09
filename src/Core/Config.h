#pragma once

#include "AppPaths.h"

#include <filesystem>
#include <string>

struct AppConfig {
    std::filesystem::path downloadDir;
    std::wstring cookie;
    std::wstring authHeader;
    std::wstring quality = L"highest";
    std::wstring container = L"auto";
    std::filesystem::path ffmpegPath;
    bool autoUpdateCheck = false;
    int maxParallelDownloads = 3;
};

class ConfigStore {
public:
    static AppConfig Load(const AppPaths& paths);
    static void Save(const AppPaths& paths, const AppConfig& config);
};
