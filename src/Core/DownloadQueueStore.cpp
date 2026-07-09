#include "DownloadQueueStore.h"

#include "Text.h"

#include <nlohmann/json.hpp>

#include <fstream>
#include <optional>
#include <stdexcept>
#include <system_error>

#ifndef NOMINMAX
#define NOMINMAX
#endif
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>

namespace {

std::string PathToJsonString(const std::filesystem::path& path) {
    return WideToUtf8(path.wstring());
}

std::filesystem::path PathFromJsonString(const nlohmann::json& json, const char* key) {
    const auto it = json.find(key);
    if (it == json.end() || !it->is_string()) {
        return {};
    }
    return std::filesystem::path(Utf8ToWide(it->get<std::string>()));
}

std::wstring WStringFromJson(const nlohmann::json& json, const char* key, const std::wstring& fallback = L"") {
    const auto it = json.find(key);
    if (it == json.end() || !it->is_string()) {
        return fallback;
    }
    return Utf8ToWide(it->get<std::string>());
}

std::uint64_t UInt64FromJson(const nlohmann::json& json, const char* key) {
    const auto it = json.find(key);
    if (it == json.end() || !it->is_number_unsigned()) {
        return 0;
    }
    return it->get<std::uint64_t>();
}

double DoubleFromJson(const nlohmann::json& json, const char* key) {
    const auto it = json.find(key);
    if (it == json.end() || !it->is_number()) {
        return 0.0;
    }
    return it->get<double>();
}

std::string StateToString(DownloadTaskState state) {
    switch (state) {
    case DownloadTaskState::Queued: return "Queued";
    case DownloadTaskState::Preparing: return "Preparing";
    case DownloadTaskState::Downloading: return "Downloading";
    case DownloadTaskState::Completed: return "Completed";
    case DownloadTaskState::Failed: return "Failed";
    case DownloadTaskState::Canceled: return "Canceled";
    }
    return "Canceled";
}

DownloadTaskState StateFromString(const std::string& state) {
    if (state == "Queued") {
        return DownloadTaskState::Queued;
    }
    if (state == "Preparing") {
        return DownloadTaskState::Preparing;
    }
    if (state == "Downloading") {
        return DownloadTaskState::Downloading;
    }
    if (state == "Completed") {
        return DownloadTaskState::Completed;
    }
    if (state == "Failed") {
        return DownloadTaskState::Failed;
    }
    return DownloadTaskState::Canceled;
}

nlohmann::json PathArrayToJson(const std::vector<std::filesystem::path>& paths) {
    nlohmann::json array = nlohmann::json::array();
    for (const std::filesystem::path& path : paths) {
        array.push_back(PathToJsonString(path));
    }
    return array;
}

std::vector<std::filesystem::path> PathArrayFromJson(const nlohmann::json& json, const char* key) {
    std::vector<std::filesystem::path> paths;
    const auto it = json.find(key);
    if (it == json.end() || !it->is_array()) {
        return paths;
    }
    for (const nlohmann::json& item : *it) {
        if (item.is_string()) {
            paths.emplace_back(Utf8ToWide(item.get<std::string>()));
        }
    }
    return paths;
}

nlohmann::json TaskToJson(const DownloadTaskSnapshot& task) {
    nlohmann::json json;
    json["id"] = task.id;
    json["url"] = WideToUtf8(task.request.url);
    json["output_directory"] = PathToJsonString(task.request.outputDirectory);
    json["quality"] = WideToUtf8(task.request.quality);
    json["container"] = WideToUtf8(task.request.container);
    json["ffmpeg_path"] = PathToJsonString(task.request.ffmpegPath);
    json["title"] = WideToUtf8(task.title);
    json["state"] = StateToString(task.state);
    json["percent"] = task.percent;
    json["status_text"] = WideToUtf8(task.statusText);
    json["error_text"] = WideToUtf8(task.errorText);
    json["quality_label"] = WideToUtf8(task.qualityLabel);
    json["eta_text"] = WideToUtf8(task.etaText);
    json["downloaded_bytes"] = task.downloadedBytes;
    json["total_bytes"] = task.totalBytes;
    json["output_files"] = PathArrayToJson(task.outputFiles);
    json["thumbnail_url"] = WideToUtf8(task.thumbnailUrl);
    json["thumbnail_path"] = PathToJsonString(task.thumbnailPath);
    return json;
}

std::optional<DownloadTaskSnapshot> TaskFromJson(const nlohmann::json& json) {
    if (!json.is_object()) {
        return std::nullopt;
    }
    const auto id = json.find("id");
    const auto url = json.find("url");
    if (id == json.end() || !id->is_number_integer() || id->get<int>() <= 0 ||
        url == json.end() || !url->is_string() || url->get<std::string>().empty()) {
        return std::nullopt;
    }

    DownloadTaskSnapshot task;
    task.id = id->get<int>();
    task.request.url = Utf8ToWide(url->get<std::string>());
    task.request.outputDirectory = PathFromJsonString(json, "output_directory");
    task.request.quality = WStringFromJson(json, "quality", L"highest");
    task.request.container = WStringFromJson(json, "container", L"auto");
    task.request.ffmpegPath = PathFromJsonString(json, "ffmpeg_path");
    task.title = WStringFromJson(json, "title", task.request.url);
    task.state = StateFromString(json.value("state", "Canceled"));
    task.percent = DoubleFromJson(json, "percent");
    task.statusText = WStringFromJson(json, "status_text");
    task.errorText = WStringFromJson(json, "error_text");
    task.qualityLabel = WStringFromJson(json, "quality_label");
    task.etaText = WStringFromJson(json, "eta_text");
    task.downloadedBytes = UInt64FromJson(json, "downloaded_bytes");
    task.totalBytes = UInt64FromJson(json, "total_bytes");
    task.outputFiles = PathArrayFromJson(json, "output_files");
    task.thumbnailUrl = WStringFromJson(json, "thumbnail_url");
    task.thumbnailPath = PathFromJsonString(json, "thumbnail_path");
    return task;
}

void ReplaceQueueStoreFile(const std::filesystem::path& tmpPath, const std::filesystem::path& storePath) {
    if (MoveFileExW(tmpPath.c_str(), storePath.c_str(), MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH)) {
        return;
    }
    const std::error_code ec(static_cast<int>(GetLastError()), std::system_category());
    throw std::runtime_error("failed to replace queue store file: " + ec.message());
}

} // namespace

std::vector<DownloadTaskSnapshot> DownloadQueueStore::Load(const AppPaths& paths) {
    std::ifstream in(paths.downloadQueuePath(), std::ios::binary);
    if (!in) {
        return {};
    }

    const nlohmann::json root = nlohmann::json::parse(in, nullptr, true, true);
    if (!root.is_object() || root.value("version", 0) != 1) {
        return {};
    }

    const auto tasks = root.find("tasks");
    if (tasks == root.end() || !tasks->is_array()) {
        return {};
    }

    std::vector<DownloadTaskSnapshot> result;
    for (const nlohmann::json& item : *tasks) {
        try {
            std::optional<DownloadTaskSnapshot> task = TaskFromJson(item);
            if (task) {
                result.push_back(std::move(*task));
            }
        } catch (...) {
        }
    }
    return result;
}

void DownloadQueueStore::Save(const AppPaths& paths, const std::vector<DownloadTaskSnapshot>& tasks) {
    std::error_code ec;
    std::filesystem::create_directories(paths.stuffDir(), ec);
    if (ec) {
        throw std::runtime_error("failed to create queue store directory");
    }

    nlohmann::json root;
    root["version"] = 1;
    root["tasks"] = nlohmann::json::array();
    for (const DownloadTaskSnapshot& task : tasks) {
        root["tasks"].push_back(TaskToJson(task));
    }

    const std::filesystem::path tmpPath = paths.downloadQueuePath().wstring() + L".tmp";
    {
        std::ofstream out(tmpPath, std::ios::binary | std::ios::trunc);
        if (!out) {
            throw std::runtime_error("failed to open temporary queue store file");
        }
        out << root.dump(2) << "\n";
    }

    ReplaceQueueStoreFile(tmpPath, paths.downloadQueuePath());
}
