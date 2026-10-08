#include "DownloadQueue.h"

#include "Logger.h"
#include "ErrorFormatting.h"

#include <algorithm>
#include <filesystem>
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>

namespace {

std::wstring FormatEta(std::uint64_t seconds) {
    const std::uint64_t hours = seconds / 3600;
    const std::uint64_t minutes = (seconds % 3600) / 60;
    seconds %= 60;
    wchar_t buffer[32] = {};
    if (hours > 0) {
        swprintf_s(buffer, L"%llu:%02llu:%02llu", hours, minutes, seconds);
    } else {
        swprintf_s(buffer, L"%llu:%02llu", minutes, seconds);
    }
    return buffer;
}

void AddUniquePath(std::vector<std::filesystem::path>& paths, const std::filesystem::path& path) {
    if (path.empty()) {
        return;
    }
    if (std::find(paths.begin(), paths.end(), path) == paths.end()) {
        paths.push_back(path);
    }
}

void RemovePartialFilesFor(const std::filesystem::path& path) {
    if (path.empty()) {
        return;
    }
    std::error_code ec;
    std::filesystem::remove(path, ec);
    ec.clear();
    std::filesystem::remove(path.wstring() + L".download", ec);
}

void RemoveInvalidTaskFiles(const DownloadTaskSnapshot& task) {
    for (const std::filesystem::path& path : task.outputFiles) {
        RemovePartialFilesFor(path);
    }
    if (!task.thumbnailPath.empty()) {
        std::error_code ec;
        std::filesystem::remove(task.thumbnailPath, ec);
        ec.clear();
        std::filesystem::remove(task.thumbnailPath.wstring() + L".download", ec);
    }
}

bool IsPersistedRunningState(DownloadTaskState state) {
    return state == DownloadTaskState::Queued ||
        state == DownloadTaskState::Preparing ||
        state == DownloadTaskState::Downloading;
}

DownloadTaskSnapshot SnapshotForShutdown(DownloadTaskSnapshot task) {
    if (IsPersistedRunningState(task.state)) {
        task.state = DownloadTaskState::Canceled;
        task.statusText = L"Отменено";
        task.etaText.clear();
    }
    return task;
}

DownloadTaskSnapshot NormalizeRestoredSnapshot(DownloadTaskSnapshot task) {
    if (IsPersistedRunningState(task.state)) {
        task.state = DownloadTaskState::Canceled;
        task.statusText = L"Отменено";
        task.etaText.clear();
    }
    if (task.title.empty()) {
        task.title = task.request.url;
    }
    if (task.statusText.empty()) {
        switch (task.state) {
        case DownloadTaskState::Queued:
            task.statusText = L"В очереди";
            break;
        case DownloadTaskState::Completed:
            task.statusText = L"Готово";
            break;
        case DownloadTaskState::Failed:
            task.statusText = L"Ошибка";
            break;
        case DownloadTaskState::Canceled:
            task.statusText = L"Отменено";
            break;
        case DownloadTaskState::Preparing:
        case DownloadTaskState::Downloading:
            task.statusText = L"Отменено";
            break;
        }
    }
    return task;
}

} // namespace

DownloadQueue::DownloadQueue(int maxParallelDownloads, Logger* logger)
    : m_maxParallelDownloads(std::max(1, maxParallelDownloads)),
      m_logger(logger),
      m_scheduler(&DownloadQueue::SchedulerLoop, this) {
}

DownloadQueue::~DownloadQueue() {
    Shutdown();
}

int DownloadQueue::Enqueue(const BoostyDownloadRequest& request, std::wstring title) {
    std::lock_guard lock(m_mutex);
    for (const auto& [id, task] : m_tasks) {
        if (task.snapshot.request.url == request.url &&
            (task.snapshot.state == DownloadTaskState::Queued ||
             task.snapshot.state == DownloadTaskState::Preparing ||
             task.snapshot.state == DownloadTaskState::Downloading)) {
            return id;
        }
    }
    const int id = m_nextId++;
    TaskRecord record;
    record.snapshot.id = id;
    record.snapshot.request = request;
    record.snapshot.title = std::move(title);
    record.snapshot.statusText = L"В очереди";
    m_tasks[id] = std::move(record);
    ++m_revision;
    m_cv.notify_all();
    if (m_logger) {
        m_logger->Info(L"Task #" + std::to_wstring(id) + L" queued: " + request.url);
    }
    return id;
}

void DownloadQueue::SetMaxParallelDownloads(int value) {
    {
        std::lock_guard lock(m_mutex);
        m_maxParallelDownloads = std::clamp(value, 1, 16);
    }
    m_cv.notify_all();
    if (m_logger) {
        m_logger->Info(L"Parallel downloads set to " + std::to_wstring(std::clamp(value, 1, 16)));
    }
}

bool DownloadQueue::Cancel(int id) {
    std::lock_guard lock(m_mutex);
    auto it = m_tasks.find(id);
    if (it == m_tasks.end()) {
        return false;
    }
    if (auto worker = m_workers.find(id); worker != m_workers.end()) {
        worker->second.request_stop();
    }
    if (!it->second.active) {
        it->second.snapshot.state = DownloadTaskState::Canceled;
        it->second.snapshot.statusText = L"Отменено";
        ++m_revision;
    }
    m_cv.notify_all();
    return true;
}

bool DownloadQueue::Retry(int id) {
    std::lock_guard lock(m_mutex);
    auto it = m_tasks.find(id);
    if (it == m_tasks.end() || it->second.active) {
        return false;
    }
    it->second.snapshot.state = DownloadTaskState::Queued;
    it->second.snapshot.statusText = L"В очереди";
    it->second.snapshot.errorText.clear();
    it->second.snapshot.progressText.clear();
    it->second.snapshot.etaText.clear();
    it->second.snapshot.speedBytesPerSecond = 0;
    it->second.snapshot.percent = 0;
    it->second.snapshot.downloadedBytes = 0;
    it->second.snapshot.totalBytes = 0;
    it->second.progressStartedTick = 0;
    it->second.lastProgressTick = 0;
    it->second.lastDownloadedBytes = 0;
    it->second.speedSampleTick = 0;
    it->second.speedSampleBytes = 0;
    ++m_revision;
    m_cv.notify_all();
    return true;
}

bool DownloadQueue::Remove(int id) {
    std::lock_guard lock(m_mutex);
    const auto it = m_tasks.find(id);
    if (it == m_tasks.end() || it->second.active) {
        return false;
    }
    const DownloadTaskState state = it->second.snapshot.state;
    if (state == DownloadTaskState::Canceled || state == DownloadTaskState::Failed) {
        RemoveInvalidTaskFiles(it->second.snapshot);
    }
    m_tasks.erase(it);
    ++m_revision;
    return true;
}

void DownloadQueue::ClearFinished() {
    std::lock_guard lock(m_mutex);
    bool changed = false;
    for (auto it = m_tasks.begin(); it != m_tasks.end();) {
        if (!it->second.active &&
            (it->second.snapshot.state == DownloadTaskState::Completed ||
             it->second.snapshot.state == DownloadTaskState::Failed ||
             it->second.snapshot.state == DownloadTaskState::Canceled)) {
            const DownloadTaskState state = it->second.snapshot.state;
            if (state == DownloadTaskState::Failed || state == DownloadTaskState::Canceled) {
                RemoveInvalidTaskFiles(it->second.snapshot);
            }
            it = m_tasks.erase(it);
            changed = true;
        } else {
            ++it;
        }
    }
    if (changed) {
        ++m_revision;
    }
}

void DownloadQueue::ClearInactive() {
    std::lock_guard lock(m_mutex);
    bool changed = false;
    for (auto it = m_tasks.begin(); it != m_tasks.end();) {
        if (!it->second.active && it->second.snapshot.state == DownloadTaskState::Queued) {
            it = m_tasks.erase(it);
            changed = true;
        } else {
            ++it;
        }
    }
    if (changed) {
        ++m_revision;
    }
}

std::vector<DownloadTaskSnapshot> DownloadQueue::Snapshot() const {
    std::lock_guard lock(m_mutex);
    std::vector<DownloadTaskSnapshot> result;
    for (const auto& [id, task] : m_tasks) {
        (void)id;
        result.push_back(task.snapshot);
    }
    return result;
}

void DownloadQueue::ImportSnapshots(const std::vector<DownloadTaskSnapshot>& tasks) {
    std::lock_guard lock(m_mutex);
    if (m_activeCount > 0 || !m_workers.empty()) {
        return;
    }

    m_tasks.clear();
    m_nextId = 1;
    for (DownloadTaskSnapshot task : tasks) {
        if (task.id <= 0 || task.request.url.empty()) {
            continue;
        }
        TaskRecord record;
        record.snapshot = NormalizeRestoredSnapshot(std::move(task));
        record.active = false;
        m_nextId = std::max(m_nextId, record.snapshot.id + 1);
        m_tasks[record.snapshot.id] = std::move(record);
    }
    ++m_revision;
    m_cv.notify_all();
}

std::vector<DownloadTaskSnapshot> DownloadQueue::ExportSnapshots() const {
    std::lock_guard lock(m_mutex);
    std::vector<DownloadTaskSnapshot> result;
    for (const auto& [id, task] : m_tasks) {
        (void)id;
        if (task.snapshot.state != DownloadTaskState::Completed) {
            result.push_back(task.snapshot);
        }
    }
    return result;
}

std::vector<DownloadTaskSnapshot> DownloadQueue::ExportSnapshotsForShutdown() const {
    std::lock_guard lock(m_mutex);
    std::vector<DownloadTaskSnapshot> result;
    for (const auto& [id, task] : m_tasks) {
        (void)id;
        if (task.snapshot.state != DownloadTaskState::Completed) {
            result.push_back(SnapshotForShutdown(task.snapshot));
        }
    }
    return result;
}

std::uint64_t DownloadQueue::Revision() const {
    std::lock_guard lock(m_mutex);
    return m_revision;
}

bool DownloadQueue::RefreshDynamicStats() {
    std::lock_guard lock(m_mutex);
    bool changed = false;
    const std::uint64_t now = GetTickCount64();
    for (auto& [id, task] : m_tasks) {
        (void)id;
        if (!task.active || task.snapshot.state != DownloadTaskState::Downloading) {
            continue;
        }
        if (task.snapshot.speedBytesPerSecond == 0 || task.lastProgressTick == 0) {
            continue;
        }
        if (now - task.lastProgressTick < 1000) {
            continue;
        }
        task.snapshot.speedBytesPerSecond = 0;
        task.snapshot.etaText.clear();
        task.speedSampleTick = now;
        task.speedSampleBytes = task.snapshot.downloadedBytes;
        task.lastProgressTick = now;
        changed = true;
    }
    if (changed) {
        ++m_revision;
    }
    return changed;
}

void DownloadQueue::Shutdown() {
    {
        std::lock_guard lock(m_mutex);
        if (m_shutdown) {
            return;
        }
        m_shutdown = true;
        for (auto& [id, worker] : m_workers) {
            (void)id;
            worker.request_stop();
        }
    }
    m_cv.notify_all();
}

void DownloadQueue::SchedulerLoop() {
    while (true) {
        std::vector<std::jthread> finished;
        {
            std::unique_lock lock(m_mutex);
            m_cv.wait(lock, [&]() {
                return m_shutdown || !m_finishedWorkerIds.empty() ||
                    (m_activeCount < m_maxParallelDownloads &&
                     std::any_of(m_tasks.begin(), m_tasks.end(), [](const auto& item) {
                         return item.second.snapshot.state == DownloadTaskState::Queued;
                     }));
            });
            if (m_shutdown) {
                break;
            }
            for (int id : m_finishedWorkerIds) {
                auto it = m_workers.find(id);
                if (it != m_workers.end()) {
                    finished.push_back(std::move(it->second));
                    m_workers.erase(it);
                }
            }
            m_finishedWorkerIds.clear();

            while (m_activeCount < m_maxParallelDownloads) {
                auto it = std::find_if(m_tasks.begin(), m_tasks.end(), [](const auto& item) {
                    return item.second.snapshot.state == DownloadTaskState::Queued;
                });
                if (it == m_tasks.end()) {
                    break;
                }
                const int id = it->first;
                it->second.active = true;
                it->second.snapshot.state = DownloadTaskState::Preparing;
                it->second.snapshot.statusText = L"Подготовка";
                ++m_revision;
                ++m_activeCount;
                m_workers.emplace(id, std::jthread([this, id](std::stop_token stopToken) {
                    StartTask(id, stopToken);
                }));
            }
        }
        finished.clear();
    }
}

void DownloadQueue::StartTask(int id, std::stop_token stopToken) {
    DownloadTaskSnapshot task;
    {
        std::lock_guard lock(m_mutex);
        auto it = m_tasks.find(id);
        if (it == m_tasks.end()) {
            return;
        }
        task = it->second.snapshot;
    }

    if (m_logger) {
        m_logger->Info(L"Task #" + std::to_wstring(id) + L" started: " + task.request.url);
    }
    const BoostyDownloadResult result = DownloadBoostyVideo(
        task.request,
        stopToken,
        [this, id](const BoostyProgress& progress) {
            std::lock_guard lock(m_mutex);
            auto it = m_tasks.find(id);
            if (it == m_tasks.end()) {
                return;
            }
            it->second.snapshot.state = DownloadTaskState::Downloading;
            if (!progress.taskTitle.empty()) {
                it->second.snapshot.title = progress.taskTitle;
            }
            if (!progress.thumbnailUrl.empty()) {
                it->second.snapshot.thumbnailUrl = progress.thumbnailUrl;
            }
            if (!progress.thumbnailPath.empty()) {
                it->second.snapshot.thumbnailPath = progress.thumbnailPath;
            }
            if (!progress.qualityLabel.empty()) {
                it->second.snapshot.qualityLabel = progress.qualityLabel;
            }
            if (!progress.containerLabel.empty()) {
                it->second.snapshot.containerLabel = progress.containerLabel;
            }
            it->second.snapshot.progressText = progress.progressText;
            AddUniquePath(it->second.snapshot.outputFiles, progress.outputPath);
            it->second.snapshot.statusText = progress.stage;
            it->second.snapshot.percent = std::clamp(progress.percent, 0.0, 100.0);
            it->second.snapshot.downloadedBytes = progress.downloadedBytes;
            it->second.snapshot.totalBytes = progress.totalBytes;
            const std::uint64_t now = GetTickCount64();
            if (progress.downloadedBytes == 0 || progress.downloadedBytes < it->second.lastDownloadedBytes) {
                it->second.progressStartedTick = now;
                it->second.lastProgressTick = now;
                it->second.speedSampleTick = now;
                it->second.speedSampleBytes = progress.downloadedBytes;
                it->second.snapshot.speedBytesPerSecond = 0;
                it->second.snapshot.etaText.clear();
            } else if (progress.totalBytes > progress.downloadedBytes && progress.downloadedBytes > 0 && it->second.progressStartedTick > 0) {
                if (it->second.speedSampleTick == 0) {
                    it->second.speedSampleTick = now;
                    it->second.speedSampleBytes = progress.downloadedBytes;
                } else if (now - it->second.speedSampleTick >= 500 && progress.downloadedBytes > it->second.speedSampleBytes) {
                    const std::uint64_t deltaMs = std::max<std::uint64_t>(1, now - it->second.speedSampleTick);
                    const std::uint64_t deltaBytes = progress.downloadedBytes - it->second.speedSampleBytes;
                    it->second.snapshot.speedBytesPerSecond = static_cast<std::uint64_t>((static_cast<double>(deltaBytes) * 1000.0 / static_cast<double>(deltaMs)) + 0.5);
                    it->second.speedSampleTick = now;
                    it->second.speedSampleBytes = progress.downloadedBytes;
                }
                const std::uint64_t elapsedMs = std::max<std::uint64_t>(1, now - it->second.progressStartedTick);
                const double bytesPerSecond = static_cast<double>(progress.downloadedBytes) * 1000.0 / static_cast<double>(elapsedMs);
                if (bytesPerSecond > 0.0) {
                    const auto remaining = static_cast<std::uint64_t>((static_cast<double>(progress.totalBytes - progress.downloadedBytes) / bytesPerSecond) + 0.5);
                    it->second.snapshot.etaText = FormatEta(remaining);
                }
            } else if (progress.totalBytes > 0 && progress.downloadedBytes >= progress.totalBytes) {
                it->second.snapshot.etaText = L"0:00";
                it->second.snapshot.speedBytesPerSecond = 0;
                it->second.speedSampleTick = 0;
                it->second.speedSampleBytes = progress.downloadedBytes;
            }
            it->second.lastDownloadedBytes = progress.downloadedBytes;
            it->second.lastProgressTick = now;
            ++m_revision;
        }
    );
    FinishTask(id, stopToken, result);
    m_cv.notify_all();
}

void DownloadQueue::FinishTask(int id, std::stop_token stopToken, const BoostyDownloadResult& result) {
    std::lock_guard lock(m_mutex);
    auto it = m_tasks.find(id);
    if (it == m_tasks.end()) {
        return;
    }
    it->second.active = false;
    --m_activeCount;
    if (!result.outputFiles.empty()) {
        it->second.snapshot.outputFiles = result.outputFiles;
    }
    if (stopToken.stop_requested()) {
        it->second.snapshot.state = DownloadTaskState::Canceled;
        it->second.snapshot.statusText = L"Отменено";
        it->second.snapshot.etaText.clear();
        if (m_logger) {
            m_logger->Info(L"Task #" + std::to_wstring(id) + L" canceled");
        }
    } else if (result.success) {
        it->second.snapshot.state = DownloadTaskState::Completed;
        it->second.snapshot.statusText = L"Готово";
        it->second.snapshot.etaText.clear();
        it->second.snapshot.percent = 100.0;
        if (m_logger) {
            m_logger->Info(L"Task #" + std::to_wstring(id) + L" completed");
        }
    } else {
        it->second.snapshot.state = DownloadTaskState::Failed;
        it->second.snapshot.statusText = L"Ошибка";
        it->second.snapshot.errorText = result.errorText;
        it->second.snapshot.etaText.clear();
        if (m_logger) {
            m_logger->Error(L"Задача #" + std::to_wstring(id) + L": " + FormatErrorDetails(result.errorText));
        }
    }
    ++m_revision;
    m_finishedWorkerIds.push_back(id);
}
