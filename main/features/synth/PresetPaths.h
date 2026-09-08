#pragma once

#include <cstddef>

// Shared preset-file naming/resolution logic, used by both synth_web_api.hpp
// (GET/POST /api/preset, GET /api/presets) and synth_manager.hpp (boot-time
// initial preset path) so the two never drift apart on how "which preset is
// active" gets resolved.
//
// current.json is deliberately NOT replaced by the new named-preset scheme
// - it stays exactly the path used whenever no name is given (the "never
// saved under a name yet" slot), so existing on-device presets keep working
// with zero migration. Named presets live alongside it as <name>.json.
// A small marker file (.last) remembers which named preset was last
// active; if absent/empty, the active slot is current.json.
namespace presetpaths
{
// Must match Feature::LittleFS::MOUNT_POINT (features/filesystem/littlefs_manager.hpp).
constexpr const char *kDir = "/littlefs/presets";
constexpr size_t kMaxNameLen = 64;

// Rejects empty/too-long names, path separators, "..", a leading '.'
// (reserved for marker/internal files), and ASCII control characters.
bool isSafeName(const char *name);

// Writes "/littlefs/presets/<name>.json" into out.
void pathForName(const char *name, char *out, size_t outSize);

// The fixed path used when no name is given - unchanged from before this
// feature existed.
const char *unnamedPath();

// Reads the last-active preset name into outName. Returns false (and
// leaves outName empty) if the marker is missing, empty, or unsafe.
bool readLastMarker(char *outName, size_t outSize);

// Sets the last-active marker. Pass "" (or nullptr) to mark "unnamed"
// (current.json) as active.
void writeLastMarker(const char *name);

// Combines readLastMarker()+isSafeName() with the current.json fallback -
// the one function both the boot path and GET /api/preset (no ?name=) use
// to figure out "what's currently active". outName is set to "" when the
// active slot is the unnamed one.
void resolveActivePath(char *outPath, size_t outPathSize, char *outName, size_t outNameSize);

// Decodes a application/x-www-form-urlencoded query value ("%XX" escapes
// and "+" as space) in place from encoded into out. Returns the decoded
// length. Used for preset names containing spaces/umlauts sent via
// encodeURIComponent() from the browser.
size_t urlDecode(const char *encoded, char *out, size_t outSize);
} // namespace presetpaths
