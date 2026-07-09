#pragma once

#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>

#include <string>

enum class MessageDialogKind {
    Info,
    Warning,
    Error
};

void ShowCustomMessageDialog(
    HWND owner,
    HINSTANCE instance,
    const std::wstring& title,
    const std::wstring& message,
    MessageDialogKind kind = MessageDialogKind::Info
);
