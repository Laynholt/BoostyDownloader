#include "Application.h"

#include "AppUpdateService.h"
#include "AppVersion.h"
#include "DownloadQueueStore.h"
#include "FfmpegTools.h"
#include "LoginDialog.h"
#include "MessageDialog.h"
#include "TaskFormatting.h"
#include "Text.h"
#include "UiHelpers.h"
#include "UiRenderer.h"

#include <commdlg.h>
#include <commctrl.h>
#include <gdiplus.h>
#include <shellapi.h>
#include <shlobj.h>
#include <shobjidl.h>
#include <windowsx.h>

#include <algorithm>
#include <fstream>
#include <functional>
#include <mutex>
#include <sstream>
#include <thread>

namespace {

constexpr UINT_PTR kRefreshTimer = 1;
constexpr UINT kBtnDownload = 1;
constexpr UINT kBtnPaste = 2;
constexpr UINT kBtnLogin = 3;
constexpr UINT kBtnBrowse = 4;
constexpr UINT kBtnClear = 5;
constexpr UINT kBtnLogs = 6;
constexpr UINT kBtnSettings = 7;
constexpr UINT kBtnOpenFolder = 8;
constexpr UINT kBtnClearFinished = 9;
constexpr UINT kTaskCancel = 201;
constexpr UINT kTaskRetry = 202;
constexpr UINT kTaskDelete = 203;
constexpr UINT kTaskClose = 204;
constexpr int kAppIcon = 1;
constexpr int kQueueHeaderTop = 264;
constexpr int kQueuePanelTop = 312;
constexpr int kQueuePanelBottomInset = 44;
constexpr int kQueuePanelTopPadding = 12;
constexpr int kQueuePanelBottomPadding = 12;
constexpr int kQueueRowHeight = 92;
constexpr int kQueueRowGap = 10;
constexpr const wchar_t* kEditContextMenuClassName = L"BoostyEditContextMenu";
constexpr UINT kEditUndo = 1001;
constexpr UINT kEditCut = 1002;
constexpr UINT kEditCopy = 1003;
constexpr UINT kEditPaste = 1004;
constexpr UINT kEditDelete = 1005;
constexpr UINT kEditSelectAll = 1006;
constexpr int kMenuItemHeight = 34;
constexpr int kMenuSeparatorHeight = 10;

struct EditContextMenuItem {
    UINT id = 0;
    std::wstring text;
    bool separator = false;
    bool enabled = true;
};

struct EditContextMenuState {
    HWND owner = nullptr;
    HWND edit = nullptr;
    std::vector<EditContextMenuItem> items;
    UINT hotItemId = 0;
};

struct EditSubclassState {
    HWND owner = nullptr;
    HINSTANCE instance = nullptr;
    bool selectLineOnContext = false;
    UINT pasteCommand = 0;
    UINT enterCommand = 0;
};

std::wstring GetText(HWND window) {
    const int length = GetWindowTextLengthW(window);
    std::wstring text(length, L'\0');
    GetWindowTextW(window, text.data(), length + 1);
    return text;
}

void SetFont(HWND window) {
    static HFONT font = CreateFontW(-16, 0, 0, 0, FW_NORMAL, FALSE, FALSE, FALSE, DEFAULT_CHARSET, OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS, CLEARTYPE_QUALITY, DEFAULT_PITCH, L"Segoe UI");
    SendMessageW(window, WM_SETFONT, reinterpret_cast<WPARAM>(font), TRUE);
}

UINT TooltipInfoSize() {
#ifdef TTTOOLINFOW_V2_SIZE
    return TTTOOLINFOW_V2_SIZE;
#else
    return sizeof(TOOLINFOW);
#endif
}

std::wstring StateText(DownloadTaskState state) {
    switch (state) {
    case DownloadTaskState::Queued: return L"В очереди";
    case DownloadTaskState::Preparing: return L"Подготовка";
    case DownloadTaskState::Downloading: return L"Скачивание";
    case DownloadTaskState::Completed: return L"Готово";
    case DownloadTaskState::Failed: return L"Ошибка";
    case DownloadTaskState::Canceled: return L"Отменено";
    }
    return {};
}

RECT QueuePanelRectForClient(const RECT& client) {
    return {20, kQueuePanelTop, client.right - 20, client.bottom - kQueuePanelBottomInset};
}

RECT QueueRowRectAt(const RECT& queuePanel, int visibleIndex) {
    const int y = queuePanel.top + kQueuePanelTopPadding + (visibleIndex * (kQueueRowHeight + kQueueRowGap));
    return {queuePanel.left + 14, y, queuePanel.right - 32, y + kQueueRowHeight};
}

int QueueVisibleRowCount(const RECT& queuePanel) {
    const int availableHeight = std::max(0, static_cast<int>(queuePanel.bottom - kQueuePanelBottomPadding - (queuePanel.top + kQueuePanelTopPadding)));
    return std::max(1, (availableHeight + kQueueRowGap) / (kQueueRowHeight + kQueueRowGap));
}

int QueueMaxScrollOffset(const RECT& queuePanel, size_t taskCount) {
    return std::max(0, static_cast<int>(taskCount) - QueueVisibleRowCount(queuePanel));
}

RECT QueueScrollbarTrackRect(const RECT& queuePanel) {
    return {queuePanel.right - 20, queuePanel.top + kQueuePanelTopPadding, queuePanel.right - 12, queuePanel.bottom - kQueuePanelBottomPadding};
}

RECT QueueScrollbarThumbRect(const RECT& queuePanel, size_t taskCount, int scrollOffset) {
    const RECT track = QueueScrollbarTrackRect(queuePanel);
    const int trackHeight = std::max(1, static_cast<int>(track.bottom - track.top));
    const int visibleRows = QueueVisibleRowCount(queuePanel);
    const int totalRows = std::max(1, static_cast<int>(taskCount));
    const int thumbHeight = std::clamp((trackHeight * visibleRows) / totalRows, 28, trackHeight);
    const int maxOffset = std::max(1, totalRows - visibleRows);
    const int maxTravel = std::max(0, trackHeight - thumbHeight);
    const int thumbTop = track.top + (std::clamp(scrollOffset, 0, maxOffset) * maxTravel) / maxOffset;
    return {track.left, thumbTop, track.right, thumbTop + thumbHeight};
}

void DrawQueueScrollbar(HDC dc, const RECT& queuePanel, size_t taskCount, int scrollOffset) {
    if (QueueMaxScrollOffset(queuePanel, taskCount) <= 0) {
        return;
    }
    const RECT track = QueueScrollbarTrackRect(queuePanel);
    const RECT thumb = QueueScrollbarThumbRect(queuePanel, taskCount, scrollOffset);
    HBRUSH trackBrush = CreateSolidBrush(RGB(30, 30, 34));
    HBRUSH thumbBrush = CreateSolidBrush(RGB(76, 76, 84));
    HPEN borderPen = CreatePen(PS_SOLID, 1, RGB(52, 52, 58));
    HGDIOBJ oldBrush = SelectObject(dc, trackBrush);
    HGDIOBJ oldPen = SelectObject(dc, borderPen);
    RoundRect(dc, track.left, track.top, track.right, track.bottom, 8, 8);
    SelectObject(dc, thumbBrush);
    RoundRect(dc, thumb.left, thumb.top, thumb.right, thumb.bottom, 8, 8);
    SelectObject(dc, oldPen);
    SelectObject(dc, oldBrush);
    DeleteObject(borderPen);
    DeleteObject(trackBrush);
    DeleteObject(thumbBrush);
}

void DrawTextLine(HDC dc, const std::wstring& text, RECT rect, int size, COLORREF color, UINT format = DT_LEFT | DT_VCENTER | DT_SINGLELINE | DT_END_ELLIPSIS) {
    SetBkMode(dc, TRANSPARENT);
    SetTextColor(dc, color);
    HFONT font = CreateFontW(-size, 0, 0, 0, FW_NORMAL, FALSE, FALSE, FALSE, DEFAULT_CHARSET, OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS, CLEARTYPE_QUALITY, DEFAULT_PITCH, L"Segoe UI");
    HGDIOBJ old = SelectObject(dc, font);
    DrawTextW(dc, text.c_str(), -1, &rect, format);
    SelectObject(dc, old);
    DeleteObject(font);
}

void DrawTextBlock(HDC dc, const std::wstring& text, RECT rect, COLORREF color, HFONT font, UINT format) {
    HFONT oldFont = reinterpret_cast<HFONT>(SelectObject(dc, font));
    SetBkMode(dc, TRANSPARENT);
    SetTextColor(dc, color);
    DrawTextW(dc, text.c_str(), -1, &rect, format | DT_NOPREFIX);
    SelectObject(dc, oldFont);
}

void PaintBuffered(HWND window, const std::function<void(HDC, const RECT&)>& paintContent) {
    PAINTSTRUCT paint = {};
    HDC screenDc = BeginPaint(window, &paint);

    RECT client = {};
    GetClientRect(window, &client);
    const int width = std::max(1, static_cast<int>(client.right - client.left));
    const int height = std::max(1, static_cast<int>(client.bottom - client.top));

    HDC bufferDc = CreateCompatibleDC(screenDc);
    HBITMAP bitmap = CreateCompatibleBitmap(screenDc, width, height);
    HGDIOBJ oldBitmap = SelectObject(bufferDc, bitmap);
    HBRUSH background = CreateSolidBrush(RGB(28, 28, 31));
    FillRect(bufferDc, &client, background);
    DeleteObject(background);

    paintContent(bufferDc, client);
    BitBlt(
        screenDc,
        paint.rcPaint.left,
        paint.rcPaint.top,
        paint.rcPaint.right - paint.rcPaint.left,
        paint.rcPaint.bottom - paint.rcPaint.top,
        bufferDc,
        paint.rcPaint.left,
        paint.rcPaint.top,
        SRCCOPY
    );

    SelectObject(bufferDc, oldBitmap);
    DeleteObject(bitmap);
    DeleteDC(bufferDc);
    EndPaint(window, &paint);
}

const wchar_t* ButtonTooltip(UINT id) {
    switch (id) {
    case kBtnDownload: return L"Добавить введенные ссылки в очередь загрузок.";
    case kBtnPaste: return L"Вставить ссылку из буфера обмена в поле URL.";
    case kBtnLogin: return L"Открыть Boosty и сохранить cookies/токен для платных постов.";
    case kBtnBrowse: return L"Выбрать папку, куда будут сохраняться видео.";
    case kBtnClear: return L"Очистить все неактивные задачи из очереди.";
    case kBtnLogs: return L"Показать журнал работы приложения.";
    case kBtnSettings: return L"Открыть настройки качества и параллельных загрузок.";
    case kBtnOpenFolder: return L"Открыть текущую папку загрузки.";
    case kBtnClearFinished: return L"Убрать завершенные, отмененные и ошибочные задачи из очереди.";
    default: return L"";
    }
}

std::vector<EditContextMenuItem> BuildEditContextMenuItems(bool canUndo, bool hasSelection, bool canPaste, bool hasText) {
    return {
        {kEditUndo, L"Отменить", false, canUndo},
        {0, {}, true, false},
        {kEditCut, L"Вырезать", false, hasSelection},
        {kEditCopy, L"Копировать", false, hasSelection},
        {kEditPaste, L"Вставить", false, canPaste},
        {kEditDelete, L"Удалить", false, hasSelection},
        {0, {}, true, false},
        {kEditSelectAll, L"Выделить всё", false, hasText}
    };
}

int EditContextMenuHeight(const std::vector<EditContextMenuItem>& items) {
    int height = 4;
    for (const auto& item : items) {
        height += item.separator ? kMenuSeparatorHeight : kMenuItemHeight;
    }
    return height + 4;
}

UINT HitTestEditContextMenuItem(const std::vector<EditContextMenuItem>& items, int y) {
    int top = 2;
    for (const auto& item : items) {
        const int height = item.separator ? kMenuSeparatorHeight : kMenuItemHeight;
        if (!item.separator && y >= top && y < top + height) {
            return item.enabled ? item.id : 0;
        }
        top += height;
    }
    return 0;
}

void ExecuteEditContextCommand(HWND edit, UINT commandId) {
    if (!IsWindow(edit)) {
        return;
    }
    switch (commandId) {
    case kEditUndo: SendMessageW(edit, EM_UNDO, 0, 0); break;
    case kEditCut: SendMessageW(edit, WM_CUT, 0, 0); break;
    case kEditCopy: SendMessageW(edit, WM_COPY, 0, 0); break;
    case kEditPaste: SendMessageW(edit, WM_PASTE, 0, 0); break;
    case kEditDelete: SendMessageW(edit, WM_CLEAR, 0, 0); break;
    case kEditSelectAll: SendMessageW(edit, EM_SETSEL, 0, -1); break;
    }
}

LRESULT CALLBACK EditContextMenuProc(HWND window, UINT message, WPARAM wParam, LPARAM lParam) {
    auto* state = reinterpret_cast<EditContextMenuState*>(GetWindowLongPtrW(window, GWLP_USERDATA));
    if (message == WM_NCCREATE) {
        const auto* create = reinterpret_cast<CREATESTRUCTW*>(lParam);
        state = static_cast<EditContextMenuState*>(create->lpCreateParams);
        SetWindowLongPtrW(window, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(state));
        return TRUE;
    }
    switch (message) {
    case WM_ERASEBKGND:
        return 1;
    case WM_MOUSEMOVE:
        if (state) {
            const UINT hot = HitTestEditContextMenuItem(state->items, GET_Y_LPARAM(lParam));
            if (hot != state->hotItemId) {
                state->hotItemId = hot;
                InvalidateRect(window, nullptr, FALSE);
            }
            TRACKMOUSEEVENT track{sizeof(track), TME_LEAVE, window, 0};
            TrackMouseEvent(&track);
        }
        return 0;
    case WM_MOUSELEAVE:
        if (state && state->hotItemId != 0) {
            state->hotItemId = 0;
            InvalidateRect(window, nullptr, FALSE);
        }
        return 0;
    case WM_PAINT: {
        PAINTSTRUCT ps{};
        HDC dc = BeginPaint(window, &ps);
        RECT client{};
        GetClientRect(window, &client);
        std::vector<UiRenderer::PopupMenuItem> items;
        if (state) {
            for (const auto& item : state->items) {
                items.push_back({item.id, item.text, item.separator, item.enabled});
            }
        }
        UiRenderer::DrawPopupMenu(dc, client, items, state ? state->hotItemId : 0);
        EndPaint(window, &ps);
        return 0;
    }
    case WM_LBUTTONUP:
        if (state) {
            const UINT commandId = HitTestEditContextMenuItem(state->items, GET_Y_LPARAM(lParam));
            if (IsWindow(state->edit)) {
                SetFocus(state->edit);
            }
            ExecuteEditContextCommand(state->edit, commandId);
        }
        CloseDialogWindow(window);
        return 0;
    case WM_KEYDOWN:
        if (wParam == VK_ESCAPE) {
            CloseDialogWindow(window);
            return 0;
        }
        break;
    case WM_KILLFOCUS:
        CloseDialogWindow(window);
        return 0;
    case WM_NCDESTROY:
        delete state;
        SetWindowLongPtrW(window, GWLP_USERDATA, 0);
        return 0;
    }
    return DefWindowProcW(window, message, wParam, lParam);
}

void ShowEditContextMenu(HWND owner, HINSTANCE instance, HWND edit, POINT screenPoint) {
    if (!IsWindow(edit)) {
        return;
    }
    DWORD selection = static_cast<DWORD>(SendMessageW(edit, EM_GETSEL, 0, 0));
    auto* state = new EditContextMenuState{};
    state->owner = owner;
    state->edit = edit;
    state->items = BuildEditContextMenuItems(
        SendMessageW(edit, EM_CANUNDO, 0, 0) != 0,
        LOWORD(selection) != HIWORD(selection),
        IsClipboardFormatAvailable(CF_UNICODETEXT) != 0,
        GetWindowTextLengthW(edit) > 0
    );

    constexpr int menuWidth = 180;
    const int menuHeight = EditContextMenuHeight(state->items);
    HWND menu = CreateWindowExW(WS_EX_TOPMOST | WS_EX_TOOLWINDOW, kEditContextMenuClassName, L"", WS_POPUP, screenPoint.x, screenPoint.y, menuWidth, menuHeight, owner, nullptr, instance, state);
    if (!menu) {
        delete state;
        return;
    }
    ShowWindow(menu, SW_SHOW);
    SetFocus(menu);
}

LRESULT CALLBACK EditControlSubclassProc(HWND window, UINT message, WPARAM wParam, LPARAM lParam, UINT_PTR subclassId, DWORD_PTR refData) {
    UNREFERENCED_PARAMETER(subclassId);
    auto* state = reinterpret_cast<EditSubclassState*>(refData);
    if (state && message == WM_KEYDOWN) {
        const bool controlDown = (GetKeyState(VK_CONTROL) & 0x8000) != 0;
        if (controlDown && wParam == 'V' && state->pasteCommand) {
            SendMessageW(state->owner, WM_COMMAND, state->pasteCommand, 0);
            return 0;
        }
        if (wParam == VK_RETURN && state->enterCommand) {
            SendMessageW(state->owner, WM_COMMAND, state->enterCommand, 0);
            return 0;
        }
    }
    if (state && message == WM_PASTE && state->pasteCommand) {
        SendMessageW(state->owner, WM_COMMAND, state->pasteCommand, 0);
        return 0;
    }
    if (message == WM_CONTEXTMENU && state) {
        SetFocus(window);
        POINT point{GET_X_LPARAM(lParam), GET_Y_LPARAM(lParam)};
        if (point.x == -1 && point.y == -1) {
            RECT rect{};
            GetWindowRect(window, &rect);
            point = {rect.left + 16, rect.top + ((rect.bottom - rect.top) / 2)};
        } else if (state->selectLineOnContext) {
            POINT clientPoint = point;
            ScreenToClient(window, &clientPoint);
            const LRESULT charResult = SendMessageW(window, EM_CHARFROMPOS, 0, MAKELPARAM(clientPoint.x, clientPoint.y));
            const int charIndex = LOWORD(charResult);
            const int line = static_cast<int>(SendMessageW(window, EM_LINEFROMCHAR, charIndex, 0));
            const int lineStart = static_cast<int>(SendMessageW(window, EM_LINEINDEX, line, 0));
            const int lineLength = static_cast<int>(SendMessageW(window, EM_LINELENGTH, lineStart, 0));
            if (lineStart >= 0) {
                SendMessageW(window, EM_SETSEL, lineStart, lineStart + lineLength);
            }
        }
        ShowEditContextMenu(state->owner, state->instance, window, point);
        return 0;
    }
    if (message == WM_NCDESTROY) {
        LRESULT result = DefSubclassProc(window, message, wParam, lParam);
        RemoveWindowSubclass(window, EditControlSubclassProc, 1);
        delete state;
        return result;
    }
    return DefSubclassProc(window, message, wParam, lParam);
}

void InstallCustomEditContextMenu(HWND edit, HWND owner, HINSTANCE instance, bool selectLineOnContext = false, UINT pasteCommand = 0, UINT enterCommand = 0) {
    if (!IsWindow(edit)) {
        return;
    }
    auto* state = new EditSubclassState{owner, instance, selectLineOnContext, pasteCommand, enterCommand};
    if (!SetWindowSubclass(edit, EditControlSubclassProc, 1, reinterpret_cast<DWORD_PTR>(state))) {
        delete state;
    }
}

void RegisterPopupMenuClasses(HINSTANCE instance) {
    WNDCLASSW wc{};
    wc.lpfnWndProc = EditContextMenuProc;
    wc.hInstance = instance;
    wc.hCursor = LoadCursorW(nullptr, IDC_ARROW);
    wc.lpszClassName = kEditContextMenuClassName;
    RegisterClassW(&wc);
}

constexpr UINT kSettingsQuality = 101;
constexpr UINT kSettingsWorkers = 102;
constexpr UINT kSettingsDownloads = 103;
constexpr UINT kSettingsTools = 104;
constexpr UINT kSettingsAbout = 105;
constexpr UINT kSettingsCancel = 106;
constexpr UINT kSettingsSave = 107;
constexpr UINT kSettingsTranscript = 108;
constexpr UINT kSettingsTranslate = 109;
constexpr UINT kSettingsBack = 110;
constexpr UINT kQualityAudio = 120;
constexpr UINT kQuality360 = 121;
constexpr UINT kQuality480 = 122;
constexpr UINT kQuality720 = 123;
constexpr UINT kQuality1080 = 124;
constexpr UINT kQualityMax = 125;
constexpr UINT kContainerAuto = 130;
constexpr UINT kContainerMp4 = 131;
constexpr UINT kContainerMkv = 132;
constexpr UINT kContainerWebm = 133;
constexpr UINT kWorkersMinus = 140;
constexpr UINT kWorkersPlus = 141;
constexpr UINT kAutoCheck = 142;
constexpr UINT kFfmpegConfigure = 143;
constexpr UINT kFfmpegDetails = 144;
constexpr UINT kSettingsCheckUpdates = 145;
constexpr UINT kLogCopyAll = 201;
constexpr UINT kLogClose = 202;
constexpr UINT kDialogInstall = 301;
constexpr UINT kDialogChoose = 302;
constexpr UINT kDialogClose = 303;
constexpr UINT kDialogDone = WM_APP + 30;
constexpr UINT kProgressUpdateMessage = WM_APP + 31;
constexpr UINT kProgressDoneMessage = WM_APP + 32;
constexpr UINT kUpdateFoundMessage = WM_APP + 80;
constexpr int kFfmpegDialogWidth = 620;
constexpr int kFfmpegDialogHeight = 370;
constexpr const wchar_t* kAppUserModelId = L"Laynholt.BoostyDownloader";
constexpr int kFolderRowOffset = 6;
constexpr const wchar_t* kLogViewClassName = L"BoostyLogView";
constexpr const wchar_t* kLogCopyMenuClassName = L"BoostyLogCopyMenu";
constexpr int kScrollTextTopPadding = 12;
constexpr int kScrollTextBottomPadding = 12;
constexpr int kScrollTextClipTopPadding = 8;
constexpr int kScrollTextClipBottomPadding = 8;
constexpr int kSettingsWindowWidth = 980;
constexpr int kSettingsWindowHeight = 760;
constexpr int kSettingsSidebarExpandedWidth = 214;
constexpr int kSettingsSidebarCollapsedWidth = 70;
constexpr int kSettingsSidebarCollapseWidth = 820;
constexpr int kSettingsSidebarGap = 18;

struct DialogButton {
    UINT id = 0;
    RECT rect{};
    std::wstring text;
    bool primary = false;
    bool enabled = true;
    bool onPanel = true;
    bool onCard = false;
};

enum class SettingsSection {
    Downloads,
    Tools,
    About
};

struct SettingsState {
    AppConfig config;
    const AppPaths* paths = nullptr;
    bool saved = false;
    bool ffmpegAvailable = false;
    bool ffmpegDetailsExpanded = false;
    bool sidebarCollapsed = false;
    SettingsSection section = SettingsSection::Downloads;
    std::vector<DialogButton> buttons;
    UINT hotButton = 0;
    UINT pressedButton = 0;
};

struct LogLineLayout {
    RECT rect = {};
};

struct LogViewState {
    std::wstring text;
    std::vector<std::wstring> lines;
    std::vector<LogLineLayout> layouts;
    int scrollY = 0;
    int contentHeight = 0;
    int selectedLine = -1;
    bool draggingThumb = false;
    int dragStartY = 0;
    int dragStartScrollY = 0;
};

struct LogCopyMenuState {
    HWND owner = nullptr;
    std::wstring text;
    bool hot = false;
};

struct LogsDialogState {
    std::wstring text;
    HWND logView = nullptr;
    std::vector<DialogButton> buttons;
    UINT hotButton = 0;
    UINT pressedButton = 0;
};

struct FfmpegDialogState {
    AppConfig* config = nullptr;
    const AppPaths* paths = nullptr;
    std::wstring title;
    std::wstring message;
    std::wstring status;
    bool saved = false;
    bool installing = false;
    bool installOk = false;
    std::vector<DialogButton> buttons;
    UINT hotButton = 0;
    UINT pressedButton = 0;
    std::jthread worker;
};

struct FfmpegInstallDialogState {
    AppConfig* config = nullptr;
    const AppPaths* paths = nullptr;
    std::wstring title = L"Установка FFmpeg";
    std::wstring message = L"Подготовка...";
    std::wstring error;
    std::uint64_t downloaded = 0;
    std::uint64_t total = 0;
    bool done = false;
    bool success = false;
    std::vector<DialogButton> buttons;
    UINT hotButton = 0;
    UINT pressedButton = 0;
    HANDLE cancelEvent = nullptr;
    std::jthread worker;
    std::mutex mutex;
};

struct UpdatePromptState {
    std::wstring message;
    bool accepted = false;
    std::vector<DialogButton> buttons;
    UINT hotButton = 0;
    UINT pressedButton = 0;
};

DialogButton* HitDialogButton(std::vector<DialogButton>& buttons, POINT point) {
    for (DialogButton& button : buttons) {
        if (button.enabled && PtInRect(&button.rect, point)) {
            return &button;
        }
    }
    return nullptr;
}

void InvalidateDialogButton(HWND window, const std::vector<DialogButton>& buttons, UINT id) {
    if (!id) {
        return;
    }
    for (const DialogButton& button : buttons) {
        if (button.id == id) {
            RECT rect = button.rect;
            InflateRect(&rect, 2, 2);
            InvalidateRect(window, &rect, FALSE);
            return;
        }
    }
}

void DrawDialogButton(HDC dc, const DialogButton& button, UINT pressed, UINT hot) {
    UiRenderer::DrawButton(
        dc,
        button.rect,
        button.text.c_str(),
        button.primary,
        pressed == button.id,
        hot == button.id,
        button.onPanel,
        button.enabled,
        button.onCard
    );
}

void DrawDialogButtons(HDC dc, const std::vector<DialogButton>& buttons, UINT pressed, UINT hot) {
    for (const DialogButton& button : buttons) {
        DrawDialogButton(dc, button, pressed, hot);
    }
}

void AddSettingsRoundedRect(Gdiplus::GraphicsPath& path, const RECT& rect, int radius) {
    const int diameter = radius * 2;
    path.AddArc(rect.left, rect.top, diameter, diameter, 180.0f, 90.0f);
    path.AddArc(rect.right - diameter, rect.top, diameter, diameter, 270.0f, 90.0f);
    path.AddArc(rect.right - diameter, rect.bottom - diameter, diameter, diameter, 0.0f, 90.0f);
    path.AddArc(rect.left, rect.bottom - diameter, diameter, diameter, 90.0f, 90.0f);
    path.CloseFigure();
}

void DrawSettingsRoundedPanel(HDC dc, const RECT& rect, Gdiplus::Color fillColor, Gdiplus::Color borderColor, int radius = 8) {
    Gdiplus::Graphics graphics(dc);
    graphics.SetSmoothingMode(Gdiplus::SmoothingModeHighQuality);
    Gdiplus::GraphicsPath path;
    AddSettingsRoundedRect(path, rect, radius);
    Gdiplus::SolidBrush fill(fillColor);
    Gdiplus::Pen border(borderColor, 1.0f);
    graphics.FillPath(&fill, &path);
    graphics.DrawPath(&border, &path);
}

void DrawSettingsButton(HDC dc, const DialogButton& button, UINT pressed, UINT hot) {
    Gdiplus::Graphics graphics(dc);
    graphics.SetSmoothingMode(Gdiplus::SmoothingModeHighQuality);
    graphics.SetTextRenderingHint(Gdiplus::TextRenderingHintClearTypeGridFit);

    RECT inset = {button.rect.left + 1, button.rect.top + 1, button.rect.right - 1, button.rect.bottom - 1};
    Gdiplus::GraphicsPath path;
    AddSettingsRoundedRect(path, inset, 7);

    const bool isPressed = pressed == button.id;
    const bool isHot = hot == button.id;
    const Gdiplus::Color fillColor = !button.enabled
        ? Gdiplus::Color(255, 36, 36, 39)
        : button.primary
        ? (isPressed ? Gdiplus::Color(255, 197, 45, 59) : (isHot ? Gdiplus::Color(255, 242, 88, 101) : Gdiplus::Color(255, 232, 72, 85)))
        : (isPressed ? Gdiplus::Color(255, 33, 33, 37) : (isHot ? Gdiplus::Color(255, 50, 50, 55) : Gdiplus::Color(255, 42, 42, 46)));
    Gdiplus::SolidBrush fill(fillColor);
    Gdiplus::Pen border((button.enabled && button.primary) ? fillColor : Gdiplus::Color(255, 58, 58, 64), 1.0f);
    graphics.FillPath(&fill, &path);
    graphics.DrawPath(&border, &path);

    Gdiplus::FontFamily family(L"Segoe UI");
    Gdiplus::Font font(&family, 10.0f, Gdiplus::FontStyleRegular, Gdiplus::UnitPoint);
    Gdiplus::SolidBrush textBrush(button.enabled ? Gdiplus::Color(255, 242, 242, 242) : Gdiplus::Color(255, 136, 136, 142));
    Gdiplus::RectF textRect(
        static_cast<Gdiplus::REAL>(button.rect.left),
        static_cast<Gdiplus::REAL>(button.rect.top),
        static_cast<Gdiplus::REAL>(button.rect.right - button.rect.left),
        static_cast<Gdiplus::REAL>(button.rect.bottom - button.rect.top)
    );
    Gdiplus::StringFormat format;
    format.SetAlignment(Gdiplus::StringAlignmentCenter);
    format.SetLineAlignment(Gdiplus::StringAlignmentCenter);
    graphics.DrawString(button.text.c_str(), -1, &font, textRect, &format, &textBrush);
}

void DrawSettingsButtons(HDC dc, const std::vector<DialogButton>& buttons, UINT pressed, UINT hot) {
    for (const DialogButton& button : buttons) {
        DrawSettingsButton(dc, button, pressed, hot);
    }
}

void DrawStatusPill(HDC dc, RECT rect, const std::wstring& text, bool ok) {
    DrawSettingsRoundedPanel(
        dc,
        rect,
        ok ? Gdiplus::Color(255, 28, 53, 39) : Gdiplus::Color(255, 56, 37, 41),
        ok ? Gdiplus::Color(255, 70, 124, 91) : Gdiplus::Color(255, 122, 58, 66),
        8
    );
    DrawTextLine(dc, text, rect, 13, ok ? RGB(171, 230, 193) : RGB(255, 190, 198), DT_CENTER | DT_VCENTER | DT_SINGLELINE);
}

void DrawTaskThumbnail(HDC dc, const RECT& rect, const std::filesystem::path& path) {
    DrawSettingsRoundedPanel(dc, rect, Gdiplus::Color(255, 31, 31, 34), Gdiplus::Color(255, 58, 58, 64), 6);
    std::error_code ec;
    if (path.empty() || !std::filesystem::is_regular_file(path, ec)) {
        return;
    }

    Gdiplus::Image image(path.c_str());
    if (image.GetLastStatus() != Gdiplus::Ok || image.GetWidth() == 0 || image.GetHeight() == 0) {
        return;
    }

    Gdiplus::Graphics graphics(dc);
    graphics.SetInterpolationMode(Gdiplus::InterpolationModeHighQualityBicubic);
    const Gdiplus::REAL destW = static_cast<Gdiplus::REAL>(rect.right - rect.left);
    const Gdiplus::REAL destH = static_cast<Gdiplus::REAL>(rect.bottom - rect.top);
    const Gdiplus::REAL srcW = static_cast<Gdiplus::REAL>(image.GetWidth());
    const Gdiplus::REAL srcH = static_cast<Gdiplus::REAL>(image.GetHeight());
    const Gdiplus::REAL scale = std::max(destW / srcW, destH / srcH);
    const Gdiplus::REAL cropW = destW / scale;
    const Gdiplus::REAL cropH = destH / scale;
    const Gdiplus::REAL cropX = (srcW - cropW) / 2.0f;
    const Gdiplus::REAL cropY = (srcH - cropH) / 2.0f;
    const Gdiplus::RectF dest(
        static_cast<Gdiplus::REAL>(rect.left),
        static_cast<Gdiplus::REAL>(rect.top),
        destW,
        destH
    );
    graphics.DrawImage(&image, dest, cropX, cropY, cropW, cropH, Gdiplus::UnitPixel);
}

void RefreshFfmpegDialogText(FfmpegDialogState* state) {
    if (!state || !state->config) {
        return;
    }
    FfmpegStatus status;
    if (state->paths) {
        status = ResolveFfmpeg(*state->paths, state->config->ffmpegPath);
    }
    state->title = status.available ? L"FFmpeg указан" : L"FFmpeg не найден";
    state->message = status.available
        ? L"FFmpeg найден и будет использоваться для объединения видео/аудио дорожек и переконвертации.\n\nПуть:\n" + status.executable.wstring()
        : L"FFmpeg не найден. Установите его автоматически или выберите папку с ffmpeg.exe.";
    if (!status.version.empty()) {
        state->message += L"\n\nВерсия: " + status.version;
    }
}

void LayoutFfmpegDialog(FfmpegDialogState* state, const RECT& client) {
    if (!state) {
        return;
    }
    constexpr int panelInset = 16;
    constexpr int buttonInset = 16;
    constexpr int buttonHeight = 42;
    constexpr int buttonGap = 16;
    const RECT panel{panelInset, panelInset, client.right - panelInset, client.bottom - panelInset};
    const int bottom = panel.bottom - buttonInset;
    if (state->config) {
        const int left = panel.left + buttonInset;
        const int availableWidth = panel.right - panel.left - (buttonInset * 2) - (buttonGap * 2);
        const int width = std::max(150, availableWidth / 3);
        state->buttons = {
            {kDialogInstall, {left, bottom - buttonHeight, left + width, bottom}, L"Установить", true, !state->installing, true},
            {kDialogChoose, {left + width + buttonGap, bottom - buttonHeight, left + (width * 2) + buttonGap, bottom}, L"Выбрать папку", false, !state->installing, true},
            {kDialogClose, {panel.right - buttonInset - width, bottom - buttonHeight, panel.right - buttonInset, bottom}, L"Пропустить", false, !state->installing, true}
        };
    } else {
        state->buttons = {
            {kDialogClose, {panel.right - buttonInset - 142, bottom - buttonHeight, panel.right - buttonInset, bottom}, L"Закрыть", true, true, true}
        };
    }
}

void PaintFfmpegDialog(HWND window, FfmpegDialogState* state, HDC dc) {
    RECT client{};
    GetClientRect(window, &client);
    LayoutFfmpegDialog(state, client);
    UiRenderer::DrawBackground(dc, client);
    UiRenderer::DrawPanel(dc, {16, 16, client.right - 16, client.bottom - 16});
    DrawTextLine(dc, state ? state->title : L"FFmpeg", {32, 48, client.right - 32, 86}, 22, RGB(242, 242, 242));
    RECT message{32, 100, client.right - 32, client.bottom - 118};
    DrawTextLine(dc, state ? state->message : L"", message, 16, RGB(242, 242, 242), DT_LEFT | DT_TOP | DT_WORDBREAK);
    if (state && state->installing) {
        DrawTextLine(dc, state->status, {32, client.bottom - 112, client.right - 32, client.bottom - 86}, 15, RGB(180, 180, 186));
        UiRenderer::DrawIndeterminateProgressBar(dc, {32, client.bottom - 84, client.right - 32, client.bottom - 74}, GetTickCount64() / 1000.0);
    }
    DrawSettingsButtons(dc, state->buttons, state->pressedButton, state->hotButton);
}

void RunModal(HWND owner, HWND dialog);

void LayoutFfmpegInstallDialog(FfmpegInstallDialogState* state, const RECT& client) {
    if (!state) {
        return;
    }
    constexpr int panelInset = 16;
    constexpr int buttonInset = 16;
    constexpr int buttonHeight = 42;
    const RECT panel{panelInset, panelInset, client.right - panelInset, client.bottom - panelInset};
    const int buttonWidth = state->done && state->success ? 132 : 112;
    const int bottom = panel.bottom - buttonInset;
    const std::wstring text = state->done ? (state->success ? L"Готово" : L"Закрыть") : L"Отмена";
    state->buttons = {
        {kDialogClose, {panel.right - buttonInset - buttonWidth, bottom - buttonHeight, panel.right - buttonInset, bottom}, text, state->done && state->success, true, true}
    };
}

int ProgressPercent(std::uint64_t downloaded, std::uint64_t total, bool success) {
    if (success) {
        return 100;
    }
    if (total == 0) {
        return 0;
    }
    return std::clamp(static_cast<int>((downloaded * 100) / total), 0, 100);
}

std::wstring ProgressByteText(std::uint64_t value) {
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

std::wstring ProgressBytesText(std::uint64_t downloaded, std::uint64_t total) {
    if (downloaded == 0 && total == 0) {
        return {};
    }
    if (total > 0) {
        return ProgressByteText(downloaded) + L" / " + ProgressByteText(total);
    }
    return ProgressByteText(downloaded);
}

void PaintFfmpegInstallDialog(HWND window, FfmpegInstallDialogState* state, HDC dc) {
    RECT client{};
    GetClientRect(window, &client);
    LayoutFfmpegInstallDialog(state, client);

    std::wstring title;
    std::wstring message;
    std::uint64_t downloaded = 0;
    std::uint64_t total = 0;
    bool success = false;
    if (state) {
        std::lock_guard lock(state->mutex);
        title = state->title;
        message = state->done && !state->success && !state->error.empty() ? state->error : state->message;
        downloaded = state->downloaded;
        total = state->total;
        success = state->success;
    }

    UiRenderer::DrawBackground(dc, client);
    UiRenderer::DrawPanel(dc, {16, 16, client.right - 16, client.bottom - 16});
    DrawTextLine(dc, title.empty() ? L"Установка FFmpeg" : title, {32, 40, client.right - 32, 70}, 18, RGB(242, 242, 242));
    DrawTextLine(dc, message, {32, 84, client.right - 32, 114}, 15, RGB(242, 242, 242), DT_LEFT | DT_VCENTER | DT_SINGLELINE | DT_END_ELLIPSIS);

    const std::wstring sizes = ProgressBytesText(downloaded, total);
    if (!sizes.empty()) {
        DrawTextLine(dc, sizes, {32, 116, client.right - 32, 138}, 13, RGB(170, 170, 178));
    }

    const int percent = ProgressPercent(downloaded, total, success);
    const int actionLeft = state && !state->buttons.empty() ? state->buttons.front().rect.left : client.right - 156;
    const int percentRight = actionLeft - 24;
    const int percentLeft = percentRight - 52;
    const int progressRight = std::max(96, percentLeft - 10);
    UiRenderer::DrawProgressBar(dc, {32, 152, progressRight, 160}, static_cast<double>(percent));
    DrawTextLine(dc, std::to_wstring(percent) + L"%", {percentLeft, 142, percentRight, 168}, 13, RGB(170, 170, 178), DT_RIGHT | DT_VCENTER | DT_SINGLELINE);
    if (state) {
        DrawSettingsButtons(dc, state->buttons, state->pressedButton, state->hotButton);
    }
}

void InvalidateFfmpegInstallContent(HWND window) {
    RECT client{};
    GetClientRect(window, &client);
    RECT content{20, 68, client.right - 20, 174};
    InvalidateRect(window, &content, FALSE);
}

void StartFfmpegInstallWorker(HWND window, FfmpegInstallDialogState* state) {
    if (!state || !state->paths || !state->config || !state->cancelEvent) {
        return;
    }
    const AppPaths paths = *state->paths;
    AppConfig* config = state->config;
    HANDLE cancelEvent = state->cancelEvent;
    state->worker = std::jthread([window, state, paths, config, cancelEvent](std::stop_token) {
        std::wstring error;
        const bool ok = InstallFfmpeg(
            paths,
            error,
            [window, state](std::uint64_t downloaded, std::uint64_t total, const std::wstring& status) {
                {
                    std::lock_guard lock(state->mutex);
                    state->downloaded = downloaded;
                    state->total = total;
                    state->message = status;
                }
                PostMessageW(window, kProgressUpdateMessage, 0, 0);
            },
            [cancelEvent] {
                return cancelEvent && WaitForSingleObject(cancelEvent, 0) == WAIT_OBJECT_0;
            }
        );
        {
            std::lock_guard lock(state->mutex);
            state->done = true;
            state->success = ok;
            state->error = ok ? std::wstring{} : error;
            state->message = ok ? L"FFmpeg установлен." : L"Установка FFmpeg не выполнена.";
            if (ok && config) {
                config->ffmpegPath = paths.root() / L"tools" / L"ffmpeg" / L"bin" / L"ffmpeg.exe";
            }
        }
        PostMessageW(window, kProgressDoneMessage, ok ? TRUE : FALSE, 0);
    });
}

LRESULT CALLBACK FfmpegInstallDialogProc(HWND window, UINT message, WPARAM wParam, LPARAM lParam) {
    auto* state = reinterpret_cast<FfmpegInstallDialogState*>(GetWindowLongPtrW(window, GWLP_USERDATA));
    switch (message) {
    case WM_NCCREATE: {
        auto* create = reinterpret_cast<CREATESTRUCTW*>(lParam);
        SetWindowLongPtrW(window, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(create->lpCreateParams));
        return TRUE;
    }
    case WM_CREATE:
        state = reinterpret_cast<FfmpegInstallDialogState*>(GetWindowLongPtrW(window, GWLP_USERDATA));
        EnableDarkTitleBar(window);
        if (state) {
            StartFfmpegInstallWorker(window, state);
        }
        return 0;
    case kProgressUpdateMessage:
        InvalidateFfmpegInstallContent(window);
        return 0;
    case kProgressDoneMessage:
        if (state) {
            LayoutFfmpegInstallDialog(state, RECT{0, 0, 560, 270});
        }
        InvalidateRect(window, nullptr, FALSE);
        return 0;
    case WM_MOUSEMOVE: {
        if (!state) {
            return 0;
        }
        POINT point{GET_X_LPARAM(lParam), GET_Y_LPARAM(lParam)};
        DialogButton* hit = HitDialogButton(state->buttons, point);
        const UINT hot = hit ? hit->id : 0;
        if (hot != state->hotButton) {
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
        if (!state) {
            return 0;
        }
        POINT point{GET_X_LPARAM(lParam), GET_Y_LPARAM(lParam)};
        if (DialogButton* hit = HitDialogButton(state->buttons, point)) {
            state->pressedButton = hit->id;
            SetCapture(window);
            InvalidateRect(window, nullptr, FALSE);
        }
        return 0;
    }
    case WM_LBUTTONUP: {
        if (!state) {
            return 0;
        }
        POINT point{GET_X_LPARAM(lParam), GET_Y_LPARAM(lParam)};
        const UINT pressed = state->pressedButton;
        state->pressedButton = 0;
        ReleaseCapture();
        InvalidateRect(window, nullptr, FALSE);
        if (DialogButton* hit = HitDialogButton(state->buttons, point); hit && hit->id == pressed) {
            if (!state->done) {
                if (state->cancelEvent) {
                    SetEvent(state->cancelEvent);
                }
                {
                    std::lock_guard lock(state->mutex);
                    state->message = L"Отмена...";
                }
                InvalidateFfmpegInstallContent(window);
            } else {
                CloseDialogWindow(window);
            }
        }
        return 0;
    }
    case WM_ERASEBKGND:
        return 1;
    case WM_PAINT: {
        PAINTSTRUCT ps{};
        HDC dc = BeginPaint(window, &ps);
        RECT client{};
        GetClientRect(window, &client);
        HDC memoryDc = CreateCompatibleDC(dc);
        HBITMAP bitmap = CreateCompatibleBitmap(dc, client.right - client.left, client.bottom - client.top);
        HGDIOBJ oldBitmap = SelectObject(memoryDc, bitmap);
        PaintFfmpegInstallDialog(window, state, memoryDc);
        BitBlt(dc, 0, 0, client.right - client.left, client.bottom - client.top, memoryDc, 0, 0, SRCCOPY);
        SelectObject(memoryDc, oldBitmap);
        DeleteObject(bitmap);
        DeleteDC(memoryDc);
        EndPaint(window, &ps);
        return 0;
    }
    case WM_CLOSE:
        if (state && !state->done) {
            if (state->cancelEvent) {
                SetEvent(state->cancelEvent);
            }
            {
                std::lock_guard lock(state->mutex);
                state->message = L"Отмена...";
            }
            InvalidateFfmpegInstallContent(window);
            return 0;
        }
        CloseDialogWindow(window);
        return 0;
    case WM_NCDESTROY:
        if (state && state->worker.joinable()) {
            if (state->cancelEvent) {
                SetEvent(state->cancelEvent);
            }
            state->worker.request_stop();
            state->worker.join();
        }
        if (state && state->cancelEvent) {
            CloseHandle(state->cancelEvent);
            state->cancelEvent = nullptr;
        }
        return 0;
    }
    return DefWindowProcW(window, message, wParam, lParam);
}

bool ShowFfmpegInstallProgress(HWND owner, HINSTANCE instance, const AppPaths& paths, AppConfig& config) {
    WNDCLASSW wc = {};
    wc.lpfnWndProc = FfmpegInstallDialogProc;
    wc.hInstance = instance;
    wc.hCursor = LoadCursorW(nullptr, IDC_ARROW);
    wc.hbrBackground = CreateSolidBrush(RGB(18, 18, 20));
    wc.lpszClassName = L"BoostyFfmpegInstallWindow";
    RegisterClassW(&wc);

    FfmpegInstallDialogState state{};
    state.paths = &paths;
    state.config = &config;
    state.cancelEvent = CreateEventW(nullptr, TRUE, FALSE, nullptr);
    if (!state.cancelEvent) {
        return false;
    }

    RECT ownerRect{};
    GetWindowRect(owner, &ownerRect);
    constexpr int width = 560;
    constexpr int height = 270;
    HWND dialog = CreateWindowExW(
        WS_EX_DLGMODALFRAME,
        wc.lpszClassName,
        L"FFmpeg",
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
        CloseHandle(state.cancelEvent);
        state.cancelEvent = nullptr;
        return false;
    }

    RunModal(owner, dialog);
    return state.success;
}

void ChooseFfmpegFolder(HWND window, FfmpegDialogState* state) {
    IFileOpenDialog* dialog = nullptr;
    if (FAILED(CoCreateInstance(CLSID_FileOpenDialog, nullptr, CLSCTX_INPROC_SERVER, IID_PPV_ARGS(&dialog)))) {
        return;
    }
    DWORD options = 0;
    if (SUCCEEDED(dialog->GetOptions(&options))) {
        dialog->SetOptions(options | FOS_PICKFOLDERS | FOS_FORCEFILESYSTEM);
    }
    dialog->SetTitle(L"Выберите папку с ffmpeg.exe или папку bin");
    if (SUCCEEDED(dialog->Show(window))) {
        IShellItem* item = nullptr;
        if (SUCCEEDED(dialog->GetResult(&item))) {
            PWSTR path = nullptr;
            if (SUCCEEDED(item->GetDisplayName(SIGDN_FILESYSPATH, &path))) {
                if (state && state->config) {
                    state->config->ffmpegPath = path;
                    state->saved = true;
                    RefreshFfmpegDialogText(state);
                    InvalidateRect(window, nullptr, FALSE);
                }
                CoTaskMemFree(path);
            }
            item->Release();
        }
    }
    dialog->Release();
}

LRESULT CALLBACK FfmpegDialogProc(HWND window, UINT message, WPARAM wParam, LPARAM lParam) {
    auto* state = reinterpret_cast<FfmpegDialogState*>(GetWindowLongPtrW(window, GWLP_USERDATA));
    switch (message) {
    case WM_NCCREATE: {
        auto* create = reinterpret_cast<CREATESTRUCTW*>(lParam);
        SetWindowLongPtrW(window, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(create->lpCreateParams));
        return TRUE;
    }
    case WM_CREATE:
        EnableDarkTitleBar(window);
        if (state) {
            RefreshFfmpegDialogText(state);
        }
        return 0;
    case WM_TIMER:
        InvalidateRect(window, nullptr, FALSE);
        return 0;
    case kDialogDone:
        if (state) {
            state->installing = false;
            KillTimer(window, 1);
            if (state->installOk) {
                state->saved = true;
                RefreshFfmpegDialogText(state);
            }
            InvalidateRect(window, nullptr, FALSE);
        }
        return 0;
    case WM_MOUSEMOVE: {
        if (!state) {
            return 0;
        }
        POINT point{GET_X_LPARAM(lParam), GET_Y_LPARAM(lParam)};
        DialogButton* hit = HitDialogButton(state->buttons, point);
        const UINT hot = hit ? hit->id : 0;
        if (hot != state->hotButton) {
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
        if (!state) {
            return 0;
        }
        POINT point{GET_X_LPARAM(lParam), GET_Y_LPARAM(lParam)};
        if (DialogButton* hit = HitDialogButton(state->buttons, point)) {
            state->pressedButton = hit->id;
            SetCapture(window);
            InvalidateRect(window, nullptr, FALSE);
        }
        return 0;
    }
    case WM_LBUTTONUP: {
        if (!state) {
            return 0;
        }
        POINT point{GET_X_LPARAM(lParam), GET_Y_LPARAM(lParam)};
        const UINT pressed = state->pressedButton;
        state->pressedButton = 0;
        ReleaseCapture();
        InvalidateRect(window, nullptr, FALSE);
        if (DialogButton* hit = HitDialogButton(state->buttons, point); hit && hit->id == pressed) {
            if (pressed == kDialogInstall && state->paths && state->config) {
                if (ShowFfmpegInstallProgress(window, reinterpret_cast<HINSTANCE>(GetWindowLongPtrW(window, GWLP_HINSTANCE)), *state->paths, *state->config)) {
                    state->saved = true;
                    RefreshFfmpegDialogText(state);
                    InvalidateRect(window, nullptr, FALSE);
                }
            } else if (pressed == kDialogChoose) {
                ChooseFfmpegFolder(window, state);
            } else if (pressed == kDialogClose) {
                CloseDialogWindow(window);
            }
        }
        return 0;
    }
    case WM_ERASEBKGND:
        return 1;
    case WM_PAINT: {
        PAINTSTRUCT ps{};
        HDC dc = BeginPaint(window, &ps);
        RECT client{};
        GetClientRect(window, &client);
        HDC memoryDc = CreateCompatibleDC(dc);
        HBITMAP bitmap = CreateCompatibleBitmap(dc, client.right - client.left, client.bottom - client.top);
        HGDIOBJ oldBitmap = SelectObject(memoryDc, bitmap);
        PaintFfmpegDialog(window, state, memoryDc);
        BitBlt(dc, 0, 0, client.right - client.left, client.bottom - client.top, memoryDc, 0, 0, SRCCOPY);
        SelectObject(memoryDc, oldBitmap);
        DeleteObject(bitmap);
        DeleteDC(memoryDc);
        EndPaint(window, &ps);
        return 0;
    }
    case WM_CLOSE:
        if (!state || !state->installing) {
            CloseDialogWindow(window);
        }
        return 0;
    }
    return DefWindowProcW(window, message, wParam, lParam);
}

void RestoreModalOwner(HWND owner, bool ownerWasEnabled) {
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

void RunModal(HWND owner, HWND dialog) {
    const bool ownerWasEnabled = owner && IsWindow(owner) && IsWindowEnabled(owner);
    if (ownerWasEnabled) {
        EnableWindow(owner, FALSE);
    }

    ShowWindow(dialog, SW_SHOW);
    UpdateWindow(dialog);

    MSG msg{};
    while (IsWindow(dialog) && GetMessageW(&msg, nullptr, 0, 0) > 0) {
        if (!IsDialogMessageW(dialog, &msg)) {
            TranslateMessage(&msg);
            DispatchMessageW(&msg);
        }
    }

    RestoreModalOwner(owner, ownerWasEnabled);
}

bool ShowFfmpegModal(HWND owner, HINSTANCE instance, const AppPaths& paths, AppConfig& config) {
    WNDCLASSW wc = {};
    wc.lpfnWndProc = FfmpegDialogProc;
    wc.hInstance = instance;
    wc.hCursor = LoadCursorW(nullptr, IDC_ARROW);
    wc.hbrBackground = CreateSolidBrush(RGB(18, 18, 20));
    wc.lpszClassName = L"BoostyFfmpegWindow";
    RegisterClassW(&wc);

    FfmpegDialogState state{};
    state.config = &config;
    state.paths = &paths;
    RECT ownerRect{};
    GetWindowRect(owner, &ownerRect);
    const int width = kFfmpegDialogWidth;
    const int height = kFfmpegDialogHeight;
    HWND dialog = CreateWindowExW(WS_EX_DLGMODALFRAME, wc.lpszClassName, L"FFmpeg", WS_POPUP | WS_CAPTION | WS_SYSMENU, ownerRect.left + ((ownerRect.right - ownerRect.left) - width) / 2, ownerRect.top + ((ownerRect.bottom - ownerRect.top) - height) / 2, width, height, owner, nullptr, instance, &state);
    if (!dialog) {
        return false;
    }
    RunModal(owner, dialog);
    return state.saved;
}

void ShowFfmpegInfoModal(HWND owner, HINSTANCE instance, const std::wstring& title, const std::wstring& message) {
    WNDCLASSW wc = {};
    wc.lpfnWndProc = FfmpegDialogProc;
    wc.hInstance = instance;
    wc.hCursor = LoadCursorW(nullptr, IDC_ARROW);
    wc.hbrBackground = CreateSolidBrush(RGB(18, 18, 20));
    wc.lpszClassName = L"BoostyInfoWindow";
    RegisterClassW(&wc);

    FfmpegDialogState state{};
    state.title = title;
    state.message = message;
    RECT ownerRect{};
    GetWindowRect(owner, &ownerRect);
    const int width = kFfmpegDialogWidth;
    const int height = kFfmpegDialogHeight;
    HWND dialog = CreateWindowExW(WS_EX_DLGMODALFRAME, wc.lpszClassName, L"FFmpeg", WS_POPUP | WS_CAPTION | WS_SYSMENU, ownerRect.left + ((ownerRect.right - ownerRect.left) - width) / 2, ownerRect.top + ((ownerRect.bottom - ownerRect.top) - height) / 2, width, height, owner, nullptr, instance, &state);
    if (!dialog) {
        return;
    }
    RunModal(owner, dialog);
}

std::wstring QualityFromButton(UINT id) {
    switch (id) {
    case kQualityAudio: return L"audio";
    case kQuality360: return L"low";
    case kQuality480: return L"medium";
    case kQuality720: return L"high";
    case kQuality1080: return L"full_hd";
    case kQualityMax: return L"highest";
    default: return L"highest";
    }
}

UINT QualityButtonFromValue(const std::wstring& quality) {
    if (quality == L"audio") {
        return kQualityAudio;
    }
    if (quality == L"low") {
        return kQuality360;
    }
    if (quality == L"medium") {
        return kQuality480;
    }
    if (quality == L"high") {
        return kQuality720;
    }
    if (quality == L"full_hd") {
        return kQuality1080;
    }
    return kQualityMax;
}

std::wstring ContainerFromButton(UINT id) {
    switch (id) {
    case kContainerMp4: return L"mp4";
    case kContainerMkv: return L"mkv";
    case kContainerWebm: return L"webm";
    default: return L"auto";
    }
}

UINT ContainerButtonFromValue(const std::wstring& container) {
    if (container == L"mp4") {
        return kContainerMp4;
    }
    if (container == L"mkv") {
        return kContainerMkv;
    }
    if (container == L"webm") {
        return kContainerWebm;
    }
    return kContainerAuto;
}

std::wstring NormalizeQualityForFfmpeg(std::wstring quality, bool ffmpegAvailable) {
    if (quality.empty()) {
        return L"highest";
    }
    if (!ffmpegAvailable && quality == L"audio") {
        return L"highest";
    }
    return quality;
}

std::wstring NormalizeContainerForFfmpeg(std::wstring container, bool ffmpegAvailable) {
    if (container.empty()) {
        return L"auto";
    }
    if (!ffmpegAvailable && container != L"auto") {
        return L"auto";
    }
    return container;
}

void NormalizeFfmpegDependentSettings(AppConfig& config, bool ffmpegAvailable) {
    config.quality = NormalizeQualityForFfmpeg(config.quality, ffmpegAvailable);
    config.container = NormalizeContainerForFfmpeg(config.container, ffmpegAvailable);
}

bool IsSettingsSidebarCollapsed(const SettingsState* state, int width) {
    return width < kSettingsSidebarCollapseWidth || (state && state->sidebarCollapsed);
}

int SettingsSidebarWidth(const SettingsState* state, int width) {
    return IsSettingsSidebarCollapsed(state, width) ? kSettingsSidebarCollapsedWidth : kSettingsSidebarExpandedWidth;
}

RECT SettingsPanelRect(const RECT& client) {
    return {12, 12, client.right - 12, client.bottom - 12};
}

RECT SettingsSidebarRect(const SettingsState* state, const RECT& client) {
    const RECT panel = SettingsPanelRect(client);
    const int left = panel.left + 16;
    const int top = panel.top + 16;
    return {left, top, left + SettingsSidebarWidth(state, client.right), panel.bottom - 16};
}

RECT SettingsContentRect(const SettingsState* state, const RECT& client) {
    const RECT panel = SettingsPanelRect(client);
    const RECT sidebar = SettingsSidebarRect(state, client);
    return {sidebar.right + kSettingsSidebarGap, panel.top + 16, panel.right - 16, panel.bottom - 68};
}

void LayoutSettingsButtons(SettingsState* state, const RECT& client) {
    if (!state) {
        return;
    }
    const RECT panel = SettingsPanelRect(client);
    const RECT sidebar = SettingsSidebarRect(state, client);
    const RECT content = SettingsContentRect(state, client);
    const bool collapsed = IsSettingsSidebarCollapsed(state, client.right);
    const auto stackCard = [&](int index, int height) {
        const int top = content.top + 86 + index * (height + 14);
        return RECT{content.left, top, content.right, top + height};
    };

    state->buttons = {
        {kSettingsBack, {sidebar.right - 46, sidebar.top + 16, sidebar.right - 14, sidebar.top + 48}, collapsed ? L">" : L"<", false, true, true},
        {kSettingsDownloads, {sidebar.left + 12, sidebar.top + 70, sidebar.right - 12, sidebar.top + 106}, collapsed ? L"⇄" : L"Загрузки", state->section == SettingsSection::Downloads, true, true},
        {kSettingsTools, {sidebar.left + 12, sidebar.top + 114, sidebar.right - 12, sidebar.top + 150}, collapsed ? L"⚒" : L"Инструменты", state->section == SettingsSection::Tools, true, true},
        {kSettingsAbout, {sidebar.left + 12, sidebar.bottom - 48, sidebar.right - 12, sidebar.bottom - 12}, collapsed ? L"ℹ" : L"О программе", state->section == SettingsSection::About, true, true},
        {kSettingsCancel, {panel.right - 16 - 128 - 12 - 128, panel.bottom - 16 - 34, panel.right - 16 - 128 - 12, panel.bottom - 16}, L"Отмена", false, true, true},
        {kSettingsSave, {panel.right - 16 - 128, panel.bottom - 16 - 34, panel.right - 16, panel.bottom - 16}, L"Сохранить", true, true, true}
    };
    if (state->section == SettingsSection::Downloads) {
        const UINT activeQuality = QualityButtonFromValue(state->config.quality);
        const bool ffmpegAvailable = state->ffmpegAvailable;
        RECT card = stackCard(0, 112);
        int x = card.left + 18;
        const int qualityWidth = std::max(62, static_cast<int>((card.right - card.left - 36 - 40) / 6));
        state->buttons.insert(state->buttons.end(), {
            {kQualityAudio, {x, card.top + 66, x + qualityWidth, card.top + 98}, L"Аудио", activeQuality == kQualityAudio, ffmpegAvailable, true, true},
            {kQuality360, {x + (qualityWidth + 8), card.top + 66, x + (qualityWidth + 8) + qualityWidth, card.top + 98}, L"360p", activeQuality == kQuality360, true, true, true},
            {kQuality480, {x + 2 * (qualityWidth + 8), card.top + 66, x + 2 * (qualityWidth + 8) + qualityWidth, card.top + 98}, L"480p", activeQuality == kQuality480, true, true, true},
            {kQuality720, {x + 3 * (qualityWidth + 8), card.top + 66, x + 3 * (qualityWidth + 8) + qualityWidth, card.top + 98}, L"720p", activeQuality == kQuality720, true, true, true},
            {kQuality1080, {x + 4 * (qualityWidth + 8), card.top + 66, x + 4 * (qualityWidth + 8) + qualityWidth, card.top + 98}, L"1080p", activeQuality == kQuality1080, true, true, true},
            {kQualityMax, {x + 5 * (qualityWidth + 8), card.top + 66, x + 5 * (qualityWidth + 8) + qualityWidth, card.top + 98}, L"Макс.", activeQuality == kQualityMax, true, true, true}
        });
        card = stackCard(1, 112);
        x = card.left + 18;
        const UINT activeContainer = ContainerButtonFromValue(state->config.container);
        const int containerWidth = std::max(76, static_cast<int>((card.right - card.left - 36 - 30) / 4));
        state->buttons.insert(state->buttons.end(), {
            {kContainerAuto, {x, card.top + 66, x + containerWidth, card.top + 98}, L"Auto", activeContainer == kContainerAuto, ffmpegAvailable, true, true},
            {kContainerMp4, {x + (containerWidth + 10), card.top + 66, x + (containerWidth + 10) + containerWidth, card.top + 98}, L"MP4", activeContainer == kContainerMp4, ffmpegAvailable, true, true},
            {kContainerMkv, {x + 2 * (containerWidth + 10), card.top + 66, x + 2 * (containerWidth + 10) + containerWidth, card.top + 98}, L"MKV", activeContainer == kContainerMkv, ffmpegAvailable, true, true},
            {kContainerWebm, {x + 3 * (containerWidth + 10), card.top + 66, x + 3 * (containerWidth + 10) + containerWidth, card.top + 98}, L"WEBM", activeContainer == kContainerWebm, ffmpegAvailable, true, true}
        });
        card = stackCard(2, 122);
        const int valueRight = card.right - 18 - 46 - 12;
        const RECT value{valueRight - 54, card.top + 66, valueRight, card.top + 100};
        state->buttons.insert(state->buttons.end(), {
            {kWorkersMinus, {value.left - 58, value.top, value.left - 12, value.top + 34}, L"-", false, true, true, true},
            {kWorkersPlus, {value.right + 12, value.top, value.right + 58, value.top + 34}, L"+", false, true, true, true}
        });
    } else if (state->section == SettingsSection::Tools) {
        RECT card = stackCard(0, state->ffmpegDetailsExpanded ? 136 : 116);
        state->buttons.insert(state->buttons.end(), {
            {kFfmpegConfigure, {card.right - 300, card.top + 18, card.right - 158, card.top + 52}, L"Настроить", false, true, true, true},
            {kFfmpegDetails, {card.right - 142, card.top + 18, card.right - 18, card.top + 52}, state->ffmpegDetailsExpanded ? L"Скрыть" : L"Подробнее", state->ffmpegDetailsExpanded, true, true, true}
        });
    } else if (state->section == SettingsSection::About) {
        RECT appCard = stackCard(0, 158);
        RECT card{content.left, content.top + 258, content.right, content.top + 398};
        constexpr int autoCheckWidth = 246;
        state->buttons.insert(state->buttons.end(), {
            {kSettingsCheckUpdates, {appCard.left + 18, appCard.top + 112, appCard.left + 234, appCard.top + 146}, L"Проверить обновления", false, true, true, true},
            {kAutoCheck, {card.right - 18 - autoCheckWidth, card.top + 66, card.right - 18, card.top + 100}, state->config.autoUpdateCheck ? L"Автопроверка: Вкл" : L"Автопроверка: Выкл", state->config.autoUpdateCheck, true, true, true}
        });
    }
}

void DrawSettingsDownloads(HDC dc, const SettingsState* state, const RECT& client) {
    const RECT content = SettingsContentRect(state, client);
    const auto stackCard = [&](int index, int height) {
        const int top = content.top + 86 + index * (height + 14);
        return RECT{content.left, top, content.right, top + height};
    };

    DrawTextLine(dc, L"Загрузки", {content.left, content.top + 2, content.right, content.top + 36}, 26, RGB(242, 242, 242));
    DrawTextLine(dc, L"Качество, контейнер и поведение новых задач.", {content.left, content.top + 36, content.right, content.top + 64}, 15, RGB(180, 180, 186));

    RECT card = stackCard(0, 112);
    DrawSettingsRoundedPanel(dc, card, Gdiplus::Color(255, 35, 35, 38), Gdiplus::Color(255, 48, 48, 52), 8);
    DrawTextLine(dc, L"Качество", {card.left + 18, card.top + 14, card.right - 18, card.top + 38}, 19, RGB(242, 242, 242));
    DrawTextLine(dc, L"Качество по умолчанию для новых загрузок.", {card.left + 18, card.top + 40, card.right - 18, card.top + 64}, 15, RGB(180, 180, 186));

    card = stackCard(1, 112);
    DrawSettingsRoundedPanel(dc, card, Gdiplus::Color(255, 35, 35, 38), Gdiplus::Color(255, 48, 48, 52), 8);
    DrawTextLine(dc, L"Контейнер", {card.left + 18, card.top + 14, card.right - 18, card.top + 38}, 19, RGB(242, 242, 242));
    DrawTextLine(dc, L"Формат итогового файла без изменения схемы имен.", {card.left + 18, card.top + 40, card.right - 18, card.top + 64}, 15, RGB(180, 180, 186));

    card = stackCard(2, 122);
    DrawSettingsRoundedPanel(dc, card, Gdiplus::Color(255, 35, 35, 38), Gdiplus::Color(255, 48, 48, 52), 8);
    DrawTextLine(dc, L"Поведение", {card.left + 18, card.top + 14, card.right - 18, card.top + 38}, 19, RGB(242, 242, 242));
    DrawTextLine(dc, L"Количество одновременных задач.", {card.left + 18, card.top + 40, card.right - 18, card.top + 64}, 15, RGB(180, 180, 186));
    DrawTextLine(dc, L"Параллельные загрузки", {card.left + 18, card.top + 66, card.right - 180, card.top + 100}, 15, RGB(242, 242, 242));
    const int valueRight = card.right - 18 - 46 - 12;
    DrawTextLine(dc, std::to_wstring(state->config.maxParallelDownloads), {valueRight - 54, card.top + 66, valueRight, card.top + 100}, 20, RGB(242, 242, 242), DT_CENTER | DT_VCENTER | DT_SINGLELINE);
}

void DrawSettingsTools(HDC dc, const SettingsState* state, const RECT& client) {
    const RECT content = SettingsContentRect(state, client);
    DrawTextLine(dc, L"Инструменты", {content.left, content.top + 2, content.right, content.top + 36}, 26, RGB(242, 242, 242));
    DrawTextLine(dc, L"Статус FFmpeg и других внешних инструментов.", {content.left, content.top + 36, content.right, content.top + 64}, 15, RGB(180, 180, 186));

    const int cardHeight = state && state->ffmpegDetailsExpanded ? 136 : 116;
    RECT card{content.left, content.top + 86, content.right, content.top + 86 + cardHeight};
    FfmpegStatus status;
    if (state && state->paths) {
        status = ResolveFfmpeg(*state->paths, state->config.ffmpegPath);
    }
    DrawSettingsRoundedPanel(dc, card, Gdiplus::Color(255, 35, 35, 38), Gdiplus::Color(255, 48, 48, 52), 8);
    DrawTextLine(dc, L"FFmpeg", {card.left + 18, card.top + 14, card.right - 330, card.top + 38}, 19, RGB(242, 242, 242));
    DrawTextLine(dc, L"Готов для контейнеров, субтитров и аудиодорожек.", {card.left + 18, card.top + 42, card.right - 330, card.top + 66}, 15, RGB(180, 180, 186));
    DrawStatusPill(dc, {card.left + 18, card.top + 74, card.left + 112, card.top + 98}, status.available ? L"Готов" : L"Не готов", status.available);
    DrawStatusPill(dc, {card.left + 122, card.top + 74, card.left + 246, card.top + 98}, status.available ? (status.version.empty() ? L"Версия ?" : status.version) : L"Не найден", status.available);
    if (state && state->ffmpegDetailsExpanded) {
        DrawTextLine(dc, status.available ? (L"Путь: " + status.executable.wstring()) : L"Статус: FFmpeg не найден", {card.left + 18, card.top + 110, card.right - 18, card.top + 132}, 13, RGB(180, 180, 186));
    }
}

void DrawSettingsAbout(HDC dc, const SettingsState* state, const RECT& client) {
    const RECT content = SettingsContentRect(state, client);
    DrawTextLine(dc, L"О программе", {content.left, content.top + 2, content.right, content.top + 36}, 26, RGB(242, 242, 242));
    DrawTextLine(dc, L"Версия приложения и обновления.", {content.left, content.top + 36, content.right, content.top + 64}, 15, RGB(180, 180, 186));

    RECT card{content.left, content.top + 86, content.right, content.top + 244};
    DrawSettingsRoundedPanel(dc, card, Gdiplus::Color(255, 35, 35, 38), Gdiplus::Color(255, 48, 48, 52), 8);
    DrawTextLine(dc, L"Boosty Downloader", {card.left + 18, card.top + 14, card.right - 18, card.top + 38}, 19, RGB(242, 242, 242));
    DrawTextLine(dc, L"Портативный Win32-загрузчик видео с Boosty.", {card.left + 18, card.top + 40, card.right - 18, card.top + 64}, 15, RGB(180, 180, 186));
    DrawTextLine(dc, std::wstring(L"Версия: ") + kAppVersionWide, {card.left + 18, card.top + 78, card.right - 18, card.top + 104}, 15, RGB(242, 242, 242));

    card = {content.left, content.top + 258, content.right, content.top + 398};
    DrawSettingsRoundedPanel(dc, card, Gdiplus::Color(255, 35, 35, 38), Gdiplus::Color(255, 48, 48, 52), 8);
    DrawTextLine(dc, L"Обновления", {card.left + 18, card.top + 14, card.right - 18, card.top + 38}, 19, RGB(242, 242, 242));
    DrawTextLine(dc, state && state->config.autoUpdateCheck ? L"Автопроверка обновлений включена." : L"Автопроверка обновлений выключена.", {card.left + 18, card.top + 44, card.right - 18, card.top + 68}, 15, RGB(180, 180, 186));
}

void PaintSettings(HWND window, SettingsState* state, HDC dc) {
    RECT client{};
    GetClientRect(window, &client);
    LayoutSettingsButtons(state, client);
    const RECT panel = SettingsPanelRect(client);
    const RECT sidebar = SettingsSidebarRect(state, client);
    const bool collapsed = IsSettingsSidebarCollapsed(state, client.right);
    UiRenderer::DrawBackground(dc, client);
    UiRenderer::DrawPanel(dc, panel);
    DrawSettingsRoundedPanel(dc, sidebar, Gdiplus::Color(255, 25, 25, 28), Gdiplus::Color(255, 48, 48, 52), 8);
    if (!collapsed) {
        DrawTextLine(dc, L"Настройки", {sidebar.left + 16, sidebar.top + 18, sidebar.right - 54, sidebar.top + 52}, 19, RGB(242, 242, 242));
    }

    if (state->section == SettingsSection::Downloads) {
        DrawSettingsDownloads(dc, state, client);
    } else if (state->section == SettingsSection::Tools) {
        DrawSettingsTools(dc, state, client);
    } else {
        DrawSettingsAbout(dc, state, client);
    }
    DrawSettingsButtons(dc, state->buttons, state->pressedButton, state->hotButton);
}

bool ShowUpdatePrompt(HWND owner, HINSTANCE instance, const std::wstring& message);

void RunAppUpdateFlow(HWND owner, const AppPaths& paths, bool manual) {
    HCURSOR oldCursor = SetCursor(LoadCursorW(nullptr, IDC_WAIT));
    try {
        const ReleaseAssetInfo release = AppUpdateService::CheckLatestRelease();
        SetCursor(oldCursor);
        if (!ShouldInstallAppUpdate(release)) {
            if (manual) {
                ShowCustomMessageDialog(owner, nullptr, L"Обновления", L"Доступных обновлений нет.", MessageDialogKind::Info);
            }
            return;
        }

        const HINSTANCE instance = reinterpret_cast<HINSTANCE>(GetWindowLongPtrW(owner, GWLP_HINSTANCE));
        if (!ShowUpdatePrompt(owner, instance, BuildAppUpdatePromptMessage(release))) {
            return;
        }

        oldCursor = SetCursor(LoadCursorW(nullptr, IDC_WAIT));
        const std::filesystem::path downloadedExe = AppUpdateService::DownloadUpdateExe(paths, release);
        SetCursor(oldCursor);
        ShowCustomMessageDialog(owner, nullptr, L"Обновление", L"Обновление скачано. Приложение будет закрыто и запущено заново.", MessageDialogKind::Info);
        AppUpdateService::StartDownloadedUpdate(paths, downloadedExe);
        HWND root = GetWindow(owner, GW_OWNER);
        if (!root) {
            root = GetAncestor(owner, GA_ROOT);
        }
        PostMessageW(root ? root : owner, WM_CLOSE, 0, 0);
    } catch (const std::exception& ex) {
        SetCursor(oldCursor);
        if (manual) {
            const std::wstring message = L"Не удалось проверить или установить обновление:\n" + Utf8ToWide(ex.what());
            ShowCustomMessageDialog(owner, nullptr, L"Обновления", message, MessageDialogKind::Error);
        }
    }
}

void LayoutUpdatePromptButtons(UpdatePromptState* state, const RECT& client) {
    if (!state) {
        return;
    }
    constexpr int width = 132;
    constexpr int height = 34;
    constexpr int gap = 12;
    constexpr int panelInset = 14;
    constexpr int panelPadding = 16;
    const int right = client.right - panelInset - panelPadding;
    const int bottom = client.bottom - panelInset - panelPadding;
    state->buttons = {
        {kDialogClose, {right - width * 2 - gap, bottom - height, right - width - gap, bottom}, L"Отмена", false, true, true},
        {kDialogInstall, {right - width, bottom - height, right, bottom}, L"Скачать", true, true, true}
    };
}

void PaintUpdatePrompt(HWND window, UpdatePromptState* state, HDC dc) {
    RECT client{};
    GetClientRect(window, &client);
    LayoutUpdatePromptButtons(state, client);
    UiRenderer::DrawBackground(dc, client);
    RECT panel{14, 14, client.right - 14, client.bottom - 14};
    DrawSettingsRoundedPanel(dc, panel, Gdiplus::Color(255, 28, 28, 31), Gdiplus::Color(255, 48, 48, 52), 8);
    DrawTextLine(dc, L"Обновление", {panel.left + 18, panel.top + 18, panel.right - 18, panel.top + 48}, 22, RGB(242, 242, 242));
    DrawTextLine(dc, state ? state->message : L"", {panel.left + 18, panel.top + 62, panel.right - 18, panel.bottom - 92}, 15, RGB(180, 180, 186), DT_LEFT | DT_TOP | DT_WORDBREAK);
    if (state) {
        DrawSettingsButtons(dc, state->buttons, state->pressedButton, state->hotButton);
    }
}

LRESULT CALLBACK UpdatePromptProc(HWND window, UINT message, WPARAM wParam, LPARAM lParam) {
    auto* state = reinterpret_cast<UpdatePromptState*>(GetWindowLongPtrW(window, GWLP_USERDATA));
    if (message == WM_NCCREATE) {
        const auto* create = reinterpret_cast<CREATESTRUCTW*>(lParam);
        state = static_cast<UpdatePromptState*>(create->lpCreateParams);
        SetWindowLongPtrW(window, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(state));
        return TRUE;
    }

    switch (message) {
    case WM_ERASEBKGND:
        return 1;
    case WM_MOUSEMOVE: {
        if (!state) {
            return 0;
        }
        POINT point{GET_X_LPARAM(lParam), GET_Y_LPARAM(lParam)};
        DialogButton* hit = HitDialogButton(state->buttons, point);
        const UINT hot = hit ? hit->id : 0;
        if (hot != state->hotButton) {
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
        if (!state) {
            return 0;
        }
        POINT point{GET_X_LPARAM(lParam), GET_Y_LPARAM(lParam)};
        if (DialogButton* hit = HitDialogButton(state->buttons, point)) {
            state->pressedButton = hit->id;
            SetCapture(window);
            InvalidateRect(window, nullptr, FALSE);
        }
        return 0;
    }
    case WM_LBUTTONUP: {
        if (!state) {
            return 0;
        }
        POINT point{GET_X_LPARAM(lParam), GET_Y_LPARAM(lParam)};
        const UINT pressed = state->pressedButton;
        state->pressedButton = 0;
        ReleaseCapture();
        InvalidateRect(window, nullptr, FALSE);
        if (DialogButton* hit = HitDialogButton(state->buttons, point); hit && hit->id == pressed) {
            state->accepted = hit->id == kDialogInstall;
            CloseDialogWindow(window);
        }
        return 0;
    }
    case WM_KEYDOWN:
        if (wParam == VK_ESCAPE) {
            CloseDialogWindow(window);
            return 0;
        }
        if (wParam == VK_RETURN && state) {
            state->accepted = true;
            CloseDialogWindow(window);
            return 0;
        }
        break;
    case WM_PAINT: {
        PAINTSTRUCT ps{};
        HDC dc = BeginPaint(window, &ps);
        RECT client{};
        GetClientRect(window, &client);
        HDC memoryDc = CreateCompatibleDC(dc);
        HBITMAP bitmap = CreateCompatibleBitmap(dc, client.right - client.left, client.bottom - client.top);
        HGDIOBJ oldBitmap = SelectObject(memoryDc, bitmap);
        PaintUpdatePrompt(window, state, memoryDc);
        BitBlt(dc, 0, 0, client.right - client.left, client.bottom - client.top, memoryDc, 0, 0, SRCCOPY);
        SelectObject(memoryDc, oldBitmap);
        DeleteObject(bitmap);
        DeleteDC(memoryDc);
        EndPaint(window, &ps);
        return 0;
    }
    case WM_CLOSE:
        CloseDialogWindow(window);
        return 0;
    }
    return DefWindowProcW(window, message, wParam, lParam);
}

bool ShowUpdatePrompt(HWND owner, HINSTANCE instance, const std::wstring& message) {
    WNDCLASSW wc = {};
    wc.lpfnWndProc = UpdatePromptProc;
    wc.hInstance = instance;
    wc.hCursor = LoadCursorW(nullptr, IDC_ARROW);
    wc.hbrBackground = CreateSolidBrush(RGB(18, 18, 20));
    wc.lpszClassName = L"BoostyUpdatePromptWindow";
    RegisterClassW(&wc);

    UpdatePromptState state{};
    state.message = message;
    RECT ownerRect{};
    GetWindowRect(owner, &ownerRect);
    constexpr int width = 560;
    constexpr int height = 258;
    HWND dialog = CreateWindowExW(
        WS_EX_DLGMODALFRAME,
        wc.lpszClassName,
        L"Обновление",
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
        return false;
    }
    EnableDarkTitleBar(dialog);
    RunModal(owner, dialog);
    return state.accepted;
}

void HandleSettingsCommand(HWND window, SettingsState* state, UINT id) {
    if (!state) {
        return;
    }
    switch (id) {
    case kSettingsBack:
        state->sidebarCollapsed = !state->sidebarCollapsed;
        InvalidateRect(window, nullptr, FALSE);
        break;
    case kSettingsDownloads:
        state->section = SettingsSection::Downloads;
        InvalidateRect(window, nullptr, FALSE);
        break;
    case kSettingsTools:
        state->section = SettingsSection::Tools;
        InvalidateRect(window, nullptr, FALSE);
        break;
    case kSettingsAbout:
        state->section = SettingsSection::About;
        InvalidateRect(window, nullptr, FALSE);
        break;
    case kQualityAudio:
        if (!state->ffmpegAvailable) {
            break;
        }
        [[fallthrough]];
    case kQuality360:
    case kQuality480:
    case kQuality720:
    case kQuality1080:
    case kQualityMax:
        state->config.quality = QualityFromButton(id);
        InvalidateRect(window, nullptr, FALSE);
        break;
    case kContainerAuto:
    case kContainerMp4:
    case kContainerMkv:
    case kContainerWebm:
        if (!state->ffmpegAvailable) {
            break;
        }
        state->config.container = ContainerFromButton(id);
        InvalidateRect(window, nullptr, FALSE);
        break;
    case kWorkersMinus:
        state->config.maxParallelDownloads = std::max(1, state->config.maxParallelDownloads - 1);
        InvalidateRect(window, nullptr, FALSE);
        break;
    case kWorkersPlus:
        state->config.maxParallelDownloads = std::min(16, state->config.maxParallelDownloads + 1);
        InvalidateRect(window, nullptr, FALSE);
        break;
    case kAutoCheck:
        state->config.autoUpdateCheck = !state->config.autoUpdateCheck;
        InvalidateRect(window, nullptr, FALSE);
        break;
    case kSettingsCheckUpdates:
        if (state->paths) {
            RunAppUpdateFlow(window, *state->paths, true);
        }
        break;
    case kFfmpegConfigure:
        if (state->paths) {
            ShowFfmpegModal(window, reinterpret_cast<HINSTANCE>(GetWindowLongPtrW(window, GWLP_HINSTANCE)), *state->paths, state->config);
            state->ffmpegAvailable = ResolveFfmpeg(*state->paths, state->config.ffmpegPath).available;
            NormalizeFfmpegDependentSettings(state->config, state->ffmpegAvailable);
            InvalidateRect(window, nullptr, FALSE);
        }
        break;
    case kFfmpegDetails:
        state->ffmpegDetailsExpanded = !state->ffmpegDetailsExpanded;
        InvalidateRect(window, nullptr, FALSE);
        break;
    case kSettingsSave:
        state->saved = true;
        CloseDialogWindow(window);
        break;
    case kSettingsCancel:
        CloseDialogWindow(window);
        break;
    default:
        break;
    }
}

LRESULT CALLBACK SettingsProc(HWND window, UINT message, WPARAM wParam, LPARAM lParam) {
    auto* state = reinterpret_cast<SettingsState*>(GetWindowLongPtrW(window, GWLP_USERDATA));
    switch (message) {
    case WM_NCCREATE: {
        auto* create = reinterpret_cast<CREATESTRUCTW*>(lParam);
        SetWindowLongPtrW(window, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(create->lpCreateParams));
        return TRUE;
    }
    case WM_CREATE:
        EnableDarkTitleBar(window);
        return 0;
    case WM_SIZE:
        InvalidateRect(window, nullptr, FALSE);
        return 0;
    case WM_ERASEBKGND:
        return 1;
    case WM_MOUSEMOVE: {
        if (!state) {
            return 0;
        }
        POINT point{GET_X_LPARAM(lParam), GET_Y_LPARAM(lParam)};
        DialogButton* hit = HitDialogButton(state->buttons, point);
        const UINT hot = hit ? hit->id : 0;
        if (hot != state->hotButton) {
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
        if (!state) {
            return 0;
        }
        POINT point{GET_X_LPARAM(lParam), GET_Y_LPARAM(lParam)};
        if (DialogButton* hit = HitDialogButton(state->buttons, point)) {
            state->pressedButton = hit->id;
            SetCapture(window);
            InvalidateRect(window, nullptr, FALSE);
        }
        return 0;
    }
    case WM_LBUTTONUP: {
        if (!state) {
            return 0;
        }
        POINT point{GET_X_LPARAM(lParam), GET_Y_LPARAM(lParam)};
        const UINT pressed = state->pressedButton;
        state->pressedButton = 0;
        ReleaseCapture();
        InvalidateRect(window, nullptr, FALSE);
        if (DialogButton* hit = HitDialogButton(state->buttons, point); hit && hit->id == pressed) {
            HandleSettingsCommand(window, state, hit->id);
        }
        return 0;
    }
    case WM_KEYDOWN:
        if (wParam == VK_ESCAPE) {
            CloseDialogWindow(window);
            return 0;
        }
        if (wParam == VK_RETURN && state) {
            state->saved = true;
            CloseDialogWindow(window);
            return 0;
        }
        break;
    case WM_PAINT: {
        PAINTSTRUCT ps{};
        HDC dc = BeginPaint(window, &ps);
        RECT client{};
        GetClientRect(window, &client);
        HDC memoryDc = CreateCompatibleDC(dc);
        HBITMAP bitmap = CreateCompatibleBitmap(dc, client.right - client.left, client.bottom - client.top);
        HGDIOBJ oldBitmap = SelectObject(memoryDc, bitmap);
        PaintSettings(window, state, memoryDc);
        BitBlt(dc, 0, 0, client.right - client.left, client.bottom - client.top, memoryDc, 0, 0, SRCCOPY);
        SelectObject(memoryDc, oldBitmap);
        DeleteObject(bitmap);
        DeleteDC(memoryDc);
        EndPaint(window, &ps);
        return 0;
    }
    case WM_CLOSE:
        CloseDialogWindow(window);
        return 0;
    }
    return DefWindowProcW(window, message, wParam, lParam);
}

bool ShowSettingsModal(HWND owner, HINSTANCE instance, const AppPaths& paths, AppConfig& config) {
    WNDCLASSW wc = {};
    wc.lpfnWndProc = SettingsProc;
    wc.hInstance = instance;
    wc.hCursor = LoadCursorW(nullptr, IDC_ARROW);
    wc.hbrBackground = CreateSolidBrush(RGB(18, 18, 20));
    wc.lpszClassName = L"BoostySettingsWindow";
    RegisterClassW(&wc);

    SettingsState state{};
    state.config = config;
    state.paths = &paths;
    state.ffmpegAvailable = ResolveFfmpeg(paths, state.config.ffmpegPath).available;
    NormalizeFfmpegDependentSettings(state.config, state.ffmpegAvailable);
    RECT ownerRect = {};
    GetWindowRect(owner, &ownerRect);
    const int width = kSettingsWindowWidth;
    const int height = kSettingsWindowHeight;
    HWND dialog = CreateWindowExW(
        WS_EX_DLGMODALFRAME,
        wc.lpszClassName,
        L"Настройки",
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
        return false;
    }
    RunModal(owner, dialog);
    if (state.saved) {
        config = state.config;
    }
    return state.saved;
}

int MeasureTextHeight(HDC dc, HFONT font, const std::wstring& text, int width) {
    HFONT oldFont = reinterpret_cast<HFONT>(SelectObject(dc, font));
    RECT measure = {0, 0, width, 1};
    DrawTextW(dc, text.c_str(), -1, &measure, DT_CALCRECT | DT_WORDBREAK | DT_NOPREFIX);
    SelectObject(dc, oldFont);
    return measure.bottom - measure.top;
}

bool ClampScroll(LogViewState* state, int visibleHeight) {
    const int previousScrollY = state->scrollY;
    const int maxScroll = std::max(0, state->contentHeight - visibleHeight);
    state->scrollY = std::clamp(state->scrollY, 0, maxScroll);
    return state->scrollY != previousScrollY;
}

bool SetScrollY(LogViewState* state, int desiredScrollY, int visibleHeight) {
    const int previousScrollY = state->scrollY;
    state->scrollY = desiredScrollY;
    ClampScroll(state, visibleHeight);
    return state->scrollY != previousScrollY;
}

int GetScrollVisibleEnd(const RECT& client) {
    return std::max(1, static_cast<int>(client.bottom - client.top) - kScrollTextClipBottomPadding);
}

RECT GetScrollbarThumb(const RECT& client, int contentHeight, int scrollY) {
    constexpr int barWidth = 8;
    constexpr int padding = 6;
    RECT track = {client.right - barWidth - padding, client.top + padding, client.right - padding, client.bottom - padding};
    const int trackHeight = std::max(1, static_cast<int>(track.bottom - track.top));
    const int visibleHeight = GetScrollVisibleEnd(client);
    const int thumbHeight = std::clamp((visibleHeight * trackHeight) / std::max(visibleHeight, contentHeight), 28, trackHeight);
    const int maxScroll = std::max(1, contentHeight - visibleHeight);
    const int maxThumbTravel = std::max(0, trackHeight - thumbHeight);
    const int thumbTop = static_cast<int>(track.top) + (scrollY * maxThumbTravel) / maxScroll;
    return {track.left, thumbTop, track.right, thumbTop + thumbHeight};
}

std::vector<std::wstring> SplitLogLines(const std::wstring& text) {
    std::vector<std::wstring> lines;
    size_t start = 0;
    while (start <= text.size()) {
        const size_t end = text.find(L'\n', start);
        std::wstring line = end == std::wstring::npos
            ? text.substr(start)
            : text.substr(start, end - start);
        if (!line.empty() && line.back() == L'\r') {
            line.pop_back();
        }
        lines.push_back(std::move(line));
        if (end == std::wstring::npos) {
            break;
        }
        start = end + 1;
    }
    if (lines.empty()) {
        lines.push_back({});
    }
    return lines;
}

void RebuildLogLayout(HDC dc, HFONT font, LogViewState* state, const RECT& client) {
    const int textWidth = std::max(40, static_cast<int>(client.right - client.left) - 42);
    int y = kScrollTextTopPadding;
    state->layouts.clear();
    state->layouts.reserve(state->lines.size());
    for (const std::wstring& line : state->lines) {
        const int textHeight = std::max(18, MeasureTextHeight(dc, font, line.empty() ? L" " : line, textWidth));
        LogLineLayout layout;
        layout.rect = {0, y, textWidth, y + textHeight + 8};
        state->layouts.push_back(layout);
        y = layout.rect.bottom;
    }
    state->contentHeight = y + kScrollTextBottomPadding;
    ClampScroll(state, GetScrollVisibleEnd(client));
}

int HitTestLogLine(LogViewState* state, int contentY) {
    for (size_t index = 0; index < state->layouts.size(); ++index) {
        const RECT& rect = state->layouts[index].rect;
        if (contentY >= rect.top && contentY < rect.bottom) {
            return static_cast<int>(index);
        }
    }
    return -1;
}

void ShowLogCopyMenu(HWND owner, HINSTANCE instance, POINT screenPoint, const std::wstring& text) {
    if (text.empty()) {
        return;
    }

    auto* state = new LogCopyMenuState{};
    state->owner = owner;
    state->text = text;

    constexpr int menuWidth = 148;
    constexpr int menuHeight = 36;
    RECT workArea = {};
    SystemParametersInfoW(SPI_GETWORKAREA, 0, &workArea, 0);
    const int x = std::max(static_cast<int>(workArea.left), std::min(static_cast<int>(screenPoint.x), static_cast<int>(workArea.right) - menuWidth));
    const int y = std::max(static_cast<int>(workArea.top), std::min(static_cast<int>(screenPoint.y), static_cast<int>(workArea.bottom) - menuHeight));

    HWND menu = CreateWindowExW(WS_EX_TOPMOST | WS_EX_TOOLWINDOW, kLogCopyMenuClassName, L"", WS_POPUP, x, y, menuWidth, menuHeight, owner, nullptr, instance, state);
    if (!menu) {
        delete state;
        return;
    }
    ShowWindow(menu, SW_SHOW);
    SetFocus(menu);
}

HWND CreateLogView(HWND parent, HINSTANCE instance, const std::wstring& text) {
    auto* state = new LogViewState{};
    state->text = text;
    state->lines = SplitLogLines(text);

    HWND view = CreateWindowExW(0, kLogViewClassName, L"", WS_CHILD | WS_VISIBLE | WS_TABSTOP, 0, 0, 10, 10, parent, nullptr, instance, state);
    if (!view) {
        delete state;
    }
    return view;
}

LRESULT CALLBACK LogCopyMenuProc(HWND window, UINT message, WPARAM wParam, LPARAM lParam) {
    LogCopyMenuState* state = reinterpret_cast<LogCopyMenuState*>(GetWindowLongPtrW(window, GWLP_USERDATA));
    if (message == WM_NCCREATE) {
        const CREATESTRUCTW* create = reinterpret_cast<CREATESTRUCTW*>(lParam);
        state = static_cast<LogCopyMenuState*>(create->lpCreateParams);
        SetWindowLongPtrW(window, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(state));
        return TRUE;
    }

    switch (message) {
    case WM_ERASEBKGND:
        return 1;
    case WM_MOUSEMOVE:
        if (state && !state->hot) {
            state->hot = true;
            TRACKMOUSEEVENT track{sizeof(track), TME_LEAVE, window, 0};
            TrackMouseEvent(&track);
            InvalidateRect(window, nullptr, FALSE);
        }
        return 0;
    case WM_MOUSELEAVE:
        if (state && state->hot) {
            state->hot = false;
            InvalidateRect(window, nullptr, FALSE);
        }
        return 0;
    case WM_PAINT:
        PaintBuffered(window, [state](HDC dc, const RECT& client) {
            const std::vector<UiRenderer::PopupMenuItem> items = {
                {1, L"Копировать", false}
            };
            UiRenderer::DrawPopupMenu(dc, client, items, state && state->hot ? 1 : 0);
        });
        return 0;
    case WM_LBUTTONUP:
        if (state) {
            CopyTextToClipboard(state->owner ? state->owner : window, state->text);
        }
        CloseDialogWindow(window);
        return 0;
    case WM_KEYDOWN:
        if (wParam == VK_ESCAPE) {
            CloseDialogWindow(window);
            return 0;
        }
        if (wParam == VK_RETURN || wParam == VK_SPACE) {
            if (state) {
                CopyTextToClipboard(state->owner ? state->owner : window, state->text);
            }
            CloseDialogWindow(window);
            return 0;
        }
        break;
    case WM_KILLFOCUS:
        CloseDialogWindow(window);
        return 0;
    case WM_NCDESTROY:
        delete state;
        SetWindowLongPtrW(window, GWLP_USERDATA, 0);
        return 0;
    }
    return DefWindowProcW(window, message, wParam, lParam);
}

LRESULT CALLBACK LogViewProc(HWND window, UINT message, WPARAM wParam, LPARAM lParam) {
    LogViewState* state = reinterpret_cast<LogViewState*>(GetWindowLongPtrW(window, GWLP_USERDATA));
    if (message == WM_NCCREATE) {
        const CREATESTRUCTW* create = reinterpret_cast<CREATESTRUCTW*>(lParam);
        state = static_cast<LogViewState*>(create->lpCreateParams);
        SetWindowLongPtrW(window, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(state));
        return TRUE;
    }

    switch (message) {
    case WM_ERASEBKGND:
        return 1;
    case WM_MOUSEWHEEL:
        if (state) {
            RECT client = {};
            GetClientRect(window, &client);
            const int delta = GET_WHEEL_DELTA_WPARAM(wParam);
            const int desiredScrollY = state->scrollY - ((delta / WHEEL_DELTA) * 44);
            if (SetScrollY(state, desiredScrollY, GetScrollVisibleEnd(client))) {
                InvalidateRect(window, nullptr, FALSE);
            }
        }
        return 0;
    case WM_LBUTTONDOWN:
    case WM_RBUTTONUP:
    case WM_CONTEXTMENU:
        if (state) {
            SetFocus(window);
            RECT client = {};
            GetClientRect(window, &client);
            POINT point = {};
            if (message == WM_CONTEXTMENU) {
                point = {GET_X_LPARAM(lParam), GET_Y_LPARAM(lParam)};
                if (point.x == -1 && point.y == -1) {
                    if (state->selectedLine >= 0 && state->selectedLine < static_cast<int>(state->layouts.size())) {
                        const RECT selected = state->layouts[static_cast<size_t>(state->selectedLine)].rect;
                        point = {client.left + 18, client.top + selected.top - state->scrollY + 4};
                    } else {
                        return 0;
                    }
                } else {
                    ScreenToClient(window, &point);
                }
            } else {
                point = {GET_X_LPARAM(lParam), GET_Y_LPARAM(lParam)};
            }
            RECT thumb = GetScrollbarThumb(client, state->contentHeight, state->scrollY);
            if (message == WM_LBUTTONDOWN && state->contentHeight > GetScrollVisibleEnd(client) && PtInRect(&thumb, point)) {
                state->draggingThumb = true;
                state->dragStartY = point.y;
                state->dragStartScrollY = state->scrollY;
                SetCapture(window);
                return 0;
            }

            HDC dc = GetDC(window);
            HFONT font = CreateUiFont(-13, FW_NORMAL);
            RebuildLogLayout(dc, font, state, client);
            DeleteObject(font);
            ReleaseDC(window, dc);

            const int hit = HitTestLogLine(state, point.y + state->scrollY);
            if (hit >= 0) {
                state->selectedLine = hit;
                InvalidateRect(window, nullptr, FALSE);
                if (message == WM_RBUTTONUP || message == WM_CONTEXTMENU) {
                    POINT screenPoint = point;
                    ClientToScreen(window, &screenPoint);
                    HINSTANCE instance = reinterpret_cast<HINSTANCE>(GetWindowLongPtrW(window, GWLP_HINSTANCE));
                    ShowLogCopyMenu(window, instance, screenPoint, state->lines[static_cast<size_t>(hit)]);
                }
            }
        }
        return 0;
    case WM_MOUSEMOVE:
        if (state && state->draggingThumb) {
            RECT client = {};
            GetClientRect(window, &client);
            RECT thumb = GetScrollbarThumb(client, state->contentHeight, state->scrollY);
            const int visibleHeight = GetScrollVisibleEnd(client);
            const int maxScroll = std::max(0, state->contentHeight - visibleHeight);
            const int trackTravel = std::max(1, static_cast<int>(client.bottom - client.top) - 12 - static_cast<int>(thumb.bottom - thumb.top));
            const int dy = GET_Y_LPARAM(lParam) - state->dragStartY;
            const int desiredScrollY = state->dragStartScrollY + (dy * maxScroll) / trackTravel;
            if (SetScrollY(state, desiredScrollY, visibleHeight)) {
                InvalidateRect(window, nullptr, FALSE);
            }
        }
        return 0;
    case WM_LBUTTONUP:
        if (state && state->draggingThumb) {
            state->draggingThumb = false;
            if (GetCapture() == window) {
                ReleaseCapture();
            }
        }
        return 0;
    case WM_KEYDOWN:
        if (state) {
            const bool controlDown = (GetKeyState(VK_CONTROL) & 0x8000) != 0;
            if (controlDown && wParam == 'C' && state->selectedLine >= 0 && state->selectedLine < static_cast<int>(state->lines.size())) {
                CopyTextToClipboard(window, state->lines[static_cast<size_t>(state->selectedLine)]);
                return 0;
            }
        }
        break;
    case WM_PAINT:
        if (state) {
            PaintBuffered(window, [window, state](HDC dc, const RECT& client) {
                UiRenderer::DrawInputFrame(dc, client);

                HFONT font = CreateUiFont(-13, FW_NORMAL);
                RebuildLogLayout(dc, font, state, client);

                HRGN clip = CreateRectRgn(client.left + 10, client.top + kScrollTextClipTopPadding, client.right - 16, client.bottom - kScrollTextClipBottomPadding);
                SelectClipRgn(dc, clip);

                const int textLeft = client.left + 14;
                const int textRight = client.right - 28;
                for (size_t index = 0; index < state->lines.size(); ++index) {
                    const RECT layout = state->layouts[index].rect;
                    RECT visualRect = {
                        textLeft,
                        client.top + layout.top - state->scrollY,
                        textRight,
                        client.top + layout.bottom - state->scrollY
                    };
                    if (visualRect.bottom < client.top || visualRect.top > client.bottom) {
                        continue;
                    }
                    if (static_cast<int>(index) == state->selectedLine) {
                        Gdiplus::Graphics graphics(dc);
                        graphics.SetSmoothingMode(Gdiplus::SmoothingModeHighQuality);
                        RECT selected = {visualRect.left - 6, visualRect.top - 2, visualRect.right + 2, visualRect.bottom - 4};
                        Gdiplus::GraphicsPath path;
                        AddSettingsRoundedRect(path, selected, 5);
                        Gdiplus::SolidBrush fill(Gdiplus::Color(255, 48, 48, 54));
                        graphics.FillPath(&fill, &path);
                    }

                    RECT textRect = {visualRect.left, visualRect.top, visualRect.right, visualRect.bottom - 6};
                    DrawTextBlock(dc, state->lines[index], textRect, RGB(242, 242, 242), font, DT_LEFT | DT_WORDBREAK);
                }
                SelectClipRgn(dc, nullptr);
                DeleteObject(clip);

                if (state->contentHeight > GetScrollVisibleEnd(client)) {
                    Gdiplus::Graphics graphics(dc);
                    graphics.SetSmoothingMode(Gdiplus::SmoothingModeHighQuality);
                    RECT thumb = GetScrollbarThumb(client, state->contentHeight, state->scrollY);
                    Gdiplus::SolidBrush trackBrush(Gdiplus::Color(255, 39, 39, 43));
                    Gdiplus::SolidBrush thumbBrush(Gdiplus::Color(255, 86, 86, 92));
                    graphics.FillRectangle(&trackBrush, static_cast<INT>(client.right - 14), static_cast<INT>(client.top + 8), 6, static_cast<INT>(client.bottom - client.top - 16));
                    graphics.FillRectangle(&thumbBrush, static_cast<INT>(thumb.left), static_cast<INT>(thumb.top), static_cast<INT>(thumb.right - thumb.left), static_cast<INT>(thumb.bottom - thumb.top));
                }

                DeleteObject(font);
            });
        }
        return 0;
    case WM_SIZE:
        if (state) {
            state->layouts.clear();
            InvalidateRect(window, nullptr, FALSE);
        }
        return 0;
    case WM_NCDESTROY:
        delete state;
        SetWindowLongPtrW(window, GWLP_USERDATA, 0);
        return 0;
    }
    return DefWindowProcW(window, message, wParam, lParam);
}

void LayoutLogsDialog(HWND window, LogsDialogState* state) {
    if (!state) {
        return;
    }
    RECT client{};
    GetClientRect(window, &client);
    constexpr int panelInset = 12;
    constexpr int buttonInset = 16;
    constexpr int buttonHeight = 34;
    constexpr int buttonGap = 12;
    const int panelRight = client.right - panelInset;
    const int panelBottom = client.bottom - panelInset;
    const int buttonY = panelBottom - buttonInset - buttonHeight;
    const RECT logRect{24, 82, client.right - 24, buttonY - 16};
    if (IsWindow(state->logView)) {
        MoveWindow(state->logView, logRect.left, logRect.top, logRect.right - logRect.left, logRect.bottom - logRect.top, TRUE);
    }
    state->buttons = {
        {kLogCopyAll, {panelRight - buttonInset - 112 - buttonGap - 150, buttonY, panelRight - buttonInset - 112 - buttonGap, buttonY + buttonHeight}, L"Скопировать всё", false, true, true},
        {kLogClose, {panelRight - buttonInset - 112, buttonY, panelRight - buttonInset, buttonY + buttonHeight}, L"Закрыть", true, true, true}
    };
}

void PaintLogs(HWND window, LogsDialogState* state, HDC dc) {
    RECT client{};
    GetClientRect(window, &client);
    LayoutLogsDialog(window, state);
    UiRenderer::DrawBackground(dc, client);
    UiRenderer::DrawPanel(dc, {12, 12, client.right - 12, client.bottom - 12});
    DrawTextLine(dc, L"Логи", {24, 28, client.right - 24, 58}, 18, RGB(242, 242, 242));
    DrawTextLine(dc, L"Выделите нужные строки или скопируйте весь текущий лог.", {24, 56, client.right - 24, 78}, 14, RGB(180, 180, 186));
    DrawDialogButtons(dc, state->buttons, state->pressedButton, state->hotButton);
}

void HandleLogsCommand(HWND window, LogsDialogState* state, UINT id) {
    if (id == kLogCopyAll && state) {
        CopyTextToClipboard(window, state->text);
        return;
    }
    if (id == kLogClose) {
        CloseDialogWindow(window);
    }
}

LRESULT CALLBACK LogsProc(HWND window, UINT message, WPARAM wParam, LPARAM lParam) {
    auto* state = reinterpret_cast<LogsDialogState*>(GetWindowLongPtrW(window, GWLP_USERDATA));
    switch (message) {
    case WM_NCCREATE: {
        auto* create = reinterpret_cast<CREATESTRUCTW*>(lParam);
        SetWindowLongPtrW(window, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(create->lpCreateParams));
        return TRUE;
    }
    case WM_CREATE:
        state = reinterpret_cast<LogsDialogState*>(GetWindowLongPtrW(window, GWLP_USERDATA));
        EnableDarkTitleBar(window);
        if (state) {
            state->logView = CreateLogView(
                window,
                reinterpret_cast<HINSTANCE>(GetWindowLongPtrW(window, GWLP_HINSTANCE)),
                state->text.empty() ? L"Лог пока пуст." : state->text
            );
            LayoutLogsDialog(window, state);
        }
        return 0;
    case WM_SIZE:
        LayoutLogsDialog(window, state);
        InvalidateRect(window, nullptr, FALSE);
        return 0;
    case WM_ERASEBKGND:
        return 1;
    case WM_MOUSEMOVE: {
        if (!state) {
            return 0;
        }
        POINT point{GET_X_LPARAM(lParam), GET_Y_LPARAM(lParam)};
        DialogButton* hit = HitDialogButton(state->buttons, point);
        const UINT hot = hit ? hit->id : 0;
        if (hot != state->hotButton) {
            const UINT oldHot = state->hotButton;
            state->hotButton = hot;
            InvalidateDialogButton(window, state->buttons, oldHot);
            InvalidateDialogButton(window, state->buttons, state->hotButton);
            TRACKMOUSEEVENT event{sizeof(event), TME_LEAVE, window, 0};
            TrackMouseEvent(&event);
        }
        return 0;
    }
    case WM_MOUSELEAVE:
        if (state) {
            InvalidateDialogButton(window, state->buttons, state->hotButton);
            state->hotButton = 0;
        }
        return 0;
    case WM_LBUTTONDOWN: {
        if (!state) {
            return 0;
        }
        POINT point{GET_X_LPARAM(lParam), GET_Y_LPARAM(lParam)};
        if (DialogButton* hit = HitDialogButton(state->buttons, point)) {
            state->pressedButton = hit->id;
            SetCapture(window);
            InvalidateDialogButton(window, state->buttons, state->pressedButton);
        }
        return 0;
    }
    case WM_LBUTTONUP: {
        if (!state) {
            return 0;
        }
        POINT point{GET_X_LPARAM(lParam), GET_Y_LPARAM(lParam)};
        const UINT pressed = state->pressedButton;
        state->pressedButton = 0;
        ReleaseCapture();
        InvalidateDialogButton(window, state->buttons, pressed);
        if (DialogButton* hit = HitDialogButton(state->buttons, point); hit && hit->id == pressed) {
            HandleLogsCommand(window, state, hit->id);
        }
        return 0;
    }
    case WM_KEYDOWN:
        if (wParam == VK_ESCAPE) {
            CloseDialogWindow(window);
            return 0;
        }
        break;
    case WM_PAINT: {
        PAINTSTRUCT ps{};
        HDC dc = BeginPaint(window, &ps);
        RECT client{};
        GetClientRect(window, &client);
        HDC memoryDc = CreateCompatibleDC(dc);
        HBITMAP bitmap = CreateCompatibleBitmap(dc, client.right - client.left, client.bottom - client.top);
        HGDIOBJ oldBitmap = SelectObject(memoryDc, bitmap);
        PaintLogs(window, state, memoryDc);
        BitBlt(dc, 0, 0, client.right - client.left, client.bottom - client.top, memoryDc, 0, 0, SRCCOPY);
        SelectObject(memoryDc, oldBitmap);
        DeleteObject(bitmap);
        DeleteDC(memoryDc);
        EndPaint(window, &ps);
        return 0;
    }
    case WM_CLOSE:
        CloseDialogWindow(window);
        return 0;
    }
    return DefWindowProcW(window, message, wParam, lParam);
}

void ShowLogsModal(HWND owner, HINSTANCE instance, const std::wstring& text) {
    WNDCLASSW wc = {};
    wc.lpfnWndProc = LogsProc;
    wc.hInstance = instance;
    wc.hCursor = LoadCursorW(nullptr, IDC_ARROW);
    wc.hbrBackground = CreateSolidBrush(RGB(18, 18, 20));
    wc.lpszClassName = L"BoostyLogsWindow";
    RegisterClassW(&wc);

    WNDCLASSEXW logViewClass = {};
    logViewClass.cbSize = sizeof(logViewClass);
    logViewClass.style = CS_HREDRAW | CS_VREDRAW;
    logViewClass.lpfnWndProc = LogViewProc;
    logViewClass.hInstance = instance;
    logViewClass.hCursor = LoadCursorW(nullptr, IDC_ARROW);
    logViewClass.hbrBackground = nullptr;
    logViewClass.lpszClassName = kLogViewClassName;
    RegisterClassExW(&logViewClass);

    WNDCLASSEXW logMenuClass = {};
    logMenuClass.cbSize = sizeof(logMenuClass);
    logMenuClass.lpfnWndProc = LogCopyMenuProc;
    logMenuClass.hInstance = instance;
    logMenuClass.hCursor = LoadCursorW(nullptr, IDC_ARROW);
    logMenuClass.hbrBackground = nullptr;
    logMenuClass.lpszClassName = kLogCopyMenuClassName;
    RegisterClassExW(&logMenuClass);

    LogsDialogState state{text};
    RECT ownerRect{};
    GetWindowRect(owner, &ownerRect);
    const int width = 860;
    const int height = 560;
    HWND dialog = CreateWindowExW(
        WS_EX_DLGMODALFRAME,
        wc.lpszClassName,
        L"Логи",
        WS_POPUP | WS_CAPTION | WS_SYSMENU | WS_THICKFRAME | WS_MAXIMIZEBOX,
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
    RunModal(owner, dialog);
}

} // namespace

int Application::Run(HINSTANCE instance, int showCommand) {
    m_instance = instance;
    SetCurrentProcessExplicitAppUserModelID(kAppUserModelId);

    INITCOMMONCONTROLSEX controls{sizeof(controls), ICC_WIN95_CLASSES | ICC_STANDARD_CLASSES};
    InitCommonControlsEx(&controls);
    RegisterPopupMenuClasses(m_instance);

    Gdiplus::GdiplusStartupInput gdiplusInput;
    ULONG_PTR gdiplusToken = 0;
    Gdiplus::GdiplusStartup(&gdiplusToken, &gdiplusInput, nullptr);
    CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED);

    if (!CreateMainWindow(showCommand)) {
        return 1;
    }

    MSG message = {};
    while (GetMessageW(&message, nullptr, 0, 0) > 0) {
        if (message.message == WM_KEYDOWN &&
            (message.hwnd == m_window || IsChild(m_window, message.hwnd))) {
            const bool controlDown = (GetKeyState(VK_CONTROL) & 0x8000) != 0;
            if (controlDown && message.wParam == 'V' && message.hwnd != m_folderEdit) {
                PasteUrl();
                continue;
            }
            if (message.wParam == VK_RETURN) {
                Click(kBtnDownload);
                continue;
            }
        }
        TranslateMessage(&message);
        DispatchMessageW(&message);
    }

    m_queue.reset();
    CoUninitialize();
    Gdiplus::GdiplusShutdown(gdiplusToken);
    return static_cast<int>(message.wParam);
}

bool Application::CreateMainWindow(int showCommand) {
    WNDCLASSEXW wc = {};
    wc.cbSize = sizeof(wc);
    wc.lpfnWndProc = Application::WindowProc;
    wc.hInstance = m_instance;
    wc.hCursor = LoadCursorW(nullptr, IDC_ARROW);
    wc.hIcon = static_cast<HICON>(LoadImageW(m_instance, MAKEINTRESOURCEW(kAppIcon), IMAGE_ICON, 32, 32, LR_DEFAULTCOLOR));
    wc.hIconSm = static_cast<HICON>(LoadImageW(m_instance, MAKEINTRESOURCEW(kAppIcon), IMAGE_ICON, 16, 16, LR_DEFAULTCOLOR));
    wc.lpszClassName = L"BoostyDownloaderWindow";
    RegisterClassExW(&wc);

    m_window = CreateWindowExW(
        0,
        wc.lpszClassName,
        L"",
        WS_OVERLAPPEDWINDOW | WS_CLIPCHILDREN | WS_CLIPSIBLINGS,
        CW_USEDEFAULT,
        CW_USEDEFAULT,
        980,
        720,
        nullptr,
        nullptr,
        m_instance,
        this
    );
    if (!m_window) {
        return false;
    }
    EnableDarkTitleBar(m_window);
    if (HICON bigIcon = static_cast<HICON>(LoadImageW(m_instance, MAKEINTRESOURCEW(kAppIcon), IMAGE_ICON, 32, 32, LR_DEFAULTCOLOR))) {
        SendMessageW(m_window, WM_SETICON, ICON_BIG, reinterpret_cast<LPARAM>(bigIcon));
    }
    if (HICON smallIcon = static_cast<HICON>(LoadImageW(m_instance, MAKEINTRESOURCEW(kAppIcon), IMAGE_ICON, 16, 16, LR_DEFAULTCOLOR))) {
        SendMessageW(m_window, WM_SETICON, ICON_SMALL, reinterpret_cast<LPARAM>(smallIcon));
    }

    ShowWindow(m_window, showCommand);
    UpdateWindow(m_window);
    return true;
}

LRESULT CALLBACK Application::WindowProc(HWND window, UINT message, WPARAM wParam, LPARAM lParam) {
    Application* app = reinterpret_cast<Application*>(GetWindowLongPtrW(window, GWLP_USERDATA));
    if (message == WM_NCCREATE) {
        auto* create = reinterpret_cast<CREATESTRUCTW*>(lParam);
        app = reinterpret_cast<Application*>(create->lpCreateParams);
        SetWindowLongPtrW(window, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(app));
        app->m_window = window;
    }
    return app ? app->HandleMessage(message, wParam, lParam) : DefWindowProcW(window, message, wParam, lParam);
}

LRESULT Application::HandleMessage(UINT message, WPARAM wParam, LPARAM lParam) {
    switch (message) {
    case WM_CREATE:
        Initialize();
        return 0;
    case kUpdateFoundMessage: {
        std::unique_ptr<ReleaseAssetInfo> release(reinterpret_cast<ReleaseAssetInfo*>(lParam));
        if (m_paths && release && ShouldInstallAppUpdate(*release)) {
            RunAppUpdateFlow(m_window, *m_paths, false);
        }
        return 0;
    }
    case WM_SIZE:
        Layout();
        if (m_queue) {
            RECT client{};
            GetClientRect(m_window, &client);
            m_queueScrollOffset = std::clamp(
                m_queueScrollOffset,
                0,
                QueueMaxScrollOffset(QueuePanelRectForClient(client), m_queue->Snapshot().size())
            );
        }
        InvalidateRect(m_window, nullptr, FALSE);
        return 0;
    case WM_GETMINMAXINFO: {
        auto* info = reinterpret_cast<MINMAXINFO*>(lParam);
        info->ptMinTrackSize.x = 900;
        info->ptMinTrackSize.y = 640;
        return 0;
    }
    case WM_ERASEBKGND:
        return 1;
    case WM_TIMER:
        if (RefreshStatus()) {
            InvalidateRect(m_window, nullptr, FALSE);
        }
        return 0;
    case WM_COMMAND:
        switch (LOWORD(wParam)) {
        case kBtnPaste:
        case kBtnDownload:
            Click(LOWORD(wParam));
            return 0;
        default:
            break;
        }
        break;
    case WM_DROPFILES: {
        wchar_t path[MAX_PATH] = {};
        HDROP drop = reinterpret_cast<HDROP>(wParam);
        if (DragQueryFileW(drop, 0, path, MAX_PATH)) {
            EnqueueFromFile(path);
        }
        DragFinish(drop);
        return 0;
    }
    case WM_MOUSEMOVE: {
        POINT point{GET_X_LPARAM(lParam), GET_Y_LPARAM(lParam)};
        if (m_draggingQueueScroll && m_queue) {
            RECT client{};
            GetClientRect(m_window, &client);
            const RECT queuePanel = QueuePanelRectForClient(client);
            const auto tasks = m_queue->Snapshot();
            const int maxOffset = QueueMaxScrollOffset(queuePanel, tasks.size());
            const RECT track = QueueScrollbarTrackRect(queuePanel);
            const RECT thumb = QueueScrollbarThumbRect(queuePanel, tasks.size(), m_queueScrollDragStartOffset);
            const int maxTravel = std::max(1, static_cast<int>(track.bottom - track.top - (thumb.bottom - thumb.top)));
            const int deltaY = point.y - m_queueScrollDragY;
            const int nextOffset = std::clamp(m_queueScrollDragStartOffset + (deltaY * maxOffset) / maxTravel, 0, maxOffset);
            if (nextOffset != m_queueScrollOffset) {
                m_queueScrollOffset = nextOffset;
                InvalidateRect(m_window, &queuePanel, FALSE);
            }
            return 0;
        }
        TaskButton* taskHit = HitTaskButton(point);
        const int hotTaskId = taskHit ? taskHit->taskId : 0;
        const UINT hotTaskAction = taskHit ? taskHit->action : 0;
        if (hotTaskId != m_hotTaskId || hotTaskAction != m_hotTaskAction) {
            const int oldTaskId = m_hotTaskId;
            const UINT oldTaskAction = m_hotTaskAction;
            m_hotTaskId = hotTaskId;
            m_hotTaskAction = hotTaskAction;
            InvalidateTaskButton(oldTaskId, oldTaskAction);
            InvalidateTaskButton(m_hotTaskId, m_hotTaskAction);
            TRACKMOUSEEVENT event{sizeof(event), TME_LEAVE, m_window, 0};
            TrackMouseEvent(&event);
        }
        Button* hit = taskHit ? nullptr : HitButton(point);
        const int hot = hit ? static_cast<int>(hit->id) : 0;
        if (hot != m_hotButton) {
            const int oldHot = m_hotButton;
            m_hotButton = hot;
            InvalidateButton(oldHot);
            InvalidateButton(m_hotButton);
            TRACKMOUSEEVENT event{sizeof(event), TME_LEAVE, m_window, 0};
            TrackMouseEvent(&event);
        }
        return 0;
    }
    case WM_MOUSELEAVE:
        InvalidateButton(m_hotButton);
        m_hotButton = 0;
        InvalidateTaskButton(m_hotTaskId, m_hotTaskAction);
        m_hotTaskId = 0;
        m_hotTaskAction = 0;
        return 0;
    case WM_LBUTTONDOWN: {
        POINT point{GET_X_LPARAM(lParam), GET_Y_LPARAM(lParam)};
        if (m_queue) {
            RECT client{};
            GetClientRect(m_window, &client);
            const RECT queuePanel = QueuePanelRectForClient(client);
            const auto tasks = m_queue->Snapshot();
            const int maxOffset = QueueMaxScrollOffset(queuePanel, tasks.size());
            if (maxOffset > 0) {
                const RECT track = QueueScrollbarTrackRect(queuePanel);
                if (PtInRect(&track, point)) {
                    const RECT thumb = QueueScrollbarThumbRect(queuePanel, tasks.size(), m_queueScrollOffset);
                    if (PtInRect(&thumb, point)) {
                        m_draggingQueueScroll = true;
                        m_queueScrollDragY = point.y;
                        m_queueScrollDragStartOffset = m_queueScrollOffset;
                    } else {
                        const int page = QueueVisibleRowCount(queuePanel);
                        m_queueScrollOffset = point.y < thumb.top
                            ? std::max(0, m_queueScrollOffset - page)
                            : std::min(maxOffset, m_queueScrollOffset + page);
                    }
                    SetCapture(m_window);
                    InvalidateRect(m_window, &queuePanel, FALSE);
                    return 0;
                }
            }
        }
        if (TaskButton* hit = HitTaskButton(point)) {
            m_pressedTaskId = hit->taskId;
            m_pressedTaskAction = hit->action;
            SetCapture(m_window);
            InvalidateTaskButton(m_pressedTaskId, m_pressedTaskAction);
            return 0;
        }
        if (Button* hit = HitButton(point)) {
            m_pressedButton = static_cast<int>(hit->id);
            SetCapture(m_window);
            InvalidateButton(m_pressedButton);
        }
        return 0;
    }
    case WM_LBUTTONUP: {
        POINT point{GET_X_LPARAM(lParam), GET_Y_LPARAM(lParam)};
        const bool wasDraggingQueueScroll = m_draggingQueueScroll;
        const int pressedTaskId = m_pressedTaskId;
        const UINT pressedTaskAction = m_pressedTaskAction;
        const int pressed = m_pressedButton;
        m_draggingQueueScroll = false;
        m_pressedTaskId = 0;
        m_pressedTaskAction = 0;
        m_pressedButton = 0;
        ReleaseCapture();
        if (wasDraggingQueueScroll) {
            InvalidateRect(m_window, nullptr, FALSE);
            return 0;
        }
        InvalidateTaskButton(pressedTaskId, pressedTaskAction);
        InvalidateButton(pressed);
        if (pressedTaskId && pressedTaskAction) {
            if (TaskButton* hit = HitTaskButton(point); hit && hit->taskId == pressedTaskId && hit->action == pressedTaskAction) {
                ClickTask(hit->taskId, hit->action);
            }
            return 0;
        }
        if (Button* hit = HitButton(point); hit && static_cast<int>(hit->id) == pressed) {
            Click(hit->id);
        }
        return 0;
    }
    case WM_MOUSEWHEEL: {
        POINT point{GET_X_LPARAM(lParam), GET_Y_LPARAM(lParam)};
        ScreenToClient(m_window, &point);
        RECT client{};
        GetClientRect(m_window, &client);
        const RECT queuePanel = QueuePanelRectForClient(client);
        if (PtInRect(&queuePanel, point)) {
            const int delta = GET_WHEEL_DELTA_WPARAM(wParam);
            if (delta != 0 && ScrollQueue(-(delta / WHEEL_DELTA))) {
                return 0;
            }
        }
        break;
    }
    case WM_CTLCOLOREDIT:
    case WM_CTLCOLORSTATIC: {
        static HBRUSH brush = CreateSolidBrush(RGB(25, 25, 28));
        SetTextColor(reinterpret_cast<HDC>(wParam), RGB(242, 242, 242));
        SetBkColor(reinterpret_cast<HDC>(wParam), RGB(25, 25, 28));
        return reinterpret_cast<LRESULT>(brush);
    }
    case WM_PAINT: {
        PAINTSTRUCT ps = {};
        HDC dc = BeginPaint(m_window, &ps);
        RECT client = {};
        GetClientRect(m_window, &client);
        HDC memoryDc = CreateCompatibleDC(dc);
        HBITMAP bitmap = CreateCompatibleBitmap(dc, client.right - client.left, client.bottom - client.top);
        HGDIOBJ oldBitmap = SelectObject(memoryDc, bitmap);
        Paint(memoryDc);
        BitBlt(dc, 0, 0, client.right - client.left, client.bottom - client.top, memoryDc, 0, 0, SRCCOPY);
        SelectObject(memoryDc, oldBitmap);
        DeleteObject(bitmap);
        DeleteDC(memoryDc);
        EndPaint(m_window, &ps);
        return 0;
    }
    case WM_DESTROY:
        if (m_updateWorker.joinable()) {
            m_updateWorker.request_stop();
        }
        if (m_queue) {
            SaveDownloadQueue(true);
            m_queue->Shutdown();
        }
        PostQuitMessage(0);
        return 0;
    }
    return DefWindowProcW(m_window, message, wParam, lParam);
}

void Application::Initialize() {
    m_paths = std::make_unique<AppPaths>(GetExecutableRoot());
    std::filesystem::create_directories(m_paths->stuffDir());
    m_config = ConfigStore::Load(*m_paths);
    m_logger = std::make_unique<Logger>(*m_paths);
    m_logger->Info(L"Application started: root=" + m_paths->root().wstring());
    try {
        AppUpdateService::EnsureLocalSha256Sums(*m_paths);
    } catch (const std::exception&) {
        m_logger->Error(L"Failed to write local SHA256SUMS");
    }
    const FfmpegStatus ffmpeg = ResolveFfmpeg(*m_paths, m_config.ffmpegPath);
    m_logger->Info(ffmpeg.available ? (L"FFmpeg found: " + ffmpeg.executable.wstring()) : L"FFmpeg not found");
    m_queue = std::make_unique<DownloadQueue>(m_config.maxParallelDownloads, m_logger.get());
    LoadDownloadQueue();

    m_urlEdit = CreateWindowExW(0, L"EDIT", L"", WS_CHILD | WS_VISIBLE | WS_TABSTOP | ES_AUTOHSCROLL, 0, 0, 100, 28, m_window, nullptr, m_instance, nullptr);
    m_folderEdit = CreateWindowExW(0, L"EDIT", m_config.downloadDir.c_str(), WS_CHILD | WS_VISIBLE | WS_TABSTOP | ES_AUTOHSCROLL, 0, 0, 100, 28, m_window, nullptr, m_instance, nullptr);
    SetFont(m_urlEdit);
    SetFont(m_folderEdit);
    InstallCustomEditContextMenu(m_urlEdit, m_window, m_instance, false, kBtnPaste, kBtnDownload);
    InstallCustomEditContextMenu(m_folderEdit, m_window, m_instance);
    SendMessageW(m_urlEdit, EM_SETMARGINS, EC_LEFTMARGIN | EC_RIGHTMARGIN, MAKELPARAM(10, 10));
    SendMessageW(m_folderEdit, EM_SETMARGINS, EC_LEFTMARGIN | EC_RIGHTMARGIN, MAKELPARAM(10, 10));
    m_tooltip = CreateWindowExW(WS_EX_TOPMOST, TOOLTIPS_CLASSW, nullptr, WS_POPUP | TTS_ALWAYSTIP | TTS_NOPREFIX, CW_USEDEFAULT, CW_USEDEFAULT, CW_USEDEFAULT, CW_USEDEFAULT, m_window, nullptr, m_instance, nullptr);
    if (m_tooltip) {
        SendMessageW(m_tooltip, TTM_SETMAXTIPWIDTH, 0, 360);
        SendMessageW(m_tooltip, TTM_SETDELAYTIME, TTDT_INITIAL, 450);
        SendMessageW(m_tooltip, TTM_SETDELAYTIME, TTDT_RESHOW, 100);
        SendMessageW(m_tooltip, TTM_SETDELAYTIME, TTDT_AUTOPOP, 10000);
        SendMessageW(m_tooltip, TTM_SETTIPBKCOLOR, RGB(35, 35, 38), 0);
        SendMessageW(m_tooltip, TTM_SETTIPTEXTCOLOR, RGB(242, 242, 242), 0);
        SendMessageW(m_tooltip, TTM_ACTIVATE, TRUE, 0);
    }

    DragAcceptFiles(m_window, TRUE);
    SetTimer(m_window, kRefreshTimer, 200, nullptr);
    SetStatus(L"Готово");
    StartAutoUpdateCheck();
    Layout();
}

void Application::Layout() {
    RECT client = {};
    GetClientRect(m_window, &client);
    const int w = client.right - client.left;
    const int margin = 28;
    const int buttonWidth = 150;
    const int buttonGap = 12;
    const int inputRight = std::max(520, w - margin - buttonWidth - buttonGap);
    const int inputWidth = std::max(260, inputRight - margin);

    MoveWindow(m_urlEdit, margin + 8, 92, inputWidth - 16, 24, TRUE);
    MoveWindow(m_folderEdit, margin + 8, 158 + kFolderRowOffset, inputWidth - 16, 24, TRUE);
    AddButtons();
    UpdateTooltips();
}

void Application::AddButtons() {
    RECT client = {};
    GetClientRect(m_window, &client);
    const int w = client.right;
    const int margin = 28;
    const int buttonWidth = 150;
    const int buttonGap = 12;
    const int inputRight = std::max(520, w - margin - buttonWidth - buttonGap);
    const int sideButtonLeft = inputRight + buttonGap;
    const int right = w - margin;
    m_buttons = {
        {kBtnPaste, {sideButtonLeft, 86, right, 122}, L"Вставить", false},
        {kBtnBrowse, {sideButtonLeft, 152 + kFolderRowOffset, right, 188 + kFolderRowOffset}, L"Выбрать...", false},
        {kBtnDownload, {margin, 220, margin + 120, 256}, L"Скачать", true},
        {kBtnLogin, {margin + 132, 220, margin + 322, 256}, L"Получить токен", false},
        {kBtnSettings, {right - 150, 220, right, 256}, L"Настройки", false},
        {kBtnOpenFolder, {right - 540, kQueueHeaderTop, right - 390, kQueueHeaderTop + 36}, L"Открыть папку", false},
        {kBtnLogs, {right - 378, kQueueHeaderTop, right - 288, kQueueHeaderTop + 36}, L"Логи", false},
        {kBtnClear, {right - 276, kQueueHeaderTop, right - 58, kQueueHeaderTop + 36}, L"Очистить очередь", false},
        {kBtnClearFinished, {right - 46, kQueueHeaderTop, right, kQueueHeaderTop + 36}, L"X", false}
    };
}

void Application::UpdateTooltips() {
    if (!m_tooltip) {
        return;
    }
    auto upsertTool = [&](UINT_PTR id, UINT flags, HWND toolWindow, const RECT* rect, const wchar_t* text) {
        TOOLINFOW info = {};
        info.cbSize = TooltipInfoSize();
        info.uFlags = flags;
        info.hwnd = m_window;
        info.uId = id;
        info.lpszText = const_cast<LPWSTR>(text);
        if (rect) {
            info.rect = *rect;
        }
        SendMessageW(m_tooltip, TTM_DELTOOLW, 0, reinterpret_cast<LPARAM>(&info));
        if (toolWindow) {
            info.hwnd = m_window;
            info.uId = reinterpret_cast<UINT_PTR>(toolWindow);
        }
        SendMessageW(m_tooltip, TTM_ADDTOOLW, 0, reinterpret_cast<LPARAM>(&info));
    };

    upsertTool(reinterpret_cast<UINT_PTR>(m_urlEdit), TTF_IDISHWND | TTF_SUBCLASS, m_urlEdit, nullptr, L"Введите одну ссылку Boosty или несколько ссылок построчно.");
    upsertTool(reinterpret_cast<UINT_PTR>(m_folderEdit), TTF_IDISHWND | TTF_SUBCLASS, m_folderEdit, nullptr, L"Папка, куда будут сохраняться скачанные видео.");
    for (const Button& button : m_buttons) {
        upsertTool(button.id, TTF_SUBCLASS, nullptr, &button.rect, ButtonTooltip(button.id));
    }
}

void Application::Paint(HDC dc) {
    RECT client = {};
    GetClientRect(m_window, &client);
    UiRenderer::DrawBackground(dc, client);

    DrawTextLine(dc, L"Boosty Downloader", {28, 22, client.right - 28, 52}, 26, RGB(242, 242, 242));
    DrawTextLine(dc, L"URL поста или несколько строк", {28, 60, 420, 80}, 15, RGB(180, 180, 186));
    DrawTextLine(dc, L"Папка загрузки", {28, 126 + kFolderRowOffset, 220, 146 + kFolderRowOffset}, 15, RGB(180, 180, 186));

    const int margin = 28;
    const int buttonWidth = 150;
    const int buttonGap = 12;
    const int inputRight = std::max(520, static_cast<int>(client.right) - margin - buttonWidth - buttonGap);
    UiRenderer::DrawInputFrame(dc, {margin, 84, inputRight, 124});
    UiRenderer::DrawInputFrame(dc, {margin, 150 + kFolderRowOffset, inputRight, 190 + kFolderRowOffset});

    for (const Button& button : m_buttons) {
        UiRenderer::DrawButton(
            dc,
            button.rect,
            button.text.c_str(),
            button.primary,
            m_pressedButton == static_cast<int>(button.id),
            m_hotButton == static_cast<int>(button.id),
            false
        );
    }

    DrawTextLine(dc, L"Очередь загрузок", {28, kQueueHeaderTop, client.right - 28, kQueueHeaderTop + 32}, 20, RGB(242, 242, 242));
    RECT queuePanel = QueuePanelRectForClient(client);
    UiRenderer::DrawPanel(dc, queuePanel);

    const auto tasks = m_queue ? m_queue->Snapshot() : std::vector<DownloadTaskSnapshot>{};
    m_taskButtons.clear();
    m_queueScrollOffset = std::clamp(m_queueScrollOffset, 0, QueueMaxScrollOffset(queuePanel, tasks.size()));
    if (tasks.empty()) {
        const int centerY = queuePanel.top + (queuePanel.bottom - queuePanel.top) / 2 - 28;
        DrawTextLine(dc, L"Задач пока нет", {queuePanel.left + 18, centerY, queuePanel.right - 18, centerY + 26}, 18, RGB(242, 242, 242), DT_CENTER | DT_VCENTER | DT_SINGLELINE);
        DrawTextLine(dc, L"Перетащите TXT со ссылками или вставьте URL и нажмите Скачать.", {queuePanel.left + 18, centerY + 32, queuePanel.right - 18, centerY + 58}, 15, RGB(156, 156, 164), DT_CENTER | DT_VCENTER | DT_SINGLELINE);
    }
    const int visibleRows = QueueVisibleRowCount(queuePanel);
    for (int visibleIndex = 0; visibleIndex < visibleRows; ++visibleIndex) {
        const int taskIndex = m_queueScrollOffset + visibleIndex;
        if (taskIndex < 0 || taskIndex >= static_cast<int>(tasks.size())) {
            break;
        }
        const auto& task = tasks[static_cast<size_t>(taskIndex)];
        RECT row = QueueRowRectAt(queuePanel, visibleIndex);
        UiRenderer::DrawPanel(dc, row);
        auto addTaskButton = [&](UINT action, const std::wstring& text, bool primary, int right, int width) {
            TaskButton button{task.id, action, {right - width, row.top + 18, right, row.top + 54}, text, primary};
            m_taskButtons.push_back(button);
            return button.rect.left - 10;
        };
        int buttonRight = row.right - 14;
        switch (task.state) {
        case DownloadTaskState::Queued:
        case DownloadTaskState::Preparing:
        case DownloadTaskState::Downloading:
            buttonRight = addTaskButton(kTaskCancel, L"Отмена", false, buttonRight, 106);
            break;
        case DownloadTaskState::Failed:
        case DownloadTaskState::Canceled:
            buttonRight = addTaskButton(kTaskDelete, L"Удалить", false, buttonRight, 96);
            buttonRight = addTaskButton(kTaskRetry, L"Возобновить", true, buttonRight, 126);
            break;
        case DownloadTaskState::Completed:
            buttonRight = addTaskButton(kTaskClose, L"Закрыть", false, buttonRight, 102);
            break;
        }

        RECT thumb{row.left + 14, row.top + 10, row.left + 86, row.bottom - 10};
        DrawTaskThumbnail(dc, thumb, task.thumbnailPath);
        const int textLeft = thumb.right + 12;
        const int textRight = std::max(textLeft + 180, buttonRight);
        DrawTextLine(dc, L"#" + std::to_wstring(task.id) + L"  " + task.title, {textLeft, row.top + 8, textRight, row.top + 30}, 16, RGB(242, 242, 242));
        std::wstring status = task.statusText.empty() ? StateText(task.state) : task.statusText;
        if (!task.errorText.empty()) {
            status += L": " + task.errorText;
        }
        DrawTextLine(dc, status, {textLeft, row.top + 32, textRight, row.top + 52}, 14, RGB(180, 180, 186));
        const std::wstring meta = FormatTaskMetaText(task);
        if (!meta.empty()) {
            DrawTextLine(dc, meta, {textLeft, row.top + 54, textRight, row.top + 74}, 13, RGB(156, 156, 164));
        }
        for (const TaskButton& button : m_taskButtons) {
            if (button.taskId != task.id) {
                continue;
            }
            UiRenderer::DrawButton(
                dc,
                button.rect,
                button.text.c_str(),
                button.primary,
                m_pressedTaskId == button.taskId && m_pressedTaskAction == button.action,
                m_hotTaskId == button.taskId && m_hotTaskAction == button.action,
                true
            );
        }
        UiRenderer::DrawProgressBar(dc, {textLeft, row.bottom - 12, row.right - 14, row.bottom - 6}, task.percent);
    }
    DrawQueueScrollbar(dc, queuePanel, tasks.size(), m_queueScrollOffset);

    DrawTextLine(dc, m_status, {24, client.bottom - 38, client.right - 24, client.bottom - 12}, 14, RGB(180, 180, 186));
}

Application::Button* Application::HitButton(POINT point) {
    for (Button& button : m_buttons) {
        if (PtInRect(&button.rect, point)) {
            return &button;
        }
    }
    return nullptr;
}

Application::TaskButton* Application::HitTaskButton(POINT point) {
    for (TaskButton& button : m_taskButtons) {
        if (PtInRect(&button.rect, point)) {
            return &button;
        }
    }
    return nullptr;
}

void Application::InvalidateButton(int id) {
    if (!id) {
        return;
    }
    for (const Button& button : m_buttons) {
        if (button.id == static_cast<UINT>(id)) {
            RECT rect = button.rect;
            InflateRect(&rect, 2, 2);
            InvalidateRect(m_window, &rect, FALSE);
            return;
        }
    }
}

void Application::InvalidateTaskButton(int taskId, UINT action) {
    if (!taskId || !action) {
        return;
    }
    for (const TaskButton& button : m_taskButtons) {
        if (button.taskId == taskId && button.action == action) {
            RECT rect = button.rect;
            InflateRect(&rect, 2, 2);
            InvalidateRect(m_window, &rect, FALSE);
            return;
        }
    }
}

void Application::Click(UINT id) {
    switch (id) {
    case kBtnDownload:
        EnqueueText(GetText(m_urlEdit));
        break;
    case kBtnPaste:
        PasteUrl();
        break;
    case kBtnBrowse: {
        BROWSEINFOW info = {};
        info.hwndOwner = m_window;
        info.lpszTitle = L"Выберите папку загрузки";
        info.ulFlags = BIF_RETURNONLYFSDIRS | BIF_NEWDIALOGSTYLE;
        if (PIDLIST_ABSOLUTE item = SHBrowseForFolderW(&info)) {
            wchar_t path[MAX_PATH] = {};
            if (SHGetPathFromIDListW(item, path)) {
                SetWindowTextW(m_folderEdit, path);
                SaveConfigFromControls();
            }
            CoTaskMemFree(item);
        }
        break;
    }
    case kBtnLogin: {
        BoostyAuth auth{m_config.cookie, m_config.authHeader};
        if (ShowBoostyLoginDialog(m_window, m_instance, *m_paths, auth)) {
            m_config.cookie = auth.cookie;
            m_config.authHeader = auth.authHeader;
            ConfigStore::Save(*m_paths, m_config);
            SetStatus(L"Токен сохранен");
        }
        break;
    }
    case kBtnClear:
        if (m_queue) {
            m_queue->ClearInactive();
        }
        break;
    case kBtnLogs:
        ShowLogsModal(m_window, m_instance, m_logger ? m_logger->ReadAll() : L"");
        break;
    case kBtnSettings:
        ShowSettings();
        break;
    case kBtnOpenFolder:
        OpenDownloadFolder();
        break;
    case kBtnClearFinished:
        if (m_queue) {
            m_queue->ClearFinished();
        }
        break;
    }
}

void Application::ClickTask(int taskId, UINT action) {
    if (!m_queue) {
        return;
    }
    switch (action) {
    case kTaskCancel:
        m_queue->Cancel(taskId);
        break;
    case kTaskRetry:
        m_queue->Retry(taskId);
        break;
    case kTaskDelete:
    case kTaskClose:
        m_queue->Remove(taskId);
        break;
    default:
        break;
    }
}

void Application::EnqueueText(const std::wstring& text) {
    SaveConfigFromControls();
    const auto urls = ExtractUrls(text);
    if (urls.empty()) {
        SetStatus(L"Нет ссылок");
        if (m_logger) {
            m_logger->Info(L"Download skipped: no URLs");
        }
        return;
    }
    const FfmpegStatus ffmpeg = ResolveFfmpeg(*m_paths, m_config.ffmpegPath);
    const bool ffmpegAvailable = ffmpeg.available;
    size_t added = 0;
    size_t skipped = 0;
    for (const std::wstring& url : urls) {
        if (!IsBoostyPostUrl(url)) {
            ++skipped;
            if (m_logger) {
                m_logger->Info(L"Download skipped: Boosty post URL expected, got " + url);
            }
            continue;
        }
        BoostyDownloadRequest request;
        request.url = url;
        request.outputDirectory = m_config.downloadDir;
        request.auth = {m_config.cookie, m_config.authHeader};
        request.quality = NormalizeQualityForFfmpeg(m_config.quality, ffmpegAvailable);
        request.container = NormalizeContainerForFfmpeg(m_config.container, ffmpegAvailable);
        request.ffmpegPath = ffmpegAvailable ? ffmpeg.executable : std::filesystem::path{};
        m_queue->Enqueue(request, url);
        ++added;
    }
    if (added == 0) {
        SetStatus(L"Нужна ссылка на пост Boosty");
        return;
    }
    std::wstring status = L"Добавлено ссылок: " + std::to_wstring(added);
    if (skipped > 0) {
        status += L", пропущено: " + std::to_wstring(skipped);
    }
    SetStatus(std::move(status));
    SetWindowTextW(m_urlEdit, L"");
}

void Application::EnqueueFromFile(const std::filesystem::path& path) {
    std::ifstream in(path, std::ios::binary);
    if (!in) {
        SetStatus(L"Не удалось прочитать файл");
        return;
    }
    EnqueueText(Utf8ToWide({std::istreambuf_iterator<char>(in), std::istreambuf_iterator<char>()}));
}

void Application::ImportFile() {
    wchar_t fileName[MAX_PATH] = {};
    OPENFILENAMEW ofn = {};
    ofn.lStructSize = sizeof(ofn);
    ofn.hwndOwner = m_window;
    ofn.lpstrFilter = L"Text files\0*.txt\0All files\0*.*\0";
    ofn.lpstrFile = fileName;
    ofn.nMaxFile = MAX_PATH;
    ofn.Flags = OFN_FILEMUSTEXIST | OFN_PATHMUSTEXIST;
    if (GetOpenFileNameW(&ofn)) {
        EnqueueFromFile(fileName);
    }
}

void Application::PasteUrl() {
    if (!OpenClipboard(m_window)) {
        SetStatus(L"Не удалось открыть буфер обмена");
        return;
    }
    HANDLE data = GetClipboardData(CF_UNICODETEXT);
    if (!data) {
        CloseClipboard();
        SetStatus(L"В буфере обмена нет текста");
        return;
    }
    const wchar_t* text = static_cast<const wchar_t*>(GlobalLock(data));
    if (text) {
        const std::wstring clipboardText = text;
        const auto urls = ExtractUrls(clipboardText);
        if (urls.size() > 1) {
            GlobalUnlock(data);
            CloseClipboard();
            EnqueueText(clipboardText);
            return;
        }
        SetWindowTextW(m_urlEdit, (urls.empty() ? clipboardText : urls.front()).c_str());
        SendMessageW(m_urlEdit, EM_SETSEL, 0, -1);
        GlobalUnlock(data);
    }
    CloseClipboard();
}

void Application::OpenDownloadFolder() {
    SaveConfigFromControls();
    std::filesystem::create_directories(m_config.downloadDir);
    ShellExecuteW(m_window, L"open", m_config.downloadDir.c_str(), nullptr, nullptr, SW_SHOWNORMAL);
}

void Application::ShowSettings() {
    SaveConfigFromControls();
    if (ShowSettingsModal(m_window, m_instance, *m_paths, m_config)) {
        if (m_queue) {
            m_queue->SetMaxParallelDownloads(m_config.maxParallelDownloads);
        }
        ConfigStore::Save(*m_paths, m_config);
        SetStatus(L"Настройки сохранены");
    }
}

void Application::StartAutoUpdateCheck() {
    if (!m_config.autoUpdateCheck || !m_paths || m_updateWorker.joinable()) {
        return;
    }
    const HWND window = m_window;
    m_updateWorker = std::jthread([window](std::stop_token stopToken) {
        try {
            ReleaseAssetInfo release = AppUpdateService::CheckLatestRelease(stopToken);
            if (stopToken.stop_requested() || !ShouldInstallAppUpdate(release)) {
                return;
            }
            auto* payload = new ReleaseAssetInfo(std::move(release));
            if (!PostMessageW(window, kUpdateFoundMessage, 0, reinterpret_cast<LPARAM>(payload))) {
                delete payload;
            }
        } catch (...) {
        }
    });
}

void Application::SaveConfigFromControls() {
    m_config.downloadDir = GetText(m_folderEdit);
    if (m_queue) {
        m_queue->SetMaxParallelDownloads(m_config.maxParallelDownloads);
    }
    ConfigStore::Save(*m_paths, m_config);
}

void Application::LoadDownloadQueue() {
    if (!m_queue || !m_paths) {
        return;
    }
    try {
        std::vector<DownloadTaskSnapshot> tasks = DownloadQueueStore::Load(*m_paths);
        const FfmpegStatus ffmpeg = ResolveFfmpeg(*m_paths, m_config.ffmpegPath);
        const bool ffmpegAvailable = ffmpeg.available;
        for (DownloadTaskSnapshot& task : tasks) {
            task.request.auth = {m_config.cookie, m_config.authHeader};
            if (task.request.outputDirectory.empty()) {
                task.request.outputDirectory = m_config.downloadDir;
            }
            if (task.request.quality.empty()) {
                task.request.quality = m_config.quality;
            }
            task.request.quality = NormalizeQualityForFfmpeg(task.request.quality, ffmpegAvailable);
            if (task.request.container.empty()) {
                task.request.container = m_config.container;
            }
            task.request.container = NormalizeContainerForFfmpeg(task.request.container, ffmpegAvailable);
            task.request.ffmpegPath = ffmpegAvailable ? ffmpeg.executable : std::filesystem::path{};
        }
        if (!tasks.empty()) {
            m_queue->ImportSnapshots(tasks);
            if (m_logger) {
                m_logger->Info(L"Restored download queue tasks: " + std::to_wstring(tasks.size()));
            }
        }
        m_lastSavedQueueRevision = m_queue->Revision();
    } catch (const std::exception&) {
        if (m_logger) {
            m_logger->Error(L"Failed to restore download queue");
        }
    }
}

void Application::SaveDownloadQueue(bool forShutdown) {
    if (!m_queue || !m_paths) {
        return;
    }
    try {
        DownloadQueueStore::Save(
            *m_paths,
            forShutdown ? m_queue->ExportSnapshotsForShutdown() : m_queue->ExportSnapshots()
        );
        m_lastSavedQueueRevision = m_queue->Revision();
    } catch (const std::exception&) {
        if (m_logger) {
            m_logger->Error(L"Failed to save download queue");
        }
    }
}

bool Application::RefreshStatus() {
    if (!m_queue) {
        return false;
    }
    m_queue->RefreshDynamicStats();
    const std::uint64_t revision = m_queue->Revision();
    if (revision != m_lastSavedQueueRevision) {
        SaveDownloadQueue(false);
    }
    if (revision == m_lastRenderedQueueRevision) {
        return false;
    }
    m_lastRenderedQueueRevision = revision;
    const auto tasks = m_queue->Snapshot();
    RECT client{};
    GetClientRect(m_window, &client);
    m_queueScrollOffset = std::clamp(
        m_queueScrollOffset,
        0,
        QueueMaxScrollOffset(QueuePanelRectForClient(client), tasks.size())
    );
    const auto active = std::count_if(tasks.begin(), tasks.end(), [](const auto& task) {
        return task.state == DownloadTaskState::Preparing || task.state == DownloadTaskState::Downloading;
    });
    const auto queued = std::count_if(tasks.begin(), tasks.end(), [](const auto& task) {
        return task.state == DownloadTaskState::Queued;
    });
    const auto completed = std::count_if(tasks.begin(), tasks.end(), [](const auto& task) {
        return task.state == DownloadTaskState::Completed;
    });
    if (active > 0 || queued > 0 || completed > 0) {
        m_status = L"Активно: " + std::to_wstring(active) +
            L", в очереди: " + std::to_wstring(queued) +
            L", завершено: " + std::to_wstring(completed);
    }
    return true;
}

void Application::SetStatus(std::wstring value) {
    m_status = std::move(value);
    if (m_window) {
        InvalidateRect(m_window, nullptr, FALSE);
    }
}

bool Application::ScrollQueue(int rows) {
    if (!m_queue || rows == 0) {
        return false;
    }
    RECT client{};
    GetClientRect(m_window, &client);
    const RECT queuePanel = QueuePanelRectForClient(client);
    const int maxOffset = QueueMaxScrollOffset(queuePanel, m_queue->Snapshot().size());
    const int previousOffset = m_queueScrollOffset;
    m_queueScrollOffset = std::clamp(m_queueScrollOffset + rows, 0, maxOffset);
    if (m_queueScrollOffset == previousOffset) {
        return false;
    }
    InvalidateRect(m_window, &queuePanel, FALSE);
    return true;
}
