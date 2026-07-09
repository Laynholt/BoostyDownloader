#include "FfmpegTools.h"

#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>

#include <array>
#include <cwctype>
#include <fstream>
#include <sstream>
#include <vector>

namespace {

std::wstring Quote(const std::filesystem::path& path);

bool IsFfmpegExe(const std::filesystem::path& path) {
    return !path.empty() && std::filesystem::is_regular_file(path);
}

std::filesystem::path ResolveConfiguredPath(const std::filesystem::path& configuredPath) {
    if (configuredPath.empty()) {
        return {};
    }
    if (IsFfmpegExe(configuredPath)) {
        return configuredPath;
    }
    if (std::filesystem::is_directory(configuredPath)) {
        for (const auto& candidate : {
            configuredPath / L"ffmpeg.exe",
            configuredPath / L"bin" / L"ffmpeg.exe"
        }) {
            if (IsFfmpegExe(candidate)) {
                return candidate;
            }
        }
    }
    return {};
}

std::filesystem::path SearchPathFfmpeg() {
    std::wstring buffer(MAX_PATH, L'\0');
    DWORD size = SearchPathW(nullptr, L"ffmpeg.exe", nullptr, static_cast<DWORD>(buffer.size()), buffer.data(), nullptr);
    if (size == 0) {
        return {};
    }
    if (size >= buffer.size()) {
        buffer.resize(size + 1);
        size = SearchPathW(nullptr, L"ffmpeg.exe", nullptr, static_cast<DWORD>(buffer.size()), buffer.data(), nullptr);
    }
    buffer.resize(size);
    return IsFfmpegExe(buffer) ? std::filesystem::path(buffer) : std::filesystem::path{};
}

std::wstring Trim(std::wstring text) {
    while (!text.empty() && iswspace(text.back())) {
        text.pop_back();
    }
    while (!text.empty() && iswspace(text.front())) {
        text.erase(text.begin());
    }
    return text;
}

std::wstring NormalizeVersion(std::wstring value) {
    value = Trim(std::move(value));
    const std::size_t start = value.find_first_of(L"0123456789");
    if (start == std::wstring::npos) {
        return value;
    }

    std::size_t end = start;
    while (end < value.size() && (iswdigit(value[end]) || value[end] == L'.')) {
        ++end;
    }
    return end > start ? value.substr(start, end - start) : value;
}

std::wstring ParseFfmpegVersionLine(const std::wstring& text) {
    std::wistringstream input(text);
    std::wstring firstLine;
    std::getline(input, firstLine);
    std::wistringstream parts(firstLine);
    std::wstring name;
    std::wstring label;
    std::wstring version;
    if (parts >> name >> label >> version && name == L"ffmpeg" && label == L"version") {
        return NormalizeVersion(version);
    }
    return {};
}

std::wstring ReadProcessOutput(const std::filesystem::path& executable, const std::wstring& arguments) {
    SECURITY_ATTRIBUTES security{sizeof(security), nullptr, TRUE};
    HANDLE readPipe = nullptr;
    HANDLE writePipe = nullptr;
    if (!CreatePipe(&readPipe, &writePipe, &security, 0)) {
        return {};
    }
    SetHandleInformation(readPipe, HANDLE_FLAG_INHERIT, 0);

    STARTUPINFOW startup{};
    startup.cb = sizeof(startup);
    startup.dwFlags = STARTF_USESTDHANDLES;
    startup.hStdOutput = writePipe;
    startup.hStdError = writePipe;
    PROCESS_INFORMATION process{};
    std::wstring command = Quote(executable) + L" " + arguments;
    if (!CreateProcessW(nullptr, command.data(), nullptr, nullptr, TRUE, CREATE_NO_WINDOW, nullptr, nullptr, &startup, &process)) {
        CloseHandle(readPipe);
        CloseHandle(writePipe);
        return {};
    }
    CloseHandle(writePipe);

    std::string bytes;
    std::array<char, 4096> buffer{};
    DWORD read = 0;
    while (ReadFile(readPipe, buffer.data(), static_cast<DWORD>(buffer.size()), &read, nullptr) && read > 0) {
        bytes.append(buffer.data(), buffer.data() + read);
    }
    CloseHandle(readPipe);
    WaitForSingleObject(process.hProcess, 15000);
    DWORD exitCode = 1;
    GetExitCodeProcess(process.hProcess, &exitCode);
    CloseHandle(process.hThread);
    CloseHandle(process.hProcess);
    if (exitCode != 0) {
        return {};
    }

    const int wideSize = MultiByteToWideChar(CP_UTF8, 0, bytes.data(), static_cast<int>(bytes.size()), nullptr, 0);
    if (wideSize <= 0) {
        return {};
    }
    std::wstring wide(wideSize, L'\0');
    MultiByteToWideChar(CP_UTF8, 0, bytes.data(), static_cast<int>(bytes.size()), wide.data(), wideSize);
    return Trim(wide);
}

std::wstring Quote(const std::filesystem::path& path) {
    std::wstring value = path.wstring();
    std::wstring escaped;
    escaped.reserve(value.size() + 2);
    escaped.push_back(L'"');
    for (wchar_t ch : value) {
        if (ch == L'"') {
            escaped.push_back(L'\\');
        }
        escaped.push_back(ch);
    }
    escaped.push_back(L'"');
    return escaped;
}

} // namespace

FfmpegStatus ResolveFfmpeg(const AppPaths& paths, const std::filesystem::path& configuredPath) {
    if (auto path = SearchPathFfmpeg(); !path.empty()) {
        return {true, path, L"PATH", FfmpegVersion(path)};
    }

    const std::vector<std::filesystem::path> localCandidates = {
        paths.root() / L"tools" / L"ffmpeg.exe",
        paths.root() / L"tools" / L"ffmpeg" / L"ffmpeg.exe",
        paths.root() / L"tools" / L"ffmpeg" / L"bin" / L"ffmpeg.exe"
    };
    for (const auto& candidate : localCandidates) {
        if (IsFfmpegExe(candidate)) {
            return {true, candidate, L"tools", FfmpegVersion(candidate)};
        }
    }

    if (auto path = ResolveConfiguredPath(configuredPath); !path.empty()) {
        return {true, path, L"config", FfmpegVersion(path)};
    }

    return {};
}

std::wstring FfmpegVersion(const std::filesystem::path& ffmpegExe) {
    if (!IsFfmpegExe(ffmpegExe)) {
        return {};
    }
    return ParseFfmpegVersionLine(ReadProcessOutput(ffmpegExe, L"-version"));
}

bool InstallFfmpeg(const AppPaths& paths, std::wstring& errorText) {
    const std::filesystem::path script = paths.stuffDir() / L"install-ffmpeg.ps1";
    const std::filesystem::path zip = paths.stuffDir() / L"ffmpeg.zip";
    const std::filesystem::path extract = paths.stuffDir() / L"ffmpeg_extract";
    const std::filesystem::path target = paths.root() / L"tools" / L"ffmpeg" / L"bin";

    std::error_code ec;
    std::filesystem::create_directories(paths.stuffDir(), ec);
    std::filesystem::create_directories(target, ec);

    std::wofstream out(script);
    if (!out) {
        errorText = L"Не удалось создать install script";
        return false;
    }
    out <<
        L"$ErrorActionPreference='Stop'\n"
        L"$url='https://github.com/BtbN/FFmpeg-Builds/releases/download/latest/ffmpeg-master-latest-win64-gpl.zip'\n"
        L"$zip='" << zip.wstring() << L"'\n"
        L"$extract='" << extract.wstring() << L"'\n"
        L"$target='" << target.wstring() << L"'\n"
        L"Remove-Item -LiteralPath $zip -Force -ErrorAction SilentlyContinue\n"
        L"Remove-Item -LiteralPath $extract -Recurse -Force -ErrorAction SilentlyContinue\n"
        L"Invoke-WebRequest -Uri $url -OutFile $zip\n"
        L"Expand-Archive -LiteralPath $zip -DestinationPath $extract -Force\n"
        L"$bin=(Get-ChildItem -LiteralPath $extract -Recurse -Filter ffmpeg.exe | Select-Object -First 1).Directory.FullName\n"
        L"if (-not $bin) { throw 'ffmpeg.exe not found in archive' }\n"
        L"New-Item -ItemType Directory -Path $target -Force | Out-Null\n"
        L"Copy-Item -LiteralPath (Join-Path $bin 'ffmpeg.exe') -Destination $target -Force\n"
        L"Copy-Item -LiteralPath (Join-Path $bin 'ffprobe.exe') -Destination $target -Force -ErrorAction SilentlyContinue\n"
        L"Copy-Item -LiteralPath (Join-Path $bin 'ffplay.exe') -Destination $target -Force -ErrorAction SilentlyContinue\n";
    out.close();

    std::wstring command = L"powershell.exe -NoProfile -ExecutionPolicy Bypass -File " + Quote(script);
    STARTUPINFOW startup{};
    startup.cb = sizeof(startup);
    PROCESS_INFORMATION process{};
    if (!CreateProcessW(nullptr, command.data(), nullptr, nullptr, FALSE, CREATE_NO_WINDOW, nullptr, nullptr, &startup, &process)) {
        errorText = L"Не удалось запустить PowerShell";
        return false;
    }
    WaitForSingleObject(process.hProcess, INFINITE);
    DWORD exitCode = 1;
    GetExitCodeProcess(process.hProcess, &exitCode);
    CloseHandle(process.hThread);
    CloseHandle(process.hProcess);
    if (exitCode != 0 || !IsFfmpegExe(target / L"ffmpeg.exe")) {
        errorText = L"Установка FFmpeg не выполнена";
        return false;
    }
    return true;
}

bool ConvertWithFfmpeg(
    const std::filesystem::path& ffmpegExe,
    const std::filesystem::path& input,
    const std::filesystem::path& output,
    bool audioOnly,
    std::stop_token stopToken,
    std::wstring& errorText
) {
    if (!IsFfmpegExe(ffmpegExe)) {
        errorText = L"FFmpeg not found";
        return false;
    }
    std::filesystem::create_directories(output.parent_path());

    std::wstring command = Quote(ffmpegExe) + L" -hide_banner -loglevel error -y -i " + Quote(input);
    if (audioOnly) {
        command += L" -vn -c:a copy ";
    } else if (output.extension() == L".webm") {
        command += L" -c:v libvpx-vp9 -b:v 0 -crf 32 -c:a libopus ";
    } else {
        command += L" -c copy ";
    }
    command += Quote(output);

    STARTUPINFOW startup{};
    startup.cb = sizeof(startup);
    PROCESS_INFORMATION process{};
    std::wstring mutableCommand = command;
    if (!CreateProcessW(nullptr, mutableCommand.data(), nullptr, nullptr, FALSE, CREATE_NO_WINDOW, nullptr, nullptr, &startup, &process)) {
        errorText = L"failed to start FFmpeg";
        return false;
    }

    DWORD wait = WAIT_TIMEOUT;
    while (wait == WAIT_TIMEOUT) {
        if (stopToken.stop_requested()) {
            TerminateProcess(process.hProcess, 1);
            CloseHandle(process.hThread);
            CloseHandle(process.hProcess);
            errorText = L"operation canceled";
            return false;
        }
        wait = WaitForSingleObject(process.hProcess, 200);
    }

    DWORD exitCode = 1;
    GetExitCodeProcess(process.hProcess, &exitCode);
    CloseHandle(process.hThread);
    CloseHandle(process.hProcess);
    if (exitCode != 0) {
        errorText = L"FFmpeg failed";
        return false;
    }
    return true;
}
