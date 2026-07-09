#include "MessageDialog.h"

#include "AppPaths.h"
#include "Text.h"
#include "UiHelpers.h"
#include "UiRenderer.h"

#include <commctrl.h>
#include <gdiplus.h>
#include <windowsx.h>

#include <algorithm>
#include <chrono>
#include <fstream>
#include <iomanip>
#include <sstream>
#include <system_error>
#include <vector>

namespace {

constexpr UINT kOk = 1;
constexpr UINT kCopy = 2;

struct DialogButton {
    UINT id = 0;
    RECT rect{};
    std::wstring text;
    bool primary = false;
};

struct DialogState {
    std::wstring title;
    std::wstring message;
    MessageDialogKind kind = MessageDialogKind::Info;
    std::vector<DialogButton> buttons;
    UINT hotButton = 0;
    UINT pressedButton = 0;
    int scrollY = 0;
    int contentHeight = 0;
    bool draggingThumb = false;
    int dragStartY = 0;
    int dragStartScrollY = 0;
};

void DrawTextLine(HDC dc, const std::wstring& text, RECT rect, int size, COLORREF color, UINT format = DT_LEFT | DT_VCENTER | DT_SINGLELINE) {
    HFONT font = CreateUiFont(-size);
    HFONT oldFont = reinterpret_cast<HFONT>(SelectObject(dc, font));
    SetBkMode(dc, TRANSPARENT);
    SetTextColor(dc, color);
    DrawTextW(dc, text.c_str(), -1, &rect, format | DT_NOPREFIX);
    SelectObject(dc, oldFont);
    DeleteObject(font);
}

Gdiplus::Color AccentForKind(MessageDialogKind kind) {
    switch (kind) {
    case MessageDialogKind::Error:
        return Gdiplus::Color(255, 240, 70, 86);
    case MessageDialogKind::Warning:
        return Gdiplus::Color(255, 238, 176, 70);
    case MessageDialogKind::Info:
        return Gdiplus::Color(255, 90, 154, 255);
    }
    return Gdiplus::Color(255, 90, 154, 255);
}

const wchar_t* SymbolForKind(MessageDialogKind kind) {
    switch (kind) {
    case MessageDialogKind::Error:
        return L"!";
    case MessageDialogKind::Warning:
        return L"!";
    case MessageDialogKind::Info:
        return L"i";
    }
    return L"i";
}

std::wstring TimestampLocal() {
    const auto now = std::chrono::system_clock::now();
    const std::time_t time = std::chrono::system_clock::to_time_t(now);
    std::tm local = {};
    localtime_s(&local, &time);
    std::wostringstream out;
    out << std::put_time(&local, L"%Y-%m-%dT%H:%M:%S");
    return out.str();
}

const wchar_t* LogLevelForKind(MessageDialogKind kind) {
    return kind == MessageDialogKind::Error ? L"ERROR" : L"INFO";
}

void AppendDialogLog(const std::wstring& title, const std::wstring& message, MessageDialogKind kind) {
    try {
        const AppPaths paths(GetExecutableRoot());
        std::error_code ec;
        std::filesystem::create_directories(paths.logPath().parent_path(), ec);
        std::ofstream out(paths.logPath(), std::ios::binary | std::ios::app);
        if (!out) {
            return;
        }
        out << WideToUtf8(
            L"[" + TimestampLocal() + L"] [" + LogLevelForKind(kind) + L"] " +
            title + L": " + message + L"\n"
        );
    } catch (...) {
    }
}

DialogButton* HitButton(DialogState* state, POINT point) {
    if (!state) {
        return nullptr;
    }
    for (DialogButton& button : state->buttons) {
        if (PtInRect(&button.rect, point)) {
            return &button;
        }
    }
    return nullptr;
}

RECT MessageTextRect(const RECT& client) {
    return {94, 66, client.right - 34, client.bottom - 78};
}

int VisibleTextHeight(const RECT& textRect) {
    return std::max(1, static_cast<int>(textRect.bottom - textRect.top));
}

int MeasureTextHeight(HDC dc, HFONT font, const std::wstring& text, int width) {
    RECT rect{0, 0, std::max(1, width), 1};
    HFONT oldFont = reinterpret_cast<HFONT>(SelectObject(dc, font));
    DrawTextW(dc, text.c_str(), -1, &rect, DT_LEFT | DT_WORDBREAK | DT_CALCRECT | DT_NOPREFIX);
    SelectObject(dc, oldFont);
    return std::max(1, static_cast<int>(rect.bottom - rect.top));
}

int MaxScrollY(const DialogState* state, const RECT& client) {
    if (!state) {
        return 0;
    }
    const RECT textRect = MessageTextRect(client);
    return std::max(0, state->contentHeight - VisibleTextHeight(textRect));
}

bool SetScrollY(HWND window, DialogState* state, int value) {
    if (!state) {
        return false;
    }
    RECT client{};
    GetClientRect(window, &client);
    const int clamped = std::clamp(value, 0, MaxScrollY(state, client));
    if (clamped == state->scrollY) {
        return false;
    }
    state->scrollY = clamped;
    return true;
}

RECT ScrollbarThumb(const DialogState* state, const RECT& client) {
    const RECT textRect = MessageTextRect(client);
    const int visible = VisibleTextHeight(textRect);
    const int content = std::max(visible, state ? state->contentHeight : visible);
    const int trackTop = textRect.top + 2;
    const int trackBottom = textRect.bottom - 2;
    const int trackHeight = std::max(1, trackBottom - trackTop);
    const int thumbHeight = std::clamp((visible * trackHeight) / content, 28, trackHeight);
    const int maxScroll = std::max(1, content - visible);
    const int travel = std::max(0, trackHeight - thumbHeight);
    const int thumbTop = trackTop + ((state ? state->scrollY : 0) * travel) / maxScroll;
    return {client.right - 28, thumbTop, client.right - 22, thumbTop + thumbHeight};
}

void LayoutButtons(DialogState* state, const RECT& client) {
    if (!state) {
        return;
    }
    constexpr int width = 132;
    constexpr int height = 34;
    const int bottom = client.bottom - 20;
    const int okLeft = client.right - 20 - width;
    state->buttons = {{kOk, {okLeft, bottom - height, okLeft + width, bottom}, L"OK", true}};
    if (state->kind == MessageDialogKind::Error) {
        state->buttons.insert(
            state->buttons.begin(),
            {kCopy, {okLeft - 12 - width, bottom - height, okLeft - 12, bottom}, L"Копировать", false}
        );
    }
}

void PaintDialog(HWND window, DialogState* state, HDC dc) {
    RECT client{};
    GetClientRect(window, &client);
    LayoutButtons(state, client);

    UiRenderer::DrawBackground(dc, client);
    UiRenderer::DrawPanel(dc, {12, 12, client.right - 12, client.bottom - 12});

    const RECT iconRect{32, 72, 76, 116};
    Gdiplus::Graphics graphics(dc);
    graphics.SetSmoothingMode(Gdiplus::SmoothingModeHighQuality);
    Gdiplus::SolidBrush iconBrush(AccentForKind(state ? state->kind : MessageDialogKind::Info));
    graphics.FillEllipse(
        &iconBrush,
        static_cast<Gdiplus::REAL>(iconRect.left),
        static_cast<Gdiplus::REAL>(iconRect.top),
        static_cast<Gdiplus::REAL>(iconRect.right - iconRect.left),
        static_cast<Gdiplus::REAL>(iconRect.bottom - iconRect.top)
    );
    DrawTextLine(dc, state ? SymbolForKind(state->kind) : L"i", iconRect, 24, RGB(255, 255, 255), DT_CENTER | DT_VCENTER | DT_SINGLELINE);

    DrawTextLine(dc, state ? state->title : L"", {28, 24, client.right - 28, 56}, 22, RGB(242, 242, 242));
    const RECT textRect = MessageTextRect(client);
    HFONT textFont = CreateUiFont(-15);
    if (state) {
        const int scrollbarSpace = state->contentHeight > VisibleTextHeight(textRect) ? 16 : 0;
        const int textWidth = std::max(1, static_cast<int>(textRect.right - textRect.left) - scrollbarSpace);
        state->contentHeight = MeasureTextHeight(dc, textFont, state->message, textWidth);
        state->scrollY = std::clamp(state->scrollY, 0, MaxScrollY(state, client));

        RECT drawRect{
            textRect.left,
            textRect.top - state->scrollY,
            textRect.right - scrollbarSpace,
            textRect.top - state->scrollY + state->contentHeight
        };
        HRGN clip = CreateRectRgn(textRect.left, textRect.top, textRect.right, textRect.bottom);
        SelectClipRgn(dc, clip);
        HFONT oldFont = reinterpret_cast<HFONT>(SelectObject(dc, textFont));
        SetBkMode(dc, TRANSPARENT);
        SetTextColor(dc, RGB(220, 220, 224));
        DrawTextW(dc, state->message.c_str(), -1, &drawRect, DT_LEFT | DT_WORDBREAK | DT_NOPREFIX);
        SelectObject(dc, oldFont);
        SelectClipRgn(dc, nullptr);
        DeleteObject(clip);

        if (state->contentHeight > VisibleTextHeight(textRect)) {
            const RECT thumb = ScrollbarThumb(state, client);
            Gdiplus::SolidBrush trackBrush(Gdiplus::Color(255, 39, 39, 43));
            Gdiplus::SolidBrush thumbBrush(Gdiplus::Color(255, 86, 86, 92));
            graphics.FillRectangle(
                &trackBrush,
                static_cast<INT>(client.right - 28),
                static_cast<INT>(textRect.top + 2),
                6,
                static_cast<INT>(textRect.bottom - textRect.top - 4)
            );
            graphics.FillRectangle(
                &thumbBrush,
                static_cast<INT>(thumb.left),
                static_cast<INT>(thumb.top),
                static_cast<INT>(thumb.right - thumb.left),
                static_cast<INT>(thumb.bottom - thumb.top)
            );
        }
    }
    DeleteObject(textFont);

    if (state) {
        for (const DialogButton& button : state->buttons) {
            UiRenderer::DrawButton(
                dc,
                button.rect,
                button.text.c_str(),
                button.primary,
                state->pressedButton == button.id,
                state->hotButton == button.id,
                true
            );
        }
    }
}

LRESULT CALLBACK MessageDialogProc(HWND window, UINT message, WPARAM wParam, LPARAM lParam) {
    auto* state = reinterpret_cast<DialogState*>(GetWindowLongPtrW(window, GWLP_USERDATA));
    if (message == WM_NCCREATE) {
        const auto* create = reinterpret_cast<CREATESTRUCTW*>(lParam);
        state = static_cast<DialogState*>(create->lpCreateParams);
        SetWindowLongPtrW(window, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(state));
        return TRUE;
    }

    switch (message) {
    case WM_ERASEBKGND:
        return 1;
    case WM_MOUSEWHEEL:
        if (state) {
            const int delta = GET_WHEEL_DELTA_WPARAM(wParam);
            if (SetScrollY(window, state, state->scrollY - ((delta / WHEEL_DELTA) * 42))) {
                InvalidateRect(window, nullptr, FALSE);
            }
        }
        return 0;
    case WM_MOUSEMOVE: {
        POINT point{GET_X_LPARAM(lParam), GET_Y_LPARAM(lParam)};
        if (state && state->draggingThumb) {
            RECT client{};
            GetClientRect(window, &client);
            const RECT textRect = MessageTextRect(client);
            const RECT thumb = ScrollbarThumb(state, client);
            const int maxScroll = MaxScrollY(state, client);
            const int trackTravel = std::max(1, VisibleTextHeight(textRect) - static_cast<int>(thumb.bottom - thumb.top) - 4);
            const int dy = point.y - state->dragStartY;
            if (SetScrollY(window, state, state->dragStartScrollY + (dy * maxScroll) / trackTravel)) {
                InvalidateRect(window, nullptr, FALSE);
            }
            return 0;
        }
        DialogButton* hit = HitButton(state, point);
        const UINT hot = hit ? hit->id : 0;
        if (state && hot != state->hotButton) {
            state->hotButton = hot;
            InvalidateRect(window, nullptr, FALSE);
            TRACKMOUSEEVENT event{sizeof(event), TME_LEAVE, window, 0};
            TrackMouseEvent(&event);
        }
        return 0;
    }
    case WM_MOUSELEAVE:
        if (state) {
            state->hotButton = 0;
            InvalidateRect(window, nullptr, FALSE);
        }
        return 0;
    case WM_LBUTTONDOWN: {
        POINT point{GET_X_LPARAM(lParam), GET_Y_LPARAM(lParam)};
        if (state) {
            RECT client{};
            GetClientRect(window, &client);
            const RECT thumb = ScrollbarThumb(state, client);
            if (state->contentHeight > VisibleTextHeight(MessageTextRect(client)) && PtInRect(&thumb, point)) {
                state->draggingThumb = true;
                state->dragStartY = point.y;
                state->dragStartScrollY = state->scrollY;
                SetCapture(window);
                return 0;
            }
        }
        if (DialogButton* hit = HitButton(state, point)) {
            state->pressedButton = hit->id;
            SetCapture(window);
            InvalidateRect(window, nullptr, FALSE);
        }
        return 0;
    }
    case WM_LBUTTONUP: {
        POINT point{GET_X_LPARAM(lParam), GET_Y_LPARAM(lParam)};
        const UINT pressed = state ? state->pressedButton : 0;
        if (state) {
            if (state->draggingThumb) {
                state->draggingThumb = false;
                ReleaseCapture();
                return 0;
            }
            state->pressedButton = 0;
        }
        ReleaseCapture();
        InvalidateRect(window, nullptr, FALSE);
        if (DialogButton* hit = HitButton(state, point); hit && hit->id == pressed) {
            if (hit->id == kCopy && state) {
                CopyTextToClipboard(window, state->message);
            } else {
                DestroyWindow(window);
            }
        }
        return 0;
    }
    case WM_KEYDOWN:
        if (wParam == VK_ESCAPE || wParam == VK_RETURN) {
            DestroyWindow(window);
            return 0;
        }
        break;
    case WM_PAINT: {
        PAINTSTRUCT ps{};
        HDC dc = BeginPaint(window, &ps);
        RECT client{};
        GetClientRect(window, &client);
        HDC memoryDc = CreateCompatibleDC(dc);
        HBITMAP bitmap = CreateCompatibleBitmap(dc, std::max(1L, client.right - client.left), std::max(1L, client.bottom - client.top));
        HGDIOBJ oldBitmap = SelectObject(memoryDc, bitmap);
        PaintDialog(window, state, memoryDc);
        BitBlt(dc, 0, 0, client.right - client.left, client.bottom - client.top, memoryDc, 0, 0, SRCCOPY);
        SelectObject(memoryDc, oldBitmap);
        DeleteObject(bitmap);
        DeleteDC(memoryDc);
        EndPaint(window, &ps);
        return 0;
    }
    case WM_CLOSE:
        DestroyWindow(window);
        return 0;
    }
    return DefWindowProcW(window, message, wParam, lParam);
}

} // namespace

void ShowCustomMessageDialog(
    HWND owner,
    HINSTANCE instance,
    const std::wstring& title,
    const std::wstring& message,
    MessageDialogKind kind
) {
    if (!instance && owner) {
        instance = reinterpret_cast<HINSTANCE>(GetWindowLongPtrW(owner, GWLP_HINSTANCE));
    }
    AppendDialogLog(title, message, kind);

    WNDCLASSW wc = {};
    wc.lpfnWndProc = MessageDialogProc;
    wc.hInstance = instance;
    wc.hCursor = LoadCursorW(nullptr, IDC_ARROW);
    wc.hbrBackground = CreateSolidBrush(RGB(18, 18, 20));
    wc.lpszClassName = L"BoostyMessageDialogWindow";
    RegisterClassW(&wc);

    DialogState state{};
    state.title = title;
    state.message = message;
    state.kind = kind;

    RECT ownerRect{};
    if (owner) {
        GetWindowRect(owner, &ownerRect);
    } else {
        ownerRect = {0, 0, GetSystemMetrics(SM_CXSCREEN), GetSystemMetrics(SM_CYSCREEN)};
    }
    constexpr int width = 540;
    constexpr int height = 236;
    HWND dialog = CreateWindowExW(
        WS_EX_DLGMODALFRAME,
        wc.lpszClassName,
        title.c_str(),
        WS_POPUP | WS_CAPTION | WS_SYSMENU,
        ownerRect.left + ((ownerRect.right - ownerRect.left) - width) / 2,
        ownerRect.top + ((ownerRect.bottom - ownerRect.top) - height) / 2,
        width,
        height,
        owner,
        nullptr,
        instance,
        &state
    );
    if (!dialog) {
        return;
    }

    EnableDarkTitleBar(dialog);
    if (owner) {
        EnableWindow(owner, FALSE);
    }
    ShowWindow(dialog, SW_SHOW);

    MSG msg{};
    while (IsWindow(dialog) && GetMessageW(&msg, nullptr, 0, 0) > 0) {
        if (!IsDialogMessageW(dialog, &msg)) {
            TranslateMessage(&msg);
            DispatchMessageW(&msg);
        }
    }
    if (owner) {
        EnableWindow(owner, TRUE);
        SetActiveWindow(owner);
    }
}
