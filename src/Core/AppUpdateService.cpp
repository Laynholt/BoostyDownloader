#include "AppUpdateService.h"

#include "AppVersion.h"
#include "Text.h"
#include "WinHttpClient.h"

#include <nlohmann/json.hpp>

#include <bcrypt.h>
#include <windows.h>

#include <array>
#include <algorithm>
#include <cctype>
#include <cwctype>
#include <fstream>
#include <iomanip>
#include <initializer_list>
#include <sstream>
#include <stdexcept>
#include <utility>
#include <vector>

#pragma comment(lib, "bcrypt.lib")

namespace {

std::wstring AsciiToWide(const std::string& value) {
    try {
        return Utf8ToWide(value);
    } catch (...) {
        std::wstring out;
        out.reserve(value.size());
        for (unsigned char ch : value) {
            out.push_back(static_cast<wchar_t>(ch));
        }
        return out;
    }
}

std::wstring NormalizeVersion(std::wstring version) {
    if (version.size() >= 2 &&
        (version.front() == L'v' || version.front() == L'V') &&
        iswdigit(version[1])) {
        version.erase(version.begin());
    }
    const size_t start = version.find_first_of(L"0123456789");
    if (start == std::wstring::npos) {
        return {};
    }
    size_t end = start;
    while (end < version.size() && (iswdigit(version[end]) || version[end] == L'.')) {
        ++end;
    }
    while (end > start && version[end - 1] == L'.') {
        --end;
    }
    return version.substr(start, end - start);
}

std::vector<int> VersionParts(std::wstring version) {
    version = NormalizeVersion(std::move(version));
    std::vector<int> parts;
    std::wistringstream input(version);
    std::wstring part;
    while (std::getline(input, part, L'.')) {
        try {
            parts.push_back(std::stoi(part));
        } catch (...) {
            parts.push_back(0);
        }
    }
    return parts;
}

bool IsVersionNewer(std::wstring latest, std::wstring current) {
    std::vector<int> left = VersionParts(std::move(latest));
    std::vector<int> right = VersionParts(std::move(current));
    const size_t count = std::max(left.size(), right.size());
    left.resize(count);
    right.resize(count);
    return left > right;
}

std::wstring NormalizeSha256Hex(std::string hash) {
    if (hash.size() != 64) {
        return {};
    }
    for (char& ch : hash) {
        const unsigned char byte = static_cast<unsigned char>(ch);
        if (!std::isxdigit(byte)) {
            return {};
        }
        ch = static_cast<char>(std::tolower(byte));
    }
    return AsciiToWide(hash);
}

std::filesystem::path CurrentExecutablePath() {
    std::wstring buffer(MAX_PATH, L'\0');
    DWORD size = GetModuleFileNameW(nullptr, buffer.data(), static_cast<DWORD>(buffer.size()));
    while (size == buffer.size()) {
        buffer.resize(buffer.size() * 2, L'\0');
        size = GetModuleFileNameW(nullptr, buffer.data(), static_cast<DWORD>(buffer.size()));
    }
    if (size == 0) {
        throw std::runtime_error("failed to resolve current executable path");
    }
    buffer.resize(size);
    return std::filesystem::path(buffer);
}

bool IsExecutableFile(const std::filesystem::path& path) {
    std::error_code ec;
    return !path.empty() && std::filesystem::is_regular_file(path, ec);
}

void WriteTextFile(const std::filesystem::path& path, const std::wstring& text) {
    std::error_code ec;
    std::filesystem::create_directories(path.parent_path(), ec);
    std::ofstream out(path, std::ios::binary | std::ios::trunc);
    if (out) {
        out << WideToUtf8(text);
    }
}

void WriteUtf16LeFile(const std::filesystem::path& path, const std::wstring& text) {
    std::error_code ec;
    std::filesystem::create_directories(path.parent_path(), ec);
    std::ofstream out(path, std::ios::binary | std::ios::trunc);
    if (!out) {
        throw std::runtime_error("failed to write update script");
    }
    const unsigned char bom[] = {0xff, 0xfe};
    out.write(reinterpret_cast<const char*>(bom), sizeof(bom));
    out.write(reinterpret_cast<const char*>(text.data()), static_cast<std::streamsize>(text.size() * sizeof(wchar_t)));
}

std::wstring QuotePowerShellLiteral(const std::filesystem::path& path) {
    std::wstring value = path.wstring();
    std::wstring escaped;
    escaped.reserve(value.size() + 8);
    escaped.push_back(L'\'');
    for (wchar_t ch : value) {
        if (ch == L'\'') {
            escaped += L"''";
        } else {
            escaped.push_back(ch);
        }
    }
    escaped.push_back(L'\'');
    return escaped;
}

std::wstring QuoteCommandLineArgument(const std::wstring& arg) {
    if (arg.empty()) {
        return L"\"\"";
    }
    const bool needsQuotes = arg.find_first_of(L" \t\n\v\"") != std::wstring::npos;
    if (!needsQuotes) {
        return arg;
    }
    std::wstring out = L"\"";
    size_t backslashes = 0;
    for (wchar_t ch : arg) {
        if (ch == L'\\') {
            ++backslashes;
            continue;
        }
        if (ch == L'"') {
            out.append(backslashes * 2 + 1, L'\\');
            out.push_back(ch);
            backslashes = 0;
            continue;
        }
        out.append(backslashes, L'\\');
        backslashes = 0;
        out.push_back(ch);
    }
    out.append(backslashes * 2, L'\\');
    out.push_back(L'"');
    return out;
}

std::wstring BuildUpdateScript(const std::filesystem::path& sourceExe, const std::filesystem::path& targetExe) {
    const std::filesystem::path backupExe = targetExe.wstring() + L".old";
    std::wostringstream script;
    script
        << L"$ErrorActionPreference = 'Stop'\r\n"
        << L"$source = " << QuotePowerShellLiteral(sourceExe) << L"\r\n"
        << L"$target = " << QuotePowerShellLiteral(targetExe) << L"\r\n"
        << L"$backup = " << QuotePowerShellLiteral(backupExe) << L"\r\n"
        << L"for ($i = 0; $i -lt 90; $i++) {\r\n"
        << L"    try {\r\n"
        << L"        if (Test-Path -LiteralPath $backup) { Remove-Item -LiteralPath $backup -Force -ErrorAction SilentlyContinue }\r\n"
        << L"        Move-Item -LiteralPath $target -Destination $backup -Force\r\n"
        << L"        break\r\n"
        << L"    } catch { Start-Sleep -Milliseconds 500 }\r\n"
        << L"}\r\n"
        << L"if (!(Test-Path -LiteralPath $backup)) { exit 1 }\r\n"
        << L"try {\r\n"
        << L"    Move-Item -LiteralPath $source -Destination $target -Force\r\n"
        << L"    Remove-Item -LiteralPath $backup -Force -ErrorAction SilentlyContinue\r\n"
        << L"    Start-Process -FilePath $target -WorkingDirectory (Split-Path -Parent $target)\r\n"
        << L"} catch {\r\n"
        << L"    if (Test-Path -LiteralPath $backup) { Move-Item -LiteralPath $backup -Destination $target -Force }\r\n"
        << L"    exit 1\r\n"
        << L"}\r\n"
        << L"Start-Sleep -Seconds 2\r\n"
        << L"Remove-Item -LiteralPath $MyInvocation.MyCommand.Path -Force -ErrorAction SilentlyContinue\r\n";
    return script.str();
}

void LaunchDetachedPowerShellScript(const std::filesystem::path& scriptPath) {
    std::filesystem::path powershell = L"C:\\Windows\\System32\\WindowsPowerShell\\v1.0\\powershell.exe";
    std::error_code ec;
    if (!std::filesystem::is_regular_file(powershell, ec)) {
        powershell = L"powershell.exe";
    }

    std::wstring commandLine =
        QuoteCommandLineArgument(powershell.wstring()) +
        L" -NoProfile -ExecutionPolicy Bypass -File " +
        QuoteCommandLineArgument(scriptPath.wstring());

    STARTUPINFOW startup = {};
    startup.cb = sizeof(startup);
    startup.dwFlags = STARTF_USESHOWWINDOW;
    startup.wShowWindow = SW_HIDE;

    PROCESS_INFORMATION process = {};
    if (!CreateProcessW(nullptr, commandLine.data(), nullptr, nullptr, FALSE, CREATE_NO_WINDOW, nullptr, nullptr, &startup, &process)) {
        throw std::runtime_error("failed to start update helper");
    }
    CloseHandle(process.hThread);
    CloseHandle(process.hProcess);
}

class CleanupPaths {
public:
    explicit CleanupPaths(std::initializer_list<std::filesystem::path> paths) : m_paths(paths) {}
    ~CleanupPaths() {
        if (!m_enabled) {
            return;
        }
        std::error_code ec;
        for (const std::filesystem::path& path : m_paths) {
            std::filesystem::remove_all(path, ec);
            ec.clear();
        }
    }
    void dismiss() { m_enabled = false; }

private:
    std::vector<std::filesystem::path> m_paths;
    bool m_enabled = true;
};

} // namespace

ReleaseAssetInfo ParseGitHubReleaseAsset(const std::string& releaseJson, const std::string& assetName) {
    ReleaseAssetInfo info;
    try {
        const nlohmann::json json = nlohmann::json::parse(releaseJson);
        if (!json.is_object()) {
            return info;
        }

        info.version = NormalizeVersion(AsciiToWide(json.value("tag_name", json.value("name", ""))));
        const auto assets = json.find("assets");
        if (assets == json.end() || !assets->is_array()) {
            return info;
        }
        for (const nlohmann::json& asset : *assets) {
            if (!asset.is_object() || asset.value("name", "") != assetName) {
                continue;
            }
            info.downloadUrl = AsciiToWide(asset.value("browser_download_url", ""));
            info.found = !info.downloadUrl.empty();
            return info;
        }
    } catch (...) {
        return ReleaseAssetInfo{};
    }
    return info;
}

bool ShouldInstallAppUpdate(const ReleaseAssetInfo& latest) {
    return latest.found && !latest.downloadUrl.empty() && IsVersionNewer(latest.version, kAppVersionWide);
}

std::wstring BuildAppUpdatePromptMessage(const ReleaseAssetInfo& release) {
    return L"Доступна новая версия " + release.version + L". Скачать и установить обновление?";
}

const char* AppUpdateService::ExeAssetName() {
    return "BoostyDownloader.exe";
}

const char* AppUpdateService::Sha256SumsAssetName() {
    return "SHA256SUMS.txt";
}

ReleaseAssetInfo AppUpdateService::CheckLatestRelease(std::stop_token stopToken) {
    const std::string json = WinHttpClient::GetString(
        L"https://api.github.com/repos/Laynholt/BoostyDownloader/releases/latest",
        {},
        [stopToken] { return stopToken.stop_requested(); }
    );
    return ParseGitHubReleaseAsset(json, ExeAssetName());
}

ReleaseAssetInfo AppUpdateService::CheckLatestSha256Sums(std::stop_token stopToken) {
    const std::string json = WinHttpClient::GetString(
        L"https://api.github.com/repos/Laynholt/BoostyDownloader/releases/latest",
        {},
        [stopToken] { return stopToken.stop_requested(); }
    );
    return ParseGitHubReleaseAsset(json, Sha256SumsAssetName());
}

std::wstring AppUpdateService::FileSha256Hex(const std::filesystem::path& path) {
    BCRYPT_ALG_HANDLE algorithm = nullptr;
    BCRYPT_HASH_HANDLE hash = nullptr;
    if (BCryptOpenAlgorithmProvider(&algorithm, BCRYPT_SHA256_ALGORITHM, nullptr, 0) != 0) {
        throw std::runtime_error("failed to open SHA-256 provider");
    }

    DWORD objectLength = 0;
    DWORD written = 0;
    if (BCryptGetProperty(algorithm, BCRYPT_OBJECT_LENGTH, reinterpret_cast<PUCHAR>(&objectLength), sizeof(objectLength), &written, 0) != 0) {
        BCryptCloseAlgorithmProvider(algorithm, 0);
        throw std::runtime_error("failed to query SHA-256 object length");
    }

    std::vector<unsigned char> hashObject(objectLength);
    if (BCryptCreateHash(algorithm, &hash, hashObject.data(), objectLength, nullptr, 0, 0) != 0) {
        BCryptCloseAlgorithmProvider(algorithm, 0);
        throw std::runtime_error("failed to create SHA-256 hash");
    }

    std::ifstream in(path, std::ios::binary);
    if (!in) {
        BCryptDestroyHash(hash);
        BCryptCloseAlgorithmProvider(algorithm, 0);
        throw std::runtime_error("failed to open file for SHA-256");
    }

    std::array<char, 65536> buffer = {};
    while (in) {
        in.read(buffer.data(), static_cast<std::streamsize>(buffer.size()));
        const std::streamsize read = in.gcount();
        if (read <= 0) {
            continue;
        }
        if (BCryptHashData(hash, reinterpret_cast<PUCHAR>(buffer.data()), static_cast<ULONG>(read), 0) != 0) {
            BCryptDestroyHash(hash);
            BCryptCloseAlgorithmProvider(algorithm, 0);
            throw std::runtime_error("failed to update SHA-256 hash");
        }
    }

    std::array<unsigned char, 32> digest = {};
    if (BCryptFinishHash(hash, digest.data(), static_cast<ULONG>(digest.size()), 0) != 0) {
        BCryptDestroyHash(hash);
        BCryptCloseAlgorithmProvider(algorithm, 0);
        throw std::runtime_error("failed to finish SHA-256 hash");
    }

    BCryptDestroyHash(hash);
    BCryptCloseAlgorithmProvider(algorithm, 0);

    std::wostringstream out;
    out << std::hex << std::setfill(L'0');
    for (unsigned char byte : digest) {
        out << std::setw(2) << static_cast<unsigned int>(byte);
    }
    return out.str();
}

void AppUpdateService::EnsureLocalSha256Sums(const AppPaths& paths) {
    const std::filesystem::path target = paths.stuffDir() / AsciiToWide(Sha256SumsAssetName());
    const std::wstring sha256 = FileSha256Hex(CurrentExecutablePath());
    WriteTextFile(target, sha256 + L"  " + AsciiToWide(ExeAssetName()) + L"\n");
}

std::wstring AppUpdateService::Sha256ForFile(const std::string& sumsText, const std::string& fileName) {
    std::istringstream input(sumsText);
    std::string line;
    while (std::getline(input, line)) {
        std::istringstream row(line);
        std::string hash;
        std::string name;
        if (!(row >> hash >> name)) {
            continue;
        }
        if (!name.empty() && name.front() == '*') {
            name.erase(name.begin());
        }
        if (name == fileName) {
            return NormalizeSha256Hex(hash);
        }
    }
    return {};
}

std::filesystem::path AppUpdateService::DownloadUpdateExe(
    const AppPaths& paths,
    const ReleaseAssetInfo& release,
    const std::function<void(std::uint64_t downloaded, std::uint64_t total)>& onProgress,
    std::stop_token stopToken
) {
    if (!release.found || release.downloadUrl.empty()) {
        throw std::runtime_error("app update executable was not found");
    }

    const std::filesystem::path target = paths.stuffDir() / L"updates" / L"BoostyDownloader.exe.new";
    CleanupPaths cleanup({target});
    std::error_code ec;
    std::filesystem::remove(target, ec);
    WinHttpClient::DownloadFile(
        release.downloadUrl,
        target,
        {},
        onProgress,
        [stopToken] { return stopToken.stop_requested(); }
    );

    const ReleaseAssetInfo sumsAsset = CheckLatestSha256Sums(stopToken);
    if (!sumsAsset.found || sumsAsset.downloadUrl.empty()) {
        std::filesystem::remove(target, ec);
        throw std::runtime_error("app update checksum asset was not found");
    }

    const std::string sumsText = WinHttpClient::GetString(
        sumsAsset.downloadUrl,
        {},
        [stopToken] { return stopToken.stop_requested(); }
    );
    const std::wstring expectedSha256 = Sha256ForFile(sumsText, ExeAssetName());
    if (expectedSha256.empty()) {
        std::filesystem::remove(target, ec);
        throw std::runtime_error("app update checksum was not found");
    }

    const std::wstring actualSha256 = FileSha256Hex(target);
    if (actualSha256 != expectedSha256) {
        std::filesystem::remove(target, ec);
        throw std::runtime_error("app update checksum validation failed");
    }
    cleanup.dismiss();
    return target;
}

void AppUpdateService::StartDownloadedUpdate(const AppPaths& paths, const std::filesystem::path& downloadedExe) {
    if (!IsExecutableFile(downloadedExe)) {
        throw std::runtime_error("downloaded app update executable is missing");
    }
    const std::filesystem::path scriptPath = paths.stuffDir() / L"updates" / L"apply-app-update.ps1";
    WriteUtf16LeFile(scriptPath, BuildUpdateScript(downloadedExe, CurrentExecutablePath()));
    LaunchDetachedPowerShellScript(scriptPath);
}
