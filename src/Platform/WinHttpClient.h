#pragma once

#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>

#include <cstdint>
#include <filesystem>
#include <functional>
#include <string>
#include <vector>

using HttpHeaders = std::vector<std::pair<std::wstring, std::wstring>>;
using HttpProgressCallback = std::function<void(std::uint64_t downloaded, std::uint64_t total)>;
using HttpCancelCallback = std::function<bool()>;

class WinHttpClient {
public:
    static std::string GetString(const std::wstring& url, const HttpHeaders& headers = {}, const HttpCancelCallback& isCanceled = {});
    static void DownloadFile(
        const std::wstring& url,
        const std::filesystem::path& target,
        const HttpHeaders& headers = {},
        const HttpProgressCallback& onProgress = {},
        const HttpCancelCallback& isCanceled = {}
    );
};
