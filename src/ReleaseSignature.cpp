#include "ReleaseSignature.h"
#include "UpdateKeys.h"

#include <windows.h>
#include <bcrypt.h>
#include <wincrypt.h>

#include <fstream>

namespace fs = std::filesystem;

namespace ReleaseSignature
{
    namespace
    {
        constexpr ULONG kP256Size = 32; // bytes per coordinate / per r and s

        std::vector<unsigned char> Sha256Bytes(const std::string& data)
        {
            BCRYPT_ALG_HANDLE alg = nullptr;
            std::vector<unsigned char> digest(32);
            if (BCryptOpenAlgorithmProvider(&alg, BCRYPT_SHA256_ALGORITHM, nullptr, 0) != 0)
                return {};
            const NTSTATUS st = BCryptHash(alg, nullptr, 0, reinterpret_cast<PUCHAR>(const_cast<char*>(data.data())),
                                           static_cast<ULONG>(data.size()), digest.data(), static_cast<ULONG>(digest.size()));
            BCryptCloseAlgorithmProvider(alg, 0);
            return st == 0 ? digest : std::vector<unsigned char>();
        }

        bool IsHex64(const std::string& s)
        {
            if (s.size() != 64)
                return false;
            for (char c : s)
                if (!((c >= '0' && c <= '9') || (c >= 'a' && c <= 'f')))
                    return false;
            return true;
        }
    }

    std::string ToHex(const std::vector<unsigned char>& bytes)
    {
        static constexpr char hex[] = "0123456789abcdef";
        std::string out;
        for (unsigned char b : bytes)
        {
            out += hex[b >> 4];
            out += hex[b & 15];
        }
        return out;
    }

    std::vector<unsigned char> FromHex(const std::string& hex)
    {
        auto nibble = [](char c) -> int {
            if (c >= '0' && c <= '9') return c - '0';
            if (c >= 'a' && c <= 'f') return c - 'a' + 10;
            if (c >= 'A' && c <= 'F') return c - 'A' + 10;
            return -1;
        };
        std::vector<unsigned char> out;
        if (hex.size() % 2 != 0)
            return out;
        for (size_t i = 0; i < hex.size(); i += 2)
        {
            const int hi = nibble(hex[i]), lo = nibble(hex[i + 1]);
            if (hi < 0 || lo < 0)
                return {};
            out.push_back(static_cast<unsigned char>(hi * 16 + lo));
        }
        return out;
    }

    std::string ToBase64(const std::vector<unsigned char>& bytes)
    {
        DWORD len = 0;
        if (bytes.empty() || !CryptBinaryToStringA(bytes.data(), static_cast<DWORD>(bytes.size()),
                                                   CRYPT_STRING_BASE64 | CRYPT_STRING_NOCRLF, nullptr, &len))
            return {};
        std::string out(len, '\0');
        CryptBinaryToStringA(bytes.data(), static_cast<DWORD>(bytes.size()), CRYPT_STRING_BASE64 | CRYPT_STRING_NOCRLF,
                             out.data(), &len);
        out.resize(len);
        return out;
    }

    std::vector<unsigned char> FromBase64(const std::string& text)
    {
        DWORD len = 0;
        if (text.empty() || !CryptStringToBinaryA(text.c_str(), static_cast<DWORD>(text.size()), CRYPT_STRING_BASE64,
                                                  nullptr, &len, nullptr, nullptr))
            return {};
        std::vector<unsigned char> out(len);
        if (!CryptStringToBinaryA(text.c_str(), static_cast<DWORD>(text.size()), CRYPT_STRING_BASE64, out.data(), &len,
                                  nullptr, nullptr))
            return {};
        out.resize(len);
        return out;
    }

    std::string Sha256File(const fs::path& path)
    {
        BCRYPT_ALG_HANDLE alg = nullptr;
        BCRYPT_HASH_HANDLE hash = nullptr;
        std::string result;
        if (BCryptOpenAlgorithmProvider(&alg, BCRYPT_SHA256_ALGORITHM, nullptr, 0) != 0)
            return {};
        if (BCryptCreateHash(alg, &hash, nullptr, 0, nullptr, 0, 0) == 0)
        {
            std::ifstream in(path, std::ios::binary);
            std::vector<unsigned char> buffer(64 * 1024), digest(32);
            bool ok = static_cast<bool>(in);
            while (ok && in)
            {
                in.read(reinterpret_cast<char*>(buffer.data()), static_cast<std::streamsize>(buffer.size()));
                if (in.gcount() > 0 && BCryptHashData(hash, buffer.data(), static_cast<ULONG>(in.gcount()), 0) != 0)
                    ok = false;
            }
            if (ok && in.eof() && BCryptFinishHash(hash, digest.data(), static_cast<ULONG>(digest.size()), 0) == 0)
                result = ToHex(digest);
            BCryptDestroyHash(hash);
        }
        BCryptCloseAlgorithmProvider(alg, 0);
        return result;
    }

    std::string Message(const std::string& version, const std::string& sha256Hex)
    {
        return "Agents-Chat-release\nversion=" + version + "\nsha256=" + sha256Hex + "\n";
    }

    bool Verify(const std::string& version, const std::string& sha256Hex, const std::string& signatureBase64,
                const std::vector<std::string>& publicKeysHex)
    {
        if (version.empty() || !IsHex64(sha256Hex) || publicKeysHex.empty())
            return false;
        const std::vector<unsigned char> signature = FromBase64(signatureBase64);
        const std::vector<unsigned char> digest = Sha256Bytes(Message(version, sha256Hex));
        if (signature.size() != 2 * kP256Size || digest.size() != 32)
            return false;

        BCRYPT_ALG_HANDLE alg = nullptr;
        if (BCryptOpenAlgorithmProvider(&alg, BCRYPT_ECDSA_P256_ALGORITHM, nullptr, 0) != 0)
            return false;
        bool valid = false;
        for (const std::string& keyHex : publicKeysHex)
        {
            std::vector<unsigned char> blob = FromHex(keyHex);
            if (blob.size() != sizeof(BCRYPT_ECCKEY_BLOB) + 2 * kP256Size)
                continue;
            const auto* header = reinterpret_cast<const BCRYPT_ECCKEY_BLOB*>(blob.data());
            if (header->dwMagic != BCRYPT_ECDSA_PUBLIC_P256_MAGIC || header->cbKey != kP256Size)
                continue;
            BCRYPT_KEY_HANDLE key = nullptr;
            if (BCryptImportKeyPair(alg, nullptr, BCRYPT_ECCPUBLIC_BLOB, &key, blob.data(), static_cast<ULONG>(blob.size()), 0) != 0)
                continue;
            valid = BCryptVerifySignature(key, nullptr, const_cast<PUCHAR>(digest.data()), static_cast<ULONG>(digest.size()),
                                          const_cast<PUCHAR>(signature.data()), static_cast<ULONG>(signature.size()), 0) == 0;
            BCryptDestroyKey(key);
            if (valid)
                break;
        }
        BCryptCloseAlgorithmProvider(alg, 0);
        return valid;
    }

    bool VerifyRelease(const std::string& version, const std::string& sha256Hex, const std::string& signatureBase64)
    {
        return Verify(version, sha256Hex, signatureBase64, TrustedReleaseKeys());
    }

    bool HasTrustedKeys()
    {
        return !TrustedReleaseKeys().empty();
    }

    bool GenerateKeyPair(std::vector<unsigned char>& privateBlob, std::string& publicKeyHex)
    {
        BCRYPT_ALG_HANDLE alg = nullptr;
        BCRYPT_KEY_HANDLE key = nullptr;
        bool ok = false;
        if (BCryptOpenAlgorithmProvider(&alg, BCRYPT_ECDSA_P256_ALGORITHM, nullptr, 0) != 0)
            return false;
        if (BCryptGenerateKeyPair(alg, &key, 256, 0) == 0 && BCryptFinalizeKeyPair(key, 0) == 0)
        {
            ULONG size = 0;
            if (BCryptExportKey(key, nullptr, BCRYPT_ECCPRIVATE_BLOB, nullptr, 0, &size, 0) == 0)
            {
                privateBlob.resize(size);
                if (BCryptExportKey(key, nullptr, BCRYPT_ECCPRIVATE_BLOB, privateBlob.data(), size, &size, 0) == 0)
                {
                    std::vector<unsigned char> pub;
                    ULONG pubSize = 0;
                    if (BCryptExportKey(key, nullptr, BCRYPT_ECCPUBLIC_BLOB, nullptr, 0, &pubSize, 0) == 0)
                    {
                        pub.resize(pubSize);
                        if (BCryptExportKey(key, nullptr, BCRYPT_ECCPUBLIC_BLOB, pub.data(), pubSize, &pubSize, 0) == 0)
                        {
                            publicKeyHex = ToHex(pub);
                            ok = true;
                        }
                    }
                }
            }
        }
        if (key)
            BCryptDestroyKey(key);
        BCryptCloseAlgorithmProvider(alg, 0);
        if (!ok)
            SecureZeroMemory(privateBlob.data(), privateBlob.size());
        return ok;
    }

    std::string Sign(const std::vector<unsigned char>& privateBlob, const std::string& version, const std::string& sha256Hex)
    {
        if (!IsHex64(sha256Hex) || version.empty())
            return {};
        const std::vector<unsigned char> digest = Sha256Bytes(Message(version, sha256Hex));
        BCRYPT_ALG_HANDLE alg = nullptr;
        BCRYPT_KEY_HANDLE key = nullptr;
        std::string result;
        if (BCryptOpenAlgorithmProvider(&alg, BCRYPT_ECDSA_P256_ALGORITHM, nullptr, 0) != 0)
            return {};
        if (BCryptImportKeyPair(alg, nullptr, BCRYPT_ECCPRIVATE_BLOB, &key, const_cast<PUCHAR>(privateBlob.data()),
                                static_cast<ULONG>(privateBlob.size()), 0) == 0)
        {
            std::vector<unsigned char> signature(2 * kP256Size);
            ULONG written = 0;
            if (BCryptSignHash(key, nullptr, const_cast<PUCHAR>(digest.data()), static_cast<ULONG>(digest.size()),
                               signature.data(), static_cast<ULONG>(signature.size()), &written, 0) == 0 &&
                written == signature.size())
                result = ToBase64(signature);
            BCryptDestroyKey(key);
        }
        BCryptCloseAlgorithmProvider(alg, 0);
        return result;
    }
}
