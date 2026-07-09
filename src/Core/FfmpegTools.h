#pragma once

#include "AppPaths.h"

#include <filesystem>
#include <functional>
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

struct FfmpegProgress {
    double percent = 0.0;
    std::uint64_t convertedMs = 0;
    std::uint64_t totalMs = 0;
    std::wstring text;
};

using FfmpegProgressCallback = std::function<void(const FfmpegProgress&)>;

bool TryParseFfmpegProgressTimeMs(const std::wstring& line, std::uint64_t& convertedMs);
std::wstring FormatFfmpegProgressText(std::uint64_t convertedMs, std::uint64_t totalMs);

bool ConvertWithFfmpeg(
    const std::filesystem::path& ffmpegExe,
    const std::filesystem::path& input,
    const std::filesystem::path& output,
    bool audioOnly,
    std::stop_token stopToken,
    std::wstring& errorText,
    const FfmpegProgressCallback& onProgress = {}
);
