#include "DownloadQueue.h"

#include <algorithm>

DownloadQueue::DownloadQueue(int maxParallelDownloads)
    : m_maxParallelDownloads(std::max(1, maxParallelDownloads)),
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
    return id;
}

void DownloadQueue::SetMaxParallelDownloads(int value) {
    {
        std::lock_guard lock(m_mutex);
        m_maxParallelDownloads = std::clamp(value, 1, 16);
    }
    m_cv.notify_all();
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
    it->second.snapshot.percent = 0;
    it->second.snapshot.downloadedBytes = 0;
    it->second.snapshot.totalBytes = 0;
    ++m_revision;
    m_cv.notify_all();
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

std::uint64_t DownloadQueue::Revision() const {
    std::lock_guard lock(m_mutex);
    return m_revision;
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
            it->second.snapshot.statusText = progress.stage;
            it->second.snapshot.percent = std::clamp(progress.percent, 0.0, 100.0);
            it->second.snapshot.downloadedBytes = progress.downloadedBytes;
            it->second.snapshot.totalBytes = progress.totalBytes;
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
    it->second.snapshot.outputFiles = result.outputFiles;
    if (stopToken.stop_requested()) {
        it->second.snapshot.state = DownloadTaskState::Canceled;
        it->second.snapshot.statusText = L"Отменено";
    } else if (result.success) {
        it->second.snapshot.state = DownloadTaskState::Completed;
        it->second.snapshot.statusText = L"Готово";
        it->second.snapshot.percent = 100.0;
    } else {
        it->second.snapshot.state = DownloadTaskState::Failed;
        it->second.snapshot.statusText = L"Ошибка";
        it->second.snapshot.errorText = result.errorText;
    }
    ++m_revision;
    m_finishedWorkerIds.push_back(id);
}
