#pragma once

#include "AppPaths.h"

#include <cstdint>
#include <filesystem>
#include <functional>
#include <stop_token>
#include <string>

struct ReleaseAssetInfo {
    bool found = false;
    std::wstring version;
    std::wstring downloadUrl;
};

ReleaseAssetInfo ParseGitHubReleaseAsset(const std::string& releaseJson, const std::string& assetName);
bool ShouldInstallAppUpdate(const ReleaseAssetInfo& latest);
std::wstring BuildAppUpdatePromptMessage(const ReleaseAssetInfo& release);

namespace AppUpdateService {
const char* ExeAssetName();
const char* Sha256SumsAssetName();
ReleaseAssetInfo CheckLatestRelease(std::stop_token stopToken = {});
ReleaseAssetInfo CheckLatestSha256Sums(std::stop_token stopToken = {});
void EnsureLocalSha256Sums(const AppPaths& paths);
std::wstring Sha256ForFile(const std::string& sumsText, const std::string& fileName);
std::wstring FileSha256Hex(const std::filesystem::path& path);
std::filesystem::path DownloadUpdateExe(
    const AppPaths& paths,
    const ReleaseAssetInfo& release,
    const std::function<void(std::uint64_t downloaded, std::uint64_t total)>& onProgress = {},
    std::stop_token stopToken = {}
);
void StartDownloadedUpdate(const AppPaths& paths, const std::filesystem::path& downloadedExe);
}
