#include "ModelCatalog.h"

// Sources (checked 2026-09-21):
//  - Claude: `claude --help` (--model aliases, --effort low..max).
//  - ChatGPT: Codex's local models_cache.json and config.toml (model_reasoning_effort).
//  - Gemini: `agy models` from the installed Antigravity CLI.
//  - Mistral, DeepSeek, Grok: the providers' official model/pricing pages.
// Model names change often; the Options window accepts any other name.

const std::vector<AiCatalogEntry>& ModelCatalog()
{
    static const std::vector<AiCatalogEntry> catalog = {
        // Aliases follow the latest model of each family; full names pin a version.
        // Haiku 4.5 takes no effort setting: use "auto" (no --effort passed).
        {"claude", "Claude", "Claude Code (forfait Pro)",
         {"haiku", "sonnet", "opus", "fable",
          "claude-haiku-4-5", "claude-sonnet-5", "claude-sonnet-4-6",
          "claude-opus-5", "claude-opus-4-8", "claude-opus-4-7", "claude-opus-4-6",
          "claude-fable-5-1", "claude-fable-5"},
         {"auto", "low", "medium", "high", "xhigh", "max"},
         {{{"claude-haiku-4-5", "auto"}, {"claude-sonnet-5", "medium"}, {"claude-opus-5", "high"}}}},

        {"chatgpt", "ChatGPT", "Codex (forfait Business)",
         {"gpt-5.6-luna", "gpt-5.6-terra", "gpt-5.6-sol", "gpt-6-astra", "gpt-5.5"},
         {"low", "medium", "high", "xhigh", "max", "ultra"},
         {{{"gpt-5.6-luna", "low"}, {"gpt-5.6-sol", "medium"}, {"gpt-6-astra", "high"}}}},

        {"gemini", "Gemini", "Antigravity CLI (recherche) ou API AI Studio",
         {"gemini-3.8-flash-low", "gemini-3.8-flash-medium", "gemini-3.8-flash-high", "gemini-3.1-pro-low", "gemini-3.1-pro-high",
          "gpt-oss-120b-medium", "claude-sonnet-4-6", "claude-opus-4-6-thinking"},
         {"auto", "low", "medium", "high"},
         {{{"gemini-3.8-flash-low", "low"}, {"gemini-3.8-flash-medium", "medium"}, {"gemini-3.1-pro-high", "high"}}}},

        {"mistral", "Mistral", "API Mistral (offre gratuite)",
         {"mistral-small-latest", "mistral-medium-latest", "mistral-large-latest", "magistral-medium-latest"},
         {"auto"},
         {{{"mistral-small-latest", "auto"}, {"mistral-medium-latest", "auto"}, {"mistral-large-latest", "auto"}}}},

        {"deepseek", "DeepSeek", "OpenRouter (modèles gratuits) ou API DeepSeek",
         {"deepseek-flash", "deepseek-v4-pro"},
         {"auto"},
         {{{"deepseek-flash", "auto"}, {"deepseek-flash", "auto"}, {"deepseek-v4-pro", "auto"}}}},

        {"grok", "Grok", "API xAI",
         {"grok-4.3", "grok-4.5", "grok-4.6", "grok-build-0.1"},
         {"auto", "low", "high"},
         {{{"grok-4.3", "auto"}, {"grok-4.5", "auto"}, {"grok-4.6", "high"}}}},
    };
    return catalog;
}

const AiCatalogEntry* FindCatalogEntry(const std::string& aiId)
{
    for (const AiCatalogEntry& entry : ModelCatalog())
        if (aiId == entry.id)
            return &entry;
    return nullptr;
}
