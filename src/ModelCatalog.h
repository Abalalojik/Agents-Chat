#pragma once
#include "Model.h"

#include <array>
#include <string>
#include <vector>

// A concrete model and its thinking (reasoning effort) setting, in the
// provider's own vocabulary so it can be passed through unchanged
// (claude --model/--effort, codex -m / model_reasoning_effort, gemini -m...).
struct ModelChoice
{
    std::string model;
    std::string thinking;
};

// What the app knows about one AI: the models and thinking values it offers,
// and the default choice for each level. Lists are a starting point: the
// Options window also accepts any other model name.
struct AiCatalogEntry
{
    const char* id;      // Sender id, e.g. "claude"
    const char* name;    // display name
    const char* backend; // where the model runs, shown in Options
    std::vector<std::string> models;
    std::vector<std::string> thinking; // "auto" = let the provider decide
    std::array<ModelChoice, 3> defaults; // Léger, Normal, Fort
};

const std::vector<AiCatalogEntry>& ModelCatalog();
const AiCatalogEntry* FindCatalogEntry(const std::string& aiId);
