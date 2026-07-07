#pragma once

#include "BoostyClient.h"

#include <condition_variable>
#include <cstdint>
#include <map>
#include <mutex>
#include <stop_token>
#include <thread>
#include <vector>

enum class DownloadTaskState {
    Queued,
    Preparing,
    Downloading,
    Completed,
    Failed,
    Canceled
};

struct DownloadTaskSnapshot {
    int id = 0;
    BoostyDownloadRequest request;
    std::wstring title;
    DownloadTaskState state = DownloadTaskState::Queued;
    double percent = 0.0;
    std::wstring statusText;
    std::wstring errorText;
    std::uint64_t downloadedBytes = 0;
    std::uint64_t totalBytes = 0;
    std::vector<std::filesystem::path> outputFiles;
};

class DownloadQueue {
public:
    explicit DownloadQueue(int maxParallelDownloads);
    ~DownloadQueue();

    int Enqueue(const BoostyDownloadRequest& request, std::wstring title);
    void SetMaxParallelDownloads(int value);
    bool Cancel(int id);
    bool Retry(int id);
    void ClearFinished();
    std::vector<DownloadTaskSnapshot> Snapshot() const;
    std::uint64_t Revision() const;
    void Shutdown();

private:
    struct TaskRecord {
        DownloadTaskSnapshot snapshot;
        bool active = false;
    };

    void SchedulerLoop();
    void StartTask(int id, std::stop_token stopToken);
    void FinishTask(int id, std::stop_token stopToken, const BoostyDownloadResult& result);

    int m_maxParallelDownloads = 1;
    int m_nextId = 1;
    int m_activeCount = 0;
    std::uint64_t m_revision = 0;
    bool m_shutdown = false;
    mutable std::mutex m_mutex;
    std::condition_variable m_cv;
    std::map<int, TaskRecord> m_tasks;
    std::map<int, std::jthread> m_workers;
    std::vector<int> m_finishedWorkerIds;
    std::jthread m_scheduler;
};
