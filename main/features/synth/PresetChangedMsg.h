#pragma once

// Posted by WebServer's POST /api/preset handler onto a length-1
// FreeRTOS queue (via xQueueOverwrite, so bursts of rapid saves coalesce
// into "rebuild once more from whatever's on disk now") and consumed by
// SynthEngine's render task, which reloads/rebuilds its graph from `path`.
struct PresetChangedMsg
{
    // Matches WebServer.cpp's kMaxPathLen (300) exactly, so the compiler
    // can statically prove a straight snprintf() from a kMaxPathLen buffer
    // into this one never truncates (a smaller size here is still plenty
    // in practice - presetpaths::kDir + '/' + a name up to
    // presetpaths::kMaxNameLen + ".json" tops out well under 128 - but
    // GCC's -Wformat-truncation reasons about declared sizes, not actual
    // content, so it needs the same bound to stop flagging it).
    char path[300];
};
