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

HFONT CreateUiFont(int height, int weight) {
    LOGFONTW font = {};
    font.lfHeight = height;
    font.lfWeight = weight;
    wcscpy_s(font.lfFaceName, L"Segoe UI");
    return CreateFontIndirectW(&font);
}
