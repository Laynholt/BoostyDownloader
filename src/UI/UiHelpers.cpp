#include "UiHelpers.h"

#include <dwmapi.h>

#include <cstring>

void CopyTextToClipboard(HWND owner, const std::wstring& text) {
    if (!OpenClipboard(owner)) {
        return;
    }
    EmptyClipboard();
    const SIZE_T bytes = (text.size() + 1) * sizeof(wchar_t);
    HGLOBAL data = GlobalAlloc(GMEM_MOVEABLE, bytes);
    if (data) {
        void* target = GlobalLock(data);
        if (target) {
            std::memcpy(target, text.c_str(), bytes);
            GlobalUnlock(data);
            SetClipboardData(CF_UNICODETEXT, data);
            data = nullptr;
        }
        if (data) {
            GlobalFree(data);
        }
    }
    CloseClipboard();
}

void EnableDarkTitleBar(HWND window) {
    BOOL enabled = TRUE;
    constexpr DWORD kDwmUseImmersiveDarkMode = 20;
    if (FAILED(DwmSetWindowAttribute(window, kDwmUseImmersiveDarkMode, &enabled, sizeof(enabled)))) {
        constexpr DWORD kDwmUseImmersiveDarkModeBefore20H1 = 19;
        DwmSetWindowAttribute(window, kDwmUseImmersiveDarkModeBefore20H1, &enabled, sizeof(enabled));
    }
}

void CloseDialogWindow(HWND window) {
    if (!window || !IsWindow(window)) {
        return;
    }
    ShowWindow(window, SW_HIDE);
    DestroyWindow(window);
}

bool DisableModalOwner(HWND owner) {
    const bool ownerWasEnabled = owner && IsWindow(owner) && IsWindowEnabled(owner);
    if (ownerWasEnabled) {
        EnableWindow(owner, FALSE);
    }
    return ownerWasEnabled;
}

void RestoreModalOwnerWindow(HWND owner, bool ownerWasEnabled) {
    if (!ownerWasEnabled || !IsWindow(owner)) {
        return;
    }
    EnableWindow(owner, TRUE);
    if (IsIconic(owner)) {
        ShowWindow(owner, SW_RESTORE);
    } else if (IsZoomed(owner)) {
        ShowWindow(owner, SW_SHOWMAXIMIZED);
    } else if (!IsWindowVisible(owner)) {
        ShowWindow(owner, SW_SHOW);
    }
    SetWindowPos(owner, HWND_TOP, 0, 0, 0, 0, SWP_NOMOVE | SWP_NOSIZE | SWP_SHOWWINDOW);
    BringWindowToTop(owner);
    SetForegroundWindow(owner);
    SetActiveWindow(owner);
    SetFocus(owner);
}

HFONT CreateUiFont(int height, int weight) {
    LOGFONTW font = {};
    font.lfHeight = height;
    font.lfWeight = weight;
    wcscpy_s(font.lfFaceName, L"Segoe UI");
    return CreateFontIndirectW(&font);
}
