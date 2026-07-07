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
}
