#pragma once

#include <cmath>
#include <cstddef>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <new>

#include "cJSON.h"
#include "esp_attr.h"
#include "esp_heap_caps.h"
#include "esp_log.h"
#include "esp_timer.h"

#include "CvConventions.h"
#include "EngineStats.h"
#include "EspEqEffect.h"
#include "EspAdditionalEffects.h"
#include "NodeInstance.h"
#include "NodeRender.h"
#include "ParamDispatch.h"
#include "PortTables.h"
#include "SoundFontPlayer.h"
#include "SynthConfig.h"
#include "VoiceAllocator.h"
#include "UsbMidiEvent.h"

#include "nodes/AdsrNode.h"
#include "nodes/ButtonNode.h"
#include "nodes/ChorusNode.h"
#include "nodes/CurtisFilterNode.h"
#include "nodes/DelayNode.h"
#include "nodes/FilterNode.h"
#include "nodes/HammondNode.h"
#include "nodes/KnobNode.h"
#include "nodes/ReverbNode.h"
#include "nodes/ValueMapperNode.h"

struct WidgetSlot
{
    char id[24] = {};
    char label[48] = {};
    NodeType type = NodeType::Unknown;
    bool voiced = false;
    bool renderWhenIdle = false;
    // True (default, safe fallback) unless GraphBuilder::build()'s
    // reachability post-pass proves no edge-path leads from this widget to
    // any hardware sink or an active monitor tap - see that pass's comment
    // (plan section 3, P2: "Nur vom Ausgang oder einem aktiven Monitor
    // benötigte Teilgraphen rendern"). Graph::renderBlock skips a widget
    // entirely (render + its own outgoing edges) when this is false; safe
    // by construction, since a widget only ends up false when NONE of its
    // outgoing edges lead anywhere needed either.
    bool needed = true;
    uint16_t instanceBase = 0;
    uint16_t instanceCount = 0; // 1 or the preset's configured voice count
    uint16_t outgoingEdgeStart = 0;
    uint16_t outgoingEdgeCount = 0;
};

struct Edge
{
    uint16_t fromInstance = 0;
    uint8_t fromPort = 0;
    uint16_t toInstance = 0;
    uint8_t toPort = 0;
    float scale = 1.0f; // 1/sqrt(N) for voiced->global fan-in, else 1.0
    bool monitorTap = false;
};

// A non-audio-rate connection resolved at build time into a direct
// (source widget) -> (target widget's control field) route: either one of
// midi-in's frequency/gate/velocity "virtual" outputs, a Const scalar, or a
// knob's/button's mapped CC value targeting an arbitrary named param on the
// target (paramName is only populated for Kind::Const/Kind::Knob/Kind::Button - the three
// MidiIn kinds have a fixed, implicit target field per Graph::handleMidiEvent).
struct ControlRoute
{
    enum class Kind : uint8_t
    {
        MidiNote,
        MidiGate,
        MidiVelocity,
        MidiRaw,
        MidiPitchCv, // MidiIn's "pitchCv" output: 1V/oct CV, see CvConventions.h
        Knob,        // Knob's "voltage" output (0-10V, mapped)
        KnobCc,      // Knob's "cc" output (raw 0-127, unmapped)
        Const,       // Const's scalar "value" output
        Button,
        UpDownButton,
    };

    uint16_t sourceWidget = 0; // midi-in, const, knob, or button widget index
    uint16_t targetWidget = 0;
    Kind kind = Kind::MidiGate;
    char paramName[16] = {}; // populated for scalar source routes

    // Target's audio-input port index (see PortTables::audioInIndex), or -1
    // if the target field this route resolves to isn't a real port (e.g. a
    // Knob route to Filter's "cutoff"). When >= 0, Graph::handleMidiEvent
    // also mirrors the route's value into NodeInstance::controlValue[targetPort]
    // so Graph::renderBlock can feed it into that port's inPorts buffer for
    // node types (Multiplier/Adder/Divide/...) that only read inPorts, with
    // no dedicated field of their own the way Oscillator's frequency/
    // amplitude have.
    int8_t targetPort = -1;

    // Only meaningful when sourceWidget is a midi-split widget: which of its
    // two outputs (0 = lower, 1 = upper) this route was generated from. Lets
    // Graph::deliverNoteControlRoutes forward a note-on/off event to only the
    // matching half's routes when recursing from midi-split's own Gate case
    // (see handleMidiEvent), instead of delivering both sides for one note.
    uint8_t side = 0;
};

// One widget's decoded Wave Player sample, shared by every voice instance of
// that widget (keyed by widget index, NOT instance index - see
// GraphBuilder's initWavePlayer). Resolved at most once per widget per graph
// build; a failed load leaves data==nullptr with attempted==true so every
// voice instance simply renders silence, no retry, no crash. `data` is a
// borrowed pointer - owned by WavLoader's own process-wide cache (see
// WavLoader.cpp), not this Graph; Graph::destroy() does not free it.
struct WaveSampleCache
{
    const int16_t *data = nullptr;
    uint32_t frameCount = 0;
    uint8_t channels = 0;
    bool attempted = false;
};

struct SoundFontCache
{
    tsf *source = nullptr;
    int presetIndex = -1;
    bool attempted = false;
};

// A fully-built, ready-to-render node graph for one loaded preset. Built
// once by GraphBuilder::build() from a parsed preset; never mutated after
// that - a preset change builds a brand new Graph and the render task swaps
// the live pointer (see SynthEngine).
class Graph
{
public:
    // Per-target stereo output buffers, filled by renderBlock(). Each of the
    // three hardware sinks (USB / ES8311 / WM8960) has its own pair, driven
    // by the corresponding output widget (UsbOut/Es8311Out/Wm8960Out) in the
    // graph. A sink with no matching widget stays silent (zeros).
    struct Outputs
    {
        float *usbL = nullptr;
        float *usbR = nullptr;
        float *esL = nullptr;
        float *esR = nullptr;
        float *wmL = nullptr;
        float *wmR = nullptr;
    };

    void renderBlock(Outputs &out, size_t frames);
    void handleMidiEvent(const UsbMidiEvent &event);
    uint16_t activeSoundFontVoiceCount() const;
    void midiVoiceUsage(uint16_t &active, uint16_t &maximum) const;

    // Applies a live parameter patch (from a browser WS "paramUpdate"
    // message) to every instance of the widget `widgetId`, without touching topology or
    // reallocating any buffers - just field writes, matching what a full
    // rebuild would set for these same keys. Unknown widgetId or unknown
    // keys within `paramsObj` are silently ignored (logged at debug level).
    void applyLiveParams(const char *widgetId, cJSON *paramsObj);

    // Releases every instance's heap buffers (input port buffers, DelayLine
    // ring buffers) and the instance array itself. Call once before
    // releasing the Graph object itself (see release()).
    void destroy();

    // Allocates + value-initializes a Graph, preferring PSRAM (falls back to
    // internal RAM if PSRAM is exhausted). The ~27KB object itself - mostly
    // widgets[]/edges[]/controlRoutes[]/voiceAllocators[] - is only walked
    // block-rate-sequentially by renderBlock(), never per-sample, so PSRAM
    // placement costs no meaningful latency in isolation. (Investigated moving this
    // internal-first to rule it out as the cause of an LVGL/MIPI-DSI-correlated render-block
    // burst pattern - measured no improvement, so reverted; the real cause is more likely
    // shared L2-cache pressure from the display's own PSRAM-backed framebuffer traffic than
    // this object's specific placement. Not worth this object's fixed ~27KB of internal RAM
    // for zero measured benefit.) Pair with release().
    static Graph *allocate();

    // Frees a Graph allocated via allocate(). Does not call destroy() -
    // callers still do that first to release the instances array/DelayLine
    // rings/caches, exactly as before this existed.
    static void release(Graph *graph);

    NodeInstance *instances = nullptr;
    uint16_t instanceCount = 0;

    WidgetSlot widgets[synth::kMaxWidgets];
    uint16_t widgetCount = 0;

    // EMA-smoothed per-widget CPU load. Profiling rotates over one widget per
    // block so timer reads do not scale with graph size. Generation advances
    // after a complete sweep and lets SynthEngine publish only fresh snapshots.
    // widgetLoadPercent covers only that widget's own render() calls (node
    // DSP); widgetEdgeLoadPercent separately covers summing its outgoing
    // edges into downstream instances - previously entirely unmeasured (see
    // documentation/native-synth-optimization-plan-2026-09-09.md section 7:
    // "Eingangspuffer vorbereiten und ausgehende Verbindungen summieren
    // liegen außerhalb"). Same rotation, same O(1)-per-block cost - just two
    // timer-read pairs for the profiled widget instead of one.
    float widgetLoadPercent[synth::kMaxWidgets] = {};
    float widgetEdgeLoadPercent[synth::kMaxWidgets] = {};
    uint16_t widgetProfileCursor = 0;
    float widgetProfileEmaAlpha = 0.0f;
    uint32_t widgetLoadGeneration = 0;

    // P0 baseline diagnostics (plan section 7): the block-level phases the
    // per-widget profiler above still can't see because they run outside any
    // single widget's render/edge span - voice-activity classification plus
    // input-buffer clear/fill, the renderOrder sweep as a whole (node DSP +
    // edge-summing across every widget), and the final per-sink output copy.
    // Timed unconditionally every block (not rotated): each is already O(1)
    // extra esp_timer_get_time() calls regardless of graph size, same cost
    // class as SynthEngine::taskLoop's own existing midi/liveParam/render
    // phase timers.
    float phaseActivityPercent = 0.0f;
    float phaseRenderPercent = 0.0f;
    float phaseOutputPercent = 0.0f;

    Edge edges[synth::kMaxRuntimeEdges];
    uint16_t edgeCount = 0;

    ControlRoute controlRoutes[synth::kMaxConnections];
    uint16_t controlRouteCount = 0;

    uint16_t renderOrder[synth::kMaxWidgets];
    uint16_t renderOrderCount = 0;

    // Indices of the (at most one each) output widgets per hardware sink.
    // -1 = no widget for that sink (renders silence).
    int16_t usbOutWidget = -1;
    int16_t es8311OutWidget = -1;
    int16_t wm8960OutWidget = -1;
    uint8_t monitorVoiceIndex = 0;

    // Per-voice-slot "still audibly sounding" flag, refreshed once per
    // renderBlock() (see there): true while any connected ADSR for that
    // voice slot has not yet decayed to idle. One block stale by the time
    // noteOn() reads it (MIDI dispatch runs before renderBlock in the
    // render task loop) - negligible, envelope idle state doesn't flip
    // faster than that. Lets VoiceAllocator avoid grabbing a slot that's
    // still ringing out its release just because its `note` field was
    // cleared on note-off (see VoiceAllocator::noteOn).
    bool voiceStillSounding[SYNTH_VOICE_COUNT] = {};

    VoiceAllocator voiceAllocators[synth::kMaxWidgets]; // meaningful only for MidiIn-type widgets

    WaveSampleCache waveSampleCache[synth::kMaxWidgets]; // meaningful only for WavePlayer-type widgets
    SoundFontCache soundFontCache[synth::kMaxWidgets];
};

namespace
{
constexpr char kGraphTag[] = "Graph";

// Per-widget CPU-load debug display. The base alpha describes the previous
// every-block sampling rate; Graph::renderBlock derives the equivalent alpha
// for its round-robin interval once per graph.
constexpr float kGraphWidgetLoadEmaAlpha = 0.05f;
constexpr float kGraphBlockBudgetUs = 1000000.0f * static_cast<float>(synth::kBlockSize) / static_cast<float>(synth::kSampleRate);

// Sends the current scalar value of a Knob/Button to every ControlRoute
// originating from it - the common delivery step for both ways such a value
// can change: a real MIDI-CC message (handleMidiEvent) or a live update from
// the browser, e.g. a knob drag (applyLiveParams - which previously did NOT
// do this: a knob dragged by mouse never reached a connected Divide/Adder/
// Multiplier, only a knob that happened to also be driven by a real CC
// message).
inline void pushControlRouteValue(Graph &graph, uint16_t sourceWidget, ControlRoute::Kind kind, float value)
{
    for (uint16_t r = 0; r < graph.controlRouteCount; ++r)
    {
        const ControlRoute &route = graph.controlRoutes[r];
        if (route.kind != kind || route.sourceWidget != sourceWidget)
        {
            continue;
        }
        const WidgetSlot &targetWidget = graph.widgets[route.targetWidget];
        for (uint16_t v = 0; v < targetWidget.instanceCount; ++v)
        {
            NodeInstance &target = graph.instances[targetWidget.instanceBase + v];
            float targetValue = value;
            // LFO Rate is configured in Hz, but its connected CV input is
            // 0-10 V. Map that voltage to the LFO's 0-50 Hz range. A raw
            // Knob CC route intentionally stays raw and is clamped by the
            // LFO parameter setter itself.
            if (target.type == NodeType::Lfo && strcmp(route.paramName, "rate") == 0 &&
                kind != ControlRoute::Kind::KnobCc)
            {
                if (targetValue < 0.0f) targetValue = 0.0f;
                if (targetValue > cv::kMaxVolts) targetValue = cv::kMaxVolts;
                targetValue *= target.lfo.maxRateHz / cv::kMaxVolts;
            }
            setFloatFieldByName(target, target.type, route.paramName, targetValue);
            if (route.targetPort >= 0)
            {
                target.controlValue[route.targetPort] = targetValue;
            }
        }
    }
}

// Delivers one note-on/off event's control routes from `sourceWidget` to
// every target it feeds. Used both for a real midi-in's own routes (side=-1:
// deliver every route regardless of ControlRoute::side) AND, recursively,
// for a midi-split widget forwarding to its own lower/upper targets (side=0
// or 1: only deliver routes tagged for that side - see the MidiGate case
// below). Also the shared body behind handleMidiEvent's own note on/off
// delivery and releaseVoiceGate's sustain-pedal-up replay, so both paths stay
// byte-identical instead of duplicating this switch.
inline void deliverNoteControlRoutes(Graph &graph, uint16_t sourceWidget, uint8_t voiceIdx, uint8_t note, bool noteOn,
                               uint8_t velocity, int side)
{
    for (uint16_t r = 0; r < graph.controlRouteCount; ++r)
    {
        const ControlRoute &route = graph.controlRoutes[r];
        if (route.sourceWidget != sourceWidget)
        {
            continue;
        }
        if (side >= 0 && route.side != static_cast<uint8_t>(side))
        {
            continue;
        }
        const WidgetSlot &targetWidget = graph.widgets[route.targetWidget];
        const uint8_t vIdx = targetWidget.voiced ? voiceIdx : 0;
        NodeInstance &target = graph.instances[targetWidget.instanceBase + vIdx];

        switch (route.kind)
        {
        case ControlRoute::Kind::MidiNote:
            if (route.targetPort >= 0)
            {
                target.controlValue[route.targetPort] = static_cast<float>(note);
            }
            break;
        case ControlRoute::Kind::MidiGate:
            if (target.type == NodeType::Oscillator)
            {
                if (noteOn && target.osc.wave != WaveType::Noise && target.osc.wave != WaveType::PinkNoise)
                {
                    target.osc.phaseGen.resetPhase();
                }
                target.osc.gatePrev = noteOn;
            }
            else if (target.type == NodeType::Adsr)
            {
                target.adsr->ramp.gate(noteOn);
                target.adsr->gatePrev = noteOn;
            }
            else if (target.type == NodeType::WavePlayer)
            {
                // Event-driven, not edge-detected (unlike NodeRender.h's own
                // gate-value fallback for non-MidiGate sources e.g. Button) -
                // a sustain-held note re-struck, or a sustain-held voice
                // stolen for a new note, delivers noteOn=true here while the
                // gate VALUE stays at 1 throughout (sustain suppresses the
                // intervening 0), so an edge-based retrigger would miss it.
                // Firing directly off noteOn, like Oscillator's phase reset
                // above, is immune to that.
                if (noteOn)
                {
                    target.wavePlayer.position = 0.0f;
                    target.wavePlayer.playing =
                        (target.wavePlayer.sampleData != nullptr && target.wavePlayer.frameCount > 1);
                    target.wavePlayer.fadeGain = 1.0f;
                }
                target.wavePlayer.gatePrev = noteOn;
            }
            else if (target.type == NodeType::SoundFontPlayer)
            {
                target.soundFontPlayer.currentNote = note;
                if (noteOn)
                {
                    // Delay until renderBlock so amplitude/velocity routes for
                    // this event have been delivered regardless of edge order.
                    target.soundFontPlayer.pendingNoteOn = true;
                }
                else
                {
                    soundfont::noteOff(target.soundFontPlayer, note);
                }
                target.soundFontPlayer.gatePrev = noteOn;
            }
            else if (target.type == NodeType::MidiSplit)
            {
                // Gate is "the event" - MidiSplit's single "midi" input
                // resolves to this same Kind (see GraphBuilder.h's MidiIn->
                // MidiSplit connection handling), so this is the one and only
                // delivery point. Zones may overlap by design: every zone
                // whose range contains the note fires independently (a held
                // key can layer multiple downstream chains at once), not
                // just the first match.
                const auto &split = target.midiSplit;
                for (int z = 0; z < split.zoneCount; ++z)
                {
                    const auto &zone = split.zones[z];
                    if (static_cast<int>(note) < zone.fromNote || static_cast<int>(note) > zone.toNote)
                    {
                        continue;
                    }
                    int transposedNote = static_cast<int>(note) + zone.octaveTranspose * 12;
                    if (transposedNote < 0)
                        transposedNote = 0;
                    if (transposedNote > 127)
                        transposedNote = 127;
                    deliverNoteControlRoutes(graph, route.targetWidget, voiceIdx, static_cast<uint8_t>(transposedNote),
                                              noteOn, velocity, z);
                }
                // note falls in a gap between all zones - silently dropped, no forward.
            }
            if (route.targetPort >= 0)
            {
                target.controlValue[route.targetPort] = noteOn ? cv::kGateOnVolts : cv::kGateOffVolts;
            }
            break;
        case ControlRoute::Kind::MidiVelocity:
        {
            if (!noteOn)
            {
                // Note-off "velocity" is the rarely used MIDI release
                // velocity (usually 0, depending on the keyboard) - a
                // different quantity than the attack strength and must not
                // overwrite amplitude/adsr.velocity, otherwise the level
                // jumps to 0 exactly on release (click, and it also
                // overrides the actual release). The JS reference engine
                // (main.js: routeMidiToWidgets) also only propagates
                // velocity on note_on.
                break;
            }
            // Internal DSP gain (0-1) stays unchanged - only the externally
            // visible CV representation (controlValue, e.g. for a connected
            // Multiplier/Adder/Divide) now follows 0-10V.
            const float velGain = static_cast<float>(velocity) / 127.0f;
            if (target.type == NodeType::Oscillator)
            {
                target.osc.amplitude = velGain;
            }
            else if (target.type == NodeType::Hammond)
            {
                // Same dedicated-field delivery as Oscillator above, and the
                // same consequence: a Hammond only sounds while something
                // actually feeds its "amplitude" port, and the level is NOT
                // cleared on note-off - shaping the note off is the job of a
                // downstream ADSR/Gain, exactly as with WaveGen.
                target.hammond->amplitude = velGain;
            }
            else if (target.type == NodeType::Adsr)
            {
                target.adsr->velocity = velGain;
            }
            if (route.targetPort >= 0)
            {
                target.controlValue[route.targetPort] = velGain * cv::kMaxVolts;
            }
            break;
        }
        case ControlRoute::Kind::MidiPitchCv:
            if (route.targetPort >= 0 && noteOn)
            {
                const MidiInState &midi = graph.instances[graph.widgets[sourceWidget].instanceBase].midiIn;
                const float targetCv = cv::noteToVoltage(static_cast<float>(note));
                target.midiPitchRampPort = route.targetPort;
                const float glideSeconds = midi.glideTime < 0.0f ? 0.0f : (midi.glideTime > 2.0f ? 2.0f : midi.glideTime);
                if (!target.midiPitchInitialized || midi.voices != 1 || !midi.glide || glideSeconds <= 0.0f)
                {
                    target.midiPitchCv = targetCv;
                    target.midiPitchTargetCv = targetCv;
                    target.midiPitchSamplesRemaining = 0;
                }
                else
                {
                    target.midiPitchTargetCv = targetCv;
                    target.midiPitchSamplesRemaining =
                        static_cast<uint32_t>(glideSeconds * static_cast<float>(synth::kSampleRate));
                }
                target.midiPitchInitialized = true;
                target.controlValue[route.targetPort] = target.midiPitchCv;
            }
            break;
        case ControlRoute::Kind::MidiRaw:
            // Raw MIDI routes are dispatched once from handleMidiEvent(),
            // never through scalar note/gate route delivery.
            break;
        case ControlRoute::Kind::Knob:
        case ControlRoute::Kind::KnobCc:
        case ControlRoute::Kind::Button:
        case ControlRoute::Kind::UpDownButton:
        case ControlRoute::Kind::Const:
            break; // handled in the CC branch above, never reached here
        }
    }
}

// Replays a full note-off delivery for a single voice index that does NOT
// come from the current MIDI event - used by the sustain-pedal-up flush,
// where a whole batch of previously-deferred voices need their gate=0
// delivered at once. Needs the voice's note (see VoiceAllocator::
// releaseSustained) purely so a midi-split downstream of this midi-in can
// still resolve which side/transpose the closing gate belongs to - narrowing
// this back to "only Gate routes" would silently break that resolution for
// any midi-in -> midi-split chain. Safe to run the full switch here:
// MidiVelocity's noteOn==false case is already a guaranteed no-op above, and
// MidiFrequency/MidiNote firing here just mirrors what a real note-off event
// already does in deliverNoteControlRoutes (frequency/note get rewritten
// there too on every note-off, not just note-on).
inline void releaseVoiceGate(Graph &graph, uint16_t midiInWidget, uint8_t voiceIndex, uint8_t note)
{
    deliverNoteControlRoutes(graph, midiInWidget, voiceIndex, note, /*noteOn=*/false, /*velocity=*/0, /*side=*/-1);
}
} // namespace

// IRAM_ATTR: the whole per-block hot path (voice-activity classification,
// input-buffer prep, every node's render() + edge-summing, output copy) -
// see documentation/native-synth-optimization-plan-2026-09-09.md section 1c
// / DspPrimitives.h's sineLookup() comment for why IRAM placement is worth
// it here.
IRAM_ATTR inline void Graph::renderBlock(Outputs &out, size_t frames)
{
    if (widgetProfileEmaAlpha == 0.0f && renderOrderCount > 0)
    {
        float retention = 1.0f;
        for (uint16_t i = 0; i < renderOrderCount; ++i)
        {
            retention *= 1.0f - kGraphWidgetLoadEmaAlpha;
        }
        widgetProfileEmaAlpha = 1.0f - retention;
    }

    enginestats::beginBlock();
    const int64_t phaseActivityStartUs = esp_timer_get_time();

    // Voice skipping: a voice v counts as active as long as any connected
    // ADSR of that voice has not truly reached 0 yet. An unconnected ADSR
    // must not silence the rest of the signal path. If there is no connected
    // ADSR, all voices stay active (current behavior for graphs without an
    // envelope).
    for (bool &a : voiceStillSounding)
    {
        a = false;
    }
    bool hasAdsr = false;
    for (uint16_t w = 0; w < widgetCount; ++w)
    {
        if (widgets[w].type != NodeType::Adsr || widgets[w].outgoingEdgeCount == 0)
        {
            continue;
        }
        hasAdsr = true;
        for (uint16_t v = 0; v < widgets[w].instanceCount; ++v)
        {
            if (!instances[widgets[w].instanceBase + v].adsr->ramp.isIdle())
            {
                voiceStillSounding[v] = true;
            }
        }
    }
    if (!hasAdsr)
    {
        for (bool &a : voiceStillSounding)
        {
            a = true;
        }
    }

    // Clear only accumulators that can receive data this block. An inactive
    // voiced instance is cleared when it becomes active again, before any
    // edge can propagate into it.
    for (uint16_t i = 0; i < instanceCount; ++i)
    {
        NodeInstance &inst = instances[i];
        const WidgetSlot &slot = widgets[inst.widgetIndex];
        if (slot.voiced && !slot.renderWhenIdle && !voiceStillSounding[inst.voiceIndex])
        {
            continue;
        }
        inst.controlRampPortMask = 0;
        inst.rampPortMask = 0;
        for (int p = 0; p < inst.audioInPortCount; ++p)
        {
            if (inst.inPorts[p] == nullptr)
            {
                continue;
            }
            const bool connected = (inst.audioConnectedPortMask & synth::portBit(p)) != 0;
            if (connected)
            {
                if ((inst.viewPortMask & synth::portBit(p)) != 0)
                {
                    // Borrowed view (see NodeInstance.h/GraphBuilder.h's
                    // post-pass) - this pointer IS the source instance's own
                    // output buffer, not an accumulator of ours. Clearing it
                    // here would just stomp the source's last block output
                    // before it even gets a chance to render this block's
                    // fresh value into the same memory. Nothing to do: the
                    // source's own render() (later in this same block, see
                    // renderOrder) is what fills it.
                    continue;
                }
                // Accumulator base for this block's edge-summing pass below.
                memset(inst.inPorts[p], 0, frames * sizeof(float));
                continue;
            }
            // No audio-rate edge, but a ControlRoute (MidiIn frequency/gate/
            // velocity, or a Knob/Button) targets this port. Oscillator's and
            // Hammond's "amplitude" (0) and "tune" (2) are excluded: both
            // already reach their own dedicated fields directly whenever the
            // route carries a paramName (Knob/Button - see ParamDispatch's
            // "tune" case writing s.tuneVolts), with zero further action
            // needed. Filling inPorts here too would be a second, redundant
            // delivery path - and NodeRender.h's Oscillator/Hammond cases
            // know this: they only read portSample() for "tune" when
            // audioConnectedPortMask is set (a genuine DSP edge, e.g. an LFO
            // wired directly), never merely because controlConnectedPortMask
            // is set, precisely so a Knob's value isn't shadowed by a stale
            // unconnected-default read from here. "pitchCv" (1) has no such
            // dedicated field (MidiPitchCv routes only ever write
            // controlValue, see deliverNoteControlRoutes) - safe, and
            // required, to fill normally, otherwise MidiIn->pitchCv never
            // reaches the inPorts buffer at all.
            const bool dedicatedPortField =
                (inst.type == NodeType::Oscillator || inst.type == NodeType::Hammond) && (p == 0 || p == 2);
            if (!dedicatedPortField &&
                (inst.controlConnectedPortMask & synth::portBit(p)) != 0)
            {
                if (p == inst.midiPitchRampPort && inst.midiPitchSamplesRemaining > 0)
                {
                    inst.controlRampPortMask |= synth::portBit(p);
                    size_t sample = 0;
                    for (; sample < frames && inst.midiPitchSamplesRemaining > 0; ++sample)
                    {
                        inst.midiPitchCv +=
                            (inst.midiPitchTargetCv - inst.midiPitchCv) /
                            static_cast<float>(inst.midiPitchSamplesRemaining);
                        inst.inPorts[p][sample] = inst.midiPitchCv;
                        --inst.midiPitchSamplesRemaining;
                    }
                    for (; sample < frames; ++sample)
                    {
                        inst.inPorts[p][sample] = inst.midiPitchCv;
                    }
                    inst.controlValue[p] = inst.midiPitchCv;
                    continue;
                }
                const float controlFill = inst.controlValue[p];
                for (size_t s = 0; s < frames; ++s)
                {
                    inst.inPorts[p][s] = controlFill;
                }
                continue;
            }
            // No preset edge targets this port at all - render its
            // type-specific "no cable here" default instead of silent 0
            // (e.g. Multiplier factors and Divide's divisor default to 1).
            const float fillValue = ports::audioInDefault(inst.type, p);
            if (fillValue == 0.0f)
            {
                memset(inst.inPorts[p], 0, frames * sizeof(float));
            }
            else
            {
                for (size_t s = 0; s < frames; ++s)
                {
                    inst.inPorts[p][s] = fillValue;
                }
            }
        }
    }

    {
        const float activityPercent =
            100.0f * static_cast<float>(esp_timer_get_time() - phaseActivityStartUs) / kGraphBlockBudgetUs;
        phaseActivityPercent += kGraphWidgetLoadEmaAlpha * (activityPercent - phaseActivityPercent);
    }
    const int64_t phaseRenderStartUs = esp_timer_get_time();

    for (uint16_t r = 0; r < renderOrderCount; ++r)
    {
        const WidgetSlot &w = widgets[renderOrder[r]];
        const bool profileWidget = r == widgetProfileCursor;
        if (w.type == NodeType::Lfo && w.outgoingEdgeCount == 0)
        {
            if (profileWidget)
            {
                float &smoothed = widgetLoadPercent[renderOrder[r]];
                smoothed += widgetProfileEmaAlpha * (0.0f - smoothed);
            }
            continue;
        }
        if (!w.needed)
        {
            // No edge-path from this widget reaches a hardware sink or an
            // active monitor tap (see WidgetSlot::needed / GraphBuilder.h's
            // reachability post-pass) - skip render AND this widget's own
            // outgoing edges, safe by construction: a widget only ends up
            // not-needed when none of ITS edges lead anywhere needed either,
            // so nothing downstream is missing data because of this.
            if (profileWidget)
            {
                float &smoothed = widgetLoadPercent[renderOrder[r]];
                smoothed += widgetProfileEmaAlpha * (0.0f - smoothed);
                float &smoothedEdge = widgetEdgeLoadPercent[renderOrder[r]];
                smoothedEdge += widgetProfileEmaAlpha * (0.0f - smoothedEdge);
            }
            continue;
        }
        // A widget-global Lfo (see LfoState::global / NodeRender.h's Lfo
        // case) must keep its phase advancing even while voice 0 itself has
        // no active note - every other voice just mirrors voice 0's output,
        // so letting the usual voice-skip freeze voice 0 on note-off would
        // silently freeze the shared signal for every still-held voice too.
        // Mirrors the exact same "is global active" check NodeRender.h's Lfo
        // case makes for itself.
        bool forceLfoPrimaryActive = false;
        if (w.type == NodeType::Lfo && w.voiced && w.instanceCount > 1)
        {
            const NodeInstance &primaryInst = instances[w.instanceBase];
            const bool modeFed = (primaryInst.controlConnectedPortMask & static_cast<uint8_t>(1u << 2)) != 0 ||
                                  (primaryInst.audioConnectedPortMask & static_cast<uint8_t>(1u << 2)) != 0;
            forceLfoPrimaryActive =
                modeFed ? (noderender::portSample(primaryInst, 2, 0) >= cv::kGateThresholdVolts) : primaryInst.lfo.global;
        }
        const int64_t widgetRenderStartUs = profileWidget ? esp_timer_get_time() : 0;
        for (uint16_t v = 0; v < w.instanceCount; ++v)
        {
            NodeInstance &inst = instances[w.instanceBase + v];
            ++enginestats::current.instancesTotal;
            // Idle here means "this voice slot's own activity check would
            // have skipped it" - forced on anyway by renderWhenIdle/global
            // widgets or the LFO-primary-stays-live exception below (see
            // ForceLfoPrimaryActive above). Matches plan section 7's request
            // for "trotz Ruhe gerenderte" instances as a distinct count from
            // ordinary active-voice rendering.
            const bool voiceIdle = w.voiced && !voiceStillSounding[v];
            if (!w.voiced || w.renderWhenIdle || voiceStillSounding[v] || (forceLfoPrimaryActive && v == 0))
            {
                processNodeBlock(inst, frames);
                inst.wasRenderedLastBlock = true;
                ++enginestats::current.instancesRendered;
                if (voiceIdle) ++enginestats::current.instancesRenderedWhileIdle;
            }
            else if (inst.wasRenderedLastBlock)
            {
                // First silent block after activity: zero once, afterwards
                // the buffer is already silent - no repeated memset needed.
                memset(inst.outL, 0, frames * sizeof(float));
                if (inst.outR != nullptr)
                {
                    memset(inst.outR, 0, frames * sizeof(float));
                }
                inst.wasRenderedLastBlock = false;
            }
        }
        if (profileWidget)
        {
            const float instPercent =
                100.0f * static_cast<float>(esp_timer_get_time() - widgetRenderStartUs) / kGraphBlockBudgetUs;
            float &smoothed = widgetLoadPercent[renderOrder[r]];
            smoothed += widgetProfileEmaAlpha * (instPercent - smoothed);
        }

        // Propagate this widget's freshly-rendered outputs along its
        // outgoing edges. Safe because renderOrder is topological: every
        // edge target is processed later in this same loop.
        const int64_t widgetEdgeStartUs = profileWidget ? esp_timer_get_time() : 0;
        const uint16_t edgeEnd = w.outgoingEdgeStart + w.outgoingEdgeCount;
        for (uint16_t e = w.outgoingEdgeStart; e < edgeEnd; ++e)
        {
            const Edge &edge = edges[e];
            const NodeInstance &src = instances[edge.fromInstance];
            NodeInstance &dst = instances[edge.toInstance];
            const WidgetSlot &srcWidget = widgets[src.widgetIndex];
            const WidgetSlot &dstWidget = widgets[dst.widgetIndex];
            if (edge.monitorTap && src.voiceIndex != monitorVoiceIndex)
            {
                continue;
            }
            if ((srcWidget.voiced && !srcWidget.renderWhenIdle && !voiceStillSounding[src.voiceIndex]) ||
                (dstWidget.voiced && !dstWidget.renderWhenIdle && !voiceStillSounding[dst.voiceIndex]))
            {
                continue;
            }
            if ((dst.viewPortMask & synth::portBit(edge.toPort)) != 0)
            {
                // dst.inPorts[edge.toPort] already IS srcBuf (see
                // NodeInstance.h/GraphBuilder.h's post-pass) - nothing to
                // accumulate, `dstBuf[i] += srcBuf[i]` here would double the
                // value since both names refer to the same memory.
                //
                // GraphBuilder.h's post-pass already proved this is the
                // sole edge into this port (view-eligibility requires
                // targetEdgeCount==1) - safe to copy src's ramp tag (if
                // any) without risk of a second edge overwriting or
                // contradicting it later in this same loop. See
                // NodeInstance.h's rampPortMask comment.
                if (src.outIsRamp)
                {
                    const uint8_t slot = static_cast<uint8_t>(__builtin_popcount(dst.rampPortMask));
                    if (slot < NodeInstance::kMaxRampedPorts)
                    {
                        dst.rampDesc[slot] = {edge.toPort, src.outRamp.start, src.outRamp.inc};
                        dst.rampPortMask |= synth::portBit(edge.toPort);
                    }
                }
                continue;
            }
            const float *srcBuf = outputPortBuffer(src, edge.fromPort);
            float *dstBuf = dst.inPorts[edge.toPort];
            if (srcBuf == nullptr || dstBuf == nullptr)
            {
                continue;
            }
            if (edge.scale == 1.0f)
            {
                for (size_t i = 0; i < frames; ++i)
                {
                    dstBuf[i] += srcBuf[i];
                }
            }
            else
            {
                for (size_t i = 0; i < frames; ++i)
                {
                    dstBuf[i] += srcBuf[i] * edge.scale;
                }
            }
        }
        if (profileWidget)
        {
            const float edgePercent =
                100.0f * static_cast<float>(esp_timer_get_time() - widgetEdgeStartUs) / kGraphBlockBudgetUs;
            float &smoothedEdge = widgetEdgeLoadPercent[renderOrder[r]];
            smoothedEdge += widgetProfileEmaAlpha * (edgePercent - smoothedEdge);
        }
    }

    {
        const float renderPercent =
            100.0f * static_cast<float>(esp_timer_get_time() - phaseRenderStartUs) / kGraphBlockBudgetUs;
        phaseRenderPercent += kGraphWidgetLoadEmaAlpha * (renderPercent - phaseRenderPercent);
    }

    if (renderOrderCount > 0 && ++widgetProfileCursor >= renderOrderCount)
    {
        widgetProfileCursor = 0;
        ++widgetLoadGeneration;
    }

    const int64_t phaseOutputStartUs = esp_timer_get_time();
    auto fillOutput = [&](int16_t widgetIdx, float *outL, float *outR) {
        if (widgetIdx >= 0)
        {
            const NodeInstance &inst = instances[widgets[widgetIdx].instanceBase];
            memcpy(outL, inst.outL, frames * sizeof(float));
            memcpy(outR, inst.outR, frames * sizeof(float));
        }
        else
        {
            memset(outL, 0, frames * sizeof(float));
            memset(outR, 0, frames * sizeof(float));
        }
    };
    fillOutput(usbOutWidget, out.usbL, out.usbR);
    fillOutput(es8311OutWidget, out.esL, out.esR);
    fillOutput(wm8960OutWidget, out.wmL, out.wmR);
    {
        const float outputPercent =
            100.0f * static_cast<float>(esp_timer_get_time() - phaseOutputStartUs) / kGraphBlockBudgetUs;
        phaseOutputPercent += kGraphWidgetLoadEmaAlpha * (outputPercent - phaseOutputPercent);
    }
}

inline void Graph::handleMidiEvent(const UsbMidiEvent &event)
{
    // Direct MIDI cable: each wired MidiIn->SoundFontPlayer route delivers
    // complete channel messages straight to that SoundFontPlayer's own
    // synth (a preset can have several, see soundfont::sweepFileCache() -
    // TinySoundFont owns polyphony independently per instance). Bypasses
    // graph voice allocation entirely.
    for (uint16_t r = 0; r < controlRouteCount; ++r)
    {
        const ControlRoute &route = controlRoutes[r];
        if (route.kind != ControlRoute::Kind::MidiRaw) continue;
        const WidgetSlot &source = widgets[route.sourceWidget];
        if (source.type != NodeType::MidiIn) continue;
        const MidiInState &midi = instances[source.instanceBase].midiIn;
        const int eventChannel = event.status & 0x0F;
        if (midi.channel >= 0 && midi.channel != eventChannel) continue;
        WidgetSlot &target = widgets[route.targetWidget];
        soundfont::handleMidi(instances[target.instanceBase].soundFontPlayer, event);
    }

    // Control Change: knob CC-learn/mapping, entirely independent of note
    // on/off below. Runs natively on the ESP regardless of whether a
    // browser is even connected (see plan: "ESP-nativer" delivery path).
    if (event.codeIndex == 0xB)
    {
        const uint8_t ccNumber = event.data1;
        const uint8_t ccValue = event.data2;
        const uint8_t ccChannel0 = event.status & 0x0F;

        for (uint16_t w = 0; w < widgetCount; ++w)
        {
            if (widgets[w].type == NodeType::Knob)
            {
                NodeInstance &knobInst = instances[widgets[w].instanceBase];
                KnobState &k = *knobInst.knob;
                // Identity is the (cc, channel) pair together - some
                // controllers reuse the same CC number across multiple
                // physical knobs/faders, distinguishing them only by MIDI
                // channel.
                if (k.cc < 0 || static_cast<uint8_t>(k.cc) != ccNumber)
                {
                    continue;
                }
                if (k.channel < 0 || static_cast<uint8_t>(k.channel) != ccChannel0)
                {
                    continue;
                }

                k.raw = static_cast<float>(ccValue);
                k.currentValue = k.mappedValue();

                pushControlRouteValue(*this, w, ControlRoute::Kind::Knob, k.currentValue);
                pushControlRouteValue(*this, w, ControlRoute::Kind::KnobCc, k.raw);
            }
            else if (widgets[w].type == NodeType::Button)
            {
                NodeInstance &btnInst = instances[widgets[w].instanceBase];
                ButtonState &b = *btnInst.button;
                if (b.cc < 0 || static_cast<uint8_t>(b.cc) != ccNumber)
                {
                    continue;
                }
                if (b.channel < 0 || static_cast<uint8_t>(b.channel) != ccChannel0)
                {
                    continue;
                }

                // List mode advances only on the press edge (wasAbove flips
                // false->true) - release, or repeated CC messages while held
                // above the threshold, must not skip entries.
                const bool above = ccValue >= 64;
                if (b.valueListCount >= 2 && above && !b.wasAbove)
                {
                    b.listIndex = static_cast<uint8_t>((b.listIndex + 1) % b.valueListCount);
                }
                b.wasAbove = above;
                b.currentValue = b.currentOutput();

                pushControlRouteValue(*this, w, ControlRoute::Kind::Button, b.currentValue);
            }
            else if (widgets[w].type == NodeType::UpDownButton)
            {
                // Every CC message on ccUp/ccDown counts as one tick,
                // regardless of its value (0 and 127 are treated identically)
                // - matches relative/encoder-style MIDI controllers that send
                // a fixed value per direction rather than a momentary
                // above/below-64 press. No edge detection needed.
                NodeInstance &udInst = instances[widgets[w].instanceBase];
                UpDownButtonState &ud = *udInst.upDownButton;
                bool matchedUp = ud.ccUp >= 0 && static_cast<uint8_t>(ud.ccUp) == ccNumber &&
                                  ud.channelUp >= 0 && static_cast<uint8_t>(ud.channelUp) == ccChannel0;
                bool matchedDown = ud.ccDown >= 0 && static_cast<uint8_t>(ud.ccDown) == ccNumber &&
                                    ud.channelDown >= 0 && static_cast<uint8_t>(ud.channelDown) == ccChannel0;
                if (!matchedUp && !matchedDown)
                {
                    continue;
                }
                if (matchedUp)
                {
                    if (ud.valueListCount >= 2)
                    {
                        if (ud.listIndex + 1 < ud.valueListCount)
                            ++ud.listIndex;
                    }
                    else
                    {
                        ud.currentValue += ud.step;
                        if (ud.currentValue > ud.maxVal)
                            ud.currentValue = ud.maxVal;
                    }
                }
                if (matchedDown)
                {
                    if (ud.valueListCount >= 2)
                    {
                        if (ud.listIndex > 0)
                            --ud.listIndex;
                    }
                    else
                    {
                        ud.currentValue -= ud.step;
                        if (ud.currentValue < ud.minVal)
                            ud.currentValue = ud.minVal;
                    }
                }
                pushControlRouteValue(*this, w, ControlRoute::Kind::UpDownButton, ud.currentOutput());
            }
            else if (widgets[w].type == NodeType::Switch)
            {
                NodeInstance &swInst = instances[widgets[w].instanceBase];
                SwitchState &sw = swInst.switchState;
                if (sw.cc < 0 || static_cast<uint8_t>(sw.cc) != ccNumber)
                {
                    continue;
                }
                if (sw.channel < 0 || static_cast<uint8_t>(sw.channel) != ccChannel0)
                {
                    continue;
                }
                // Direct value mapping, not a toggle: CC 0 -> In 1, CC 127 ->
                // In 2 (>= 64, same threshold as every other CC-to-bool
                // mapping in this codebase), so an on/off-style controller
                // works as expected instead of needing a momentary press.
                sw.selectIn2 = ccValue >= 64;
            }
            else if (widgets[w].type == NodeType::MidiIn)
            {
                // Sustain pedal, CC64: >=64 down / <64 up, same convention
                // already used by Button's CC-learn above. Always active per
                // MidiIn, no enable/disable toggle (see plan).
                if (ccNumber != 64)
                {
                    continue;
                }
                NodeInstance &midiInst = instances[widgets[w].instanceBase];
                MidiInState &m = midiInst.midiIn;
                if (m.channel >= 0 && static_cast<uint8_t>(m.channel) != ccChannel0)
                {
                    continue;
                }

                const bool wasDown = m.sustainPedalDown;
                const bool nowDown = ccValue >= 64;
                m.sustainPedalDown = nowDown;

                if (wasDown && !nowDown)
                {
                    int8_t released[SYNTH_VOICE_COUNT];
                    uint8_t releasedNotes[SYNTH_VOICE_COUNT];
                    const uint8_t count =
                        voiceAllocators[w].releaseSustained(released, releasedNotes, SYNTH_VOICE_COUNT);
                    for (uint8_t i = 0; i < count; ++i)
                    {
                        releaseVoiceGate(*this, w, static_cast<uint8_t>(released[i]), releasedNotes[i]);
                    }
                }
            }
        }
        return;
    }

    // Only note on/off drive the graph beyond this point (CIN 0x9 note-on,
    // 0x8 note-off; a note-on with velocity 0 is a note-off, matching the
    // USB-MIDI/mido convention already used elsewhere in this codebase).
    bool noteOn = false;
    uint8_t note = 0;
    uint8_t velocity = 0;
    if (event.codeIndex == 0x9)
    {
        note = event.data1;
        velocity = event.data2;
        if (velocity == 0)
        {
            noteOn = false;
        }
        else
        {
            noteOn = true;
        }
    }
    else if (event.codeIndex == 0x8)
    {
        note = event.data1;
        velocity = event.data2;
    }
    else
    {
        return;
    }

    const uint8_t eventChannel0 = event.status & 0x0F;

    for (uint16_t w = 0; w < widgetCount; ++w)
    {
        if (widgets[w].type != NodeType::MidiIn)
        {
            continue;
        }
        NodeInstance &midiInst = instances[widgets[w].instanceBase];
        const int8_t midiChannel = midiInst.midiIn.channel;
        if (midiChannel >= 0 && midiChannel != static_cast<int8_t>(eventChannel0))
        {
            continue;
        }

        VoiceAllocator &alloc = voiceAllocators[w];
        const int16_t previousNote = alloc.currentNote(0);
        const int8_t voiceIdx =
            noteOn ? alloc.noteOn(note, voiceStillSounding) : alloc.noteOff(note, midiInst.midiIn.sustainPedalDown);
        if (voiceIdx < 0)
        {
            continue;
        }
        if (noteOn)
        {
            monitorVoiceIndex = static_cast<uint8_t>(voiceIdx);
        }

        if (!noteOn && alloc.voiceCount() == 1)
        {
            const int16_t fallbackNote = alloc.currentNote(0);
            if (fallbackNote >= 0 && fallbackNote != previousNote)
            {
                deliverNoteControlRoutes(*this, w, 0, static_cast<uint8_t>(fallbackNote), true, 127, /*side=*/-1);
            }
            else if (fallbackNote < 0)
            {
                deliverNoteControlRoutes(*this, w, 0, note, false, velocity, /*side=*/-1);
            }
            continue;
        }

        deliverNoteControlRoutes(*this, w, static_cast<uint8_t>(voiceIdx), note, noteOn, velocity, /*side=*/-1);
    }
}

inline uint16_t Graph::activeSoundFontVoiceCount() const
{
    uint16_t total = 0;
    for (uint16_t w = 0; w < widgetCount; ++w)
    {
        if (widgets[w].type == NodeType::SoundFontPlayer)
        {
            const int count = soundfont::activeVoiceCount(instances[widgets[w].instanceBase].soundFontPlayer);
            if (count > 0)
            {
                total += static_cast<uint16_t>(count);
            }
        }
    }
    return total;
}

inline void Graph::midiVoiceUsage(uint16_t &active, uint16_t &maximum) const
{
    active = 0;
    maximum = 0;
    for (uint16_t w = 0; w < widgetCount; ++w)
    {
        if (widgets[w].type != NodeType::MidiIn)
        {
            continue;
        }
        active += voiceAllocators[w].activeVoiceCount();
        const uint16_t configured = instances[widgets[w].instanceBase].midiIn.voices;
        maximum += configured < 1 ? 1 : configured;
    }
}

namespace
{
// Live-param counterpart of PresetFill's getFloatArray() - same job (parse
// a JSON number array into a fixed buffer, capped at maxCount), just against
// an already-resolved cJSON item instead of a (parent, key) lookup.
inline uint8_t applyFloatArrayItem(cJSON *item, float *out, uint8_t maxCount)
{
    if (!cJSON_IsArray(item))
    {
        return 0;
    }
    uint8_t count = 0;
    cJSON *entry = nullptr;
    cJSON_ArrayForEach(entry, item)
    {
        if (count >= maxCount)
        {
            break;
        }
        out[count] = cJSON_IsNumber(entry) ? static_cast<float>(entry->valuedouble) : 0.0f;
        ++count;
    }
    return count;
}
} // namespace

inline void Graph::applyLiveParams(const char *widgetId, cJSON *paramsObj)
{
    if (widgetId == nullptr || paramsObj == nullptr)
    {
        return;
    }

    int32_t widgetIdx = -1;
    for (uint16_t w = 0; w < widgetCount; ++w)
    {
        if (strcmp(widgets[w].id, widgetId) == 0)
        {
            widgetIdx = w;
            break;
        }
    }
    if (widgetIdx < 0)
    {
        return;
    }
    WidgetSlot &slot = widgets[widgetIdx];

    cJSON *item = nullptr;
    cJSON_ArrayForEach(item, paramsObj)
    {
        const char *key = item->string;
        if (key == nullptr)
        {
            continue;
        }

        if (slot.type == NodeType::MidiIn && strcmp(key, "glide") == 0 && cJSON_IsBool(item))
        {
            instances[slot.instanceBase].midiIn.glide = cJSON_IsTrue(item);
            continue;
        }
        if (slot.type == NodeType::MidiIn && strcmp(key, "glideTime") == 0 && cJSON_IsNumber(item))
        {
            const float value = static_cast<float>(item->valuedouble);
            instances[slot.instanceBase].midiIn.glideTime = value < 0.0f ? 0.0f : (value > 2.0f ? 2.0f : value);
            continue;
        }

        if (cJSON_IsNumber(item))
        {
            const float value = static_cast<float>(item->valuedouble);
            bool handled = false;
            for (uint16_t v = 0; v < slot.instanceCount; ++v)
            {
                handled = setFloatFieldByName(instances[slot.instanceBase + v], slot.type, key, value) || handled;
            }
            if (handled)
            {
                continue;
            }
        }

        if (slot.type == NodeType::SoundFontPlayer && cJSON_IsBool(item) &&
            (strcmp(key, "enableCc") == 0 || strcmp(key, "enableModWheel") == 0 ||
             strcmp(key, "enablePitchWheel") == 0))
        {
            setFloatFieldByName(instances[slot.instanceBase], slot.type, key, cJSON_IsTrue(item) ? 1.0f : 0.0f);
            continue;
        }

        // Non-float / special-cased fields that setFloatFieldByName doesn't
        // cover (strings, nullable ints, bools).
        if ((slot.type == NodeType::Oscillator || slot.type == NodeType::Lfo) && strcmp(key, "waveType") == 0 &&
            cJSON_IsString(item))
        {
            const WaveType wave = waveTypeFromString(item->valuestring);
            for (uint16_t v = 0; v < slot.instanceCount; ++v)
            {
                NodeInstance &inst = instances[slot.instanceBase + v];
                if (slot.type == NodeType::Oscillator)
                {
                    inst.osc.wave = wave;
                }
                else
                {
                    inst.lfo.wave = wave;
                }
            }
        }
        else if (slot.type == NodeType::Filter && strcmp(key, "filterType") == 0 && cJSON_IsString(item))
        {
            const FilterKind kind = filterKindFromString(item->valuestring);
            for (uint16_t v = 0; v < slot.instanceCount; ++v)
            {
                NodeInstance &inst = instances[slot.instanceBase + v];
                inst.filter->kind = kind;
                inst.filter->coeffsDirty = true;
            }
        }
        else if (slot.type == NodeType::CurtisFilter && strcmp(key, "filterType") == 0 && cJSON_IsString(item))
        {
            const CurtisFilterMode mode = curtisFilterModeFromString(item->valuestring);
            for (uint16_t v = 0; v < slot.instanceCount; ++v)
                nodes::curtisfilter::setMode(instances[slot.instanceBase + v], mode);
        }
        else if (slot.type == NodeType::Filter && strcmp(key, "cutoffCurve") == 0 && cJSON_IsString(item))
        {
            const bool log = strcmp(item->valuestring, "log") == 0;
            for (uint16_t v = 0; v < slot.instanceCount; ++v)
            {
                instances[slot.instanceBase + v].filter->cutoffLog = log;
                instances[slot.instanceBase + v].filter->refreshLog2Ratios();
                instances[slot.instanceBase + v].filter->coeffsDirty = true;
            }
        }
        else if (slot.type == NodeType::CurtisFilter && strcmp(key, "cutoffCurve") == 0 && cJSON_IsString(item))
        {
            const bool log = strcmp(item->valuestring, "log") == 0;
            for (uint16_t v = 0; v < slot.instanceCount; ++v)
            {
                instances[slot.instanceBase + v].curtisFilter->cutoffLog = log;
                instances[slot.instanceBase + v].curtisFilter->refreshLog2Ratios();
                instances[slot.instanceBase + v].curtisFilter->coeffsDirty = true;
            }
        }
        else if (slot.type == NodeType::Filter && strcmp(key, "resonanceCurve") == 0 && cJSON_IsString(item))
        {
            const bool log = strcmp(item->valuestring, "log") == 0;
            for (uint16_t v = 0; v < slot.instanceCount; ++v)
            {
                instances[slot.instanceBase + v].filter->resonanceLog = log;
                instances[slot.instanceBase + v].filter->refreshLog2Ratios();
                instances[slot.instanceBase + v].filter->coeffsDirty = true;
            }
        }
        else if (slot.type == NodeType::CurtisFilter && strcmp(key, "resonanceCurve") == 0 && cJSON_IsString(item))
        {
            const bool log = strcmp(item->valuestring, "log") == 0;
            for (uint16_t v = 0; v < slot.instanceCount; ++v)
            {
                instances[slot.instanceBase + v].curtisFilter->resonanceLog = log;
                instances[slot.instanceBase + v].curtisFilter->refreshLog2Ratios();
                instances[slot.instanceBase + v].curtisFilter->coeffsDirty = true;
            }
        }
        else if (slot.type == NodeType::Adsr && strcmp(key, "attackCurve") == 0 && cJSON_IsString(item))
        {
            const bool log = strcmp(item->valuestring, "log") == 0;
            for (uint16_t v = 0; v < slot.instanceCount; ++v)
            {
                instances[slot.instanceBase + v].adsr->attackLog = log;
            }
        }
        else if (slot.type == NodeType::Adsr && strcmp(key, "decayCurve") == 0 && cJSON_IsString(item))
        {
            const bool log = strcmp(item->valuestring, "log") == 0;
            for (uint16_t v = 0; v < slot.instanceCount; ++v)
            {
                instances[slot.instanceBase + v].adsr->decayLog = log;
            }
        }
        else if (slot.type == NodeType::Adsr && strcmp(key, "releaseCurve") == 0 && cJSON_IsString(item))
        {
            const bool log = strcmp(item->valuestring, "log") == 0;
            for (uint16_t v = 0; v < slot.instanceCount; ++v)
            {
                instances[slot.instanceBase + v].adsr->releaseLog = log;
            }
        }
        else if (slot.type == NodeType::Delay &&
                 (strcmp(key, "timeCurve") == 0 || strcmp(key, "feedbackCurve") == 0 || strcmp(key, "mixCurve") == 0) &&
                 cJSON_IsString(item))
        {
            const bool log = strcmp(item->valuestring, "log") == 0;
            for (uint16_t v = 0; v < slot.instanceCount; ++v)
            {
                DelayState &d = *instances[slot.instanceBase + v].delay;
                if (strcmp(key, "timeCurve") == 0)
                    d.timeLog = log;
                else if (strcmp(key, "feedbackCurve") == 0)
                    d.feedbackLog = log;
                else
                    d.mixLog = log;
            }
        }
        else if (slot.type == NodeType::Reverb &&
                 (strcmp(key, "roomSizeCurve") == 0 || strcmp(key, "dampingCurve") == 0 ||
                  strcmp(key, "wetLevelCurve") == 0 || strcmp(key, "dryLevelCurve") == 0 ||
                  strcmp(key, "preDelayCurve") == 0) &&
                 cJSON_IsString(item))
        {
            const bool log = strcmp(item->valuestring, "log") == 0;
            for (uint16_t v = 0; v < slot.instanceCount; ++v)
            {
                ReverbState &r = *instances[slot.instanceBase + v].reverb;
                if (strcmp(key, "roomSizeCurve") == 0)
                    r.roomSizeLog = log;
                else if (strcmp(key, "dampingCurve") == 0)
                    r.dampingLog = log;
                else if (strcmp(key, "wetLevelCurve") == 0)
                    r.wetLevelLog = log;
                else if (strcmp(key, "dryLevelCurve") == 0)
                    r.dryLevelLog = log;
                else
                    r.preDelayLog = log;
            }
        }
        else if (slot.type == NodeType::ReverbLight &&
                 (strcmp(key, "roomSizeCurve") == 0 || strcmp(key, "dampingCurve") == 0 ||
                  strcmp(key, "wetLevelCurve") == 0 || strcmp(key, "dryLevelCurve") == 0) &&
                 cJSON_IsString(item))
        {
            const bool log = strcmp(item->valuestring, "log") == 0;
            for (uint16_t v = 0; v < slot.instanceCount; ++v)
            {
                ReverbLightState &r = *instances[slot.instanceBase + v].reverbLight;
                if (strcmp(key, "roomSizeCurve") == 0)
                    r.roomSizeLog = log;
                else if (strcmp(key, "dampingCurve") == 0)
                    r.dampingLog = log;
                else if (strcmp(key, "wetLevelCurve") == 0)
                    r.wetLevelLog = log;
                else
                    r.dryLevelLog = log;
            }
        }
        else if (slot.type == NodeType::Gain && strcmp(key, "curve") == 0 && cJSON_IsString(item))
        {
            const bool exponential = strcmp(item->valuestring, "exponential") == 0;
            for (uint16_t v = 0; v < slot.instanceCount; ++v)
            {
                instances[slot.instanceBase + v].gain.exponential = exponential;
            }
        }
        else if (slot.type == NodeType::EspEq && strcmp(key, "filterType") == 0 && cJSON_IsString(item))
        {
            const EqFilterKind kind = eqFilterKindFromString(item->valuestring);
            for (uint16_t v = 0; v < slot.instanceCount; ++v)
            {
                EspEqState &eq = *instances[slot.instanceBase + v].espEq;
                updateEspEq(eq, kind, eq.frequencyHz, eq.q, eq.gainDb);
            }
        }
        else if (slot.type == NodeType::MidiIn && strcmp(key, "overflowMode") == 0 && cJSON_IsString(item))
        {
            const OverflowMode mode = overflowModeFromString(item->valuestring);
            for (uint16_t v = 0; v < slot.instanceCount; ++v)
            {
                instances[slot.instanceBase + v].midiIn.overflowMode = mode;
            }
        }
        else if (slot.type == NodeType::MidiIn && strcmp(key, "channel") == 0)
        {
            const int8_t ch = cJSON_IsNumber(item) ? static_cast<int8_t>(item->valueint) : -1;
            for (uint16_t v = 0; v < slot.instanceCount; ++v)
            {
                instances[slot.instanceBase + v].midiIn.channel = ch;
            }
        }
        else if (slot.type == NodeType::Knob && strcmp(key, "cc") == 0)
        {
            const int16_t cc = cJSON_IsNumber(item) ? static_cast<int16_t>(item->valueint) : -1;
            for (uint16_t v = 0; v < slot.instanceCount; ++v)
            {
                instances[slot.instanceBase + v].knob->cc = cc;
            }
        }
        else if (slot.type == NodeType::Knob && strcmp(key, "channel") == 0)
        {
            const int8_t ch = cJSON_IsNumber(item) ? static_cast<int8_t>(item->valueint) : -1;
            for (uint16_t v = 0; v < slot.instanceCount; ++v)
            {
                instances[slot.instanceBase + v].knob->channel = ch;
            }
        }
        else if (slot.type == NodeType::Knob && strcmp(key, "constraintMin") == 0)
        {
            const bool has = cJSON_IsNumber(item);
            const float val = has ? static_cast<float>(item->valuedouble) : 0.0f;
            for (uint16_t v = 0; v < slot.instanceCount; ++v)
            {
                instances[slot.instanceBase + v].knob->hasMin = has;
                instances[slot.instanceBase + v].knob->minVal = val;
                instances[slot.instanceBase + v].knob->currentValue =
                    instances[slot.instanceBase + v].knob->mappedValue();
            }
        }
        else if (slot.type == NodeType::Knob && strcmp(key, "constraintMax") == 0)
        {
            const bool has = cJSON_IsNumber(item);
            const float val = has ? static_cast<float>(item->valuedouble) : 0.0f;
            for (uint16_t v = 0; v < slot.instanceCount; ++v)
            {
                instances[slot.instanceBase + v].knob->hasMax = has;
                instances[slot.instanceBase + v].knob->maxVal = val;
                instances[slot.instanceBase + v].knob->currentValue =
                    instances[slot.instanceBase + v].knob->mappedValue();
            }
        }
        else if (slot.type == NodeType::Knob && strcmp(key, "valueList") == 0)
        {
            for (uint16_t v = 0; v < slot.instanceCount; ++v)
            {
                KnobState &k = *instances[slot.instanceBase + v].knob;
                k.valueListCount = applyFloatArrayItem(item, k.valueList, static_cast<uint8_t>(synth::kValueListMax));
                k.currentValue = k.mappedValue();
            }
        }
        else if (slot.type == NodeType::ValueMapper && strcmp(key, "constraintMin") == 0)
        {
            const bool has = cJSON_IsNumber(item);
            const float val = has ? static_cast<float>(item->valuedouble) : 0.0f;
            for (uint16_t v = 0; v < slot.instanceCount; ++v)
            {
                instances[slot.instanceBase + v].valueMapper->hasMin = has;
                instances[slot.instanceBase + v].valueMapper->minVal = val;
            }
        }
        else if (slot.type == NodeType::ValueMapper && strcmp(key, "constraintMax") == 0)
        {
            const bool has = cJSON_IsNumber(item);
            const float val = has ? static_cast<float>(item->valuedouble) : 0.0f;
            for (uint16_t v = 0; v < slot.instanceCount; ++v)
            {
                instances[slot.instanceBase + v].valueMapper->hasMax = has;
                instances[slot.instanceBase + v].valueMapper->maxVal = val;
            }
        }
        else if (slot.type == NodeType::ValueMapper && strcmp(key, "valueList") == 0)
        {
            for (uint16_t v = 0; v < slot.instanceCount; ++v)
            {
                ValueMapperState &vm = *instances[slot.instanceBase + v].valueMapper;
                vm.valueListCount = applyFloatArrayItem(item, vm.valueList, static_cast<uint8_t>(synth::kValueListMax));
            }
        }
        else if (slot.type == NodeType::Button && strcmp(key, "cc") == 0)
        {
            const int16_t cc = cJSON_IsNumber(item) ? static_cast<int16_t>(item->valueint) : -1;
            for (uint16_t v = 0; v < slot.instanceCount; ++v)
            {
                instances[slot.instanceBase + v].button->cc = cc;
            }
        }
        else if (slot.type == NodeType::Button && strcmp(key, "channel") == 0)
        {
            const int8_t ch = cJSON_IsNumber(item) ? static_cast<int8_t>(item->valueint) : -1;
            for (uint16_t v = 0; v < slot.instanceCount; ++v)
            {
                instances[slot.instanceBase + v].button->channel = ch;
            }
        }
        else if (slot.type == NodeType::Button && strcmp(key, "valueList") == 0)
        {
            for (uint16_t v = 0; v < slot.instanceCount; ++v)
            {
                ButtonState &b = *instances[slot.instanceBase + v].button;
                b.valueListCount = applyFloatArrayItem(item, b.valueList, static_cast<uint8_t>(synth::kValueListMax));
                b.listIndex = 0;
                b.currentValue = b.currentOutput();
            }
        }
        else if (slot.type == NodeType::UpDownButton && strcmp(key, "ccUp") == 0)
        {
            const int16_t cc = cJSON_IsNumber(item) ? static_cast<int16_t>(item->valueint) : -1;
            for (uint16_t v = 0; v < slot.instanceCount; ++v)
            {
                instances[slot.instanceBase + v].upDownButton->ccUp = cc;
            }
        }
        else if (slot.type == NodeType::UpDownButton && strcmp(key, "channelUp") == 0)
        {
            const int8_t ch = cJSON_IsNumber(item) ? static_cast<int8_t>(item->valueint) : -1;
            for (uint16_t v = 0; v < slot.instanceCount; ++v)
            {
                instances[slot.instanceBase + v].upDownButton->channelUp = ch;
            }
        }
        else if (slot.type == NodeType::UpDownButton && strcmp(key, "ccDown") == 0)
        {
            const int16_t cc = cJSON_IsNumber(item) ? static_cast<int16_t>(item->valueint) : -1;
            for (uint16_t v = 0; v < slot.instanceCount; ++v)
            {
                instances[slot.instanceBase + v].upDownButton->ccDown = cc;
            }
        }
        else if (slot.type == NodeType::UpDownButton && strcmp(key, "channelDown") == 0)
        {
            const int8_t ch = cJSON_IsNumber(item) ? static_cast<int8_t>(item->valueint) : -1;
            for (uint16_t v = 0; v < slot.instanceCount; ++v)
            {
                instances[slot.instanceBase + v].upDownButton->channelDown = ch;
            }
        }
        else if (slot.type == NodeType::UpDownButton && strcmp(key, "valueList") == 0)
        {
            for (uint16_t v = 0; v < slot.instanceCount; ++v)
            {
                UpDownButtonState &ud = *instances[slot.instanceBase + v].upDownButton;
                ud.valueListCount = applyFloatArrayItem(item, ud.valueList, static_cast<uint8_t>(synth::kValueListMax));
                if (ud.listIndex >= ud.valueListCount && ud.valueListCount > 0)
                {
                    ud.listIndex = static_cast<uint8_t>(ud.valueListCount - 1);
                }
            }
        }
        else if (slot.type == NodeType::Switch && strcmp(key, "cc") == 0)
        {
            const int16_t cc = cJSON_IsNumber(item) ? static_cast<int16_t>(item->valueint) : -1;
            for (uint16_t v = 0; v < slot.instanceCount; ++v)
            {
                instances[slot.instanceBase + v].switchState.cc = cc;
            }
        }
        else if (slot.type == NodeType::Switch && strcmp(key, "channel") == 0)
        {
            const int8_t ch = cJSON_IsNumber(item) ? static_cast<int8_t>(item->valueint) : -1;
            for (uint16_t v = 0; v < slot.instanceCount; ++v)
            {
                instances[slot.instanceBase + v].switchState.channel = ch;
            }
        }
        else if (slot.type == NodeType::Switch && strcmp(key, "selectIn2") == 0 && cJSON_IsBool(item))
        {
            const bool sel = cJSON_IsTrue(item);
            for (uint16_t v = 0; v < slot.instanceCount; ++v)
            {
                instances[slot.instanceBase + v].switchState.selectIn2 = sel;
            }
        }
        else if (slot.type == NodeType::Switch && strcmp(key, "in1Default") == 0 && cJSON_IsNumber(item))
        {
            const float value = static_cast<float>(item->valuedouble);
            for (uint16_t v = 0; v < slot.instanceCount; ++v)
            {
                instances[slot.instanceBase + v].switchState.in1Default = value;
            }
        }
        else if (slot.type == NodeType::Switch && strcmp(key, "in2Default") == 0 && cJSON_IsNumber(item))
        {
            const float value = static_cast<float>(item->valuedouble);
            for (uint16_t v = 0; v < slot.instanceCount; ++v)
            {
                instances[slot.instanceBase + v].switchState.in2Default = value;
            }
        }
        else if (slot.type == NodeType::Lfo && strcmp(key, "zeroPointFixed") == 0 && cJSON_IsBool(item))
        {
            const bool fixed = cJSON_IsTrue(item);
            for (uint16_t v = 0; v < slot.instanceCount; ++v)
            {
                instances[slot.instanceBase + v].lfo.zeroPointFixed = fixed;
            }
        }
        else if (slot.type == NodeType::Lfo && strcmp(key, "oneShot") == 0 && cJSON_IsBool(item))
        {
            const bool oneShot = cJSON_IsTrue(item);
            for (uint16_t v = 0; v < slot.instanceCount; ++v)
            {
                instances[slot.instanceBase + v].lfo.oneShot = oneShot;
            }
        }
        else if (slot.type == NodeType::Lfo && strcmp(key, "global") == 0 && cJSON_IsBool(item))
        {
            const bool global = cJSON_IsTrue(item);
            for (uint16_t v = 0; v < slot.instanceCount; ++v)
            {
                instances[slot.instanceBase + v].lfo.global = global;
            }
        }
        else if (slot.type == NodeType::Gain && strcmp(key, "global") == 0 && cJSON_IsBool(item))
        {
            const bool global = cJSON_IsTrue(item);
            for (uint16_t v = 0; v < slot.instanceCount; ++v)
            {
                instances[slot.instanceBase + v].gain.global = global;
            }
        }
        else if (slot.type == NodeType::Filter && strcmp(key, "global") == 0 && cJSON_IsBool(item))
        {
            const bool global = cJSON_IsTrue(item);
            for (uint16_t v = 0; v < slot.instanceCount; ++v)
            {
                instances[slot.instanceBase + v].filter->global = global;
            }
        }
        else if (slot.type == NodeType::CurtisFilter && strcmp(key, "global") == 0 && cJSON_IsBool(item))
        {
            const bool global = cJSON_IsTrue(item);
            for (uint16_t v = 0; v < slot.instanceCount; ++v)
                instances[slot.instanceBase + v].curtisFilter->global = global;
        }
        else if (slot.type == NodeType::Reverb && strcmp(key, "bypass") == 0 && cJSON_IsBool(item))
        {
            const bool bypass = cJSON_IsTrue(item);
            for (uint16_t v = 0; v < slot.instanceCount; ++v)
            {
                instances[slot.instanceBase + v].reverb->bypass = bypass;
            }
        }
        else if (slot.type == NodeType::ReverbLight && strcmp(key, "bypass") == 0 && cJSON_IsBool(item))
        {
            const bool bypass = cJSON_IsTrue(item);
            for (uint16_t v = 0; v < slot.instanceCount; ++v)
            {
                instances[slot.instanceBase + v].reverbLight->bypass = bypass;
            }
        }
        else if (slot.type == NodeType::Filter && strcmp(key, "bypass") == 0 && cJSON_IsBool(item))
        {
            const bool bypass = cJSON_IsTrue(item);
            for (uint16_t v = 0; v < slot.instanceCount; ++v)
            {
                instances[slot.instanceBase + v].filter->bypass = bypass;
            }
        }
        else if (slot.type == NodeType::CurtisFilter && strcmp(key, "bypass") == 0 && cJSON_IsBool(item))
        {
            const bool bypass = cJSON_IsTrue(item);
            for (uint16_t v = 0; v < slot.instanceCount; ++v)
            {
                instances[slot.instanceBase + v].curtisFilter->bypass = bypass;
            }
        }
        else if (slot.type == NodeType::Delay && strcmp(key, "bypass") == 0 && cJSON_IsBool(item))
        {
            const bool bypass = cJSON_IsTrue(item);
            for (uint16_t v = 0; v < slot.instanceCount; ++v)
            {
                instances[slot.instanceBase + v].delay->bypass = bypass;
            }
        }
        else if (slot.type == NodeType::Chorus && strcmp(key, "bypass") == 0 && cJSON_IsBool(item))
        {
            const bool bypass = cJSON_IsTrue(item);
            for (uint16_t v = 0; v < slot.instanceCount; ++v)
            {
                instances[slot.instanceBase + v].chorus->bypass = bypass;
            }
        }
        else if (slot.type == NodeType::EspEq && strcmp(key, "bypass") == 0 && cJSON_IsBool(item))
        {
            const bool bypass = cJSON_IsTrue(item);
            for (uint16_t v = 0; v < slot.instanceCount; ++v)
            {
                instances[slot.instanceBase + v].espEq->bypass = bypass;
            }
        }
        else if ((slot.type == NodeType::EspAlc || slot.type == NodeType::EspChannelConvert ||
                  slot.type == NodeType::EspSonic || slot.type == NodeType::EspDrc ||
                  slot.type == NodeType::EspMbc) &&
                 strcmp(key, "bypass") == 0 && cJSON_IsBool(item))
        {
            // EspMixer deliberately excluded - see GraphBuilder.h's build-time
            // init comment.
            const bool bypass = cJSON_IsTrue(item);
            for (uint16_t v = 0; v < slot.instanceCount; ++v)
            {
                instances[slot.instanceBase + v].espEffect->bypass = bypass;
            }
        }
        else if ((slot.type == NodeType::AudioOut || slot.type == NodeType::UsbOut ||
                  slot.type == NodeType::Es8311Out || slot.type == NodeType::Wm8960Out) &&
                 strcmp(key, "muted") == 0 && cJSON_IsBool(item))
        {
            const bool muted = cJSON_IsTrue(item);
            for (uint16_t v = 0; v < slot.instanceCount; ++v)
            {
                instances[slot.instanceBase + v].audioOut.muted = muted;
            }
        }
    }

    // A Knob/Button delivers its value to connected targets only via a
    // ControlRoute, never via a real edge (see Graph.h). A physical MIDI-CC
    // message already reaches targets via handleMidiEvent's CC branch; a live
    // update from the browser (e.g. knob drag) previously only updated this
    // one widget, never forwarded the route (Knob -> Divide.In1 stayed silent
    // during the drag).
    if (slot.type == NodeType::Knob)
    {
        pushControlRouteValue(*this, static_cast<uint16_t>(widgetIdx), ControlRoute::Kind::Knob,
                               instances[slot.instanceBase].knob->currentValue);
        pushControlRouteValue(*this, static_cast<uint16_t>(widgetIdx), ControlRoute::Kind::KnobCc,
                               instances[slot.instanceBase].knob->raw);
    }
    else if (slot.type == NodeType::Button)
    {
        pushControlRouteValue(*this, static_cast<uint16_t>(widgetIdx), ControlRoute::Kind::Button,
                               instances[slot.instanceBase].button->currentValue);
    }
    else if (slot.type == NodeType::UpDownButton)
    {
        pushControlRouteValue(*this, static_cast<uint16_t>(widgetIdx), ControlRoute::Kind::UpDownButton,
                               instances[slot.instanceBase].upDownButton->currentOutput());
    }
    else if (slot.type == NodeType::Const)
    {
        pushControlRouteValue(*this, static_cast<uint16_t>(widgetIdx), ControlRoute::Kind::Const,
                               instances[slot.instanceBase].constant.value);
    }
}

inline Graph *Graph::allocate()
{
    void *storage = heap_caps_malloc(sizeof(Graph), MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (storage == nullptr)
    {
        storage = heap_caps_malloc(sizeof(Graph), MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT);
    }
    if (storage == nullptr)
    {
        // Both pools out of ~27KB - every caller (GraphBuilder::build(),
        // SynthEngine's startup/reload swap) assumes a valid Graph* the same
        // way plain `new Graph()` did before this existed, and a device this
        // starved of memory (internal AND all of PSRAM) cannot continue
        // rendering anyway.
        ESP_LOGE(kGraphTag, "Graph allocation failed (PSRAM and internal exhausted)");
        abort();
    }
    return new (storage) Graph();
}

inline void Graph::release(Graph *graph)
{
    if (graph != nullptr)
    {
        graph->~Graph();
        heap_caps_free(graph);
    }
}

inline void Graph::destroy()
{
    for (uint16_t i = 0; i < instanceCount; ++i)
    {
        NodeInstance &inst = instances[i];
        for (int p = 0; p < static_cast<int>(synth::kMaxAudioInPorts); ++p)
        {
            float *&port = inst.inPorts[p];
            if (port != nullptr)
            {
                // A view port (see NodeInstance.h's viewPortMask) doesn't
                // own this memory - it's another instance's own outL/outR/
                // link-bus buffer, freed when THAT instance's own slot is
                // processed by this same loop. Only free an actually-owned
                // buffer here. Allocated via heap_caps_calloc (see
                // GraphBuilder.h's allocateBlockBuffer, used uniformly for
                // every type now, internal RAM or PSRAM depending on node
                // type) - matching heap_caps_free, not delete[].
                if ((inst.viewPortMask & synth::portBit(p)) == 0)
                {
                    heap_caps_free(port);
                }
                port = nullptr;
            }
        }
        switch (inst.type)
        {
        case NodeType::EspEq:
            if (inst.espEq != nullptr)
            {
                destroyEspEq(*inst.espEq);
                heap_caps_free(inst.espEq);
                inst.espEq = nullptr;
            }
            break;
        case NodeType::EspAlc:
        case NodeType::EspChannelConvert:
        case NodeType::EspMixer:
        case NodeType::EspSonic:
        case NodeType::EspDrc:
        case NodeType::EspMbc:
            // Null here means either a never-completed build (see
            // GraphBuilder.h's build's state-alloc failure path, which calls
            // this mid-build before every instance's pointer is set) or
            // simply an instance slot this loop hasn't reached construction
            // for yet - never a fully-built instance missing its state.
            if (inst.espEffect != nullptr)
            {
                destroyEspEffect(*inst.espEffect);
                heap_caps_free(inst.espEffect);
                inst.espEffect = nullptr;
            }
            break;
        case NodeType::Chorus:
            nodes::chorus::destroy(inst);
            break;
        case NodeType::Delay:
            nodes::delay::destroy(inst);
            break;
        case NodeType::Reverb:
            nodes::reverb::destroyReverb(inst);
            break;
        case NodeType::ReverbLight:
            nodes::reverb::destroyReverbLight(inst);
            break;
        case NodeType::Hammond:
            nodes::hammond::destroy(inst);
            break;
        case NodeType::SoundFontPlayer:
            soundfont::close(inst.soundFontPlayer.synth);
            inst.soundFontPlayer.synth = nullptr;
            break;
        case NodeType::Adsr:
            nodes::adsr::destroy(inst);
            break;
        case NodeType::Filter:
            nodes::filter::destroy(inst);
            break;
        case NodeType::CurtisFilter:
            nodes::curtisfilter::destroy(inst);
            break;
        case NodeType::Knob:
            nodes::knob::destroy(inst);
            break;
        case NodeType::Button:
            nodes::button::destroyButton(inst);
            break;
        case NodeType::UpDownButton:
            nodes::button::destroyUpDownButton(inst);
            break;
        case NodeType::ValueMapper:
            nodes::valuemapper::destroy(inst);
            break;
        default:
            break;
        }
        heap_caps_free(inst.outL);
        heap_caps_free(inst.outR);
        if (inst.link != nullptr)
        {
            for (float *&bus : inst.link->busOut)
            {
                heap_caps_free(bus);
                bus = nullptr;
            }
            heap_caps_free(inst.link);
            inst.link = nullptr;
        }
        inst.outL = nullptr;
        inst.outR = nullptr;
    }
    // Allocated via heap_caps_malloc + per-element placement-new (see
    // GraphBuilder.h's build) - matching raw free, not delete[]. No explicit
    // per-element destructor calls needed: NodeInstance's destructor is
    // trivial (no member anywhere in its chain declares a custom one).
    heap_caps_free(instances);
    instances = nullptr;
    instanceCount = 0;

    // waveSampleCache[w].data is NOT freed here: ownership lives in
    // WavLoader's own process-wide cache now (see wavloader::load()/
    // sweepCache() in WavLoader.h), so the same decoded buffer can survive
    // into the next Graph if its widget still references the same file -
    // freeing it per-Graph would defeat that cache (and double-free once the
    // next build's sweepCache() runs). GraphBuilder.h's build() sweeps
    // entries no longer referenced by any WavePlayer widget before loading
    // anything.
}
