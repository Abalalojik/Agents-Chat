#include "CloudSync.h"
#include "Http.h"
#include "Platform.h"
#include "Secrets.h"

#ifdef _WIN32
#include <winsock2.h>
#include <ws2tcpip.h>
#include <windows.h>
#include <wincrypt.h>
#else
#include <arpa/inet.h>
#include <cerrno>
#include <netinet/in.h>
#include <sys/ioctl.h>
#include <sys/socket.h>
#include <unistd.h>
// The loopback OAuth listener is written against Winsock; POSIX sockets are the same calls.
namespace
{
    using SOCKET = int;
    using u_long = unsigned long;
    constexpr int INVALID_SOCKET = -1;
    constexpr int SOCKET_ERROR = -1;
    constexpr int WSAEWOULDBLOCK = EWOULDBLOCK;
    struct WSADATA {};
    int WSAStartup(int, WSADATA*) { return 0; }
    void WSACleanup() {}
    int WSAGetLastError() { return errno; }
    int closesocket(int s) { return close(s); }
    int ioctlsocket(int s, unsigned long request, u_long* value)
    {
        int v = static_cast<int>(*value);
        return ioctl(s, request, &v);
    }
    constexpr int MAKEWORD(int, int) { return 0; }
}
#endif
#include <nlohmann/json.hpp>

#include <chrono>
#include <algorithm>
#include <cctype>
#include <cstring>
#include <fstream>
#include <iomanip>
#include <sstream>
#include <thread>

using nlohmann::json;
namespace fs = std::filesystem;

namespace
{
    std::string Encode(const std::string& value)
    {
        static constexpr char hex[] = "0123456789ABCDEF";
        std::string out;
        for (unsigned char c : value)
        {
            if (std::isalnum(c) || c == '-' || c == '_' || c == '.' || c == '~') out += static_cast<char>(c);
            else { out += '%'; out += hex[c >> 4]; out += hex[c & 15]; }
        }
        return out;
    }

    std::string Decode(const std::string& value)
    {
        std::string out;
        for (size_t i = 0; i < value.size(); ++i)
        {
            if (value[i] == '%' && i + 2 < value.size())
            {
                const auto hex = [](char c) { return c >= '0' && c <= '9' ? c - '0' : c >= 'A' && c <= 'F' ? c - 'A' + 10 : c - 'a' + 10; };
                out += static_cast<char>((hex(value[i + 1]) << 4) | hex(value[i + 2])); i += 2;
            }
            else out += value[i] == '+' ? ' ' : value[i];
        }
        return out;
    }

    Http::Response FormPost(const std::string& url, const std::string& form, const std::atomic<bool>& cancel)
    {
        return Http::Request("POST", url, {{"Content-Type", "application/x-www-form-urlencoded"}}, form, {}, cancel);
    }

    std::string GraphGet(const std::string& url, const std::string& token, const std::atomic<bool>& cancel,
                         std::string& error)
    {
        const Http::Response r = Http::Request("GET", url, {{"Authorization", "Bearer " + token},
                                                              {"Accept", "application/json"}}, "", {}, cancel);
        if (!r.error.empty()) { error = r.error; return {}; }
        if (r.status < 200 || r.status >= 300)
        {
            error = "Microsoft Graph HTTP " + std::to_string(r.status) + " : " + r.body.substr(0, 500);
            return {};
        }
        return r.body;
    }

    std::string UtcDate(int dayOffset)
    {
        const auto now = std::chrono::system_clock::now() + std::chrono::hours(24 * dayOffset);
        const std::time_t t = std::chrono::system_clock::to_time_t(now);
        std::tm utc{};
#ifdef _WIN32
        gmtime_s(&utc, &t);
#else
        gmtime_r(&t, &utc);
#endif
        std::ostringstream out;
        out << std::put_time(&utc, "%Y-%m-%dT%H:%M:%SZ");
        return out.str();
    }

    std::string Trim(std::string value)
    {
        const auto first = value.find_first_not_of(" \t\r\n");
        const auto last = value.find_last_not_of(" \t\r\n");
        return first == std::string::npos ? std::string() : value.substr(first, last - first + 1);
    }

#ifndef _WIN32
    constexpr const char* kB64 = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
    std::string Base64Decode(std::string value)
    {
        std::replace(value.begin(), value.end(), '-', '+');
        std::replace(value.begin(), value.end(), '_', '/');
        std::string out;
        unsigned buffer = 0;
        int bits = 0;
        for (char c : value)
        {
            if (c == '=')
                break;
            const char* at = std::strchr(kB64, c);
            if (!at || !*at)
                return {};
            buffer = (buffer << 6) | static_cast<unsigned>(at - kB64);
            bits += 6;
            if (bits >= 8)
            {
                bits -= 8;
                out.push_back(static_cast<char>((buffer >> bits) & 0xFF));
            }
        }
        return out;
    }

    std::string Base64Encode(const std::string& value)
    {
        std::string out;
        for (size_t i = 0; i < value.size(); i += 3)
        {
            const unsigned v = (static_cast<unsigned char>(value[i]) << 16) |
                               (i + 1 < value.size() ? static_cast<unsigned char>(value[i + 1]) << 8 : 0) |
                               (i + 2 < value.size() ? static_cast<unsigned char>(value[i + 2]) : 0);
            out.push_back(kB64[(v >> 18) & 63]);
            out.push_back(kB64[(v >> 12) & 63]);
            out.push_back(i + 1 < value.size() ? kB64[(v >> 6) & 63] : '=');
            out.push_back(i + 2 < value.size() ? kB64[v & 63] : '=');
        }
        return out;
    }
#else
    std::string Base64Decode(std::string value)
    {
        std::replace(value.begin(), value.end(), '-', '+');
        std::replace(value.begin(), value.end(), '_', '/');
        while (value.size() % 4) value.push_back('=');
        DWORD size = 0;
        if (!CryptStringToBinaryA(value.c_str(), 0, CRYPT_STRING_BASE64, nullptr, &size, nullptr, nullptr)) return {};
        std::string out(size, '\0');
        if (!CryptStringToBinaryA(value.c_str(), 0, CRYPT_STRING_BASE64,
                                  reinterpret_cast<BYTE*>(out.data()), &size, nullptr, nullptr)) return {};
        out.resize(size);
        return out;
    }

    std::string Base64Encode(const std::string& value)
    {
        DWORD size = 0;
        if (!CryptBinaryToStringA(reinterpret_cast<const BYTE*>(value.data()), static_cast<DWORD>(value.size()),
                                  CRYPT_STRING_BASE64 | CRYPT_STRING_NOCRLF, nullptr, &size)) return {};
        std::string out(size, '\0');
        if (!CryptBinaryToStringA(reinterpret_cast<const BYTE*>(value.data()), static_cast<DWORD>(value.size()),
                                  CRYPT_STRING_BASE64 | CRYPT_STRING_NOCRLF, out.data(), &size)) return {};
        if (!out.empty() && out.back() == '\0') out.pop_back();
        return out;
    }
#endif

    bool ParseSynciUrl(const std::string& url, std::string& requestUrl, std::string& basicCredentials)
    {
        constexpr std::string_view prefix = "https://";
        if (url.rfind(prefix, 0) != 0) return false;
        const size_t authorityStart = prefix.size();
        const size_t path = url.find('/', authorityStart);
        const size_t authorityEnd = path == std::string::npos ? url.size() : path;
        const std::string authority = url.substr(authorityStart, authorityEnd - authorityStart);
        const size_t at = authority.rfind('@');
        const std::string host = authority.substr(at == std::string::npos ? 0 : at + 1);
        if (host != "api.synci.io") return false;
        basicCredentials = at == std::string::npos ? std::string() : authority.substr(0, at);
        requestUrl = "https://api.synci.io" + (path == std::string::npos ? std::string() : url.substr(path));
        return true;
    }
}

CloudSync::CloudSync(fs::path dataRoot)
    : m_root(std::move(dataRoot) / "cloud"), m_configFile(m_root / "microsoft.json"),
      m_cacheFile(m_root / "cache.json"), m_financeCacheFile(m_root / "finance.json")
{
    std::error_code ec;
    fs::create_directories(m_root, ec);
    try
    {
        std::ifstream in(m_configFile, std::ios::binary);
        if (in)
        {
            const json j = json::parse(in);
            m_clientId = j.value("clientId", "");
            m_protectedRefreshToken = j.value("refreshToken", "");
            m_googleClientId = j.value("googleClientId", "");
            m_protectedGoogleSecret = j.value("googleClientSecret", "");
            m_protectedGoogleRefreshToken = j.value("googleRefreshToken", "");
            m_protectedSynciAccessUrl = j.value("synciAccessUrl", "");
            m_status = (m_protectedRefreshToken.empty() && m_protectedGoogleRefreshToken.empty() && m_protectedSynciAccessUrl.empty())
                           ? "identifiants enregistrés ; connexion requise" : "connecté ; synchronisation en attente";
        }
    }
    catch (...) { m_status = "configuration Microsoft illisible"; }
}

CloudSync::~CloudSync()
{
    m_stop = true;
    if (m_worker.joinable()) m_worker.join();
}

bool CloudSync::SaveConfig()
{
    std::string client, refresh, googleId, googleSecret, googleRefresh, synci;
    { std::lock_guard lock(m_mutex); client = m_clientId; refresh = m_protectedRefreshToken;
      googleId = m_googleClientId; googleSecret = m_protectedGoogleSecret; googleRefresh = m_protectedGoogleRefreshToken;
      synci = m_protectedSynciAccessUrl; }
    std::string error;
    return Platform::WriteFileAtomic(m_configFile, json{{"version", 1}, {"clientId", client},
                                                        {"refreshToken", refresh}, {"googleClientId", googleId},
                                                        {"googleClientSecret", googleSecret},
                                                        {"googleRefreshToken", googleRefresh},
                                                        {"synciAccessUrl", synci}}.dump(2), error);
}

bool CloudSync::SaveMicrosoftClientId(const std::string& clientId)
{
    { std::lock_guard lock(m_mutex); m_clientId = clientId; }
    const bool ok = SaveConfig();
    SetStatus(clientId.empty() ? "non configuré" : "Client ID enregistré ; connexion requise");
    return ok;
}

std::string CloudSync::MicrosoftClientId() const { std::lock_guard lock(m_mutex); return m_clientId; }
bool CloudSync::HasMicrosoftAccount() const { std::lock_guard lock(m_mutex); return !m_protectedRefreshToken.empty(); }
std::string CloudSync::GoogleClientId() const { std::lock_guard lock(m_mutex); return m_googleClientId; }
bool CloudSync::HasGoogleCredentials() const { std::lock_guard lock(m_mutex); return !m_googleClientId.empty() && !m_protectedGoogleSecret.empty(); }
bool CloudSync::HasGoogleAccount() const { std::lock_guard lock(m_mutex); return !m_protectedGoogleRefreshToken.empty(); }
bool CloudSync::HasSynciAccount() const { std::lock_guard lock(m_mutex); return !m_protectedSynciAccessUrl.empty(); }
std::string CloudSync::Status() const { std::lock_guard lock(m_mutex); return m_status; }
std::string CloudSync::UserCode() const { std::lock_guard lock(m_mutex); return m_userCode; }
std::string CloudSync::VerificationUrl() const { std::lock_guard lock(m_mutex); return m_verificationUrl; }
void CloudSync::SetStatus(const std::string& value) { std::lock_guard lock(m_mutex); m_status = value; }

void CloudSync::StartMicrosoftLogin()
{
    if (m_busy || MicrosoftClientId().empty()) { if (MicrosoftClientId().empty()) SetStatus("renseigne d'abord le Client ID"); return; }
    if (m_worker.joinable()) m_worker.join();
    m_busy = true;
    m_stop = false;
    m_worker = std::thread(&CloudSync::LoginWorker, this);
}

bool CloudSync::SaveGoogleCredentials(const std::string& clientId, const std::string& clientSecret)
{
    const std::string protectedSecret = clientSecret.empty() ? std::string() : Secrets::Protect(clientSecret);
    if (!clientSecret.empty() && protectedSecret.empty()) { SetStatus("chiffrement du secret Google impossible"); return false; }
    { std::lock_guard lock(m_mutex); m_googleClientId = clientId; m_protectedGoogleSecret = protectedSecret; }
    const bool ok = SaveConfig();
    SetStatus(ok ? "identifiants Google enregistrés ; connexion requise" : "enregistrement Google impossible");
    return ok;
}

void CloudSync::StartGoogleLogin()
{
    if (m_busy || !HasGoogleCredentials()) { if (!HasGoogleCredentials()) SetStatus("renseigne le Client ID et le secret Google"); return; }
    if (m_worker.joinable()) m_worker.join();
    m_busy = true; m_stop = false;
    m_worker = std::thread(&CloudSync::GoogleLoginWorker, this);
}

void CloudSync::StartSynciConnect(const std::string& setupToken)
{
    if (m_busy || Trim(setupToken).empty()) { if (Trim(setupToken).empty()) SetStatus("colle d'abord le jeton SimpleFIN Synci"); return; }
    if (m_worker.joinable()) m_worker.join();
    { std::lock_guard lock(m_mutex); m_pendingSynciSetupToken = Trim(setupToken); }
    m_busy = true; m_stop = false;
    m_worker = std::thread(&CloudSync::SynciConnectWorker, this);
}

void CloudSync::SynciConnectWorker()
{
    try
    {
        std::string setup;
        { std::lock_guard lock(m_mutex); setup.swap(m_pendingSynciSetupToken); }
        const std::string claimUrl = Trim(Base64Decode(setup));
        setup.assign(setup.size(), '\0');
        std::string safeClaim, unused;
        if (!ParseSynciUrl(claimUrl, safeClaim, unused) || !unused.empty())
            throw std::runtime_error("jeton Synci invalide (URL de réclamation non reconnue)");
        SetStatus("connexion Synci en lecture seule…");
        const Http::Response response = Http::Request("POST", safeClaim, {{"Content-Length", "0"}}, "", {}, m_stop);
        if (!response.error.empty() || response.status != 200)
            throw std::runtime_error(response.status == 403 ? "jeton expiré, déjà utilisé ou révoqué" :
                                     (response.error.empty() ? "Synci HTTP " + std::to_string(response.status) : response.error));
        const std::string accessUrl = Trim(response.body);
        std::string safeAccess, credentials;
        if (!ParseSynciUrl(accessUrl, safeAccess, credentials) || credentials.find(':') == std::string::npos)
            throw std::runtime_error("réponse Synci invalide");
        const std::string protectedUrl = Secrets::Protect(accessUrl);
        if (protectedUrl.empty()) throw std::runtime_error("chiffrement de l'accès Synci impossible");
        { std::lock_guard lock(m_mutex); m_protectedSynciAccessUrl = protectedUrl; }
        if (!SaveConfig()) throw std::runtime_error("enregistrement de l'accès Synci impossible");
        SetStatus("Synci connecté en lecture seule");
        m_nextSyncEpoch = 0;
    }
    catch (const std::exception& e) { SetStatus(std::string("échec Synci : ") + e.what()); }
    m_busy = false;
}

void CloudSync::GoogleLoginWorker()
{
    constexpr unsigned short port = 53682;
    WSADATA data{};
    SOCKET listener = INVALID_SOCKET, clientSocket = INVALID_SOCKET;
    try
    {
        if (WSAStartup(MAKEWORD(2, 2), &data) != 0) throw std::runtime_error("initialisation réseau locale impossible");
        listener = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
        sockaddr_in addr{}; addr.sin_family = AF_INET; addr.sin_addr.s_addr = htonl(INADDR_LOOPBACK); addr.sin_port = htons(port);
        if (listener == INVALID_SOCKET || bind(listener, reinterpret_cast<sockaddr*>(&addr), sizeof(addr)) == SOCKET_ERROR || listen(listener, 1) == SOCKET_ERROR)
            throw std::runtime_error("le port local OAuth 53682 est indisponible");
        u_long nonBlocking = 1;
        ioctlsocket(listener, FIONBIO, &nonBlocking);
        std::string id, secret;
        { std::lock_guard lock(m_mutex); id = m_googleClientId; secret = Secrets::Unprotect(m_protectedGoogleSecret); }
        const std::string state = Platform::NewId();
        const std::string redirect = "http://127.0.0.1:" + std::to_string(port) + "/oauth2callback";
        const std::string scope = "https://www.googleapis.com/auth/gmail.readonly https://www.googleapis.com/auth/calendar.readonly";
        const std::string url = "https://accounts.google.com/o/oauth2/v2/auth?client_id=" + Encode(id) +
            "&redirect_uri=" + Encode(redirect) + "&response_type=code&scope=" + Encode(scope) +
            "&access_type=offline&prompt=consent&state=" + Encode(state);
        SetStatus("connexion Google ouverte dans le navigateur…");
        Platform::OpenUrl(url);
        const auto acceptDeadline = std::chrono::steady_clock::now() + std::chrono::minutes(5);
        while (!m_stop && std::chrono::steady_clock::now() < acceptDeadline)
        {
            clientSocket = accept(listener, nullptr, nullptr);
            if (clientSocket != INVALID_SOCKET) break;
            if (WSAGetLastError() != WSAEWOULDBLOCK) break;
            std::this_thread::sleep_for(std::chrono::milliseconds(100));
        }
        if (clientSocket == INVALID_SOCKET) throw std::runtime_error("connexion Google expirée");
        char buf[16384]; const int n = recv(clientSocket, buf, sizeof(buf) - 1, 0);
        if (n <= 0) throw std::runtime_error("retour OAuth Google vide");
        buf[n] = 0; const std::string request(buf);
        const size_t q = request.find('?'), space = request.find(' ', q);
        if (q == std::string::npos || space == std::string::npos) throw std::runtime_error("retour OAuth Google invalide");
        const std::string query = request.substr(q + 1, space - q - 1);
        auto param = [&](const std::string& name) {
            const std::string key = name + "=";
            size_t p = query.find(key);
            if (p == std::string::npos)
                return std::string();
            p += key.size();
            const size_t e = query.find('&', p);
            return Decode(query.substr(p, e == std::string::npos ? std::string::npos : e - p));
        };
        const std::string html = "HTTP/1.1 200 OK\r\nContent-Type: text/html; charset=utf-8\r\nConnection: close\r\n\r\n"
                                 "<html><body><h2>Agents Chat connecté à Google.</h2><p>Tu peux fermer cette fenêtre.</p></body></html>";
        send(clientSocket, html.data(), static_cast<int>(html.size()), 0);
        if (param("state") != state) throw std::runtime_error("état OAuth Google invalide");
        const std::string code = param("code");
        if (code.empty()) throw std::runtime_error(param("error").empty() ? "Google a refusé la connexion" : param("error"));
        const Http::Response token = FormPost("https://oauth2.googleapis.com/token", "client_id=" + Encode(id) +
            "&client_secret=" + Encode(secret) + "&code=" + Encode(code) + "&grant_type=authorization_code&redirect_uri=" + Encode(redirect), m_stop);
        const json t = json::parse(token.body.empty() ? "{}" : token.body);
        if (token.status != 200 || !t.contains("refresh_token")) throw std::runtime_error(t.value("error_description", token.body));
        const std::string protectedToken = Secrets::Protect(t.at("refresh_token").get<std::string>());
        if (protectedToken.empty()) throw std::runtime_error("chiffrement du jeton Google impossible");
        { std::lock_guard lock(m_mutex); m_protectedGoogleRefreshToken = protectedToken; }
        if (!SaveConfig()) throw std::runtime_error("enregistrement du jeton Google impossible");
        SetStatus("Google connecté");
    }
    catch (const std::exception& e) { SetStatus(std::string("échec Google : ") + e.what()); }
    if (clientSocket != INVALID_SOCKET) closesocket(clientSocket);
    if (listener != INVALID_SOCKET) closesocket(listener);
    WSACleanup();
    m_busy = false;
    if (HasGoogleAccount()) m_nextSyncEpoch = 0; // the UI tick starts synchronization after this worker exits
}

void CloudSync::LoginWorker()
{
    const std::string client = MicrosoftClientId();
    const std::string scope = "offline_access Mail.Read Calendars.Read User.Read";
    const Http::Response start = FormPost("https://login.microsoftonline.com/consumers/oauth2/v2.0/devicecode",
                                          "client_id=" + Encode(client) + "&scope=" + Encode(scope), m_stop);
    try
    {
        if (start.status != 200) throw std::runtime_error(start.body.empty() ? start.error : start.body);
        const json d = json::parse(start.body);
        const std::string device = d.at("device_code").get<std::string>();
        const int interval = d.value("interval", 5);
        const int expires = d.value("expires_in", 900);
        {
            std::lock_guard lock(m_mutex);
            m_userCode = d.value("user_code", "");
            m_verificationUrl = d.value("verification_uri", "https://microsoft.com/devicelogin");
            m_status = "ouvre l'adresse et saisis le code " + m_userCode;
        }
        Platform::OpenUrl(VerificationUrl());
        const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(expires);
        while (!m_stop && std::chrono::steady_clock::now() < deadline)
        {
            for (int i = 0; i < interval * 10 && !m_stop; ++i) std::this_thread::sleep_for(std::chrono::milliseconds(100));
            const Http::Response token = FormPost("https://login.microsoftonline.com/consumers/oauth2/v2.0/token",
                "grant_type=urn%3Aietf%3Aparams%3Aoauth%3Agrant-type%3Adevice_code&client_id=" + Encode(client) +
                "&device_code=" + Encode(device), m_stop);
            const json t = json::parse(token.body.empty() ? "{}" : token.body);
            if (token.status == 200 && t.contains("refresh_token"))
            {
                const std::string protectedToken = Secrets::Protect(t.at("refresh_token").get<std::string>());
                if (protectedToken.empty()) throw std::runtime_error("chiffrement du jeton impossible");
                { std::lock_guard lock(m_mutex); m_protectedRefreshToken = protectedToken; m_userCode.clear(); }
                if (!SaveConfig()) throw std::runtime_error("enregistrement du jeton impossible");
                SetStatus("Hotmail personnel connecté");
                m_busy = false;
                m_nextSyncEpoch = 0; // the UI tick starts synchronization after this worker exits
                return;
            }
            const std::string code = t.value("error", "");
            if (code != "authorization_pending" && code != "slow_down")
                throw std::runtime_error(t.value("error_description", code.empty() ? "connexion refusée" : code));
        }
        throw std::runtime_error("connexion expirée ou annulée");
    }
    catch (const std::exception& e) { SetStatus(std::string("échec de connexion : ") + e.what()); }
    m_busy = false;
}

bool CloudSync::RefreshAccessToken(std::string& token, std::string& error)
{
    std::string client, protectedRefresh;
    { std::lock_guard lock(m_mutex); client = m_clientId; protectedRefresh = m_protectedRefreshToken; }
    const std::string refresh = Secrets::Unprotect(protectedRefresh);
    if (client.empty() || refresh.empty()) { error = "compte Microsoft non connecté"; return false; }
    const std::string scope = "offline_access Mail.Read Calendars.Read User.Read";
    const Http::Response r = FormPost("https://login.microsoftonline.com/consumers/oauth2/v2.0/token",
        "client_id=" + Encode(client) + "&grant_type=refresh_token&refresh_token=" + Encode(refresh) +
        "&scope=" + Encode(scope), m_stop);
    try
    {
        const json j = json::parse(r.body);
        if (r.status != 200) { error = j.value("error_description", r.body); return false; }
        token = j.at("access_token").get<std::string>();
        if (j.contains("refresh_token"))
        {
            const std::string rotated = Secrets::Protect(j.at("refresh_token").get<std::string>());
            if (!rotated.empty()) { { std::lock_guard lock(m_mutex); m_protectedRefreshToken = rotated; } SaveConfig(); }
        }
        return true;
    }
    catch (...) { error = r.error.empty() ? "réponse OAuth Microsoft invalide" : r.error; return false; }
}

bool CloudSync::RefreshGoogleAccessToken(std::string& token, std::string& error)
{
    std::string id, secret, refresh;
    { std::lock_guard lock(m_mutex); id = m_googleClientId; secret = Secrets::Unprotect(m_protectedGoogleSecret);
      refresh = Secrets::Unprotect(m_protectedGoogleRefreshToken); }
    if (id.empty() || secret.empty() || refresh.empty()) { error = "compte Google non connecté"; return false; }
    const Http::Response r = FormPost("https://oauth2.googleapis.com/token", "client_id=" + Encode(id) +
        "&client_secret=" + Encode(secret) + "&refresh_token=" + Encode(refresh) + "&grant_type=refresh_token", m_stop);
    try { const json j = json::parse(r.body); if (r.status != 200) { error = j.value("error_description", r.body); return false; }
          token = j.at("access_token").get<std::string>(); return true; }
    catch (...) { error = r.error.empty() ? "réponse OAuth Google invalide" : r.error; return false; }
}

void CloudSync::SyncNow()
{
    if (m_busy || (!HasMicrosoftAccount() && !HasGoogleAccount() && !HasSynciAccount())) return;
    if (m_worker.joinable()) m_worker.join();
    m_busy = true;
    m_stop = false;
    m_worker = std::thread(&CloudSync::SyncWorker, this);
}

bool CloudSync::SyncBlocking()
{
    if (!HasMicrosoftAccount() && !HasGoogleAccount() && !HasSynciAccount())
    {
        SetStatus("aucun compte connecté");
        return false;
    }
    if (m_worker.joinable()) m_worker.join();
    m_busy = true;
    m_stop = false;
    SyncWorker();
    const std::string status = Status();
    std::string error;
    Platform::WriteFileAtomic(m_root / "last-scheduled-sync.txt", Platform::NowIsoUtc() + " — " + status + "\n", error);
    return status.rfind("synchronisé à ", 0) == 0;
}

void CloudSync::SyncWorker()
{
    SetStatus("synchronisation des données personnelles…");
    std::string token, error;
    json allMessages = json::array(), allEvents = json::array();
    if (HasMicrosoftAccount() && !RefreshAccessToken(token, error)) { SetStatus("Microsoft : " + error); m_busy = false; return; }
    const std::string mailUrl = "https://graph.microsoft.com/v1.0/me/messages?$top=100&$orderby=receivedDateTime%20desc&$select=id,subject,from,receivedDateTime,isRead,bodyPreview,webLink";
    const std::string calendarUrl = "https://graph.microsoft.com/v1.0/me/calendarView?startDateTime=" + Encode(UtcDate(-7)) +
        "&endDateTime=" + Encode(UtcDate(60)) + "&$top=250&$orderby=start/dateTime&$select=id,subject,start,end,location,organizer,isAllDay,webLink";
    if (HasMicrosoftAccount())
    {
        const std::string mails = GraphGet(mailUrl, token, m_stop, error);
        if (mails.empty()) { SetStatus("mails Microsoft : " + error); m_busy = false; return; }
        const std::string calendar = GraphGet(calendarUrl, token, m_stop, error);
        if (calendar.empty()) { SetStatus("agenda Microsoft : " + error); m_busy = false; return; }
        for (json m : json::parse(mails).value("value", json::array())) { m["account"] = "Microsoft"; allMessages.push_back(std::move(m)); }
        for (json e : json::parse(calendar).value("value", json::array())) { e["account"] = "Microsoft"; allEvents.push_back(std::move(e)); }
    }
    if (HasGoogleAccount())
    {
        std::string googleToken;
        if (!RefreshGoogleAccessToken(googleToken, error)) { SetStatus("Google : " + error); m_busy = false; return; }
        const std::string listText = GraphGet("https://gmail.googleapis.com/gmail/v1/users/me/messages?maxResults=40&q=newer_than:30d", googleToken, m_stop, error);
        if (listText.empty()) { SetStatus("Gmail : " + error); m_busy = false; return; }
        for (const json& item : json::parse(listText).value("messages", json::array()))
        {
            const std::string detail = GraphGet("https://gmail.googleapis.com/gmail/v1/users/me/messages/" + item.value("id", "") +
                "?format=metadata&metadataHeaders=Subject&metadataHeaders=From&metadataHeaders=Date", googleToken, m_stop, error);
            if (detail.empty()) continue;
            const json g = json::parse(detail); json m{{"id", g.value("id", "")}, {"account", "Google"},
                {"bodyPreview", g.value("snippet", "")}, {"isRead", true}, {"receivedDateTime", ""}};
            for (const json& label : g.value("labelIds", json::array())) if (label == "UNREAD") m["isRead"] = false;
            for (const json& h : g["payload"].value("headers", json::array()))
            {
                const std::string name = h.value("name", ""), value = h.value("value", "");
                if (name == "Subject") m["subject"] = value;
                else if (name == "From") m["from"] = {{"emailAddress", {{"name", value}, {"address", value}}}};
                else if (name == "Date") m["receivedDateTime"] = value;
            }
            allMessages.push_back(std::move(m));
        }
        const std::string eventsText = GraphGet("https://www.googleapis.com/calendar/v3/calendars/primary/events?singleEvents=true&orderBy=startTime&timeMin=" +
            Encode(UtcDate(-7)) + "&timeMax=" + Encode(UtcDate(60)) + "&maxResults=250", googleToken, m_stop, error);
        if (eventsText.empty()) { SetStatus("Google Calendar : " + error); m_busy = false; return; }
        for (const json& g : json::parse(eventsText).value("items", json::array()))
        {
            json e{{"id", g.value("id", "")}, {"account", "Google"}, {"subject", g.value("summary", "(sans titre)")},
                   {"location", {{"displayName", g.value("location", "")}}}, {"webLink", g.value("htmlLink", "")}};
            e["start"] = {{"dateTime", g.contains("start") ? g["start"].value("dateTime", g["start"].value("date", "")) : ""}};
            e["end"] = {{"dateTime", g.contains("end") ? g["end"].value("dateTime", g["end"].value("date", "")) : ""}};
            allEvents.push_back(std::move(e));
        }
    }
    try
    {
        json cache{{"version", 1}, {"syncedAt", Platform::NowIsoUtc()}, {"provider", "Microsoft et Google"},
                   {"messages", std::move(allMessages)}, {"events", std::move(allEvents)}};
        const std::string protectedCache = Secrets::Protect(cache.dump());
        if (protectedCache.empty()) throw std::runtime_error("chiffrement du cache impossible");
        std::string writeError;
        if (!Platform::WriteFileAtomic(m_cacheFile, json{{"version", 1}, {"protected", protectedCache}}.dump(2), writeError))
            throw std::runtime_error(writeError);

        if (HasSynciAccount())
        {
            std::string protectedUrl;
            { std::lock_guard lock(m_mutex); protectedUrl = m_protectedSynciAccessUrl; }
            const std::string accessUrl = Secrets::Unprotect(protectedUrl);
            std::string safeAccess, credentials;
            if (!ParseSynciUrl(accessUrl, safeAccess, credentials) || credentials.empty())
                throw std::runtime_error("accès Synci local illisible");
            const auto start = static_cast<long long>(std::time(nullptr)) - 90LL * 24 * 60 * 60;
            const std::string url = safeAccess + "/accounts?start-date=" + std::to_string(start) + "&pending=1";
            const Http::Response finance = Http::Request("GET", url,
                {{"Authorization", "Basic " + Base64Encode(credentials)}, {"Accept", "application/json"}}, "", {}, m_stop);
            if (!finance.error.empty() || finance.status != 200)
                throw std::runtime_error(finance.status == 403 ? "accès Synci révoqué" :
                    (finance.error.empty() ? "Synci HTTP " + std::to_string(finance.status) : finance.error));
            json financeCache = json::parse(finance.body);
            financeCache["syncedAt"] = Platform::NowIsoUtc();
            financeCache["version"] = 1;
            const std::string protectedFinance = Secrets::Protect(financeCache.dump());
            if (protectedFinance.empty()) throw std::runtime_error("chiffrement du cache bancaire impossible");
            if (!Platform::WriteFileAtomic(m_financeCacheFile,
                    json{{"version", 1}, {"protected", protectedFinance}}.dump(2), writeError))
                throw std::runtime_error(writeError);
        }
        SetStatus("synchronisé à " + Platform::NowIsoUtc());
        m_nextSyncEpoch = static_cast<long long>(std::time(nullptr)) + 600;
    }
    catch (const std::exception& e) { SetStatus(std::string("cache impossible : ") + e.what()); }
    m_busy = false;
}

void CloudSync::Tick()
{
    if ((HasMicrosoftAccount() || HasGoogleAccount() || HasSynciAccount()) && !m_busy && static_cast<long long>(std::time(nullptr)) >= m_nextSyncEpoch)
        SyncNow();
}

void CloudSync::Disconnect()
{
    m_stop = true;
    if (m_worker.joinable()) m_worker.join();
    { std::lock_guard lock(m_mutex); m_protectedRefreshToken.clear(); m_protectedGoogleRefreshToken.clear(); m_userCode.clear(); }
    SaveConfig();
    std::error_code ec;
    fs::remove(m_cacheFile, ec);
    m_nextSyncEpoch = 0;
    SetStatus("déconnecté");
}

void CloudSync::DisconnectSynci()
{
    m_stop = true;
    if (m_worker.joinable()) m_worker.join();
    { std::lock_guard lock(m_mutex); m_protectedSynciAccessUrl.clear(); m_pendingSynciSetupToken.clear(); }
    SaveConfig();
    std::error_code ec;
    fs::remove(m_financeCacheFile, ec);
    m_nextSyncEpoch = 0;
    SetStatus("Synci déconnecté localement ; révoque aussi la destination dans Synci si nécessaire");
}
