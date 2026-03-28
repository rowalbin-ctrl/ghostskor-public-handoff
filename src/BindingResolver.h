#pragma once
#include <string>

namespace BindingResolver {
// Resolve a binding command (e.g. "+attack") to a display key (e.g. "MOUSE1").
std::string ResolveBindingDisplay(const std::string &command);

// Replace binding placeholders with resolved key display strings.
// Supported forms:
// - [{+command}]
// - &&1 / &&2 ... (platform stance/mantle prompts)
// englishFallback is optional; if binding config is missing, try to infer key
// from the English text (e.g. "Press F to ...").
std::string ResolveBindingsInText(const std::string &text,
                                  const std::string &englishFallback = "");

// Localize keybind display text (supports composite binds like "A or B").
// preserveSingleAsciiAlpha=true keeps single A-Z tokens unchanged.
std::string LocalizeBindingDisplayText(
    const std::string &text, bool preserveSingleAsciiAlpha = false);

// Best-effort key extraction from English prompts (e.g. "Press F to ...").
std::string ExtractKeyFromEnglish(const std::string &english);

// Invalidate cached bindings so they are re-read from disk on next use.
// Call when the user may have changed key bindings (e.g. options menu visit).
void InvalidateBindings();
} // namespace BindingResolver
