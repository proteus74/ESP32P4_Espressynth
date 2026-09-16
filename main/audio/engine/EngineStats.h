#pragma once

#include <cstdint>

// P0 baseline diagnostics: fast-path and instance-activity counters for the
// native optimization plan (documentation/native-synth-optimization-plan-
// 2026-09-09.md, section 8's "native C++ benchmark mode" request). Written
// only by the render task (SynthEngine::taskLoop, right after
// Graph::renderBlock), which then copies lastBlock into its own published
// atomics (see SynthEngine::blockStatsSnapshot()) for any other task to read
// - so this struct itself stays single-writer/single-reader (the render task
// reads its own lastBlock immediately after writing it), plain (non-atomic)
// counters are enough, same "one shared inline instance, no .cpp needed"
// pattern as DspPrimitives' LUTs (see dsp::sineLutTable). The actual
// ESP_LOGI print of that published snapshot happens elsewhere entirely (see
// Feature::Synth::log_p0_baseline_tick(), synth_manager.hpp) - deliberately
// NOT on this render task, see that function's comment for why.
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
    // "amplitude" (port 0) - typically an ADSR envelope. Constant whenever
    // that envelope is sitting on its sustain plateau (or unconnected) -
    // see OscillatorNode.h's render() (plan section 3, P2 extension).
    uint32_t oscAmpConstant = 0;
    uint32_t oscAmpAudioRate = 0;
    // Oscillator instances skipped entirely because gain is already at 0 and
    // converged (see OscillatorNode.h's kSilentGain fast path).
    uint32_t oscSilentSkipped = 0;
    // Oscillator instances whose gain smoothing had already converged this
    // block, so the per-sample OnePoleLowpass::process() call was skipped.
    uint32_t oscGainConverged = 0;

    // Same fields as above, mirrored for nodes::fmoscillator::render() (see
    // fm-plan.md section "Vorschlag für P0: FM-Zähler") - identical
    // reasoning, just for the FM operator's pitch/tune/amp/gain fast paths.
    uint32_t fmPitchConstant = 0;
    uint32_t fmPitchAudioRate = 0;
    uint32_t fmTuneConstant = 0;
    uint32_t fmTuneAudioRate = 0;
    uint32_t fmAmpConstant = 0;
    uint32_t fmAmpAudioRate = 0;
    // Diagnostic-only subset of fmAmpConstant: how often the constant block
    // amplitude was already ~0 (kSilentGain tolerance) - i.e. how often a
    // modulator sitting past its ADSR decay (Sustain 0) would qualify for
    // P1's silent-modulator fast path, without P1 itself existing yet.
    uint32_t fmAmpConstantZero = 0;
    uint32_t fmSilentSkipped = 0;
    uint32_t fmGainConverged = 0;
    // How many FM instances had an active audio-rate phase modulator
    // (fmActive) resp. active self feedback (feedbackActive) this block.
    uint32_t fmModActive = 0;
    uint32_t fmFeedbackActive = 0;
    uint32_t fmSuperSawActive = 0;

    // DX-envelope block paths. These distinguish the cheap sustain/idle
    // fill from a full linear-ramp block and the uncommon boundary block
    // that crosses from one segment into the next.
    uint32_t dxHolding = 0;
    uint32_t dxRamping = 0;
    uint32_t dxBoundary = 0;
    uint32_t dxShapeConfigured = 0;

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
