#include "LoginDialog.h"

#include "MessageDialog.h"

bool ShowBoostyLoginDialog(HWND owner, HINSTANCE instance, const AppPaths&, BoostyAuth&) {
    ShowCustomMessageDialog(
        owner,
        instance,
        L"Boosty авторизация",
        L"WebView2 SDK не подключен при сборке.\n\nСоберите с -DWEBVIEW2_SDK_DIR=путь_к_Microsoft.Web.WebView2 или временно вставьте cookie и Authorization в stuff\\config.ini.",
        MessageDialogKind::Info
    );
    return false;
}
