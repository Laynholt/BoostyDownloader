#include "Text.h"

#include <algorithm>
#include <cwctype>
#include <sstream>

#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>

std::string WideToUtf8(const std::wstring& value) {
    if (value.empty()) {
        return {};
    }
    const int size = WideCharToMultiByte(CP_UTF8, 0, value.c_str(), static_cast<int>(value.size()), nullptr, 0, nullptr, nullptr);
    std::string result(size, '\0');
    WideCharToMultiByte(CP_UTF8, 0, value.c_str(), static_cast<int>(value.size()), result.data(), size, nullptr, nullptr);
    return result;
}

std::wstring Utf8ToWide(const std::string& value) {
    if (value.empty()) {
        return {};
    }
    const int size = MultiByteToWideChar(CP_UTF8, 0, value.data(), static_cast<int>(value.size()), nullptr, 0);
    std::wstring result(size, L'\0');
    MultiByteToWideChar(CP_UTF8, 0, value.data(), static_cast<int>(value.size()), result.data(), size);
    return result;
}

std::wstring Trim(std::wstring value) {
    auto isSpace = [](wchar_t ch) { return std::iswspace(ch) != 0; };
    value.erase(value.begin(), std::find_if_not(value.begin(), value.end(), isSpace));
    value.erase(std::find_if_not(value.rbegin(), value.rend(), isSpace).base(), value.end());
    return value;
}

std::vector<std::wstring> ExtractUrls(const std::wstring& text) {
    std::vector<std::wstring> result;
    std::wstringstream input(text);
    std::wstring line;
    while (std::getline(input, line)) {
        line = Trim(line);
        if (!line.empty() && line.front() == L'[') {
            line.erase(line.begin());
            line = Trim(line);
        }
        if (!line.empty() && line.back() == L']') {
            line.pop_back();
            line = Trim(line);
        }
        if (!line.empty() && line.back() == L',') {
            line.pop_back();
            line = Trim(line);
        }
        if (line.size() >= 2 &&
            ((line.front() == L'"' && line.back() == L'"') ||
             (line.front() == L'\'' && line.back() == L'\''))) {
            line = line.substr(1, line.size() - 2);
            line = Trim(line);
        }
        if (!line.empty() && line.find(L"://") != std::wstring::npos) {
            result.push_back(line);
        }
    }
    return result;
}

std::wstring UrlDecode(const std::wstring& value) {
    std::wstring out;
    for (size_t i = 0; i < value.size(); ++i) {
        if (value[i] == L'%' && i + 2 < value.size()) {
            const wchar_t hex[] = {value[i + 1], value[i + 2], 0};
            wchar_t* end = nullptr;
            const auto ch = static_cast<wchar_t>(std::wcstoul(hex, &end, 16));
            if (end && *end == 0) {
                out.push_back(ch);
                i += 2;
                continue;
            }
        }
        out.push_back(value[i] == L'+' ? L' ' : value[i]);
    }
    return out;
}

std::wstring SanitizeFileName(std::wstring value) {
    for (wchar_t& ch : value) {
        if (ch < 32 || std::wstring_view(L"<>:\"/\\|?*").find(ch) != std::wstring_view::npos) {
            ch = L'_';
        }
    }
    value = Trim(value);
    return value.empty() ? L"boosty_video" : value;
}
