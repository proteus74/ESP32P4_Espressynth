#pragma once

#include <cstddef>

// Shared sample-file naming/resolution logic for the Wave Player node type,
// used by both synth_web_api.hpp (GET /api/samples) and the engine's
// WavLoader (loading the actual WAV data) - mirrors PresetPaths.h's
// presetpaths namespace.
namespace samplepaths
{
// Must match Feature::LittleFS::MOUNT_POINT (features/filesystem/littlefs_manager.hpp).
constexpr const char *kDir = "/littlefs/samples";
constexpr size_t kMaxNameLen = 40;

// Rejects empty/too-long names, path separators, "..", a leading '.', and
// ASCII control characters - sampleFile comes from untrusted preset JSON.
bool isSafeName(const char *name);

// Writes "/littlefs/samples/<name>" into out.
void pathForName(const char *name, char *out, size_t outSize);
} // namespace samplepaths
