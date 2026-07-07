#pragma once

#include "AppPaths.h"

#include <filesystem>
#include <string>

struct AppConfig {
    std::filesystem::path downloadDir;
    std::wstring cookie;
    std::wstring authHeader;
    std::wstring quality = L"highest";
    int maxParallelDownloads = 3;
};

class ConfigStore {
public:
    static AppConfig Load(const AppPaths& paths);
    static void Save(const AppPaths& paths, const AppConfig& config);
};
