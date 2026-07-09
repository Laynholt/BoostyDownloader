#pragma once

#include "AppPaths.h"

#include <filesystem>
#include <stop_token>
#include <string>

struct FfmpegStatus {
    bool available = false;
    std::filesystem::path executable;
    std::wstring source;
    std::wstring version;
};

FfmpegStatus ResolveFfmpeg(const AppPaths& paths, const std::filesystem::path& configuredPath);
std::wstring FfmpegVersion(const std::filesystem::path& ffmpegExe);
bool InstallFfmpeg(const AppPaths& paths, std::wstring& errorText);

bool ConvertWithFfmpeg(
    const std::filesystem::path& ffmpegExe,
    const std::filesystem::path& input,
    const std::filesystem::path& output,
    bool audioOnly,
    std::stop_token stopToken,
    std::wstring& errorText
);
