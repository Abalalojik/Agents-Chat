#include "Secrets.h"

#include <windows.h>
#include <dpapi.h>
#include <wincrypt.h>

#include <vector>

namespace Secrets
{
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
}
