#pragma once

#include <string>
#include <vector>

std::string WideToUtf8(const std::wstring& value);
std::wstring Utf8ToWide(const std::string& value);
std::wstring Trim(std::wstring value);
std::vector<std::wstring> ExtractUrls(const std::wstring& text);
std::wstring UrlDecode(const std::wstring& value);
std::wstring SanitizeFileName(std::wstring value);
