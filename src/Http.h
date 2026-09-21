#pragma once
#include <atomic>
#include <functional>
#include <map>
#include <string>
#include <utility>
#include <vector>

// Minimal HTTPS client on WinHTTP. Bodies are streamed to a callback so
// server-sent events (token streaming) reach the UI as they arrive.
namespace Http
{
    struct Response
    {
        int status = 0;
        std::map<std::string, std::string> headers; // lower-case names
        std::string body;                            // full body when no onData callback
        std::string error;                           // transport error ("" when a status was received)
    };

    using Headers = std::vector<std::pair<std::string, std::string>>;

    Response Request(const std::string& method, const std::string& url, const Headers& headers,
                     const std::string& body, const std::function<void(const std::string& chunk)>& onData,
                     const std::atomic<bool>& cancel);

    // Splits a server-sent-events stream into "data:" payloads. Feed raw chunks;
    // each complete event's data is passed to onEvent.
    class SseParser
    {
    public:
        explicit SseParser(std::function<void(const std::string& data)> onEvent) : m_onEvent(std::move(onEvent)) {}
        void Feed(const std::string& chunk);
        bool Overflowed() const { return m_overflowed; }

    private:
        std::function<void(const std::string&)> m_onEvent;
        std::string m_buffer;
        bool m_overflowed = false;
    };
}
