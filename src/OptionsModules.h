#pragma once

#include <string_view>
#include <vector>

enum class OptionsView
{
    General,
    Updates,
    Connections,
    Models,
    Memory,
    Connectors,
    Plugins,
    Mcps,
};

struct OptionsModule
{
    std::string_view id;
    std::string_view category;
    std::string_view label;
    OptionsView view;
};

// Central catalogue for the application's main settings menu. Adding a native
// module only requires one catalogue entry and one renderer dispatch case.
const std::vector<OptionsModule>& OptionsModules();
