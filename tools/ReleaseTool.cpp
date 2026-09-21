// AgentChatsRelease — maintainer tool to sign Agents Chat releases.
// Never shipped with the app. See RELEASING.md.
//
//   AgentChatsRelease keygen                 create the signing key (once)
//   AgentChatsRelease pubkey                 print the public key for src/UpdateKeys.h
//   AgentChatsRelease sign <exe> <version>   write <exe>.sig and <exe>.sha256
//   AgentChatsRelease backup <file>          export the key, encrypted with a passphrase
//   AgentChatsRelease restore <file>         import a backup on this Windows account
//
// The private key lives in %LOCALAPPDATA%\AgentChats-release\signing-key.dpapi,
// encrypted with DPAPI for the current Windows user, never in the repository.

#include "Platform.h"
#include "ReleaseSignature.h"
#include "Secrets.h"

#include <windows.h>
#include <bcrypt.h>

#include <cstdio>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <string>
#include <vector>

namespace fs = std::filesystem;

namespace
{
    constexpr char kBackupMagic[8] = {'A', 'G', 'C', 'H', 'K', 'E', 'Y', '1'};
    constexpr ULONG kIterations = 600000;

    fs::path KeyFile()
    {
        wchar_t buf[MAX_PATH];
        const DWORD n = GetEnvironmentVariableW(L"LOCALAPPDATA", buf, MAX_PATH);
        return fs::path(std::wstring(buf, n)) / L"AgentChats-release" / L"signing-key.dpapi";
    }

    bool ReadAll(const fs::path& p, std::string& out)
    {
        std::ifstream in(p, std::ios::binary);
        if (!in)
            return false;
        out.assign(std::istreambuf_iterator<char>(in), std::istreambuf_iterator<char>());
        return true;
    }

    bool WriteAll(const fs::path& p, const std::string& data)
    {
        std::error_code ec;
        fs::create_directories(p.parent_path(), ec);
        std::ofstream out(p, std::ios::binary | std::ios::trunc);
        out << data;
        return static_cast<bool>(out);
    }

    bool LoadKey(std::vector<unsigned char>& blob)
    {
        std::string stored;
        if (!ReadAll(KeyFile(), stored))
        {
            std::fprintf(stderr, "Aucune clé : lance d'abord « keygen » (ou « restore »).\n");
            return false;
        }
        const std::string plain = Secrets::Unprotect(stored);
        if (plain.empty())
        {
            std::fprintf(stderr, "Clé illisible (autre compte Windows ?). Utilise « restore » avec ta sauvegarde.\n");
            return false;
        }
        blob.assign(plain.begin(), plain.end());
        return true;
    }

    bool StoreKey(const std::vector<unsigned char>& blob)
    {
        const std::string protectedKey = Secrets::Protect(std::string(blob.begin(), blob.end()));
        return !protectedKey.empty() && WriteAll(KeyFile(), protectedKey);
    }

    // Public blob = private blob's header (with the public magic) + X + Y.
    std::string PublicFromPrivate(const std::vector<unsigned char>& priv)
    {
        if (priv.size() < sizeof(BCRYPT_ECCKEY_BLOB))
            return {};
        BCRYPT_ECCKEY_BLOB header = *reinterpret_cast<const BCRYPT_ECCKEY_BLOB*>(priv.data());
        if (header.dwMagic != BCRYPT_ECDSA_PRIVATE_P256_MAGIC || priv.size() < sizeof(header) + 3 * header.cbKey)
            return {};
        header.dwMagic = BCRYPT_ECDSA_PUBLIC_P256_MAGIC;
        std::vector<unsigned char> pub(sizeof(header) + 2 * header.cbKey);
        memcpy(pub.data(), &header, sizeof(header));
        memcpy(pub.data() + sizeof(header), priv.data() + sizeof(header), 2 * header.cbKey);
        return ReleaseSignature::ToHex(pub);
    }

    std::string ReadPassphrase(const char* prompt)
    {
        std::fprintf(stderr, "%s", prompt);
        HANDLE in = GetStdHandle(STD_INPUT_HANDLE);
        DWORD mode = 0;
        const bool console = GetConsoleMode(in, &mode);
        if (console)
            SetConsoleMode(in, mode & ~ENABLE_ECHO_INPUT);
        std::string line;
        std::getline(std::cin, line);
        if (console)
            SetConsoleMode(in, mode);
        std::fprintf(stderr, "\n");
        while (!line.empty() && (line.back() == '\r' || line.back() == '\n'))
            line.pop_back();
        // A UTF-8 byte-order mark (added by some shells when piping) is not part of the passphrase.
        if (line.rfind("\xEF\xBB\xBF", 0) == 0)
            line.erase(0, 3);
        return line;
    }

    bool DeriveKey(const std::string& passphrase, const unsigned char* salt, ULONG iterations, std::vector<unsigned char>& key)
    {
        BCRYPT_ALG_HANDLE hmac = nullptr;
        if (BCryptOpenAlgorithmProvider(&hmac, BCRYPT_SHA256_ALGORITHM, nullptr, BCRYPT_ALG_HANDLE_HMAC_FLAG) != 0)
            return false;
        key.resize(32);
        const bool ok = BCryptDeriveKeyPBKDF2(hmac, reinterpret_cast<PUCHAR>(const_cast<char*>(passphrase.data())),
                                              static_cast<ULONG>(passphrase.size()), const_cast<PUCHAR>(salt), 16,
                                              iterations, key.data(), 32, 0) == 0;
        BCryptCloseAlgorithmProvider(hmac, 0);
        return ok;
    }

    // AES-256-GCM. encrypt=true fills tag; encrypt=false checks it.
    bool AesGcm(bool encrypt, const std::vector<unsigned char>& key, const unsigned char* nonce, unsigned char* tag,
                const std::vector<unsigned char>& input, std::vector<unsigned char>& output)
    {
        BCRYPT_ALG_HANDLE aes = nullptr;
        BCRYPT_KEY_HANDLE k = nullptr;
        bool ok = false;
        if (BCryptOpenAlgorithmProvider(&aes, BCRYPT_AES_ALGORITHM, nullptr, 0) != 0)
            return false;
        if (BCryptSetProperty(aes, BCRYPT_CHAINING_MODE, reinterpret_cast<PUCHAR>(const_cast<wchar_t*>(BCRYPT_CHAIN_MODE_GCM)),
                              sizeof(BCRYPT_CHAIN_MODE_GCM), 0) == 0 &&
            BCryptGenerateSymmetricKey(aes, &k, nullptr, 0, const_cast<PUCHAR>(key.data()), static_cast<ULONG>(key.size()), 0) == 0)
        {
            BCRYPT_AUTHENTICATED_CIPHER_MODE_INFO info;
            BCRYPT_INIT_AUTH_MODE_INFO(info);
            info.pbNonce = const_cast<PUCHAR>(nonce);
            info.cbNonce = 12;
            info.pbTag = tag;
            info.cbTag = 16;
            output.resize(input.size());
            ULONG written = 0;
            const NTSTATUS st = encrypt
                ? BCryptEncrypt(k, const_cast<PUCHAR>(input.data()), static_cast<ULONG>(input.size()), &info, nullptr, 0,
                                output.data(), static_cast<ULONG>(output.size()), &written, 0)
                : BCryptDecrypt(k, const_cast<PUCHAR>(input.data()), static_cast<ULONG>(input.size()), &info, nullptr, 0,
                                output.data(), static_cast<ULONG>(output.size()), &written, 0);
            ok = st == 0 && written == input.size();
            BCryptDestroyKey(k);
        }
        BCryptCloseAlgorithmProvider(aes, 0);
        return ok;
    }

    int Keygen()
    {
        std::error_code ec;
        if (fs::exists(KeyFile(), ec))
        {
            std::fprintf(stderr, "Une clé existe déjà (%ls). Elle n'est jamais écrasée.\n", KeyFile().c_str());
            return 1;
        }
        std::vector<unsigned char> priv;
        std::string pub;
        if (!ReleaseSignature::GenerateKeyPair(priv, pub) || !StoreKey(priv))
        {
            std::fprintf(stderr, "Génération ou enregistrement de la clé impossible.\n");
            return 1;
        }
        SecureZeroMemory(priv.data(), priv.size());
        std::printf("Clé créée : %ls\n\nClé publique à ajouter dans src/UpdateKeys.h :\n%s\n\n"
                    "Fais maintenant une sauvegarde : AgentChatsRelease backup <fichier>\n",
                    KeyFile().c_str(), pub.c_str());
        return 0;
    }

    int Pubkey()
    {
        std::vector<unsigned char> priv;
        if (!LoadKey(priv))
            return 1;
        std::printf("%s\n", PublicFromPrivate(priv).c_str());
        SecureZeroMemory(priv.data(), priv.size());
        return 0;
    }

    int SignRelease(const fs::path& exe, const std::string& version)
    {
        std::vector<unsigned char> priv;
        if (!LoadKey(priv))
            return 1;
        const std::string digest = ReleaseSignature::Sha256File(exe);
        const std::string signature = digest.empty() ? std::string() : ReleaseSignature::Sign(priv, version, digest);
        const std::string pub = PublicFromPrivate(priv);
        SecureZeroMemory(priv.data(), priv.size());
        if (signature.empty() || !ReleaseSignature::Verify(version, digest, signature, {pub}))
        {
            std::fprintf(stderr, "Signature impossible (fichier illisible ?).\n");
            return 1;
        }
        fs::path sig = exe, sha = exe;
        sig += ".sig";
        sha += ".sha256";
        if (!WriteAll(sig, signature + "\n") || !WriteAll(sha, digest + "  " + Platform::Narrow(exe.filename().wstring()) + "\n"))
            return 1;
        std::printf("Signé %s pour %s\n  %ls\n  %ls\n", digest.c_str(), version.c_str(), sig.c_str(), sha.c_str());
        return 0;
    }

    int Backup(const fs::path& file)
    {
        std::vector<unsigned char> priv;
        if (!LoadKey(priv))
            return 1;
        const std::string pass = ReadPassphrase("Phrase de passe de la sauvegarde (12 caractères minimum) : ");
        const std::string again = ReadPassphrase("Confirme : ");
        if (pass.size() < 12 || pass != again)
        {
            std::fprintf(stderr, "Phrase de passe trop courte ou différente.\n");
            return 1;
        }
        unsigned char salt[16], nonce[12], tag[16];
        BCryptGenRandom(nullptr, salt, sizeof(salt), BCRYPT_USE_SYSTEM_PREFERRED_RNG);
        BCryptGenRandom(nullptr, nonce, sizeof(nonce), BCRYPT_USE_SYSTEM_PREFERRED_RNG);
        std::vector<unsigned char> key, cipher;
        if (!DeriveKey(pass, salt, kIterations, key) || !AesGcm(true, key, nonce, tag, priv, cipher))
            return 1;
        SecureZeroMemory(priv.data(), priv.size());
        std::string out(kBackupMagic, sizeof(kBackupMagic));
        out.append(reinterpret_cast<char*>(salt), sizeof(salt));
        const ULONG iterations = kIterations;
        out.append(reinterpret_cast<const char*>(&iterations), sizeof(iterations));
        out.append(reinterpret_cast<char*>(nonce), sizeof(nonce));
        out.append(reinterpret_cast<char*>(tag), sizeof(tag));
        out.append(reinterpret_cast<char*>(cipher.data()), cipher.size());
        if (!WriteAll(file, out))
            return 1;
        std::printf("Sauvegarde écrite : %ls\nRange-la hors de ce PC (clé USB, gestionnaire de mots de passe).\n", file.c_str());
        return 0;
    }

    int Restore(const fs::path& file)
    {
        std::error_code ec;
        if (fs::exists(KeyFile(), ec))
        {
            std::fprintf(stderr, "Une clé existe déjà ici ; restauration refusée pour ne rien écraser.\n");
            return 1;
        }
        std::string data;
        const size_t header = sizeof(kBackupMagic) + 16 + 4 + 12 + 16;
        if (!ReadAll(file, data) || data.size() <= header || data.compare(0, sizeof(kBackupMagic), kBackupMagic, sizeof(kBackupMagic)) != 0)
        {
            std::fprintf(stderr, "Fichier de sauvegarde invalide.\n");
            return 1;
        }
        const unsigned char* p = reinterpret_cast<const unsigned char*>(data.data()) + sizeof(kBackupMagic);
        const unsigned char* salt = p;
        ULONG iterations = 0;
        memcpy(&iterations, p + 16, 4);
        const unsigned char* nonce = p + 20;
        unsigned char tag[16];
        memcpy(tag, p + 32, 16);
        const std::vector<unsigned char> cipher(p + 48, reinterpret_cast<const unsigned char*>(data.data()) + data.size());
        const std::string pass = ReadPassphrase("Phrase de passe : ");
        std::vector<unsigned char> key, priv;
        if (!DeriveKey(pass, salt, iterations, key) || !AesGcm(false, key, nonce, tag, cipher, priv) || PublicFromPrivate(priv).empty())
        {
            std::fprintf(stderr, "Phrase de passe incorrecte ou sauvegarde abîmée.\n");
            return 1;
        }
        const bool ok = StoreKey(priv);
        const std::string pub = PublicFromPrivate(priv);
        SecureZeroMemory(priv.data(), priv.size());
        if (!ok)
            return 1;
        std::printf("Clé restaurée. Clé publique :\n%s\n", pub.c_str());
        return 0;
    }
}

int wmain(int argc, wchar_t** argv)
{
    SetConsoleOutputCP(CP_UTF8);
    const std::wstring cmd = argc > 1 ? argv[1] : L"";
    if (cmd == L"keygen")
        return Keygen();
    if (cmd == L"pubkey")
        return Pubkey();
    if (cmd == L"sign" && argc > 3)
        return SignRelease(argv[2], Platform::Narrow(argv[3]));
    if (cmd == L"backup" && argc > 2)
        return Backup(argv[2]);
    if (cmd == L"restore" && argc > 2)
        return Restore(argv[2]);
    std::fprintf(stderr,
                 "Usage :\n"
                 "  AgentChatsRelease keygen\n"
                 "  AgentChatsRelease pubkey\n"
                 "  AgentChatsRelease sign <AgentChats.exe> <version, ex. v0.1.1>\n"
                 "  AgentChatsRelease backup <fichier>\n"
                 "  AgentChatsRelease restore <fichier>\n");
    return 2;
}
