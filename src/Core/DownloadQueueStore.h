#pragma once

#include "AppPaths.h"
#include "DownloadQueue.h"

#include <vector>

namespace DownloadQueueStore {
std::vector<DownloadTaskSnapshot> Load(const AppPaths& paths);
void Save(const AppPaths& paths, const std::vector<DownloadTaskSnapshot>& tasks);
}
