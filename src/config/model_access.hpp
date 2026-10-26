#pragma once

#include "config/json_file.hpp"
#include "config/model_catalog.hpp"

#include <optional>
#include <string>
#include <vector>

namespace config
{
// Which models Claude Code would let this account pick, reconstructed from
// what it persists — so pickers can grey out what Claude Code would refuse.
//
// Mirrors Claude Code 2.1.x `isMythosAvailable`: Mythos needs the first-party
// API (no CLAUDE_CODE_USE_BEDROCK/VERTEX/… provider switch) and an entry in the
// account's `additionalModelOptionsCache` (~/.claude.json) that isn't
// `disabled: true`. That cache is also honoured for any other model it marks
// disabled. What it can't see: a model catalog Claude Code fetches from the
// server at runtime and doesn't persist.
struct ModelAccess
{
    struct Offer
    {
        std::string modelId; // canonical id, "[1m]" and date stamp stripped
        std::string label;
        bool disabled = false;
    };
    std::vector<Offer> offers;
    std::string providerSwitch; // e.g. "CLAUDE_CODE_USE_BEDROCK" when not first-party

    // nullopt → Claude Code has no objection; otherwise why it would refuse.
    std::optional<std::string> Unavailable(const ModelInfo& model) const;
};

ModelAccess ReadModelAccess(const Json& globalConfig, const Json& userSettings);

// "claude-opus-4-5-20251101[1m]" → "claude-opus-4-5"
std::string CanonicalModelId(std::string id);
} // namespace config
