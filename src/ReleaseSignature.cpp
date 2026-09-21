#include "ReleaseSignature.h"
#include "UpdateKeys.h"

#ifdef _WIN32
#include <windows.h>
#include <bcrypt.h>
#include <wincrypt.h>
#else
#include <openssl/bn.h>
#include <openssl/core_names.h>
#include <openssl/ec.h>
#include <openssl/evp.h>
#include <openssl/param_build.h>
#include <cstring>
#endif

#include <fstream>

namespace fs = std::filesystem;

namespace ReleaseSignature
{
#ifdef _WIN32
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
#else
    // Linux: OpenSSL 3. Keys and signatures keep the Windows (BCrypt) formats, so a release
    // signed on Windows verifies here: public blob = magic, size, X, Y; private adds d;
    // signature = r || s.
    namespace
    {
        constexpr size_t kP256Size = 32;
        constexpr uint32_t kPublicMagic = 0x31534345;  // "ECS1"
        constexpr uint32_t kPrivateMagic = 0x32534345; // "ECS2"

        std::vector<unsigned char> Sha256Bytes(const std::string& data)
        {
            std::vector<unsigned char> digest(32);
            unsigned int len = 0;
            if (EVP_Digest(data.data(), data.size(), digest.data(), &len, EVP_sha256(), nullptr) != 1 || len != 32)
                return {};
            return digest;
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

        uint32_t ReadLe32(const unsigned char* p) { return p[0] | (p[1] << 8) | (p[2] << 16) | (static_cast<uint32_t>(p[3]) << 24); }
        void WriteLe32(std::vector<unsigned char>& out, uint32_t v)
        {
            for (int i = 0; i < 4; ++i)
                out.push_back(static_cast<unsigned char>(v >> (8 * i)));
        }

        // EVP key from X, Y and optionally d (all 32 bytes, big-endian).
        EVP_PKEY* MakeKey(const unsigned char* x, const unsigned char* y, const unsigned char* d)
        {
            unsigned char point[1 + 2 * kP256Size];
            point[0] = 0x04;
            std::memcpy(point + 1, x, kP256Size);
            std::memcpy(point + 1 + kP256Size, y, kP256Size);
            OSSL_PARAM_BLD* bld = OSSL_PARAM_BLD_new();
            BIGNUM* priv = d ? BN_bin2bn(d, static_cast<int>(kP256Size), nullptr) : nullptr;
            OSSL_PARAM_BLD_push_utf8_string(bld, OSSL_PKEY_PARAM_GROUP_NAME, "prime256v1", 0);
            OSSL_PARAM_BLD_push_octet_string(bld, OSSL_PKEY_PARAM_PUB_KEY, point, sizeof(point));
            if (priv)
                OSSL_PARAM_BLD_push_BN(bld, OSSL_PKEY_PARAM_PRIV_KEY, priv);
            OSSL_PARAM* params = OSSL_PARAM_BLD_to_param(bld);
            EVP_PKEY* key = nullptr;
            EVP_PKEY_CTX* ctx = EVP_PKEY_CTX_new_from_name(nullptr, "EC", nullptr);
            if (ctx && params && EVP_PKEY_fromdata_init(ctx) == 1)
                EVP_PKEY_fromdata(ctx, &key, d ? EVP_PKEY_KEYPAIR : EVP_PKEY_PUBLIC_KEY, params);
            EVP_PKEY_CTX_free(ctx);
            OSSL_PARAM_free(params);
            OSSL_PARAM_BLD_free(bld);
            BN_clear_free(priv);
            return key;
        }

        // r || s  <->  DER
        std::vector<unsigned char> RawToDer(const std::vector<unsigned char>& raw)
        {
            ECDSA_SIG* sig = ECDSA_SIG_new();
            ECDSA_SIG_set0(sig, BN_bin2bn(raw.data(), static_cast<int>(kP256Size), nullptr),
                           BN_bin2bn(raw.data() + kP256Size, static_cast<int>(kP256Size), nullptr));
            unsigned char* der = nullptr;
            const int len = i2d_ECDSA_SIG(sig, &der);
            std::vector<unsigned char> out;
            if (len > 0)
                out.assign(der, der + len);
            OPENSSL_free(der);
            ECDSA_SIG_free(sig);
            return out;
        }

        std::vector<unsigned char> DerToRaw(const std::vector<unsigned char>& der)
        {
            const unsigned char* p = der.data();
            ECDSA_SIG* sig = d2i_ECDSA_SIG(nullptr, &p, static_cast<long>(der.size()));
            std::vector<unsigned char> out;
            if (!sig)
                return out;
            out.resize(2 * kP256Size);
            const bool ok = BN_bn2binpad(ECDSA_SIG_get0_r(sig), out.data(), static_cast<int>(kP256Size)) == static_cast<int>(kP256Size) &&
                            BN_bn2binpad(ECDSA_SIG_get0_s(sig), out.data() + kP256Size, static_cast<int>(kP256Size)) == static_cast<int>(kP256Size);
            ECDSA_SIG_free(sig);
            if (!ok)
                out.clear();
            return out;
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
        if (bytes.empty())
            return {};
        std::string out(4 * ((bytes.size() + 2) / 3), '\0');
        const int n = EVP_EncodeBlock(reinterpret_cast<unsigned char*>(out.data()), bytes.data(), static_cast<int>(bytes.size()));
        out.resize(n > 0 ? static_cast<size_t>(n) : 0);
        return out;
    }

    std::vector<unsigned char> FromBase64(const std::string& text)
    {
        std::string clean;
        for (char c : text)
            if (c != '\r' && c != '\n' && c != ' ')
                clean.push_back(c);
        if (clean.empty() || clean.size() % 4 != 0)
            return {};
        std::vector<unsigned char> out(clean.size() / 4 * 3);
        const int n = EVP_DecodeBlock(out.data(), reinterpret_cast<const unsigned char*>(clean.data()), static_cast<int>(clean.size()));
        if (n < 0)
            return {};
        size_t len = static_cast<size_t>(n);
        if (clean.size() >= 1 && clean[clean.size() - 1] == '=') --len;
        if (clean.size() >= 2 && clean[clean.size() - 2] == '=') --len;
        out.resize(len);
        return out;
    }

    std::string Sha256File(const fs::path& path)
    {
        std::ifstream in(path, std::ios::binary);
        if (!in)
            return {};
        EVP_MD_CTX* ctx = EVP_MD_CTX_new();
        std::string result;
        if (ctx && EVP_DigestInit_ex(ctx, EVP_sha256(), nullptr) == 1)
        {
            std::vector<char> buffer(64 * 1024);
            bool ok = true;
            while (ok && in)
            {
                in.read(buffer.data(), static_cast<std::streamsize>(buffer.size()));
                if (in.gcount() > 0 && EVP_DigestUpdate(ctx, buffer.data(), static_cast<size_t>(in.gcount())) != 1)
                    ok = false;
            }
            std::vector<unsigned char> digest(32);
            unsigned int len = 0;
            if (ok && in.eof() && EVP_DigestFinal_ex(ctx, digest.data(), &len) == 1 && len == 32)
                result = ToHex(digest);
        }
        EVP_MD_CTX_free(ctx);
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
        const std::vector<unsigned char> der = RawToDer(signature);
        for (const std::string& keyHex : publicKeysHex)
        {
            const std::vector<unsigned char> blob = FromHex(keyHex);
            if (blob.size() != 8 + 2 * kP256Size || ReadLe32(blob.data()) != kPublicMagic || ReadLe32(blob.data() + 4) != kP256Size)
                continue;
            EVP_PKEY* key = MakeKey(blob.data() + 8, blob.data() + 8 + kP256Size, nullptr);
            if (!key)
                continue;
            EVP_PKEY_CTX* ctx = EVP_PKEY_CTX_new(key, nullptr);
            const bool valid = ctx && EVP_PKEY_verify_init(ctx) == 1 &&
                               EVP_PKEY_verify(ctx, der.data(), der.size(), digest.data(), digest.size()) == 1;
            EVP_PKEY_CTX_free(ctx);
            EVP_PKEY_free(key);
            if (valid)
                return true;
        }
        return false;
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
        EVP_PKEY* key = EVP_EC_gen("P-256");
        if (!key)
            return false;
        unsigned char point[1 + 2 * kP256Size];
        size_t pointLen = 0;
        BIGNUM* d = nullptr;
        bool ok = EVP_PKEY_get_octet_string_param(key, OSSL_PKEY_PARAM_PUB_KEY, point, sizeof(point), &pointLen) == 1 &&
                  pointLen == sizeof(point) && point[0] == 0x04 && EVP_PKEY_get_bn_param(key, OSSL_PKEY_PARAM_PRIV_KEY, &d) == 1;
        if (ok)
        {
            std::vector<unsigned char> pub;
            WriteLe32(pub, kPublicMagic);
            WriteLe32(pub, static_cast<uint32_t>(kP256Size));
            pub.insert(pub.end(), point + 1, point + sizeof(point));
            privateBlob.clear();
            WriteLe32(privateBlob, kPrivateMagic);
            WriteLe32(privateBlob, static_cast<uint32_t>(kP256Size));
            privateBlob.insert(privateBlob.end(), point + 1, point + sizeof(point));
            privateBlob.resize(privateBlob.size() + kP256Size);
            ok = BN_bn2binpad(d, privateBlob.data() + 8 + 2 * kP256Size, static_cast<int>(kP256Size)) == static_cast<int>(kP256Size);
            publicKeyHex = ToHex(pub);
        }
        BN_clear_free(d);
        EVP_PKEY_free(key);
        if (!ok)
            OPENSSL_cleanse(privateBlob.data(), privateBlob.size());
        return ok;
    }

    std::string Sign(const std::vector<unsigned char>& privateBlob, const std::string& version, const std::string& sha256Hex)
    {
        if (!IsHex64(sha256Hex) || version.empty() || privateBlob.size() != 8 + 3 * kP256Size ||
            ReadLe32(privateBlob.data()) != kPrivateMagic)
            return {};
        const std::vector<unsigned char> digest = Sha256Bytes(Message(version, sha256Hex));
        EVP_PKEY* key = MakeKey(privateBlob.data() + 8, privateBlob.data() + 8 + kP256Size, privateBlob.data() + 8 + 2 * kP256Size);
        if (!key)
            return {};
        std::string result;
        EVP_PKEY_CTX* ctx = EVP_PKEY_CTX_new(key, nullptr);
        size_t len = 0;
        if (ctx && EVP_PKEY_sign_init(ctx) == 1 && EVP_PKEY_sign(ctx, nullptr, &len, digest.data(), digest.size()) == 1)
        {
            std::vector<unsigned char> der(len);
            if (EVP_PKEY_sign(ctx, der.data(), &len, digest.data(), digest.size()) == 1)
            {
                der.resize(len);
                const std::vector<unsigned char> raw = DerToRaw(der);
                if (raw.size() == 2 * kP256Size)
                    result = ToBase64(raw);
            }
        }
        EVP_PKEY_CTX_free(ctx);
        EVP_PKEY_free(key);
        return result;
    }
#endif
}
