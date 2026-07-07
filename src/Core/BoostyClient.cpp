#include "BoostyClient.h"

#include "Text.h"
#include "WinHttpClient.h"

#include <nlohmann/json.hpp>

#include <algorithm>
#include <map>
#include <sstream>
#include <stdexcept>

using json = nlohmann::json;

namespace {

struct PostRef {
    std::wstring author;
    std::wstring id;
};

struct VideoChoice {
    std::wstring url;
    std::wstring title;
    std::wstring quality;
};

PostRef ParseBoostyPostUrl(const std::wstring& url) {
    const std::wstring marker = L"boosty.to/";
    const size_t host = url.find(marker);
    if (host == std::wstring::npos) {
        throw std::runtime_error("not a Boosty URL");
    }
    const size_t authorStart = host + marker.size();
    const size_t posts = url.find(L"/posts/", authorStart);
    if (posts == std::wstring::npos) {
        throw std::runtime_error("Boosty post URL expected");
    }
    const size_t idStart = posts + 7;
    size_t idEnd = url.find_first_of(L"?#/", idStart);
    if (idEnd == std::wstring::npos) {
        idEnd = url.size();
    }
    PostRef ref{url.substr(authorStart, posts - authorStart), url.substr(idStart, idEnd - idStart)};
    if (ref.author.empty() || ref.id.empty()) {
        throw std::runtime_error("invalid Boosty post URL");
    }
    return ref;
}

HttpHeaders BuildHeaders(const BoostyAuth& auth) {
    HttpHeaders headers = {
        {L"Accept", L"application/json, text/plain, */*"},
        {L"User-Agent", L"Mozilla/5.0 BoostyDownloader/0.1"}
    };
    if (!auth.cookie.empty()) {
        headers.push_back({L"Cookie", auth.cookie});
    }
    if (!auth.authHeader.empty()) {
        headers.push_back({L"Authorization", auth.authHeader});
    }
    return headers;
}

int RankQuality(const std::wstring& value) {
    static const std::map<std::wstring, int> rank = {
        {L"ultra_hd", 17}, {L"quad_hd", 16}, {L"full_hd", 15}, {L"high", 14},
        {L"medium", 13}, {L"low", 12}, {L"tiny", 11}, {L"lowest", 10},
        {L"live_playback_dash", 9}, {L"live_playback_hls", 8}, {L"live_ondemand_hls", 7},
        {L"live_dash", 6}, {L"live_hls", 5}, {L"hls", 4}, {L"dash", 3},
        {L"dash_uni", 2}, {L"live_cmaf", 1}
    };
    const auto it = rank.find(value);
    return it == rank.end() ? 0 : it->second;
}

std::wstring PreferredType(const std::wstring& quality) {
    if (quality == L"low") return L"low";
    if (quality == L"medium") return L"medium";
    if (quality == L"high") return L"high";
    return L"ultra_hd";
}

json ArrayField(const json& value, const char* first, const char* second) {
    const auto firstIt = value.find(first);
    if (firstIt != value.end() && firstIt->is_array()) {
        return *firstIt;
    }
    const auto secondIt = value.find(second);
    if (secondIt != value.end() && secondIt->is_array()) {
        return *secondIt;
    }
    return json::array();
}

VideoChoice PickVideo(const json& post, const std::wstring& quality) {
    const std::wstring preferred = PreferredType(quality);
    VideoChoice best;
    int bestRank = -1;

    for (const auto& chunk : post.value("data", json::array())) {
        if (chunk.value("type", "") != "ok_video" || !chunk.value("complete", false)) {
            continue;
        }
        const std::wstring title = Utf8ToWide(chunk.value("title", "boosty_video"));
        for (const auto& item : ArrayField(chunk, "playerUrls", "player_urls")) {
            const std::wstring type = Utf8ToWide(item.value("type", ""));
            const std::wstring url = Utf8ToWide(item.value("url", ""));
            if (url.empty()) {
                continue;
            }
            int rank = RankQuality(type);
            if (type == preferred) {
                rank = 1000;
            }
            if (rank > bestRank) {
                bestRank = rank;
                best = {url, title, type};
            }
        }
    }
    if (best.url.empty()) {
        throw std::runtime_error("post has no downloadable Boosty video");
    }
    return best;
}

json FindPost(const PostRef& ref, const BoostyAuth& auth, std::stop_token stopToken) {
    if (stopToken.stop_requested()) {
        throw std::runtime_error("operation canceled");
    }
    try {
        const std::wstring api = L"https://api.boosty.to/v1/blog/" + ref.author + L"/post/" + ref.id;
        return json::parse(WinHttpClient::GetString(api, BuildHeaders(auth), [&]() { return stopToken.stop_requested(); }));
    } catch (const std::exception&) {
        if (stopToken.stop_requested()) {
            throw std::runtime_error("operation canceled");
        }
    }

    std::wstring offset;
    for (int page = 0; page < 100; ++page) {
        if (stopToken.stop_requested()) {
            throw std::runtime_error("operation canceled");
        }
        std::wstring api = L"https://api.boosty.to/v1/blog/" + ref.author + L"/post/?limit=100";
        if (!offset.empty()) {
            api += L"&offset=" + offset;
        }
        const std::string body = WinHttpClient::GetString(api, BuildHeaders(auth), [&]() { return stopToken.stop_requested(); });
        const json parsed = json::parse(body);
        for (const auto& post : parsed.value("data", json::array())) {
            if (Utf8ToWide(post.value("id", "")) == ref.id) {
                return post;
            }
        }
        const json extra = parsed.value("extra", json::object());
        if (extra.value("isLast", extra.value("is_last", true))) {
            break;
        }
        offset = Utf8ToWide(extra.value("offset", ""));
        if (offset.empty()) {
            break;
        }
    }
    throw std::runtime_error("post not found or not available");
}

std::filesystem::path BuildTargetPath(const BoostyDownloadRequest& request, const PostRef& ref, const VideoChoice& video) {
    const std::wstring base = SanitizeFileName(video.title + L" [" + ref.id.substr(0, 8) + L"]");
    return request.outputDirectory / ref.author / (base + L".mp4");
}

} // namespace

std::wstring ExtractAccessTokenFromCookie(const std::wstring& cookieHeader) {
    return ExtractAccessTokenFromText(cookieHeader);
}

std::wstring ExtractAccessTokenFromText(const std::wstring& text) {
    const std::wstring key = L"auth=";
    const size_t start = text.find(key);
    std::wstring decoded = UrlDecode(text);
    if (start != std::wstring::npos) {
        size_t end = text.find(L';', start);
        if (end == std::wstring::npos) {
            end = text.size();
        }
        decoded += L"\n" + UrlDecode(text.substr(start + key.size(), end - start - key.size()));
    }

    for (const std::wstring tokenKey : {L"\"accessToken\":\"", L"\\\"accessToken\\\":\\\""}) {
        const size_t tokenStart = decoded.find(tokenKey);
        if (tokenStart == std::wstring::npos) {
            continue;
        }
        const size_t valueStart = tokenStart + tokenKey.size();
        const size_t valueEnd = decoded.find(tokenKey.starts_with(L"\\\"") ? L"\\\"" : L"\"", valueStart);
        if (valueEnd != std::wstring::npos) {
            return decoded.substr(valueStart, valueEnd - valueStart);
        }
    }
    return {};
}

BoostyDownloadResult DownloadBoostyVideo(
    const BoostyDownloadRequest& request,
    std::stop_token stopToken,
    const BoostyProgressCallback& onProgress
) {
    try {
        if (request.auth.cookie.empty() && request.auth.authHeader.empty()) {
            throw std::runtime_error("Boosty auth is empty");
        }
        if (onProgress) {
            onProgress({L"Поиск поста", 0.0, 0, 0});
        }
        const PostRef ref = ParseBoostyPostUrl(request.url);
        const json post = FindPost(ref, request.auth, stopToken);
        const VideoChoice video = PickVideo(post, request.quality);
        const std::filesystem::path target = BuildTargetPath(request, ref, video);

        if (onProgress) {
            onProgress({L"Скачивание " + video.quality, 0.0, 0, 0});
        }
        WinHttpClient::DownloadFile(
            video.url,
            target,
            BuildHeaders(request.auth),
            [&](std::uint64_t downloaded, std::uint64_t total) {
                if (!onProgress) {
                    return;
                }
                const double percent = total > 0 ? (static_cast<double>(downloaded) / static_cast<double>(total)) * 100.0 : 0.0;
                onProgress({L"Скачивание " + video.quality, percent, downloaded, total});
            },
            [&]() { return stopToken.stop_requested(); }
        );
        return {true, {}, {target}};
    } catch (const std::exception& ex) {
        return {false, Utf8ToWide(ex.what()), {}};
    }
}
