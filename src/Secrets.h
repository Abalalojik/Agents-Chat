#pragma once
#include <string>

// API keys are encrypted with Windows DPAPI (tied to the current Windows user)
// before being written to settings.json, and never shown in clear once saved.
namespace Secrets
{
    std::string Protect(const std::string& plain);      // -> base64, "" on failure
    std::string Unprotect(const std::string& base64);   // -> plain, "" on failure
}
