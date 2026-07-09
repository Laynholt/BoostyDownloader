#include "BoostyClient.h"

#include "FfmpegTools.h"
#include "Text.h"
#include "WinHttpClient.h"

#include <nlohmann/json.hpp>

#include <algorithm>
#include <cctype>
#include <cwctype>
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

std::vector<VideoChoice> PickVideos(const json& post, const std::wstring& quality);

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
    if (quality == L"360" || quality == L"low") return L"low";
    if (quality == L"480" || quality == L"medium") return L"medium";
    if (quality == L"720" || quality == L"high") return L"high";
    if (quality == L"1080" || quality == L"full_hd") return L"full_hd";
    if (quality == L"low") return L"low";
    if (quality == L"medium") return L"medium";
    if (quality == L"high") return L"high";
    return L"ultra_hd";
}

std::wstring QualityLabel(const std::wstring& quality) {
    if (quality == L"ultra_hd") return L"2160";
    if (quality == L"quad_hd") return L"1440";
    if (quality == L"full_hd") return L"1080";
    if (quality == L"high") return L"720";
    if (quality == L"medium") return L"480";
    if (quality == L"low") return L"360";
    if (quality == L"tiny" || quality == L"lowest") return L"240";
    return quality;
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
    const auto videos = PickVideos(post, quality);
    if (videos.empty()) {
        throw std::runtime_error("post has no downloadable Boosty video");
    }
    return videos.front();
}

std::vector<VideoChoice> PickVideos(const json& post, const std::wstring& quality) {
    const std::wstring preferred = PreferredType(quality);
    std::vector<VideoChoice> videos;

    for (const auto& chunk : post.value("data", json::array())) {
        if (chunk.value("type", "") != "ok_video" || !chunk.value("complete", false)) {
            continue;
        }
        const std::wstring title = Utf8ToWide(chunk.value("title", "boosty_video"));
        VideoChoice best;
        int bestRank = -1;
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
        if (!best.url.empty()) {
            videos.push_back(std::move(best));
        }
    }
    if (videos.empty()) {
        throw std::runtime_error("post has no downloadable Boosty video");
    }
    return videos;
}

std::wstring PostTitle(const json& post) {
    const std::wstring title = Utf8ToWide(post.value("title", ""));
    return title.empty() ? L"boosty_video" : title;
}

bool LooksLikeImageUrl(const std::wstring& value) {
    if (!(value.starts_with(L"https://") || value.starts_with(L"http://") || value.starts_with(L"//"))) {
        return false;
    }
    std::wstring lower = value;
    std::transform(lower.begin(), lower.end(), lower.begin(), [](wchar_t ch) {
        return static_cast<wchar_t>(std::towlower(ch));
    });
    return lower.find(L".jpg") != std::wstring::npos ||
        lower.find(L".jpeg") != std::wstring::npos ||
        lower.find(L".png") != std::wstring::npos ||
        lower.find(L".webp") != std::wstring::npos;
}

std::wstring NormalizeMediaUrl(std::wstring value) {
    if (value.starts_with(L"//")) {
        value.insert(0, L"https:");
    }
    return value;
}

bool IsThumbnailKey(const std::string& key) {
    std::string lower = key;
    std::transform(lower.begin(), lower.end(), lower.begin(), [](unsigned char ch) {
        return static_cast<char>(std::tolower(ch));
    });
    return lower.find("preview") != std::string::npos ||
        lower.find("thumb") != std::string::npos ||
        lower.find("cover") != std::string::npos ||
        lower == "image" ||
        lower == "imageurl" ||
        lower == "image_url";
}

std::wstring FindThumbnailUrl(const json& value, bool keyHint = false) {
    if (value.is_string()) {
        const std::wstring candidate = NormalizeMediaUrl(Utf8ToWide(value.get<std::string>()));
        if ((keyHint && (candidate.starts_with(L"https://") || candidate.starts_with(L"http://"))) || LooksLikeImageUrl(candidate)) {
            return candidate;
        }
        return {};
    }
    if (value.is_object()) {
        for (const auto& [key, child] : value.items()) {
            if (IsThumbnailKey(key)) {
                const std::wstring found = FindThumbnailUrl(child, true);
                if (!found.empty()) {
                    return found;
                }
            }
        }
        for (const auto& [key, child] : value.items()) {
            (void)key;
            const std::wstring found = FindThumbnailUrl(child, false);
            if (!found.empty()) {
                return found;
            }
        }
    } else if (value.is_array()) {
        for (const auto& child : value) {
            const std::wstring found = FindThumbnailUrl(child, keyHint);
            if (!found.empty()) {
                return found;
            }
        }
    }
    return {};
}

std::wstring PostThumbnailUrl(const json& post) {
    return FindThumbnailUrl(post);
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

std::filesystem::path BuildTargetPath(const BoostyDownloadRequest& request, const PostRef& ref, const std::wstring& postTitle, size_t videoIndex, size_t videoCount) {
    std::wstring base = postTitle;
    if (videoCount > 1) {
        std::wostringstream number;
        number.width(2);
        number.fill(L'0');
        number << (videoIndex + 1);
        base += L" " + number.str();
    }
    base = SanitizeFileName(base + L" [" + ref.id.substr(0, 8) + L"]");
    return request.outputDirectory / ref.author / (base + L".mp4");
}

std::filesystem::path BuildThumbnailPath(const BoostyDownloadRequest& request, const PostRef& ref, const std::wstring& postTitle) {
    const std::wstring base = SanitizeFileName(postTitle + L" [" + ref.id.substr(0, 8) + L"]");
    return request.outputDirectory / ref.author / L"thumbnails" / (base + L".thumb.jpg");
}

std::filesystem::path WithExtension(const std::filesystem::path& path, const std::wstring& extension) {
    std::filesystem::path result = path;
    result.replace_extension(extension);
    return result;
}

std::wstring ContainerLabel(const std::filesystem::path& path) {
    std::wstring extension = path.extension().wstring();
    if (!extension.empty() && extension.front() == L'.') {
        extension.erase(extension.begin());
    }
    std::transform(extension.begin(), extension.end(), extension.begin(), towupper);
    return extension;
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

bool IsBoostyPostUrl(const std::wstring& url) {
    try {
        ParseBoostyPostUrl(url);
        return true;
    } catch (const std::exception&) {
        return false;
    }
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
        const std::wstring postTitle = PostTitle(post);
        const std::wstring thumbnailUrl = PostThumbnailUrl(post);
        std::filesystem::path thumbnailPath;
        if (!thumbnailUrl.empty()) {
            thumbnailPath = BuildThumbnailPath(request, ref, postTitle);
            try {
                WinHttpClient::DownloadFile(
                    thumbnailUrl,
                    thumbnailPath,
                    BuildHeaders(request.auth),
                    {},
                    [&]() { return stopToken.stop_requested(); }
                );
            } catch (...) {
                thumbnailPath.clear();
            }
        }
        const std::vector<VideoChoice> videos = PickVideos(post, request.quality);
        std::vector<std::filesystem::path> outputFiles;
        outputFiles.reserve(videos.size());

        for (size_t index = 0; index < videos.size(); ++index) {
            const VideoChoice& video = videos[index];
            const std::filesystem::path target = BuildTargetPath(request, ref, postTitle, index, videos.size());

            const std::wstring qualityLabel = QualityLabel(video.quality);

            if (onProgress) {
                onProgress({L"Скачивание", 0.0, 0, 0, qualityLabel, ContainerLabel(target), {}, postTitle, thumbnailUrl, thumbnailPath, target});
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
                    onProgress({L"Скачивание", percent, downloaded, total, qualityLabel, ContainerLabel(target), {}, postTitle, thumbnailUrl, thumbnailPath, target});
                },
                [&]() { return stopToken.stop_requested(); }
            );

            std::filesystem::path finalTarget = target;
            const bool audioOnly = request.quality == L"audio";
            const bool convertContainer =
                !audioOnly &&
                !request.container.empty() &&
                request.container != L"auto" &&
                request.container != L"mp4";
            if (audioOnly || convertContainer) {
                if (request.ffmpegPath.empty()) {
                    throw std::runtime_error("FFmpeg not found");
                }
                std::wstring error;
                const std::wstring conversionStage = audioOnly ? L"Извлечение аудио" : L"Конвертация в " + request.container;
                if (audioOnly) {
                    finalTarget = WithExtension(target, L".m4a");
                    if (onProgress) {
                        onProgress({conversionStage, 0.0, 0, 0, qualityLabel, ContainerLabel(finalTarget), {}, postTitle, thumbnailUrl, thumbnailPath, finalTarget});
                    }
                } else {
                    finalTarget = WithExtension(target, L"." + request.container);
                    if (onProgress) {
                        onProgress({conversionStage, 0.0, 0, 0, qualityLabel, ContainerLabel(finalTarget), {}, postTitle, thumbnailUrl, thumbnailPath, finalTarget});
                    }
                }
                if (!ConvertWithFfmpeg(
                        request.ffmpegPath,
                        target,
                        finalTarget,
                        audioOnly,
                        stopToken,
                        error,
                        [&](const FfmpegProgress& progress) {
                            if (onProgress) {
                                onProgress({conversionStage, progress.percent, 0, 0, qualityLabel, ContainerLabel(finalTarget), progress.text, postTitle, thumbnailUrl, thumbnailPath, finalTarget});
                            }
                        })) {
                    throw std::runtime_error(WideToUtf8(error.empty() ? L"FFmpeg failed" : error).c_str());
                }
                std::error_code ec;
                std::filesystem::remove(target, ec);
            }

            outputFiles.push_back(finalTarget);
        }

        return {true, {}, outputFiles};
    } catch (const std::exception& ex) {
        return {false, Utf8ToWide(ex.what()), {}};
    }
}
