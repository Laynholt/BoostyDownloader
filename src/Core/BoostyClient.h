#pragma once

#include <cstdint>
#include <filesystem>
#include <functional>
#include <stop_token>
#include <string>
#include <vector>

struct BoostyAuth {
    std::wstring cookie;
    std::wstring authHeader;
};

struct BoostyDownloadRequest {
    std::wstring url;
    std::filesystem::path outputDirectory;
    BoostyAuth auth;
    std::wstring quality = L"highest";
    std::wstring container = L"auto";
    std::filesystem::path ffmpegPath;
};

struct BoostyProgress {
    std::wstring stage;
    double percent = 0.0;
    std::uint64_t downloadedBytes = 0;
    std::uint64_t totalBytes = 0;
    std::wstring qualityLabel;
    std::wstring containerLabel;
    std::wstring progressText;
    std::wstring taskTitle;
    std::wstring thumbnailUrl;
    std::filesystem::path thumbnailPath;
    std::filesystem::path outputPath;
};

struct BoostyDownloadResult {
    bool success = false;
    std::wstring errorText;
    std::vector<std::filesystem::path> outputFiles;
};

using BoostyProgressCallback = std::function<void(const BoostyProgress&)>;

std::wstring ExtractAccessTokenFromCookie(const std::wstring& cookieHeader);
std::wstring ExtractAccessTokenFromText(const std::wstring& text);
BoostyDownloadResult DownloadBoostyVideo(
    const BoostyDownloadRequest& request,
    std::stop_token stopToken,
    const BoostyProgressCallback& onProgress
);
