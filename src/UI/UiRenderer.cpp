#include "UiRenderer.h"

#include <gdiplus.h>

#include <algorithm>

using Gdiplus::Color;
using Gdiplus::Font;
using Gdiplus::FontFamily;
using Gdiplus::Graphics;
using Gdiplus::GraphicsPath;
using Gdiplus::Pen;
using Gdiplus::RectF;
using Gdiplus::SolidBrush;
using Gdiplus::StringAlignment;
using Gdiplus::StringFormat;
using Gdiplus::UnitPoint;

namespace {

const Color kBg(255, 20, 20, 22);
const Color kPanel(255, 28, 28, 31);
const Color kPanel2(255, 35, 35, 38);
const Color kBorder(255, 46, 46, 50);
const Color kInput(255, 25, 25, 28);
const Color kAccent(255, 232, 72, 85);
const Color kAccentPressed(255, 197, 45, 59);
const Color kText(255, 242, 242, 242);
constexpr int kPopupItemHeight = 34;
constexpr int kPopupSeparatorHeight = 10;
constexpr int kPopupTextPaddingLeft = 14;

void AddRoundedRect(GraphicsPath& path, const RECT& rect, int radius) {
    const int diameter = radius * 2;
    path.AddArc(rect.left, rect.top, diameter, diameter, 180.0f, 90.0f);
    path.AddArc(rect.right - diameter, rect.top, diameter, diameter, 270.0f, 90.0f);
    path.AddArc(rect.right - diameter, rect.bottom - diameter, diameter, diameter, 0.0f, 90.0f);
    path.AddArc(rect.left, rect.bottom - diameter, diameter, diameter, 90.0f, 90.0f);
    path.CloseFigure();
}

} // namespace

void UiRenderer::DrawBackground(HDC dc, const RECT& rect) {
    Graphics graphics(dc);
    SolidBrush brush(kBg);
    graphics.FillRectangle(
        &brush,
        static_cast<INT>(rect.left),
        static_cast<INT>(rect.top),
        static_cast<INT>(rect.right - rect.left),
        static_cast<INT>(rect.bottom - rect.top)
    );
}

void UiRenderer::DrawPanel(HDC dc, const RECT& rect) {
    Graphics graphics(dc);
    graphics.SetSmoothingMode(Gdiplus::SmoothingModeHighQuality);
    GraphicsPath path;
    AddRoundedRect(path, rect, 10);

    SolidBrush fill(kPanel);
    Pen border(kBorder, 1.0f);
    graphics.FillPath(&fill, &path);
    graphics.DrawPath(&border, &path);
}

void UiRenderer::DrawPreviewCard(HDC dc, const RECT& rect) {
    DrawPanel(dc, rect);

    Graphics graphics(dc);
    graphics.SetSmoothingMode(Gdiplus::SmoothingModeHighQuality);

    RECT thumb = {rect.left + 14, rect.top + 14, rect.left + 224, rect.top + 132};
    GraphicsPath thumbPath;
    AddRoundedRect(thumbPath, thumb, 8);
    SolidBrush thumbFill(kPanel2);
    graphics.FillPath(&thumbFill, &thumbPath);
}

void UiRenderer::DrawInputFrame(HDC dc, const RECT& rect) {
    Graphics graphics(dc);
    graphics.SetSmoothingMode(Gdiplus::SmoothingModeHighQuality);

    RECT inset = {rect.left, rect.top, rect.right, rect.bottom};
    GraphicsPath path;
    AddRoundedRect(path, inset, 8);

    SolidBrush fill(kInput);
    Pen border(kBorder, 1.0f);
    graphics.FillPath(&fill, &path);
    graphics.DrawPath(&border, &path);
}

void UiRenderer::DrawProgressBar(HDC dc, const RECT& rect, double percent) {
    Graphics graphics(dc);
    graphics.SetSmoothingMode(Gdiplus::SmoothingModeHighQuality);

    GraphicsPath trackPath;
    AddRoundedRect(trackPath, rect, 4);
    SolidBrush trackBrush(Color(255, 26, 26, 29));
    graphics.FillPath(&trackBrush, &trackPath);

    percent = std::clamp(percent, 0.0, 100.0);
    if (percent <= 0.0) {
        return;
    }

    RECT fillRect = rect;
    const int width = std::max(1, static_cast<int>(rect.right - rect.left));
    fillRect.right = fillRect.left + static_cast<LONG>((width * percent) / 100.0);
    GraphicsPath fillPath;
    AddRoundedRect(fillPath, fillRect, 4);
    SolidBrush fillBrush(kAccent);
    graphics.FillPath(&fillBrush, &fillPath);
}

void UiRenderer::DrawIndeterminateProgressBar(HDC dc, const RECT& rect, double phase) {
    Graphics graphics(dc);
    graphics.SetSmoothingMode(Gdiplus::SmoothingModeHighQuality);

    GraphicsPath trackPath;
    AddRoundedRect(trackPath, rect, 4);
    SolidBrush trackBrush(Color(255, 26, 26, 29));
    graphics.FillPath(&trackBrush, &trackPath);

    const int width = std::max(1, static_cast<int>(rect.right - rect.left));
    const int blockWidth = std::max(18, width / 3);
    const int travel = std::max(0, width - blockWidth);
    phase = std::clamp(phase, 0.0, 1.0);

    RECT blockRect = rect;
    blockRect.left = rect.left + static_cast<LONG>(travel * phase);
    blockRect.right = blockRect.left + blockWidth;

    GraphicsPath blockPath;
    AddRoundedRect(blockPath, blockRect, 4);
    SolidBrush blockBrush(kAccent);
    graphics.FillPath(&blockBrush, &blockPath);
}

void UiRenderer::DrawButton(
    HDC dc,
    const RECT& rect,
    const wchar_t* text,
    bool primary,
    bool pressed,
    bool hot,
    bool onPanel,
    bool enabled,
    bool onCard
) {
    Graphics graphics(dc);
    graphics.SetSmoothingMode(Gdiplus::SmoothingModeHighQuality);
    graphics.SetTextRenderingHint(Gdiplus::TextRenderingHintClearTypeGridFit);

    SolidBrush clearBrush(onCard ? kPanel2 : (onPanel ? kPanel : kBg));
    graphics.FillRectangle(
        &clearBrush,
        static_cast<INT>(rect.left),
        static_cast<INT>(rect.top),
        static_cast<INT>(rect.right - rect.left),
        static_cast<INT>(rect.bottom - rect.top)
    );

    GraphicsPath path;
    RECT inset = {rect.left + 1, rect.top + 1, rect.right - 1, rect.bottom - 1};
    AddRoundedRect(path, inset, 7);

    const Color fillColor = !enabled
        ? Color(255, 36, 36, 39)
        : primary
        ? (pressed ? kAccentPressed : (hot ? Color(255, 242, 88, 101) : kAccent))
        : (pressed ? Color(255, 33, 33, 37) : (hot ? Color(255, 50, 50, 55) : Color(255, 42, 42, 46)));
    SolidBrush fill(fillColor);
    Pen border((enabled && primary) ? fillColor : Color(255, 58, 58, 64), 1.0f);
    graphics.FillPath(&fill, &path);
    graphics.DrawPath(&border, &path);

    FontFamily family(L"Segoe UI");
    Font font(&family, 10.0f, Gdiplus::FontStyleRegular, UnitPoint);
    SolidBrush textBrush(enabled ? kText : Color(255, 136, 136, 142));
    RectF textRect(
        static_cast<Gdiplus::REAL>(rect.left),
        static_cast<Gdiplus::REAL>(rect.top),
        static_cast<Gdiplus::REAL>(rect.right - rect.left),
        static_cast<Gdiplus::REAL>(rect.bottom - rect.top)
    );
    StringFormat format;
    format.SetAlignment(StringAlignment::StringAlignmentCenter);
    format.SetLineAlignment(StringAlignment::StringAlignmentCenter);
    graphics.DrawString(text, -1, &font, textRect, &format, &textBrush);
}

void UiRenderer::DrawPopupMenu(HDC dc, const RECT& rect, const std::vector<PopupMenuItem>& items, UINT hoveredItemId) {
    if (!dc) {
        return;
    }

    HBRUSH backgroundBrush = CreateSolidBrush(RGB(45, 45, 45));
    FillRect(dc, &rect, backgroundBrush);
    DeleteObject(backgroundBrush);

    HPEN outerBorderPen = CreatePen(PS_SOLID, 1, RGB(22, 22, 24));
    HPEN oldPen = static_cast<HPEN>(SelectObject(dc, outerBorderPen));
    MoveToEx(dc, rect.left, rect.top, nullptr);
    LineTo(dc, rect.right - 1, rect.top);
    LineTo(dc, rect.right - 1, rect.bottom - 1);
    LineTo(dc, rect.left, rect.bottom - 1);
    LineTo(dc, rect.left, rect.top);
    SelectObject(dc, oldPen);
    DeleteObject(outerBorderPen);

    RECT innerBorder = {rect.left + 1, rect.top + 1, rect.right - 1, rect.bottom - 1};
    HPEN borderPen = CreatePen(PS_SOLID, 1, RGB(82, 82, 86));
    oldPen = static_cast<HPEN>(SelectObject(dc, borderPen));
    MoveToEx(dc, innerBorder.left, innerBorder.top, nullptr);
    LineTo(dc, innerBorder.right - 1, innerBorder.top);
    LineTo(dc, innerBorder.right - 1, innerBorder.bottom - 1);
    LineTo(dc, innerBorder.left, innerBorder.bottom - 1);
    LineTo(dc, innerBorder.left, innerBorder.top);
    SelectObject(dc, oldPen);
    DeleteObject(borderPen);

    HFONT font = CreateFontW(
        -13,
        0,
        0,
        0,
        FW_NORMAL,
        FALSE,
        FALSE,
        FALSE,
        DEFAULT_CHARSET,
        OUT_DEFAULT_PRECIS,
        CLIP_DEFAULT_PRECIS,
        CLEARTYPE_QUALITY,
        DEFAULT_PITCH | FF_DONTCARE,
        L"Segoe UI"
    );
    HFONT oldFont = static_cast<HFONT>(SelectObject(dc, font));
    SetBkMode(dc, TRANSPARENT);
    SetTextColor(dc, RGB(235, 235, 235));

    int top = 2;
    for (const PopupMenuItem& item : items) {
        const int itemHeight = item.separator ? kPopupSeparatorHeight : kPopupItemHeight;
        RECT itemRect = {rect.left + 2, rect.top + top, rect.right - 2, rect.top + top + itemHeight};
        top += itemHeight;

        if (item.separator) {
            HPEN separatorPen = CreatePen(PS_SOLID, 1, RGB(78, 78, 78));
            HPEN oldSeparatorPen = static_cast<HPEN>(SelectObject(dc, separatorPen));
            const int y = (itemRect.top + itemRect.bottom) / 2;
            MoveToEx(dc, itemRect.left + 11, y, nullptr);
            LineTo(dc, itemRect.right - 11, y);
            SelectObject(dc, oldSeparatorPen);
            DeleteObject(separatorPen);
            continue;
        }

        if (item.enabled && item.id == hoveredItemId) {
            HBRUSH selectedBrush = CreateSolidBrush(RGB(66, 66, 66));
            FillRect(dc, &itemRect, selectedBrush);
            DeleteObject(selectedBrush);
        }

        RECT textRect = itemRect;
        textRect.left += kPopupTextPaddingLeft;
        textRect.right -= 10;
        SetTextColor(dc, item.enabled ? RGB(235, 235, 235) : RGB(128, 128, 132));
        DrawTextW(dc, item.text.c_str(), -1, &textRect, DT_LEFT | DT_VCENTER | DT_SINGLELINE | DT_END_ELLIPSIS);
    }

    SelectObject(dc, oldFont);
    DeleteObject(font);
}
