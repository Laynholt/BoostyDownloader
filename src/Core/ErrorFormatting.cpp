#include "ErrorFormatting.h"

#include "Text.h"

#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <winsock2.h>
#include <winhttp.h>

#include <array>
#include <cerrno>
#include <cstdlib>
#include <string_view>
#include <utility>

namespace {

constexpr const wchar_t* kUnknownError = L"Не удалось выполнить операцию из-за непредвиденной ошибки.";

std::wstring SystemErrorMessage(unsigned long code) {
    std::array<wchar_t, 2048> buffer{};
    DWORD size = FormatMessageW(FORMAT_MESSAGE_FROM_SYSTEM | FORMAT_MESSAGE_IGNORE_INSERTS,
        nullptr, code, 0, buffer.data(), static_cast<DWORD>(buffer.size()), nullptr);
    if (size == 0) {
        size = FormatMessageW(FORMAT_MESSAGE_FROM_HMODULE | FORMAT_MESSAGE_IGNORE_INSERTS,
            GetModuleHandleW(L"winhttp.dll"), code, 0, buffer.data(), static_cast<DWORD>(buffer.size()), nullptr);
    }
    std::wstring message(buffer.data(), size);
    for (wchar_t& ch : message) {
        if (ch == L'\r' || ch == L'\n' || ch == L'\t') {
            ch = L' ';
        }
    }
    return Trim(std::move(message));
}

unsigned long ErrorCodeAfter(const std::wstring& detail, std::wstring_view marker) {
    const size_t found = detail.find(marker);
    if (found == std::wstring_view::npos) {
        return 0;
    }
    const wchar_t* start = detail.c_str() + found + marker.size();
    if (*start < L'0' || *start > L'9') {
        return 0;
    }
    wchar_t* end = nullptr;
    errno = 0;
    const unsigned long code = std::wcstoul(start, &end, 10);
    if (errno == ERANGE || (*end != L'\0' && *end != L')' && *end != L':' && *end != L' ')) {
        return 0;
    }
    return code;
}

std::wstring WindowsErrorSummary(unsigned long code) {
    switch (code) {
    case ERROR_WINHTTP_TIMEOUT:
    case WSAETIMEDOUT:
        return L"Истекло время ожидания ответа сервера. Проверьте соединение и повторите попытку.";
    case ERROR_WINHTTP_NAME_NOT_RESOLVED:
    case WSAHOST_NOT_FOUND:
        return L"Не удалось определить адрес сервера. Проверьте интернет и настройки DNS.";
    case ERROR_WINHTTP_CANNOT_CONNECT:
    case WSAECONNREFUSED:
        return L"Не удалось подключиться к серверу. Проверьте интернет, прокси или VPN.";
    case ERROR_WINHTTP_CONNECTION_ERROR:
    case ERROR_CONNECTION_ABORTED:
    case WSAECONNRESET:
    case WSAECONNABORTED:
        return L"Соединение с сервером прервано. Повторите попытку после восстановления связи.";
    case ERROR_NO_NETWORK:
    case ERROR_NETWORK_UNREACHABLE:
    case ERROR_HOST_UNREACHABLE:
    case ERROR_NOT_CONNECTED:
    case WSAENETDOWN:
    case WSAENETUNREACH:
    case WSAEHOSTUNREACH:
        return L"Сеть недоступна. Проверьте подключение к интернету.";
    case ERROR_WINHTTP_SECURE_FAILURE:
    case ERROR_WINHTTP_SECURE_CHANNEL_ERROR:
    case ERROR_WINHTTP_SECURE_CERT_DATE_INVALID:
    case ERROR_WINHTTP_SECURE_CERT_CN_INVALID:
    case ERROR_WINHTTP_SECURE_INVALID_CA:
    case ERROR_WINHTTP_SECURE_INVALID_CERT:
    case ERROR_WINHTTP_SECURE_CERT_REVOKED:
    case ERROR_WINHTTP_SECURE_CERT_REV_FAILED:
    case ERROR_WINHTTP_SECURE_CERT_WRONG_USAGE:
        return L"Не удалось установить защищённое соединение. Проверьте дату, сертификаты и настройки сети.";
    case ERROR_WINHTTP_LOGIN_FAILURE:
        return L"Не удалось пройти авторизацию на сервере или прокси. Проверьте данные входа.";
    case ERROR_WINHTTP_INVALID_URL:
    case ERROR_WINHTTP_UNRECOGNIZED_SCHEME:
        return L"Некорректная ссылка. Используйте полный адрес http:// или https://.";
    case ERROR_WINHTTP_INVALID_SERVER_RESPONSE:
        return L"Сервер вернул некорректный ответ. Повторите попытку позже.";
    case ERROR_WINHTTP_REDIRECT_FAILED:
        return L"Не удалось перейти по перенаправлению сервера. Проверьте ссылку.";
    case ERROR_WINHTTP_AUTODETECTION_FAILED:
    case ERROR_WINHTTP_AUTO_PROXY_SERVICE_ERROR:
    case ERROR_WINHTTP_BAD_AUTO_PROXY_SCRIPT:
    case ERROR_WINHTTP_UNABLE_TO_DOWNLOAD_SCRIPT:
        return L"Не удалось настроить соединение через прокси. Проверьте настройки сети.";
    case ERROR_WINHTTP_OPERATION_CANCELLED:
    case ERROR_CANCELLED:
        return L"Операция отменена.";
    case ERROR_ACCESS_DENIED:
        return L"Нет прав доступа. Проверьте разрешения на файл или папку.";
    case ERROR_DISK_FULL:
    case ERROR_HANDLE_DISK_FULL:
        return L"Недостаточно свободного места на диске. Освободите место и повторите попытку.";
    case ERROR_SHARING_VIOLATION:
    case ERROR_LOCK_VIOLATION:
        return L"Файл занят другой программой. Закройте её и повторите попытку.";
    case ERROR_FILE_NOT_FOUND:
    case ERROR_PATH_NOT_FOUND:
        return L"Файл или папка не найдены. Проверьте указанный путь.";
    default:
        const std::wstring message = SystemErrorMessage(code);
        return message.empty() ? L"Не удалось выполнить операцию из-за ошибки Windows." : L"Ошибка Windows: " + message;
    }
}

} // namespace

std::wstring FormatErrorSummary(const std::wstring& detail) {
    if (detail.starts_with(L"FFmpeg failed")) {
        return L"FFmpeg не смог обработать видео. Подробности — в логах.";
    }
    if (detail.starts_with(L"HTTP request failed with status ")) {
        const unsigned long status = ErrorCodeAfter(detail, L"HTTP request failed with status ");
        switch (status) {
        case 401: return L"Сервер не принял авторизацию. Войдите заново и обновите токен.";
        case 403: return L"Сервер запретил доступ. Проверьте права доступа или подписку на пост.";
        case 404: case 410: return L"Пост или файл не найден либо удалён. Проверьте ссылку.";
        case 407: return L"Прокси-сервер требует авторизацию. Проверьте настройки прокси.";
        case 408: case 504: return L"Истекло время ожидания ответа сервера. Повторите попытку позже.";
        case 429: return L"Слишком много запросов. Подождите и повторите попытку.";
        default:
            if (status >= 500 && status <= 599) {
                return L"Ошибка на стороне сервера. Повторите попытку позже.";
            }
            return status >= 400 && status <= 499 ? L"Сервер отклонил запрос. Проверьте ссылку и доступ." : kUnknownError;
        }
    }
    const unsigned long code = ErrorCodeAfter(detail, L"Win32 error ");
    if (code != 0) {
        return WindowsErrorSummary(code);
    }
    // These are the error prefixes produced by the download, file and update code.
    const std::pair<std::wstring_view, std::wstring_view> messages[] = {
        {L"Boosty auth is empty", L"Нет данных авторизации Boosty. Войдите и получите токен."},
        {L"not a Boosty URL", L"Это не ссылка Boosty. Укажите ссылку на пост."},
        {L"Boosty post URL expected", L"Нужна ссылка на конкретный пост Boosty."},
        {L"invalid Boosty post URL", L"Некорректная ссылка на пост Boosty. Проверьте адрес."},
        {L"invalid URL", L"Некорректная ссылка. Проверьте адрес."},
        {L"post has no downloadable Boosty video", L"В посте нет доступного для скачивания видео. Проверьте доступ к полной версии поста."},
        {L"post not found or not available", L"Пост не найден или недоступен. Проверьте ссылку и подписку."},
        {L"operation canceled", L"Операция отменена."},
        {L"HTTP request failed", L"Не удалось выполнить сетевой запрос. Проверьте соединение."},
        {L"failed to query HTTP data", L"Не удалось получить данные от сервера. Проверьте соединение."},
        {L"failed to query HTTP status", L"Не удалось определить результат сетевого запроса. Повторите попытку."},
        {L"failed to read HTTP data", L"Не удалось прочитать ответ сервера. Проверьте соединение."},
        {L"failed to connect", L"Не удалось подключиться к серверу. Проверьте соединение."},
        {L"failed to open WinHTTP session", L"Не удалось начать сетевой запрос. Проверьте настройки сети."},
        {L"failed to open HTTP request", L"Не удалось подготовить сетевой запрос. Проверьте ссылку."},
        {L"failed to create download directory", L"Не удалось создать папку загрузки. Проверьте путь и права доступа."},
        {L"failed to open download target", L"Не удалось открыть файл для записи. Проверьте папку загрузки и права доступа."},
        {L"failed to write downloaded data", L"Не удалось записать скачанные данные. Проверьте место на диске и права доступа."},
        {L"failed to commit downloaded file", L"Не удалось сохранить скачанный файл. Проверьте доступ к папке и не занят ли файл."},
        {L"staged download is missing", L"Временный файл загрузки не найден. Повторите скачивание."},
        {L"download size validation failed", L"Файл скачан не полностью или повреждён. Повторите скачивание."},
        {L"FFmpeg not found", L"FFmpeg не найден. Установите его или укажите путь в настройках."},
        {L"failed to start FFmpeg", L"Не удалось запустить FFmpeg. Проверьте путь и права доступа."},
        {L"failed to create FFmpeg progress pipe", L"Не удалось создать канал связи с FFmpeg. Повторите попытку."},
        {L"failed to start process", L"Не удалось запустить программу для установки FFmpeg. Проверьте права доступа."},
        {L"failed to extract FFmpeg archive", L"Не удалось распаковать архив FFmpeg. Проверьте архив и место на диске."},
        {L"process failed", L"Не удалось выполнить установку FFmpeg. Повторите попытку."},
        {L"failed to create extract directory", L"Не удалось создать папку для распаковки FFmpeg. Проверьте права доступа."},
        {L"failed to create FFmpeg target directory", L"Не удалось создать папку установки FFmpeg. Проверьте права доступа."},
        {L"failed to copy FFmpeg binary", L"Не удалось скопировать FFmpeg. Проверьте место на диске и права доступа."},
        {L"ffmpeg.exe was not found in archive", L"В архиве нет ffmpeg.exe. Скачайте другой архив FFmpeg."},
        {L"installed FFmpeg could not be resolved", L"Не удалось найти установленный FFmpeg. Проверьте папку установки."},
        {L"failed to resolve current executable path", L"Не удалось определить путь к приложению для обновления."},
        {L"failed to write update script", L"Не удалось записать сценарий обновления. Проверьте права доступа."},
        {L"failed to start update helper", L"Не удалось запустить обновление приложения. Проверьте права доступа."},
        {L"failed to open file for SHA-256", L"Не удалось прочитать файл для проверки целостности."},
        {L"failed to open SHA-256 provider", L"Не удалось подготовить проверку целостности файла."},
        {L"failed to query SHA-256 object length", L"Не удалось подготовить проверку целостности файла."},
        {L"failed to create SHA-256 hash", L"Не удалось начать проверку целостности файла."},
        {L"failed to update SHA-256 hash", L"Не удалось проверить целостность файла."},
        {L"failed to finish SHA-256 hash", L"Не удалось завершить проверку целостности файла."},
        {L"app update executable was not found", L"В выпуске обновления нет файла приложения."},
        {L"app update checksum asset was not found", L"В выпуске обновления нет файла контрольных сумм."},
        {L"app update checksum was not found", L"Не найдена контрольная сумма обновления. Установка остановлена."},
        {L"app update checksum validation failed", L"Файл обновления повреждён или не соответствует контрольной сумме. Скачайте его заново."},
        {L"downloaded app update executable is missing", L"Скачанный файл обновления не найден. Повторите скачивание."},
        {L"failed to replace queue store file", L"Не удалось сохранить очередь загрузок. Проверьте права доступа к файлу."},
        {L"failed to create queue store directory", L"Не удалось создать папку для сохранения очереди загрузок."},
        {L"failed to open temporary queue store file", L"Не удалось открыть файл для сохранения очереди загрузок."},
        {L"[json.exception.", L"Получен неожиданный формат данных. Не удалось прочитать данные."},
        {L"filesystem error:", L"Не удалось выполнить операцию с файлом или папкой. Проверьте путь, место на диске и права доступа."},
    };
    for (const auto& [prefix, summary] : messages) {
        if (detail.starts_with(prefix)) {
            return std::wstring(summary);
        }
    }
    return kUnknownError;
}

std::wstring FormatErrorDetails(const std::wstring& detail) {
    const std::wstring summary = FormatErrorSummary(detail);
    return detail.empty() ? summary : summary + L" (" + detail + L")";
}

std::wstring WindowsErrorDetails(const std::wstring& context, unsigned long code) {
    const std::wstring message = SystemErrorMessage(code);
    return context + L" (Win32 error " + std::to_wstring(code) +
        (message.empty() ? L"" : L": " + message) + L")";
}
