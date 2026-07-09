#include "LoginDialog.h"

#ifdef BOOSTY_HAS_WEBVIEW2

#include "Config.h"
#include "Text.h"
#include "UiRenderer.h"

#include <WebView2.h>
#include <dwmapi.h>
#include <nlohmann/json.hpp>
#include <windowsx.h>
#include <wrl.h>

#include <string>
#include <vector>

using Microsoft::WRL::Callback;
using Microsoft::WRL::ComPtr;

namespace {

constexpr UINT kSaveToken = 1001;
constexpr UINT kClose = 1002;
constexpr UINT kAuthSaved = WM_APP + 1;

struct LoginButton {
    UINT id = 0;
    RECT rect{};
    std::wstring text;
    bool primary = false;
};

struct LoginState {
    HINSTANCE instance = nullptr;
    AppPaths paths;
    BoostyAuth* auth = nullptr;
    ComPtr<ICoreWebView2Controller> controller;
    ComPtr<ICoreWebView2> webview;
    std::vector<LoginButton> buttons;
    UINT hotButton = 0;
    UINT pressedButton = 0;
    bool saved = false;

    LoginState(HINSTANCE instanceValue, const AppPaths& pathsValue, BoostyAuth* authValue)
        : instance(instanceValue), paths(pathsValue), auth(authValue) {
    }
};

void EnableDarkTitleBar(HWND window) {
    BOOL enabled = TRUE;
    constexpr DWORD kDwmUseImmersiveDarkMode = 20;
    if (FAILED(DwmSetWindowAttribute(window, kDwmUseImmersiveDarkMode, &enabled, sizeof(enabled)))) {
        constexpr DWORD kDwmUseImmersiveDarkModeBefore20H1 = 19;
        DwmSetWindowAttribute(window, kDwmUseImmersiveDarkModeBefore20H1, &enabled, sizeof(enabled));
    }
}

void DrawTextLine(HDC dc, const std::wstring& text, const RECT& rect, int size, COLORREF color, UINT format = DT_LEFT | DT_VCENTER | DT_SINGLELINE) {
    HFONT font = CreateFontW(-MulDiv(size, GetDeviceCaps(dc, LOGPIXELSY), 72), 0, 0, 0, FW_NORMAL, FALSE, FALSE, FALSE, DEFAULT_CHARSET, OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS, CLEARTYPE_QUALITY, DEFAULT_PITCH, L"Segoe UI");
    HGDIOBJ oldFont = SelectObject(dc, font);
    SetBkMode(dc, TRANSPARENT);
    SetTextColor(dc, color);
    RECT copy = rect;
    DrawTextW(dc, text.c_str(), -1, &copy, format);
    SelectObject(dc, oldFont);
    DeleteObject(font);
}

LoginButton* HitButton(LoginState* state, POINT point) {
    if (!state) {
        return nullptr;
    }
    for (LoginButton& button : state->buttons) {
        if (PtInRect(&button.rect, point)) {
            return &button;
        }
    }
    return nullptr;
}

void LayoutButtons(LoginState* state, const RECT& client) {
    if (!state) {
        return;
    }
    constexpr int top = 16;
    constexpr int height = 38;
    constexpr int gap = 14;
    constexpr int rightInset = 20;
    const int closeWidth = 112;
    const int saveWidth = 220;
    state->buttons = {
        {kSaveToken, {client.right - rightInset - closeWidth - gap - saveWidth, top, client.right - rightInset - closeWidth - gap, top + height}, L"Сохранить токен", false},
        {kClose, {client.right - rightInset - closeWidth, top, client.right - rightInset, top + height}, L"Закрыть", true}
    };
}

void PaintLogin(HWND window, LoginState* state, HDC dc) {
    RECT client{};
    GetClientRect(window, &client);
    LayoutButtons(state, client);
    UiRenderer::DrawBackground(dc, client);
    DrawTextLine(dc, L"Boosty авторизация", {20, 12, client.right - 380, 38}, 18, RGB(242, 242, 242));
    DrawTextLine(dc, L"Войдите в Boosty, затем сохраните временный токен.", {20, 38, client.right - 380, 60}, 13, RGB(180, 180, 186));
    if (state) {
        for (const LoginButton& button : state->buttons) {
            UiRenderer::DrawButton(
                dc,
                button.rect,
                button.text.c_str(),
                button.primary,
                state->pressedButton == button.id,
                state->hotButton == button.id,
                false
            );
        }
    }
}

std::wstring CookieValue(ICoreWebView2Cookie* cookie, bool name) {
    LPWSTR raw = nullptr;
    if (FAILED(name ? cookie->get_Name(&raw) : cookie->get_Value(&raw)) || !raw) {
        return {};
    }
    std::wstring result(raw);
    CoTaskMemFree(raw);
    return result;
}

void ResizeWebView(HWND window, LoginState* state) {
    if (!state || !state->controller) {
        return;
    }
    RECT rect = {};
    GetClientRect(window, &rect);
    rect.left = 16;
    rect.top = 72;
    rect.right -= 16;
    rect.bottom -= 16;
    state->controller->put_Bounds(rect);
}

std::wstring ScriptStringResult(LPCWSTR raw) {
    if (!raw) {
        return {};
    }
    try {
        const auto parsed = nlohmann::json::parse(WideToUtf8(raw));
        if (parsed.is_string()) {
            return Utf8ToWide(parsed.get<std::string>());
        }
    } catch (...) {
    }
    return raw;
}

void FinishAuthSave(HWND window, LoginState* state, const std::wstring& cookieHeader, const std::wstring& pageState) {
    const std::wstring token = ExtractAccessTokenFromText(cookieHeader + L"\n" + pageState);
    if (token.empty()) {
        MessageBoxW(window, L"Access token не найден. Залогиньтесь на Boosty и попробуйте снова.", L"Boosty авторизация", MB_ICONWARNING);
        return;
    }
    state->auth->cookie = cookieHeader;
    state->auth->authHeader = L"Bearer " + token;
    state->saved = true;
    MessageBoxW(window, L"Временный токен сохранен.", L"Boosty авторизация", MB_OK);
    PostMessageW(window, kAuthSaved, 0, 0);
}

void SaveAuth(HWND window, LoginState* state) {
    if (!state || !state->webview || !state->auth) {
        return;
    }
    ComPtr<ICoreWebView2_2> webview2;
    if (FAILED(state->webview.As(&webview2))) {
        MessageBoxW(window, L"WebView2 CookieManager недоступен.", L"Boosty авторизация", MB_ICONERROR);
        return;
    }
    ComPtr<ICoreWebView2CookieManager> manager;
    if (FAILED(webview2->get_CookieManager(&manager))) {
        MessageBoxW(window, L"Не удалось прочитать cookies.", L"Boosty авторизация", MB_ICONERROR);
        return;
    }
    manager->GetCookies(
        L"https://boosty.to",
        Callback<ICoreWebView2GetCookiesCompletedHandler>(
            [window, state](HRESULT error, ICoreWebView2CookieList* list) -> HRESULT {
                if (FAILED(error) || !list) {
                    MessageBoxW(window, L"Не удалось получить cookies Boosty.", L"Boosty авторизация", MB_ICONERROR);
                    return S_OK;
                }
                UINT count = 0;
                list->get_Count(&count);
                std::vector<std::pair<std::wstring, std::wstring>> cookies;
                for (UINT i = 0; i < count; ++i) {
                    ComPtr<ICoreWebView2Cookie> cookie;
                    if (SUCCEEDED(list->GetValueAtIndex(i, &cookie)) && cookie) {
                        cookies.push_back({CookieValue(cookie.Get(), true), CookieValue(cookie.Get(), false)});
                    }
                }
                std::wstring header;
                for (const auto& [name, value] : cookies) {
                    if (!name.empty()) {
                        if (!header.empty()) {
                            header += L"; ";
                        }
                        header += name + L"=" + value;
                    }
                }
                state->webview->ExecuteScript(
                    LR"((() => {
                        const items = {};
                        for (let i = 0; i < localStorage.length; i++) {
                            const key = localStorage.key(i);
                            items[key] = localStorage.getItem(key);
                        }
                        return JSON.stringify({ cookie: document.cookie, localStorage: items });
                    })())",
                    Callback<ICoreWebView2ExecuteScriptCompletedHandler>(
                        [window, state, header](HRESULT scriptError, LPCWSTR result) -> HRESULT {
                            FinishAuthSave(window, state, header, SUCCEEDED(scriptError) ? ScriptStringResult(result) : L"");
                            return S_OK;
                        }
                    ).Get()
                );
                return S_OK;
            }
        ).Get()
    );
}

LRESULT CALLBACK LoginProc(HWND window, UINT message, WPARAM wParam, LPARAM lParam) {
    auto* state = reinterpret_cast<LoginState*>(GetWindowLongPtrW(window, GWLP_USERDATA));
    switch (message) {
    case WM_CREATE: {
        const auto* create = reinterpret_cast<CREATESTRUCTW*>(lParam);
        state = reinterpret_cast<LoginState*>(create->lpCreateParams);
        SetWindowLongPtrW(window, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(state));
        EnableDarkTitleBar(window);
        CreateCoreWebView2EnvironmentWithOptions(
            nullptr,
            state->paths.webViewDataDir().c_str(),
            nullptr,
            Callback<ICoreWebView2CreateCoreWebView2EnvironmentCompletedHandler>(
                [window](HRESULT result, ICoreWebView2Environment* env) -> HRESULT {
                    if (FAILED(result) || !env) {
                        MessageBoxW(window, L"Не удалось запустить WebView2 Runtime.", L"Boosty авторизация", MB_ICONERROR);
                        return S_OK;
                    }
                    env->CreateCoreWebView2Controller(
                        window,
                        Callback<ICoreWebView2CreateCoreWebView2ControllerCompletedHandler>(
                            [window](HRESULT controllerResult, ICoreWebView2Controller* controller) -> HRESULT {
                                auto* current = reinterpret_cast<LoginState*>(GetWindowLongPtrW(window, GWLP_USERDATA));
                                if (FAILED(controllerResult) || !controller || !current) {
                                    return S_OK;
                                }
                                current->controller = controller;
                                current->controller->get_CoreWebView2(&current->webview);
                                ResizeWebView(window, current);
                                current->webview->Navigate(L"https://boosty.to");
                                return S_OK;
                            }
                        ).Get()
                    );
                    return S_OK;
                }
            ).Get()
        );
        return 0;
    }
    case WM_SIZE:
        ResizeWebView(window, state);
        InvalidateRect(window, nullptr, FALSE);
        return 0;
    case WM_ERASEBKGND:
        return 1;
    case WM_MOUSEMOVE: {
        POINT point{GET_X_LPARAM(lParam), GET_Y_LPARAM(lParam)};
        const LoginButton* hit = HitButton(state, point);
        const UINT hotButton = hit ? hit->id : 0;
        if (state && state->hotButton != hotButton) {
            state->hotButton = hotButton;
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
        if (LoginButton* hit = HitButton(state, point)) {
            state->pressedButton = hit->id;
            SetCapture(window);
            InvalidateRect(window, nullptr, FALSE);
            return 0;
        }
        break;
    }
    case WM_LBUTTONUP: {
        POINT point{GET_X_LPARAM(lParam), GET_Y_LPARAM(lParam)};
        const UINT pressed = state ? state->pressedButton : 0;
        if (state) {
            state->pressedButton = 0;
        }
        ReleaseCapture();
        InvalidateRect(window, nullptr, FALSE);
        if (pressed != 0) {
            if (LoginButton* hit = HitButton(state, point); hit && hit->id == pressed) {
                if (pressed == kSaveToken) {
                    SaveAuth(window, state);
                } else if (pressed == kClose) {
                    DestroyWindow(window);
                }
            }
            return 0;
        }
        break;
    }
    case WM_PAINT: {
        PAINTSTRUCT ps{};
        HDC dc = BeginPaint(window, &ps);
        RECT client{};
        GetClientRect(window, &client);
        HDC memoryDc = CreateCompatibleDC(dc);
        HBITMAP bitmap = CreateCompatibleBitmap(dc, client.right - client.left, client.bottom - client.top);
        HGDIOBJ oldBitmap = SelectObject(memoryDc, bitmap);
        PaintLogin(window, state, memoryDc);
        BitBlt(dc, 0, 0, client.right - client.left, client.bottom - client.top, memoryDc, 0, 0, SRCCOPY);
        SelectObject(memoryDc, oldBitmap);
        DeleteObject(bitmap);
        DeleteDC(memoryDc);
        EndPaint(window, &ps);
        return 0;
    }
    case WM_COMMAND:
        if (LOWORD(wParam) == kSaveToken) {
            SaveAuth(window, state);
            return 0;
        }
        if (LOWORD(wParam) == kClose) {
            DestroyWindow(window);
            return 0;
        }
        break;
    case kAuthSaved:
        DestroyWindow(window);
        return 0;
    case WM_DESTROY:
        if (state) {
            state->controller.Reset();
            state->webview.Reset();
        }
        return 0;
    }
    return DefWindowProcW(window, message, wParam, lParam);
}

} // namespace

bool ShowBoostyLoginDialog(HWND owner, HINSTANCE instance, const AppPaths& paths, BoostyAuth& auth) {
    const wchar_t* className = L"BoostyLoginWindow";
    WNDCLASSW wc = {};
    wc.lpfnWndProc = LoginProc;
    wc.hInstance = instance;
    wc.hCursor = LoadCursorW(nullptr, IDC_ARROW);
    wc.lpszClassName = className;
    RegisterClassW(&wc);

    LoginState state(instance, paths, &auth);
    HWND window = CreateWindowExW(
        WS_EX_DLGMODALFRAME,
        className,
        L"Boosty авторизация",
        WS_OVERLAPPEDWINDOW | WS_VISIBLE,
        CW_USEDEFAULT,
        CW_USEDEFAULT,
        1100,
        760,
        owner,
        nullptr,
        instance,
        &state
    );
    if (!window) {
        return false;
    }
    EnableWindow(owner, FALSE);
    MSG msg = {};
    while (IsWindow(window) && GetMessageW(&msg, nullptr, 0, 0) > 0) {
        TranslateMessage(&msg);
        DispatchMessageW(&msg);
    }
    if (IsWindow(window)) {
        DestroyWindow(window);
    }
    EnableWindow(owner, TRUE);
    SetForegroundWindow(owner);
    return state.saved;
}

#endif
