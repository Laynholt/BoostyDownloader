#include "LoginDialog.h"

bool ShowBoostyLoginDialog(HWND owner, HINSTANCE, const AppPaths&, BoostyAuth&) {
    MessageBoxW(
        owner,
        L"WebView2 SDK не подключен при сборке.\n\nСоберите с -DWEBVIEW2_SDK_DIR=путь_к_Microsoft.Web.WebView2 или временно вставьте cookie и Authorization в stuff\\config.ini.",
        L"Boosty авторизация",
        MB_ICONINFORMATION
    );
    return false;
}
