#pragma once

#include "config/model_access.hpp"
#include "config/model_catalog.hpp"

#include <string>

namespace ui
{
// Model selector: a combo whose popup has one submenu per family (Opus,
// Sonnet, Haiku, Fable) listing "latest" (the alias) and every version,
// plus opusplan/best, a 1M-context toggle and a free-form model id.
//
// `value` is what goes into settings.model / --model: an alias ("opus"),
// a model id ("claude-opus-4-8"), either with an optional "[1m]" suffix,
// or `unsetValue` for "not chosen". Returns true when the user changed it.
// Legacy (pre-4) versions are listed only with showLegacy; a value that
// already points at one is still recognised and displayed either way.
// Models Claude Code would refuse (see ModelAccess) are shown but disabled,
// with the reason in their tooltip.
bool ModelCombo(const char* id, std::string& value, const config::ModelCatalog& catalog,
                const config::ModelAccess& access, const char* unsetLabel, const char* unsetValue, bool showLegacy);
} // namespace ui
