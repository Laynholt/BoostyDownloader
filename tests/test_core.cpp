#include "BoostyClient.h"
#include "AppUpdateService.h"
#include "FfmpegTools.h"
#include "ErrorFormatting.h"
#include "Logger.h"
#include "TaskFormatting.h"
#include "Text.h"

#include <cstdlib>
#include <chrono>
#include <cstdio>
#include <source_location>

#include "../src/Core/BoostyClient.cpp"

void Require(bool condition, const std::source_location& location = std::source_location::current()) {
    if (!condition) {
        std::fprintf(stderr, "Check failed at %s:%u\n", location.file_name(), location.line());
        std::abort();
    }
}

void CheckErrorFormatting() {
    const std::pair<const wchar_t*, const wchar_t*> cases[] = {
        {L"HTTP request failed (Win32 error 12029)", L"подключиться"},
        {L"failed to read HTTP data (Win32 error 12030)", L"прервано"},
        {L"HTTP request failed (Win32 error 12002)", L"время ожидания"},
        {L"HTTP request failed (Win32 error 12007)", L"адрес сервера"},
        {L"HTTP request failed (Win32 error 12175)", L"защищённое соединение"},
        {L"HTTP request failed with status 401", L"авторизац"},
        {L"HTTP request failed with status 403", L"доступ"},
        {L"HTTP request failed with status 404", L"не найден"},
        {L"HTTP request failed with status 407", L"прокси"},
        {L"HTTP request failed with status 429", L"Слишком много запросов"},
        {L"HTTP request failed with status 503", L"сервер"},
        {L"HTTP request failed with status 418", L"Сервер отклонил запрос"},
        {L"failed to commit downloaded file (Win32 error 5)", L"доступ"},
        {L"failed to write downloaded data (Win32 error 112)", L"места"},
        {L"failed to open download target", L"файл"},
        {L"download size validation failed", L"не полностью"},
        {L"Boosty auth is empty", L"авторизац"},
        {L"post not found or not available", L"Пост"},
        {L"post has no downloadable Boosty video", L"видео"},
        {L"invalid Boosty post URL", L"ссылк"},
        {L"FFmpeg not found", L"FFmpeg"},
        {L"FFmpeg failed (exit code 1):\nInvalid data found", L"обработать видео"},
        {L"[json.exception.parse_error.101] unexpected token", L"формат данных"},
        {L"app update checksum validation failed", L"повреждён"},
        {L"operation canceled", L"отменена"},
        {L"unexpected failure: (detail)\nline 2", L"Не удалось выполнить операцию"},
        {L"HTTP request failed with status 4010", L"Не удалось выполнить операцию"},
        {L"HTTP request failed (Win32 error malformed)", L"сетевой запрос"},
        {L"HTTP request failed (Win32 error 999999999999999999999)", L"сетевой запрос"},
        {L"HTTP request failed (Win32 error 10051)", L"интернет"},
        {L"invalid URL (Win32 error 12005)", L"ссылк"},
        {L"invalid URL (Win32 error 12006)", L"ссылк"},
    };
    for (const auto& [detail, explanation] : cases) {
        const std::wstring summary = FormatErrorSummary(detail);
        if (summary.find(explanation) == std::wstring::npos) {
            std::fprintf(stderr, "Wrong explanation for: %s\n", WideToUtf8(detail).c_str());
            Require(false);
        }
        Require(summary.find(L"Win32 error") == std::wstring::npos);
        Require(summary.find(L'\n') == std::wstring::npos);
        const std::wstring log = FormatErrorDetails(detail);
        Require(log.starts_with(summary));
        Require(log.find(detail) != std::wstring::npos);
    }
    Require(!FormatErrorSummary(L"").empty());
    Require(FormatErrorDetails(L"") == FormatErrorSummary(L""));
    Require(FormatErrorSummary(L"HTTP request failed with status 401").find(L"интернет") == std::wstring::npos);

    const std::wstring windowsDetail = WindowsErrorDetails(L"test operation", ERROR_ACCESS_DENIED);
    Require(windowsDetail.starts_with(L"test operation (Win32 error 5: "));
    Require(FormatErrorSummary(windowsDetail).find(L"доступ") != std::wstring::npos);
    try {
        WinHttpClient::GetString(L"not a URL");
        Require(false);
    } catch (const std::exception& ex) {
        const std::wstring detail = Utf8ToWide(ex.what());
        Require(detail.find(L"Win32 error 12005") != std::wstring::npos ||
            detail.find(L"Win32 error 12006") != std::wstring::npos);
        Require(FormatErrorSummary(detail).find(L"ссылк") != std::wstring::npos);
    }
}

void CheckTaskErrorLog() {
    const std::filesystem::path root = std::filesystem::temp_directory_path() /
        (L"boosty-error-test-" + std::to_wstring(GetCurrentProcessId()));
    {
        Logger logger{AppPaths(root)};
        DownloadQueue queue(1, &logger);
        BoostyDownloadRequest request;
        request.url = L"https://boosty.to/author/posts/test";
        const int id = queue.Enqueue(request, L"Error test");
        const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(5);
        std::vector<DownloadTaskSnapshot> tasks;
        do {
            tasks = queue.Snapshot();
            if (!tasks.empty() && tasks.front().state == DownloadTaskState::Failed) {
                break;
            }
            std::this_thread::sleep_for(std::chrono::milliseconds(10));
        } while (std::chrono::steady_clock::now() < deadline);
        Require(tasks.size() == 1 && tasks.front().state == DownloadTaskState::Failed);
        Require(tasks.front().errorText == L"Boosty auth is empty");
        Require(FormatErrorSummary(tasks.front().errorText).find(L"авторизац") != std::wstring::npos);
        const std::wstring log = logger.ReadAll();
        Require(log.find(L"[ERROR] Задача #" + std::to_wstring(id) + L": Нет данных авторизации") != std::wstring::npos);
        Require(log.find(L"(Boosty auth is empty)") != std::wstring::npos);
    }
    std::filesystem::remove_all(root);
}

int main() {
    CheckErrorFormatting();
    CheckTaskErrorLog();
    const std::wstring cookie = L"_clientId=x; auth=%7B%22accessToken%22%3A%22abc123%22%2C%22refreshToken%22%3A%22def%22%7D; last_acc=x";
    Require(ExtractAccessTokenFromCookie(cookie) == L"abc123");
    Require(ExtractAccessTokenFromText(L"{\\\"accessToken\\\":\\\"from_local_storage\\\"}") == L"from_local_storage");

    const auto lines = ExtractUrls(L"\r\nhttps://boosty.to/a/posts/1\n  https://boosty.to/b/posts/2  \n");
    Require(lines.size() == 2);
    Require(lines[0] == L"https://boosty.to/a/posts/1");
    Require(lines[1] == L"https://boosty.to/b/posts/2");

    const auto pythonList = ExtractUrls(LR"([
      "https://boosty.to/a/posts/1",
      "https://boosty.to/b/posts/2",
    ])");
    Require(pythonList.size() == 2);
    Require(pythonList[0] == L"https://boosty.to/a/posts/1");
    Require(pythonList[1] == L"https://boosty.to/b/posts/2");

    const auto oneUrl = ExtractUrls(L"  https://boosty.to/c/posts/3  ");
    Require(oneUrl.size() == 1);
    Require(oneUrl[0] == L"https://boosty.to/c/posts/3");
    Require(IsBoostyPostUrl(L"https://boosty.to/a/posts/1"));
    Require(IsBoostyPostUrl(L"https://boosty.to/a/posts/1?share=1"));
    Require(!IsBoostyPostUrl(L"https://boosty.to/a/posts/"));
    Require(!IsBoostyPostUrl(L"https://boosty.to/posts/1"));
    Require(!IsBoostyPostUrl(L"https://example.com/a/posts/1"));

    const json post = json::parse(R"({
        "data": [{
            "type": "ok_video",
            "complete": true,
            "title": "Paid video",
            "playerUrls": [{
                "type": "ultra_hd",
                "url": "https://cdn.example/video.mp4"
            }]
        }]
    })");
    const VideoChoice video = PickVideo(post, L"highest");
    Require(video.url == L"https://cdn.example/video.mp4");
    Require(video.title == L"Paid video");
    Require(video.quality == L"ultra_hd");
    Require(PreferredType(L"full_hd") == L"full_hd");
    Require(PreferredType(L"1080") == L"full_hd");

    const json multiPost = json::parse(R"({
        "title": "Post Title",
        "data": [
            {"type": "ok_video", "complete": true, "title": "Video A", "playerUrls": [{"type": "high", "url": "https://cdn.example/a.mp4"}]},
            {"type": "ok_video", "complete": true, "title": "Video B", "playerUrls": [{"type": "high", "url": "https://cdn.example/b.mp4"}]}
        ]
    })");
    const auto videos = PickVideos(multiPost, L"high");
    Require(videos.size() == 2);
    Require(PostTitle(multiPost) == L"Post Title");
    const auto thumbnailUrl = PostThumbnailUrl(json::parse(R"({"previewUrl": "https://cdn.example/thumb.jpg"})"));
    Require(thumbnailUrl == L"https://cdn.example/thumb.jpg");
    const auto nestedThumbnailUrl = PostThumbnailUrl(json::parse(R"({"data": [{"video": {"thumbnail": {"url": "//cdn.example/nested.jpg"}}}]})"));
    Require(nestedThumbnailUrl == L"https://cdn.example/nested.jpg");
    Require(QualityLabel(L"full_hd") == L"1080");

    BoostyDownloadRequest request;
    request.outputDirectory = L"C:\\Downloads";
    const PostRef ref{L"author", L"734a0fcc-dd95-40df-b02c-8eb4f0ed37ff"};
    Require(BuildTargetPath(request, ref, L"Post Title", 0, 2).filename().wstring() == L"Post Title 01 [734a0fcc].mp4");
    Require(BuildTargetPath(request, ref, L"Post Title", 1, 2).filename().wstring() == L"Post Title 02 [734a0fcc].mp4");

    std::uint64_t convertedMs = 0;
    if (!TryParseFfmpegProgressTimeMs(L"out_time_ms=1500000", convertedMs) || convertedMs != 1500) {
        return 1;
    }
    if (FormatFfmpegProgressText(1500, 120000) != L"Конвертировано: 00:01 / 02:00") {
        return 1;
    }
    if (FormatFfmpegProgressText(1500, 0) != L"Конвертировано: 00:01") {
        return 1;
    }

    DownloadTaskSnapshot task;
    task.containerLabel = L"MP4";
    task.qualityLabel = L"1080";
    task.etaText = L"0:42";
    task.speedBytesPerSecond = 1'500'000;
    task.downloadedBytes = 5ull * 1024ull * 1024ull;
    task.totalBytes = 10ull * 1024ull * 1024ull;
    if (FormatTaskMetaText(task) != L"MP4  |  1080  |  12.0 Мбит/с  |  ETA 0:42  |  5.0 MB / 10.0 MB") {
        return 1;
    }

    const std::string releaseJson = R"({"tag_name":"v9.9.9","assets":[{"name":"BoostyDownloader.exe","browser_download_url":"https://example.test/BoostyDownloader.exe"}]})";
    const ReleaseAssetInfo release = ParseGitHubReleaseAsset(releaseJson, "BoostyDownloader.exe");
    if (!release.found || release.version != L"9.9.9" || !ShouldInstallAppUpdate(release)) {
        return 1;
    }
    const std::string sums = "ABCDEFABCDEFABCDEFABCDEFABCDEFABCDEFABCDEFABCDEFABCDEFABCDEFABCD *BoostyDownloader.exe\n";
    if (AppUpdateService::Sha256ForFile(sums, "BoostyDownloader.exe") != L"abcdefabcdefabcdefabcdefabcdefabcdefabcdefabcdefabcdefabcdefabcd") {
        return 1;
    }
}
