#pragma once

#include <cstdint>

// P0 baseline diagnostics: fast-path and instance-activity counters for the
// native optimization plan (documentation/native-synth-optimization-plan-
// 2026-09-09.md, section 8's "native C++ benchmark mode" request). Read only
// by the render task's own periodic diagnostic log (see SynthEngine::
// taskLoop) - single-writer, single-reader-of-the-stable-snapshot, so plain
// (non-atomic) counters are enough, same "one shared inline instance, no
// .cpp needed" pattern as DspPrimitives' LUTs (see dsp::sineLutTable).
namespace enginestats
{
struct BlockCounters
{
    // How many Oscillator::render() calls this block resolved pitch/tune to
    // a single per-block Hz value (audioPortConstantForBlock fast path) vs.
    // how many needed the full per-sample audio-rate re-evaluation - see
    // documentation's section 2 ("Warum die WaveGens hinter Frequency
    // Dividern mehr kosten") and OscillatorNode.h's render().
    uint32_t oscPitchConstant = 0;
    uint32_t oscPitchAudioRate = 0;
    uint32_t oscTuneConstant = 0;
    uint32_t oscTuneAudioRate = 0;
    // Oscillator instances skipped entirely because gain is already at 0 and
    // converged (see OscillatorNode.h's kSilentGain fast path).
    uint32_t oscSilentSkipped = 0;
    // Oscillator instances whose gain smoothing had already converged this
    // block, so the per-sample OnePoleLowpass::process() call was skipped.
    uint32_t oscGainConverged = 0;

    // Graph::renderBlock instance bookkeeping: total voice instances that
    // exist in the live graph, how many were actually rendered
    // (processNodeBlock called) this block, and how many of those were
    // rendered despite their voice slot being idle (renderWhenIdle/global
    // widgets, e.g. effects/output nodes that must keep running for tails).
    uint32_t instancesTotal = 0;
    uint32_t instancesRendered = 0;
    uint32_t instancesRenderedWhileIdle = 0;
};

inline BlockCounters current = {};
inline BlockCounters lastBlock = {};

// Called once at the top of Graph::renderBlock(): snapshots the previous
// block's finished counts for the diagnostic log to read (lastBlock, stable
// for the whole next block) and resets the live counters this block will
// fill in.
inline void beginBlock()
{
    lastBlock = current;
    current = BlockCounters{};
}
} // namespace enginestats
