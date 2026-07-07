#include "Application.h"

#include "LoginDialog.h"
#include "Text.h"
#include "UiRenderer.h"

#include <commdlg.h>
#include <gdiplus.h>
#include <shellapi.h>
#include <shlobj.h>
#include <windowsx.h>

#include <algorithm>
#include <fstream>
#include <sstream>

namespace {

constexpr UINT_PTR kRefreshTimer = 1;
constexpr UINT kBtnDownload = 1;
constexpr UINT kBtnImport = 2;
constexpr UINT kBtnLogin = 3;
constexpr UINT kBtnBrowse = 4;
constexpr UINT kBtnClear = 5;
constexpr UINT kBtnLogs = 6;

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

std::wstring BytesText(std::uint64_t value) {
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

void DrawTextLine(HDC dc, const std::wstring& text, RECT rect, int size, COLORREF color, UINT format = DT_LEFT | DT_VCENTER | DT_SINGLELINE | DT_END_ELLIPSIS) {
    SetBkMode(dc, TRANSPARENT);
    SetTextColor(dc, color);
    HFONT font = CreateFontW(-size, 0, 0, 0, FW_NORMAL, FALSE, FALSE, FALSE, DEFAULT_CHARSET, OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS, CLEARTYPE_QUALITY, DEFAULT_PITCH, L"Segoe UI");
    HGDIOBJ old = SelectObject(dc, font);
    DrawTextW(dc, text.c_str(), -1, &rect, format);
    SelectObject(dc, old);
    DeleteObject(font);
}

} // namespace

int Application::Run(HINSTANCE instance, int showCommand) {
    m_instance = instance;
    Gdiplus::GdiplusStartupInput gdiplusInput;
    ULONG_PTR gdiplusToken = 0;
    Gdiplus::GdiplusStartup(&gdiplusToken, &gdiplusInput, nullptr);
    CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED);

    if (!CreateMainWindow(showCommand)) {
        return 1;
    }

    MSG message = {};
    while (GetMessageW(&message, nullptr, 0, 0) > 0) {
        TranslateMessage(&message);
        DispatchMessageW(&message);
    }

    m_queue.reset();
    CoUninitialize();
    Gdiplus::GdiplusShutdown(gdiplusToken);
    return static_cast<int>(message.wParam);
}

bool Application::CreateMainWindow(int showCommand) {
    WNDCLASSW wc = {};
    wc.lpfnWndProc = Application::WindowProc;
    wc.hInstance = m_instance;
    wc.hCursor = LoadCursorW(nullptr, IDC_ARROW);
    wc.lpszClassName = L"BoostyDownloaderWindow";
    RegisterClassW(&wc);

    m_window = CreateWindowExW(
        0,
        wc.lpszClassName,
        L"Boosty Downloader",
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
    case WM_SIZE:
        Layout();
        InvalidateRect(m_window, nullptr, FALSE);
        return 0;
    case WM_ERASEBKGND:
        return 1;
    case WM_TIMER:
        if (RefreshStatus()) {
            InvalidateRect(m_window, nullptr, FALSE);
        }
        return 0;
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
        Button* hit = HitButton(point);
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
        return 0;
    case WM_LBUTTONDOWN: {
        POINT point{GET_X_LPARAM(lParam), GET_Y_LPARAM(lParam)};
        if (Button* hit = HitButton(point)) {
            m_pressedButton = static_cast<int>(hit->id);
            SetCapture(m_window);
            InvalidateButton(m_pressedButton);
        }
        return 0;
    }
    case WM_LBUTTONUP: {
        POINT point{GET_X_LPARAM(lParam), GET_Y_LPARAM(lParam)};
        const int pressed = m_pressedButton;
        m_pressedButton = 0;
        ReleaseCapture();
        InvalidateButton(pressed);
        if (Button* hit = HitButton(point); hit && static_cast<int>(hit->id) == pressed) {
            Click(hit->id);
        }
        return 0;
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
        if (m_queue) {
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
    m_queue = std::make_unique<DownloadQueue>(m_config.maxParallelDownloads);

    m_urlEdit = CreateWindowExW(0, L"EDIT", L"", WS_CHILD | WS_VISIBLE | WS_TABSTOP | ES_AUTOHSCROLL, 0, 0, 100, 28, m_window, nullptr, m_instance, nullptr);
    m_folderEdit = CreateWindowExW(0, L"EDIT", m_config.downloadDir.c_str(), WS_CHILD | WS_VISIBLE | WS_TABSTOP | ES_AUTOHSCROLL, 0, 0, 100, 28, m_window, nullptr, m_instance, nullptr);
    m_workersEdit = CreateWindowExW(0, L"EDIT", std::to_wstring(m_config.maxParallelDownloads).c_str(), WS_CHILD | WS_VISIBLE | WS_TABSTOP | ES_NUMBER | ES_AUTOHSCROLL, 0, 0, 40, 28, m_window, nullptr, m_instance, nullptr);
    SetFont(m_urlEdit);
    SetFont(m_folderEdit);
    SetFont(m_workersEdit);
    SendMessageW(m_urlEdit, EM_SETMARGINS, EC_LEFTMARGIN | EC_RIGHTMARGIN, MAKELPARAM(10, 10));
    SendMessageW(m_folderEdit, EM_SETMARGINS, EC_LEFTMARGIN | EC_RIGHTMARGIN, MAKELPARAM(10, 10));
    SendMessageW(m_workersEdit, EM_SETMARGINS, EC_LEFTMARGIN | EC_RIGHTMARGIN, MAKELPARAM(8, 8));

    DragAcceptFiles(m_window, TRUE);
    SetTimer(m_window, kRefreshTimer, 200, nullptr);
    SetStatus(L"Готово");
    Layout();
}

void Application::Layout() {
    RECT client = {};
    GetClientRect(m_window, &client);
    const int w = client.right - client.left;
    const int folderRight = std::max(520, w - 500);

    MoveWindow(m_urlEdit, 36, 90, std::max(240, w - 380), 24, TRUE);
    MoveWindow(m_folderEdit, 36, 156, std::max(220, folderRight - 48), 24, TRUE);
    MoveWindow(m_workersEdit, w - 124, 156, 60, 24, TRUE);
    AddButtons();
}

void Application::AddButtons() {
    RECT client = {};
    GetClientRect(m_window, &client);
    const int w = client.right;
    const int folderRight = std::max(520, w - 500);
    const int browseLeft = folderRight + 16;
    m_buttons = {
        {kBtnDownload, {w - 340, 84, w - 184, 120}, L"Скачать", true},
        {kBtnImport, {w - 172, 84, w - 36, 120}, L"TXT", false},
        {kBtnBrowse, {browseLeft, 150, browseLeft + 76, 186}, L"...", false},
        {kBtnLogin, {28, 204, 230, 240}, L"Сохранить токен", false},
        {kBtnClear, {238, 204, 360, 240}, L"Очистить", false},
        {kBtnLogs, {368, 204, 468, 240}, L"Логи", false}
    };
}

void Application::Paint(HDC dc) {
    RECT client = {};
    GetClientRect(m_window, &client);
    UiRenderer::DrawBackground(dc, client);

    DrawTextLine(dc, L"Boosty Downloader", {28, 22, client.right - 28, 52}, 26, RGB(242, 242, 242));
    DrawTextLine(dc, L"URL поста или несколько строк", {28, 60, 420, 80}, 15, RGB(180, 180, 186));
    DrawTextLine(dc, L"Папка загрузки", {28, 126, 220, 146}, 15, RGB(180, 180, 186));
    DrawTextLine(dc, L"Воркеры", {client.right - 204, 154, client.right - 132, 182}, 15, RGB(180, 180, 186));

    const int folderRight = std::max(520, static_cast<int>(client.right) - 500);
    UiRenderer::DrawInputFrame(dc, {24, 82, client.right - 360, 122});
    UiRenderer::DrawInputFrame(dc, {24, 148, folderRight, 188});
    UiRenderer::DrawInputFrame(dc, {client.right - 132, 148, client.right - 56, 188});

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

    RECT queuePanel{20, 262, client.right - 20, client.bottom - 50};
    UiRenderer::DrawPanel(dc, queuePanel);
    DrawTextLine(dc, L"Очередь", {queuePanel.left + 18, queuePanel.top + 12, queuePanel.right - 18, queuePanel.top + 42}, 20, RGB(242, 242, 242));

    const auto tasks = m_queue ? m_queue->Snapshot() : std::vector<DownloadTaskSnapshot>{};
    int y = queuePanel.top + 56;
    if (tasks.empty()) {
        DrawTextLine(dc, L"Перетащите .txt со ссылками или вставьте URL и нажмите Скачать.", {queuePanel.left + 18, y, queuePanel.right - 18, y + 30}, 16, RGB(156, 156, 164));
    }
    for (const auto& task : tasks) {
        RECT row{queuePanel.left + 14, y, queuePanel.right - 14, y + 74};
        UiRenderer::DrawPanel(dc, row);
        DrawTextLine(dc, L"#" + std::to_wstring(task.id) + L"  " + task.title, {row.left + 14, row.top + 8, row.right - 14, row.top + 30}, 16, RGB(242, 242, 242));
        std::wstring status = StateText(task.state) + L" - " + task.statusText;
        if (!task.errorText.empty()) {
            status += L": " + task.errorText;
        }
        DrawTextLine(dc, status, {row.left + 14, row.top + 34, row.right - 210, row.top + 56}, 14, RGB(180, 180, 186));
        if (task.totalBytes > 0) {
            DrawTextLine(dc, BytesText(task.downloadedBytes) + L" / " + BytesText(task.totalBytes), {row.right - 196, row.top + 34, row.right - 14, row.top + 56}, 14, RGB(180, 180, 186), DT_RIGHT | DT_VCENTER | DT_SINGLELINE);
        }
        UiRenderer::DrawProgressBar(dc, {row.left + 14, row.bottom - 12, row.right - 14, row.bottom - 6}, task.percent);
        y += 84;
        if (y + 80 > queuePanel.bottom) {
            break;
        }
    }

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

void Application::Click(UINT id) {
    switch (id) {
    case kBtnDownload:
        EnqueueText(GetText(m_urlEdit));
        break;
    case kBtnImport:
        ImportFile();
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
            m_queue->ClearFinished();
        }
        break;
    case kBtnLogs:
        MessageBoxW(m_window, (m_logger ? m_logger->ReadAll() : L"").c_str(), L"Логи", MB_OK);
        break;
    }
}

void Application::EnqueueText(const std::wstring& text) {
    SaveConfigFromControls();
    const auto urls = SplitLines(text);
    if (urls.empty()) {
        SetStatus(L"Нет ссылок");
        return;
    }
    for (const std::wstring& url : urls) {
        BoostyDownloadRequest request;
        request.url = url;
        request.outputDirectory = m_config.downloadDir;
        request.auth = {m_config.cookie, m_config.authHeader};
        request.quality = m_config.quality;
        m_queue->Enqueue(request, url);
    }
    SetStatus(L"Добавлено ссылок: " + std::to_wstring(urls.size()));
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

void Application::SaveConfigFromControls() {
    m_config.downloadDir = GetText(m_folderEdit);
    const int workers = std::max(1, _wtoi(GetText(m_workersEdit).c_str()));
    m_config.maxParallelDownloads = std::clamp(workers, 1, 16);
    if (m_queue) {
        m_queue->SetMaxParallelDownloads(m_config.maxParallelDownloads);
    }
    ConfigStore::Save(*m_paths, m_config);
}

bool Application::RefreshStatus() {
    if (!m_queue) {
        return false;
    }
    const std::uint64_t revision = m_queue->Revision();
    if (revision == m_lastRenderedQueueRevision) {
        return false;
    }
    m_lastRenderedQueueRevision = revision;
    const auto tasks = m_queue->Snapshot();
    const auto active = std::count_if(tasks.begin(), tasks.end(), [](const auto& task) {
        return task.state == DownloadTaskState::Preparing || task.state == DownloadTaskState::Downloading;
    });
    const auto queued = std::count_if(tasks.begin(), tasks.end(), [](const auto& task) {
        return task.state == DownloadTaskState::Queued;
    });
    if (active > 0 || queued > 0) {
        m_status = L"Активно: " + std::to_wstring(active) + L", в очереди: " + std::to_wstring(queued);
    }
    return true;
}

void Application::SetStatus(std::wstring value) {
    m_status = std::move(value);
    if (m_window) {
        InvalidateRect(m_window, nullptr, FALSE);
    }
}
