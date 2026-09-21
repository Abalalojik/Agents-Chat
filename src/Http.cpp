#include "Http.h"
#include "Platform.h"

#include <windows.h>
#include <winhttp.h>

#include <algorithm>

namespace Http
{
    namespace
    {
        struct Handle
        {
            HINTERNET h = nullptr;
            ~Handle()
            {
                if (h)
                    WinHttpCloseHandle(h);
            }
        };

        std::string Lower(std::string s)
        {
            std::transform(s.begin(), s.end(), s.begin(), [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
            return s;
        }
    }

    Response Request(const std::string& method, const std::string& url, const Headers& headers,
                     const std::string& body, const std::function<void(const std::string& chunk)>& onData,
                     const std::atomic<bool>& cancel)
    {
        Response resp;
        const std::wstring wurl = Platform::Widen(url);

        URL_COMPONENTS parts{};
        parts.dwStructSize = sizeof(parts);
        wchar_t host[256], path[4096];
        parts.lpszHostName = host;
        parts.dwHostNameLength = 256;
        parts.lpszUrlPath = path;
        parts.dwUrlPathLength = 4096;
        wchar_t extra[4096];
        parts.lpszExtraInfo = extra;
        parts.dwExtraInfoLength = 4096;
        if (!WinHttpCrackUrl(wurl.c_str(), 0, 0, &parts))
        {
            resp.error = "adresse invalide : " + url;
            return resp;
        }
        const std::wstring object = std::wstring(path, parts.dwUrlPathLength) + std::wstring(extra, parts.dwExtraInfoLength);

        Handle session{WinHttpOpen(L"AgentChats/1.0", WINHTTP_ACCESS_TYPE_AUTOMATIC_PROXY, WINHTTP_NO_PROXY_NAME,
                                   WINHTTP_NO_PROXY_BYPASS, 0)};
        if (!session.h)
        {
            resp.error = "WinHttpOpen a échoué";
            return resp;
        }
        WinHttpSetTimeouts(session.h, 15000, 15000, 30000, 180000);

        Handle connect{WinHttpConnect(session.h, std::wstring(host, parts.dwHostNameLength).c_str(), parts.nPort, 0)};
        if (!connect.h)
        {
            resp.error = "connexion impossible à " + url;
            return resp;
        }
        const DWORD flags = parts.nScheme == INTERNET_SCHEME_HTTPS ? WINHTTP_FLAG_SECURE : 0;
        Handle request{WinHttpOpenRequest(connect.h, Platform::Widen(method).c_str(), object.c_str(), nullptr,
                                          WINHTTP_NO_REFERER, WINHTTP_DEFAULT_ACCEPT_TYPES, flags)};
        if (!request.h)
        {
            resp.error = "requête impossible";
            return resp;
        }

        std::wstring headerBlock;
        for (const auto& [name, value] : headers)
            headerBlock += Platform::Widen(name + ": " + value + "\r\n");

        if (!WinHttpSendRequest(request.h, headerBlock.empty() ? WINHTTP_NO_ADDITIONAL_HEADERS : headerBlock.c_str(),
                                headerBlock.empty() ? 0 : static_cast<DWORD>(-1L),
                                body.empty() ? WINHTTP_NO_REQUEST_DATA : const_cast<char*>(body.data()),
                                static_cast<DWORD>(body.size()), static_cast<DWORD>(body.size()), 0) ||
            !WinHttpReceiveResponse(request.h, nullptr))
        {
            resp.error = "échec réseau (erreur " + std::to_string(GetLastError()) + ")";
            return resp;
        }

        DWORD status = 0, size = sizeof(status);
        WinHttpQueryHeaders(request.h, WINHTTP_QUERY_STATUS_CODE | WINHTTP_QUERY_FLAG_NUMBER, WINHTTP_HEADER_NAME_BY_INDEX,
                            &status, &size, WINHTTP_NO_HEADER_INDEX);
        resp.status = static_cast<int>(status);

        DWORD rawSize = 0;
        WinHttpQueryHeaders(request.h, WINHTTP_QUERY_RAW_HEADERS_CRLF, WINHTTP_HEADER_NAME_BY_INDEX, nullptr, &rawSize,
                            WINHTTP_NO_HEADER_INDEX);
        if (rawSize > 0)
        {
            std::wstring raw(rawSize / sizeof(wchar_t), L'\0');
            if (WinHttpQueryHeaders(request.h, WINHTTP_QUERY_RAW_HEADERS_CRLF, WINHTTP_HEADER_NAME_BY_INDEX, raw.data(),
                                    &rawSize, WINHTTP_NO_HEADER_INDEX))
            {
                const std::string text = Platform::Narrow(raw);
                size_t start = 0;
                while (start < text.size())
                {
                    size_t end = text.find("\r\n", start);
                    if (end == std::string::npos)
                        end = text.size();
                    const std::string line = text.substr(start, end - start);
                    const size_t colon = line.find(':');
                    if (colon != std::string::npos)
                    {
                        std::string value = line.substr(colon + 1);
                        value.erase(0, value.find_first_not_of(' '));
                        resp.headers[Lower(line.substr(0, colon))] = value;
                    }
                    start = end + 2;
                }
            }
        }

        // Errors are read whole so the caller can explain them; success streams.
        const bool stream = onData && resp.status >= 200 && resp.status < 300;
        for (;;)
        {
            if (cancel)
            {
                resp.error = "annulé";
                break;
            }
            DWORD available = 0;
            if (!WinHttpQueryDataAvailable(request.h, &available) || available == 0)
                break;
            std::string chunk(available, '\0');
            DWORD read = 0;
            if (!WinHttpReadData(request.h, chunk.data(), available, &read) || read == 0)
                break;
            chunk.resize(read);
            if (stream)
                onData(chunk);
            else
            {
                constexpr size_t kMaxErrorBody = 2 * 1024 * 1024;
                const size_t room = resp.body.size() < kMaxErrorBody ? kMaxErrorBody - resp.body.size() : 0;
                resp.body.append(chunk.data(), std::min(room, chunk.size()));
                if (chunk.size() > room)
                {
                    resp.body += "\n[…réponse tronquée à 2 Mio]";
                    break;
                }
            }
        }
        return resp;
    }

    void SseParser::Feed(const std::string& chunk)
    {
        m_buffer += chunk;
        if (m_buffer.size() > 4 * 1024 * 1024)
        {
            m_buffer.clear();
            m_overflowed = true;
            return;
        }
        for (;;)
        {
            // Events end with a blank line; tolerate CRLF.
            size_t end = m_buffer.find("\n\n");
            size_t sepLen = 2;
            const size_t crlf = m_buffer.find("\r\n\r\n");
            if (crlf != std::string::npos && (end == std::string::npos || crlf < end))
            {
                end = crlf;
                sepLen = 4;
            }
            if (end == std::string::npos)
                return;
            const std::string event = m_buffer.substr(0, end);
            m_buffer.erase(0, end + sepLen);

            std::string data;
            size_t start = 0;
            while (start <= event.size())
            {
                size_t nl = event.find('\n', start);
                if (nl == std::string::npos)
                    nl = event.size();
                std::string line = event.substr(start, nl - start);
                if (!line.empty() && line.back() == '\r')
                    line.pop_back();
                if (line.rfind("data:", 0) == 0)
                {
                    std::string payload = line.substr(5);
                    if (!payload.empty() && payload[0] == ' ')
                        payload.erase(0, 1);
                    if (!data.empty())
                        data += '\n';
                    data += payload;
                }
                start = nl + 1;
            }
            if (!data.empty())
                m_onEvent(data);
        }
    }
}
