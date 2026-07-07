#pragma once

#include <cstdint>
#include <filesystem>

void CommitDownloadedFile(
    const std::filesystem::path& staged,
    const std::filesystem::path& target,
    std::uint64_t downloaded,
    std::uint64_t expectedSize
);
