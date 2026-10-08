#pragma once

#include <string>

// Task rows show the summary; logs and error dialogs retain the original detail.
std::wstring FormatErrorSummary(const std::wstring& detail);
std::wstring FormatErrorDetails(const std::wstring& detail);
std::wstring WindowsErrorDetails(const std::wstring& context, unsigned long code);
