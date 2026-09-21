#pragma once
#include <filesystem>
#include <string>
#include <vector>

// Signed updates. A release is trusted only if AgentChats.exe.sig is a valid
// ECDSA P-256 signature, by one of the embedded release keys, of:
//
//   Agents-Chat-release
//   version=<tag>
//   sha256=<hex digest of AgentChats.exe>
//
// so the signature binds both the executable and its version (no silent
// downgrade to an older signed build). Keys never come from the release itself.
namespace ReleaseSignature
{
    std::string Sha256File(const std::filesystem::path& path); // lower-case hex, "" on error
    std::string Message(const std::string& version, const std::string& sha256Hex);

    // Verification against explicit public keys (hex of BCRYPT_ECCPUBLIC_BLOB).
    bool Verify(const std::string& version, const std::string& sha256Hex, const std::string& signatureBase64,
                const std::vector<std::string>& publicKeysHex);
    // Verification against the keys built into this copy of the app (src/UpdateKeys.h).
    bool VerifyRelease(const std::string& version, const std::string& sha256Hex, const std::string& signatureBase64);
    bool HasTrustedKeys();

    // Key handling for the release tool and tests (never used by the app itself).
    bool GenerateKeyPair(std::vector<unsigned char>& privateBlob, std::string& publicKeyHex);
    std::string Sign(const std::vector<unsigned char>& privateBlob, const std::string& version, const std::string& sha256Hex);

    std::string ToHex(const std::vector<unsigned char>& bytes);
    std::vector<unsigned char> FromHex(const std::string& hex);
    std::string ToBase64(const std::vector<unsigned char>& bytes);
    std::vector<unsigned char> FromBase64(const std::string& text);
}
