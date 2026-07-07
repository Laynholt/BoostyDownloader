#include "LoginDialog.h"

#ifdef BOOSTY_HAS_WEBVIEW2

#include "Config.h"
#include "Text.h"

#include <WebView2.h>
#include <nlohmann/json.hpp>
#include <wrl.h>

#include <string>
#include <vector>

using Microsoft::WRL::Callback;
using Microsoft::WRL::ComPtr;

namespace {

constexpr UINT kSaveToken = 1001;
constexpr UINT kClose = 1002;
constexpr UINT kAuthSaved = WM_APP + 1;

struct LoginState {
    HINSTANCE instance = nullptr;
    AppPaths paths;
    BoostyAuth* auth = nullptr;
    ComPtr<ICoreWebView2Controller> controller;
    ComPtr<ICoreWebView2> webview;
    bool saved = false;

    LoginState(HINSTANCE instanceValue, const AppPaths& pathsValue, BoostyAuth* authValue)
        : instance(instanceValue), paths(pathsValue), auth(authValue) {
    }
};

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
    rect.top = 46;
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
        CreateWindowW(L"BUTTON", L"Сохранить временный токен", WS_CHILD | WS_VISIBLE, 10, 8, 210, 30, window, reinterpret_cast<HMENU>(static_cast<INT_PTR>(kSaveToken)), state->instance, nullptr);
        CreateWindowW(L"BUTTON", L"Закрыть", WS_CHILD | WS_VISIBLE, 230, 8, 90, 30, window, reinterpret_cast<HMENU>(static_cast<INT_PTR>(kClose)), state->instance, nullptr);
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
        return 0;
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
