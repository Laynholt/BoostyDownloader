#include "WinHttpClient.h"

#include "FileOperations.h"

#include <winhttp.h>

#include <array>
#include <fstream>
#include <sstream>
#include <stdexcept>
#include <system_error>

namespace {

class InternetHandle {
public:
    explicit InternetHandle(HINTERNET handle = nullptr) : m_handle(handle) {}
    ~InternetHandle() { if (m_handle) WinHttpCloseHandle(m_handle); }
    InternetHandle(const InternetHandle&) = delete;
    InternetHandle& operator=(const InternetHandle&) = delete;
    InternetHandle(InternetHandle&& other) noexcept : m_handle(other.m_handle) { other.m_handle = nullptr; }
    InternetHandle& operator=(InternetHandle&& other) noexcept {
        if (this != &other) {
            if (m_handle) WinHttpCloseHandle(m_handle);
            m_handle = other.m_handle;
            other.m_handle = nullptr;
        }
        return *this;
    }
    HINTERNET get() const { return m_handle; }

private:
    HINTERNET m_handle = nullptr;
};

struct UrlParts {
    std::wstring host;
    std::wstring path;
    INTERNET_PORT port = 0;
    bool secure = false;
};

struct HttpRequest {
    InternetHandle session;
    InternetHandle connect;
    InternetHandle request;
    HINTERNET get() const { return request.get(); }
};

std::runtime_error LastError(const char* context) {
    std::ostringstream out;
    out << context << " (Win32 error " << GetLastError() << ")";
    return std::runtime_error(out.str());
}

UrlParts CrackUrl(const std::wstring& url) {
    URL_COMPONENTSW components = {};
    components.dwStructSize = sizeof(components);
    components.dwSchemeLength = static_cast<DWORD>(-1);
    components.dwHostNameLength = static_cast<DWORD>(-1);
    components.dwUrlPathLength = static_cast<DWORD>(-1);
    components.dwExtraInfoLength = static_cast<DWORD>(-1);
    if (!WinHttpCrackUrl(url.c_str(), static_cast<DWORD>(url.size()), 0, &components)) {
        throw LastError("invalid URL");
    }

    UrlParts parts;
    parts.host.assign(components.lpszHostName, components.dwHostNameLength);
    if (components.lpszUrlPath) {
        parts.path.assign(components.lpszUrlPath, components.dwUrlPathLength);
    }
    if (components.lpszExtraInfo) {
        parts.path.append(components.lpszExtraInfo, components.dwExtraInfoLength);
    }
    if (parts.path.empty()) {
        parts.path = L"/";
    }
    parts.port = components.nPort;
    parts.secure = components.nScheme == INTERNET_SCHEME_HTTPS;
    return parts;
}

std::wstring HeaderBlock(const HttpHeaders& headers) {
    std::wstring block;
    for (const auto& [name, value] : headers) {
        if (!name.empty() && !value.empty()) {
            block += name + L": " + value + L"\r\n";
        }
    }
    return block;
}

DWORD QueryStatusCode(HINTERNET request) {
    DWORD status = 0;
    DWORD size = sizeof(status);
    WinHttpQueryHeaders(request, WINHTTP_QUERY_STATUS_CODE | WINHTTP_QUERY_FLAG_NUMBER, WINHTTP_HEADER_NAME_BY_INDEX, &status, &size, WINHTTP_NO_HEADER_INDEX);
    return status;
}

std::wstring QueryHeaderString(HINTERNET request, DWORD query) {
    DWORD size = 0;
    WinHttpQueryHeaders(request, query, WINHTTP_HEADER_NAME_BY_INDEX, nullptr, &size, WINHTTP_NO_HEADER_INDEX);
    if (GetLastError() != ERROR_INSUFFICIENT_BUFFER || size == 0) {
        return {};
    }
    std::wstring value(size / sizeof(wchar_t), L'\0');
    if (!WinHttpQueryHeaders(request, query, WINHTTP_HEADER_NAME_BY_INDEX, value.data(), &size, WINHTTP_NO_HEADER_INDEX)) {
        return {};
    }
    if (!value.empty() && value.back() == L'\0') {
        value.pop_back();
    }
    return value;
}

HttpRequest OpenRequest(const std::wstring& url, const HttpHeaders& headers, int redirectsRemaining = 8) {
    const UrlParts parts = CrackUrl(url);
    InternetHandle session(WinHttpOpen(L"BoostyDownloader/0.1", WINHTTP_ACCESS_TYPE_AUTOMATIC_PROXY, WINHTTP_NO_PROXY_NAME, WINHTTP_NO_PROXY_BYPASS, 0));
    if (!session.get()) {
        throw LastError("failed to open WinHTTP session");
    }
    WinHttpSetTimeouts(session.get(), 15000, 15000, 30000, 30000);

    InternetHandle connect(WinHttpConnect(session.get(), parts.host.c_str(), parts.port, 0));
    if (!connect.get()) {
        throw LastError("failed to connect");
    }
    InternetHandle request(WinHttpOpenRequest(connect.get(), L"GET", parts.path.c_str(), nullptr, WINHTTP_NO_REFERER, WINHTTP_DEFAULT_ACCEPT_TYPES, parts.secure ? WINHTTP_FLAG_SECURE : 0));
    if (!request.get()) {
        throw LastError("failed to open HTTP request");
    }

    DWORD redirectPolicy = WINHTTP_OPTION_REDIRECT_POLICY_ALWAYS;
    WinHttpSetOption(request.get(), WINHTTP_OPTION_REDIRECT_POLICY, &redirectPolicy, sizeof(redirectPolicy));
    const std::wstring block = HeaderBlock(headers);
    const wchar_t* extraHeaders = block.empty() ? WINHTTP_NO_ADDITIONAL_HEADERS : block.c_str();
    const DWORD headerLength = block.empty() ? 0 : static_cast<DWORD>(block.size());
    if (!WinHttpSendRequest(request.get(), extraHeaders, headerLength, WINHTTP_NO_REQUEST_DATA, 0, 0, 0) ||
        !WinHttpReceiveResponse(request.get(), nullptr)) {
        throw LastError("HTTP request failed");
    }

    const DWORD status = QueryStatusCode(request.get());
    if (status >= 300 && status < 400 && redirectsRemaining > 0) {
        const std::wstring location = QueryHeaderString(request.get(), WINHTTP_QUERY_LOCATION);
        if (!location.empty() && location.starts_with(L"http")) {
            return OpenRequest(location, headers, redirectsRemaining - 1);
        }
    }
    if (status >= 400) {
        std::ostringstream out;
        out << "HTTP request failed with status " << status;
        throw std::runtime_error(out.str());
    }

    return {std::move(session), std::move(connect), std::move(request)};
}

std::uint64_t QueryContentLength(HINTERNET request) {
    wchar_t buffer[64] = {};
    DWORD size = sizeof(buffer);
    if (!WinHttpQueryHeaders(request, WINHTTP_QUERY_CONTENT_LENGTH, WINHTTP_HEADER_NAME_BY_INDEX, buffer, &size, WINHTTP_NO_HEADER_INDEX)) {
        return 0;
    }
    return std::wcstoull(buffer, nullptr, 10);
}

void ThrowIfCanceled(const HttpCancelCallback& isCanceled) {
    if (isCanceled && isCanceled()) {
        throw std::runtime_error("operation canceled");
    }
}

} // namespace

std::string WinHttpClient::GetString(const std::wstring& url, const HttpHeaders& headers, const HttpCancelCallback& isCanceled) {
    HttpRequest request = OpenRequest(url, headers);
    std::string result;
    std::array<char, 8192> buffer = {};
    while (true) {
        ThrowIfCanceled(isCanceled);
        DWORD available = 0;
        if (!WinHttpQueryDataAvailable(request.get(), &available)) {
            throw std::runtime_error("failed to query HTTP data");
        }
        if (available == 0) {
            break;
        }
        DWORD read = 0;
        if (!WinHttpReadData(request.get(), buffer.data(), std::min<DWORD>(available, static_cast<DWORD>(buffer.size())), &read)) {
            throw std::runtime_error("failed to read HTTP data");
        }
        result.append(buffer.data(), buffer.data() + read);
    }
    return result;
}

void WinHttpClient::DownloadFile(
    const std::wstring& url,
    const std::filesystem::path& target,
    const HttpHeaders& headers,
    const HttpProgressCallback& onProgress,
    const HttpCancelCallback& isCanceled
) {
    HttpRequest request = OpenRequest(url, headers);
    const std::uint64_t total = QueryContentLength(request.get());

    std::error_code ec;
    std::filesystem::create_directories(target.parent_path(), ec);
    if (ec) {
        throw std::runtime_error("failed to create download directory");
    }

    const std::filesystem::path staged = target.wstring() + L".download";
    std::filesystem::remove(staged, ec);
    std::ofstream out(staged, std::ios::binary | std::ios::trunc);
    if (!out) {
        throw std::runtime_error("failed to open download target");
    }

    try {
        std::array<char, 65536> buffer = {};
        std::uint64_t downloaded = 0;
        while (true) {
            ThrowIfCanceled(isCanceled);
            DWORD available = 0;
            if (!WinHttpQueryDataAvailable(request.get(), &available)) {
                throw std::runtime_error("failed to query HTTP data");
            }
            if (available == 0) {
                break;
            }
            DWORD read = 0;
            if (!WinHttpReadData(request.get(), buffer.data(), std::min<DWORD>(available, static_cast<DWORD>(buffer.size())), &read)) {
                throw std::runtime_error("failed to read HTTP data");
            }
            out.write(buffer.data(), static_cast<std::streamsize>(read));
            if (!out) {
                throw std::runtime_error("failed to write downloaded data");
            }
            downloaded += read;
            if (onProgress) {
                onProgress(downloaded, total);
            }
        }
        out.close();
        CommitDownloadedFile(staged, target, downloaded, total);
    } catch (...) {
        out.close();
        std::filesystem::remove(staged, ec);
        throw;
    }
}
