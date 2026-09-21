#include "Secrets.h"

#ifdef _WIN32
#include <windows.h>
#include <dpapi.h>
#include <wincrypt.h>
#else
#include "Platform.h"
#include <libsecret/secret.h>
#endif

#include <vector>

namespace Secrets
{
#ifdef _WIN32
    std::string Protect(const std::string& plain)
    {
        DATA_BLOB in{static_cast<DWORD>(plain.size()), reinterpret_cast<BYTE*>(const_cast<char*>(plain.data()))};
        DATA_BLOB out{};
        if (!CryptProtectData(&in, L"AgentChats API key", nullptr, nullptr, nullptr, CRYPTPROTECT_UI_FORBIDDEN, &out))
            return {};
        DWORD len = 0;
        CryptBinaryToStringA(out.pbData, out.cbData, CRYPT_STRING_BASE64 | CRYPT_STRING_NOCRLF, nullptr, &len);
        std::string b64(len, '\0');
        CryptBinaryToStringA(out.pbData, out.cbData, CRYPT_STRING_BASE64 | CRYPT_STRING_NOCRLF, b64.data(), &len);
        LocalFree(out.pbData);
        b64.resize(len);
        return b64;
    }

    std::string Unprotect(const std::string& base64)
    {
        if (base64.empty())
            return {};
        DWORD len = 0;
        if (!CryptStringToBinaryA(base64.c_str(), 0, CRYPT_STRING_BASE64, nullptr, &len, nullptr, nullptr))
            return {};
        std::vector<BYTE> bytes(len);
        CryptStringToBinaryA(base64.c_str(), 0, CRYPT_STRING_BASE64, bytes.data(), &len, nullptr, nullptr);
        DATA_BLOB in{len, bytes.data()};
        DATA_BLOB out{};
        if (!CryptUnprotectData(&in, nullptr, nullptr, nullptr, nullptr, CRYPTPROTECT_UI_FORBIDDEN, &out))
            return {};
        std::string plain(reinterpret_cast<char*>(out.pbData), out.cbData);
        SecureZeroMemory(out.pbData, out.cbData);
        LocalFree(out.pbData);
        return plain;
    }
#else
    // Linux: the desktop keyring (Secret Service, via libsecret). The stored value is only a
    // reference; DPAPI blobs from Windows cannot be read here and must be entered again.
    namespace
    {
        const SecretSchema* Schema()
        {
            static const SecretSchema schema = {"com.agentschat.Secret", SECRET_SCHEMA_NONE,
                                                {{"id", SECRET_SCHEMA_ATTRIBUTE_STRING}, {nullptr, SECRET_SCHEMA_ATTRIBUTE_STRING}},
                                                0, nullptr, nullptr, nullptr, nullptr, nullptr, nullptr, nullptr};
            return &schema;
        }
        constexpr const char* kPrefix = "secret-service:";
    }

    std::string Protect(const std::string& plain)
    {
        const std::string id = Platform::NewId();
        GError* error = nullptr;
        const gboolean ok = secret_password_store_sync(Schema(), SECRET_COLLECTION_DEFAULT, "Agents Chat", plain.c_str(),
                                                       nullptr, &error, "id", id.c_str(), nullptr);
        if (error)
            g_error_free(error);
        return ok ? kPrefix + id : std::string();
    }

    std::string Unprotect(const std::string& reference)
    {
        if (reference.rfind(kPrefix, 0) != 0)
            return {};
        const std::string id = reference.substr(std::string(kPrefix).size());
        GError* error = nullptr;
        gchar* value = secret_password_lookup_sync(Schema(), nullptr, &error, "id", id.c_str(), nullptr);
        if (error)
            g_error_free(error);
        if (!value)
            return {};
        std::string plain(value);
        secret_password_free(value);
        return plain;
    }
#endif
}
