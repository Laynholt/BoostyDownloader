#pragma once

#ifndef NOMINMAX
#define NOMINMAX
#endif

#include <windows.h>

#include <string>
#include <vector>

class UiRenderer {
public:
    struct PopupMenuItem {
        UINT id = 0;
        std::wstring text;
        bool separator = false;
        bool enabled = true;
    };

    static void DrawBackground(HDC dc, const RECT& rect);
    static void DrawPanel(HDC dc, const RECT& rect);
    static void DrawPreviewCard(HDC dc, const RECT& rect);
    static void DrawInputFrame(HDC dc, const RECT& rect);
    static void DrawProgressBar(HDC dc, const RECT& rect, double percent);
    static void DrawIndeterminateProgressBar(HDC dc, const RECT& rect, double phase);
    static void DrawButton(
        HDC dc,
        const RECT& rect,
        const wchar_t* text,
        bool primary,
        bool pressed,
        bool hot,
        bool onPanel,
        bool enabled = true,
        bool onCard = false
    );
    static void DrawPopupMenu(HDC dc, const RECT& rect, const std::vector<PopupMenuItem>& items, UINT hoveredItemId);
};
