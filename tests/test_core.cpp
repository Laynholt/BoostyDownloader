#include "BoostyClient.h"
#include "AppUpdateService.h"
#include "FfmpegTools.h"
#include "TaskFormatting.h"
#include "Text.h"

#include <cstdlib>

#include "../src/Core/BoostyClient.cpp"

void Require(bool condition) {
    if (!condition) {
        std::abort();
    }
}

int main() {
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

    const std::string releaseJson = R"({"tag_name":"v1.0.1","assets":[{"name":"BoostyDownloader.exe","browser_download_url":"https://example.test/BoostyDownloader.exe"}]})";
    const ReleaseAssetInfo release = ParseGitHubReleaseAsset(releaseJson, "BoostyDownloader.exe");
    if (!release.found || release.version != L"1.0.1" || !ShouldInstallAppUpdate(release)) {
        return 1;
    }
    const std::string sums = "ABCDEFABCDEFABCDEFABCDEFABCDEFABCDEFABCDEFABCDEFABCDEFABCDEFABCD *BoostyDownloader.exe\n";
    if (AppUpdateService::Sha256ForFile(sums, "BoostyDownloader.exe") != L"abcdefabcdefabcdefabcdefabcdefabcdefabcdefabcdefabcdefabcdefabcd") {
        return 1;
    }
}
