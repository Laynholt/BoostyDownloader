#pragma once

#include <filesystem>

class AppPaths {
public:
    explicit AppPaths(std::filesystem::path root);

    const std::filesystem::path& root() const;
    std::filesystem::path stuffDir() const;
    std::filesystem::path configPath() const;
    std::filesystem::path logPath() const;
    std::filesystem::path downloadQueuePath() const;
    std::filesystem::path webViewDataDir() const;

private:
    std::filesystem::path m_root;
};

std::filesystem::path GetExecutableRoot();
