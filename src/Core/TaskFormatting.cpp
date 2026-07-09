#include "TaskFormatting.h"

#include <sstream>
#include <vector>

namespace {

std::wstring BytesText(std::uint64_t value) {
    const wchar_t* units[] = {L"B", L"KB", L"MB", L"GB"};
    double amount = static_cast<double>(value);
    int unit = 0;
    while (amount >= 1024.0 && unit < 3) {
        amount /= 1024.0;
        ++unit;
    }
    std::wostringstream out;
    out.setf(std::ios::fixed);
    out.precision(unit == 0 ? 0 : 1);
    out << amount << L' ' << units[unit];
    return out.str();
}

std::wstring SpeedText(std::uint64_t bytesPerSecond) {
    if (bytesPerSecond == 0) {
        return {};
    }
    double bitsPerSecond = static_cast<double>(bytesPerSecond) * 8.0;
    const wchar_t* units[] = {L"бит/с", L"Кбит/с", L"Мбит/с", L"Гбит/с"};
    int unit = 0;
    while (bitsPerSecond >= 1000.0 && unit < 3) {
        bitsPerSecond /= 1000.0;
        ++unit;
    }
    std::wostringstream out;
    out.setf(std::ios::fixed);
    out.precision(unit == 0 ? 0 : 1);
    out << bitsPerSecond << L' ' << units[unit];
    return out.str();
}

void AddPart(std::vector<std::wstring>& parts, std::wstring value) {
    if (!value.empty()) {
        parts.push_back(std::move(value));
    }
}

} // namespace

std::wstring FormatTaskMetaText(const DownloadTaskSnapshot& task) {
    std::vector<std::wstring> parts;
    AddPart(parts, task.containerLabel);
    AddPart(parts, task.qualityLabel);
    AddPart(parts, task.progressText);
    AddPart(parts, SpeedText(task.speedBytesPerSecond));
    if (!task.etaText.empty()) {
        AddPart(parts, L"ETA " + task.etaText);
    }
    if (task.totalBytes > 0) {
        parts.push_back(BytesText(task.downloadedBytes) + L" / " + BytesText(task.totalBytes));
    }

    std::wstring result;
    for (const std::wstring& part : parts) {
        if (!result.empty()) {
            result += L"  |  ";
        }
        result += part;
    }
    return result;
}
