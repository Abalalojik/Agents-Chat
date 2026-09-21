#pragma once
#include <string>
#include <vector>

// Public keys allowed to sign Agents Chat releases (hex of BCRYPT_ECCPUBLIC_BLOB,
// ECDSA P-256), as printed by `AgentChatsRelease keygen`. Several keys may be
// trusted at once so a key can be rotated: add the new key in a release signed
// with the old one, and remove the old key in a later release.
//
// While this list is empty, automatic updates are refused: nothing can be verified.
inline const std::vector<std::string>& TrustedReleaseKeys()
{
    static const std::vector<std::string> keys = {
    };
    return keys;
}
