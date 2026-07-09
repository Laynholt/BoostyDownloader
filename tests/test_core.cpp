#include "BoostyClient.h"
#include "Text.h"

#include <cassert>

#include "../src/Core/BoostyClient.cpp"

int main() {
    const std::wstring cookie = L"_clientId=x; auth=%7B%22accessToken%22%3A%22abc123%22%2C%22refreshToken%22%3A%22def%22%7D; last_acc=x";
    assert(ExtractAccessTokenFromCookie(cookie) == L"abc123");
    assert(ExtractAccessTokenFromText(L"{\\\"accessToken\\\":\\\"from_local_storage\\\"}") == L"from_local_storage");

    const auto lines = SplitLines(L"\r\nhttps://boosty.to/a/posts/1\n  https://boosty.to/b/posts/2  \n");
    assert(lines.size() == 2);
    assert(lines[0] == L"https://boosty.to/a/posts/1");
    assert(lines[1] == L"https://boosty.to/b/posts/2");

    const auto pythonList = ExtractUrls(LR"([
      "https://boosty.to/a/posts/1",
      "https://boosty.to/b/posts/2",
    ])");
    assert(pythonList.size() == 2);
    assert(pythonList[0] == L"https://boosty.to/a/posts/1");
    assert(pythonList[1] == L"https://boosty.to/b/posts/2");

    const auto oneUrl = ExtractUrls(L"  https://boosty.to/c/posts/3  ");
    assert(oneUrl.size() == 1);
    assert(oneUrl[0] == L"https://boosty.to/c/posts/3");

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
    assert(video.url == L"https://cdn.example/video.mp4");
    assert(video.title == L"Paid video");
    assert(video.quality == L"ultra_hd");
    assert(PreferredType(L"full_hd") == L"full_hd");
    assert(PreferredType(L"1080") == L"full_hd");

    const json multiPost = json::parse(R"({
        "title": "Post Title",
        "data": [
            {"type": "ok_video", "complete": true, "title": "Video A", "playerUrls": [{"type": "high", "url": "https://cdn.example/a.mp4"}]},
            {"type": "ok_video", "complete": true, "title": "Video B", "playerUrls": [{"type": "high", "url": "https://cdn.example/b.mp4"}]}
        ]
    })");
    const auto videos = PickVideos(multiPost, L"high");
    assert(videos.size() == 2);
    assert(PostTitle(multiPost) == L"Post Title");
    const auto thumbnailUrl = PostThumbnailUrl(json::parse(R"({"previewUrl": "https://cdn.example/thumb.jpg"})"));
    assert(thumbnailUrl == L"https://cdn.example/thumb.jpg");
    const auto nestedThumbnailUrl = PostThumbnailUrl(json::parse(R"({"data": [{"video": {"thumbnail": {"url": "//cdn.example/nested.jpg"}}}]})"));
    assert(nestedThumbnailUrl == L"https://cdn.example/nested.jpg");
    assert(QualityLabel(L"full_hd") == L"1080");

    BoostyDownloadRequest request;
    request.outputDirectory = L"C:\\Downloads";
    const PostRef ref{L"author", L"734a0fcc-dd95-40df-b02c-8eb4f0ed37ff"};
    assert(BuildTargetPath(request, ref, L"Post Title", 0, 2).filename().wstring() == L"Post Title 01 [734a0fcc].mp4");
    assert(BuildTargetPath(request, ref, L"Post Title", 1, 2).filename().wstring() == L"Post Title 02 [734a0fcc].mp4");
}
