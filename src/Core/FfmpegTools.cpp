#include "FfmpegTools.h"

#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>

#include <algorithm>
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

std::wstring Utf8ChunkToWide(const std::string& bytes) {
    if (bytes.empty()) {
        return {};
    }
    const int wideSize = MultiByteToWideChar(CP_UTF8, 0, bytes.data(), static_cast<int>(bytes.size()), nullptr, 0);
    if (wideSize <= 0) {
        return {};
    }
    std::wstring wide(wideSize, L'\0');
    MultiByteToWideChar(CP_UTF8, 0, bytes.data(), static_cast<int>(bytes.size()), wide.data(), wideSize);
    return wide;
}

std::uint64_t ParseDurationSecondsToMs(const std::wstring& text) {
    try {
        const double seconds = std::stod(text);
        if (seconds > 0.0) {
            return static_cast<std::uint64_t>((seconds * 1000.0) + 0.5);
        }
    } catch (...) {
    }
    return 0;
}

std::uint64_t ProbeDurationMs(const std::filesystem::path& ffmpegExe, const std::filesystem::path& input) {
    const std::filesystem::path probe = ffmpegExe.parent_path() / L"ffprobe.exe";
    if (!IsFfmpegExe(probe)) {
        return 0;
    }
    const std::wstring output = ReadProcessOutput(
        probe,
        L"-v error -show_entries format=duration -of default=noprint_wrappers=1:nokey=1 " + Quote(input)
    );
    return ParseDurationSecondsToMs(output);
}

bool HasNvencEncoder(const std::filesystem::path& ffmpegExe) {
    const std::wstring encoders = ReadProcessOutput(ffmpegExe, L"-hide_banner -encoders");
    return encoders.find(L"h264_nvenc") != std::wstring::npos;
}

std::wstring BuildConvertArguments(
    const std::filesystem::path& input,
    const std::filesystem::path& output,
    bool audioOnly,
    bool useGpu
) {
    std::wstring arguments = L"-hide_banner -nostdin -nostats -loglevel error -y -i " + Quote(input);
    if (audioOnly) {
        arguments += L" -vn -c:a copy ";
    } else if (output.extension() == L".webm") {
        arguments += L" -c:v libvpx-vp9 -b:v 0 -crf 32 -c:a libopus ";
    } else if (output.extension() == L".mp4") {
        arguments += useGpu
            ? L" -c:v h264_nvenc -preset p4 -cq 23 -c:a aac -b:a 192k "
            : L" -c:v libx264 -preset veryfast -crf 23 -c:a aac -b:a 192k ";
    } else {
        arguments += L" -c copy ";
    }
    arguments += L"-progress pipe:1 " + Quote(output);
    return arguments;
}

bool RunFfmpegConvert(
    const std::filesystem::path& ffmpegExe,
    const std::wstring& arguments,
    std::uint64_t totalMs,
    std::stop_token stopToken,
    std::wstring& errorText,
    const FfmpegProgressCallback& onProgress
) {
    SECURITY_ATTRIBUTES security{sizeof(security), nullptr, TRUE};
    HANDLE readPipe = nullptr;
    HANDLE writePipe = nullptr;
    if (!CreatePipe(&readPipe, &writePipe, &security, 0)) {
        errorText = L"failed to create FFmpeg progress pipe";
        return false;
    }
    SetHandleInformation(readPipe, HANDLE_FLAG_INHERIT, 0);

    STARTUPINFOW startup{};
    startup.cb = sizeof(startup);
    startup.dwFlags = STARTF_USESTDHANDLES;
    startup.hStdOutput = writePipe;
    startup.hStdError = writePipe;
    PROCESS_INFORMATION process{};
    std::wstring command = Quote(ffmpegExe) + L" " + arguments;
    if (!CreateProcessW(nullptr, command.data(), nullptr, nullptr, TRUE, CREATE_NO_WINDOW, nullptr, nullptr, &startup, &process)) {
        CloseHandle(readPipe);
        CloseHandle(writePipe);
        errorText = L"failed to start FFmpeg";
        return false;
    }
    CloseHandle(writePipe);

    std::string pending;
    std::wstring processOutput;
    std::uint64_t lastConvertedMs = 0;
    auto emitProgress = [&](std::uint64_t convertedMs) {
        lastConvertedMs = std::max(lastConvertedMs, convertedMs);
        if (!onProgress) {
            return;
        }
        FfmpegProgress progress;
        progress.convertedMs = lastConvertedMs;
        progress.totalMs = totalMs;
        progress.percent = totalMs > 0
            ? std::clamp((static_cast<double>(lastConvertedMs) / static_cast<double>(totalMs)) * 100.0, 0.0, 100.0)
            : 0.0;
        progress.text = FormatFfmpegProgressText(lastConvertedMs, totalMs);
        onProgress(progress);
    };
    emitProgress(0);

    DWORD wait = WAIT_TIMEOUT;
    while (wait == WAIT_TIMEOUT) {
        if (stopToken.stop_requested()) {
            TerminateProcess(process.hProcess, 1);
            CloseHandle(readPipe);
            CloseHandle(process.hThread);
            CloseHandle(process.hProcess);
            errorText = L"operation canceled";
            return false;
        }

        DWORD available = 0;
        while (PeekNamedPipe(readPipe, nullptr, 0, nullptr, &available, nullptr) && available > 0) {
            std::array<char, 4096> buffer{};
            DWORD read = 0;
            if (!ReadFile(readPipe, buffer.data(), std::min<DWORD>(available, static_cast<DWORD>(buffer.size())), &read, nullptr) || read == 0) {
                break;
            }
            pending.append(buffer.data(), buffer.data() + read);

            size_t lineEnd = std::string::npos;
            while ((lineEnd = pending.find('\n')) != std::string::npos) {
                std::string line = pending.substr(0, lineEnd);
                pending.erase(0, lineEnd + 1);
                if (!line.empty() && line.back() == '\r') {
                    line.pop_back();
                }
                const std::wstring wideLine = Utf8ChunkToWide(line);
                processOutput += wideLine;
                processOutput += L"\n";
                std::uint64_t convertedMs = 0;
                if (TryParseFfmpegProgressTimeMs(wideLine, convertedMs)) {
                    emitProgress(convertedMs);
                }
            }
        }

        wait = WaitForSingleObject(process.hProcess, 100);
    }

    DWORD available = 0;
    while (PeekNamedPipe(readPipe, nullptr, 0, nullptr, &available, nullptr) && available > 0) {
        std::array<char, 4096> buffer{};
        DWORD read = 0;
        if (!ReadFile(readPipe, buffer.data(), std::min<DWORD>(available, static_cast<DWORD>(buffer.size())), &read, nullptr) || read == 0) {
            break;
        }
        processOutput += Utf8ChunkToWide(std::string(buffer.data(), buffer.data() + read));
    }
    CloseHandle(readPipe);

    DWORD exitCode = 1;
    GetExitCodeProcess(process.hProcess, &exitCode);
    CloseHandle(process.hThread);
    CloseHandle(process.hProcess);
    if (exitCode != 0) {
        errorText = processOutput.empty() ? L"FFmpeg failed" : Trim(processOutput);
        return false;
    }
    if (totalMs > 0) {
        emitProgress(totalMs);
    }
    return true;
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

bool TryParseFfmpegProgressTimeMs(const std::wstring& line, std::uint64_t& convertedMs) {
    constexpr const wchar_t* prefix = L"out_time_ms=";
    constexpr size_t prefixSize = 12;
    if (line.rfind(prefix, 0) != 0) {
        return false;
    }
    try {
        const std::uint64_t microseconds = std::stoull(line.substr(prefixSize));
        convertedMs = microseconds / 1000;
        return true;
    } catch (...) {
        return false;
    }
}

std::wstring FormatFfmpegProgressText(std::uint64_t convertedMs, std::uint64_t totalMs) {
    auto formatDuration = [](std::uint64_t ms) {
        std::uint64_t seconds = ms / 1000;
        const std::uint64_t hours = seconds / 3600;
        const std::uint64_t minutes = (seconds % 3600) / 60;
        seconds %= 60;
        wchar_t buffer[32] = {};
        if (hours > 0) {
            swprintf_s(buffer, L"%llu:%02llu:%02llu", hours, minutes, seconds);
        } else {
            swprintf_s(buffer, L"%02llu:%02llu", minutes, seconds);
        }
        return std::wstring(buffer);
    };

    std::wstring text = L"Конвертировано: " + formatDuration(convertedMs);
    if (totalMs > 0) {
        text += L" / " + formatDuration(totalMs);
    }
    return text;
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
    std::wstring& errorText,
    const FfmpegProgressCallback& onProgress
) {
    if (!IsFfmpegExe(ffmpegExe)) {
        errorText = L"FFmpeg not found";
        return false;
    }
    std::filesystem::create_directories(output.parent_path());

    const std::uint64_t totalMs = ProbeDurationMs(ffmpegExe, input);
    const bool canUseGpu = !audioOnly && output.extension() == L".mp4" && HasNvencEncoder(ffmpegExe);
    if (canUseGpu) {
        std::wstring gpuError;
        if (RunFfmpegConvert(ffmpegExe, BuildConvertArguments(input, output, audioOnly, true), totalMs, stopToken, gpuError, onProgress)) {
            return true;
        }
        if (stopToken.stop_requested()) {
            errorText = gpuError;
            return false;
        }
        std::error_code ec;
        std::filesystem::remove(output, ec);
    }

    return RunFfmpegConvert(ffmpegExe, BuildConvertArguments(input, output, audioOnly, false), totalMs, stopToken, errorText, onProgress);
}
