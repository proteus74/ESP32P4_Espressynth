#pragma once

#include <cmath>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <new>

#include "cJSON.h"
#include "esp_attr.h"
#include "esp_heap_caps.h"
#include "esp_log.h"
#include "esp_timer.h"

#include "CvConventions.h"
#include "DeviceSettings.h"
#include "EngineStats.h"
#include "EspEqEffect.h"
#include "GraphicEqEffect.h"
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
#include "nodes/DxEnvelopeNode.h"
#include "nodes/FilterNode.h"
#include "nodes/HammondNode.h"
#include "nodes/KnobNode.h"
#include "nodes/ReverbNode.h"
#include "nodes/StepArpeggiatorNode.h"
#include "nodes/DrumSequencerNode.h"
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
    // any audio/CV hardware sink or an active monitor tap - see that pass's comment
    // (plan section 3, P2: "Nur vom Ausgang oder einem aktiven Monitor
    // benÃ¶tigte Teilgraphen rendern"). Graph::renderBlock skips a widget
    // entirely (render + its own outgoing edges) when this is false; safe
    // by construction, since a widget only ends up false when NONE of its
    // outgoing edges lead anywhere needed either.
    bool needed = true;
    uint16_t instanceBase = 0;
    uint16_t instanceCount = 0; // 1 or the preset's configured voice count
    uint16_t outgoingEdgeStart = 0;
    uint16_t outgoingEdgeCount = 0;
    // controlRoutes[] whose sourceWidget is this widget - GraphBuilder sorts
    // the routes by source (stable, so per-source order is unchanged) and
    // fills these, same scheme as outgoingEdgeStart/Count above. Every
    // per-event delivery walks only this range instead of all routes.
    uint16_t routeStart = 0;
    uint16_t routeCount = 0;
};

// Optional out-param for Graph::applyLiveParams: reports the Knob control
// a live (browser) update touched, when it's MIDI-learned - lets the caller
// (SynthEngine's applyLiveParamMessage) mirror it to the on-device activity
// display too, the same overlay a real incoming CC message already lights
// up (see SynthEngine::publishMidiControlActivity). Left default-constructed
// (changed == false) for every other kind of update.
struct KnobLiveChangeInfo
{
    bool changed = false;
    const char *label = nullptr;
    uint8_t channel = 0;
    uint8_t cc = 0;
    uint8_t raw = 0;
    float value = 0.0f;
};

constexpr size_t kMaxMidiCcDebugTargets = 16;

struct MidiCcDebugTarget
{
    char targetId[24] = {};
    char targetLabel[48] = {};
    char paramName[16] = {};
    NodeType targetType = NodeType::Unknown;
    float value = 0.0f;
    bool raw = false;
};

struct MidiCcDebugSnapshot
{
    char sourceId[24] = {};
    char knobName[48] = {};
    bool isKnob = false;
    bool isButton = false; // knobName then holds the button's name
    uint8_t controlIndex = 0;
    char sourceLabel[96] = {};
    bool fromUi = false;
    bool continuation = false;
    float sourceValue = 0.0f;
    uint8_t channel = 0;
    uint8_t cc = 0;
    uint8_t rawValue = 0;
    uint8_t targetCount = 0;
    MidiCcDebugTarget targets[kMaxMidiCcDebugTargets];
};

struct ControlDebugEvent
{
    uint16_t widget = 0;
    uint8_t side = 0;
    uint8_t channel = 0;
    uint8_t cc = 0;
    uint8_t raw = 0;
    bool fromUi = false;
};

struct Edge
{
    uint16_t fromInstance = 0;
    uint8_t fromPort = 0;
    uint16_t toInstance = 0;
    uint8_t toPort = 0;
    float scale = 1.0f; // 1/sqrt(N) for voiced->global fan-in, else 1.0
    bool monitorTap = false;
    char paramName[16] = {};
};

// A non-audio-rate connection resolved at build time into a direct
// (source widget) -> (target widget's control field) route: either one of
// midi-in's frequency/gate/velocity "virtual" outputs, a Const scalar, or a
// knob's/button's mapped CC value targeting an arbitrary named param on the
// target. Scalar routes, including MIDI aftertouch and pitch bend, name their
// target field; note/gate/velocity delivery also has specialized MIDI handling.
struct ControlRoute
{
    enum class Kind : uint8_t
    {
        MidiNote,
        MidiGate,
        MidiVelocity,
        MidiRaw,
        MidiPitchCv, // MidiIn's "pitchCv" output: 1V/oct CV, see CvConventions.h
        MidiAftertouch, // MidiIn's "aftertouch" output: channel pressure (0xD), linear 0-10V
        MidiPitchBend,  // MidiIn's "pitchBend" output: pitch-bend wheel (0xE), 0V-centered, per-widget range
        Knob,        // Knob's "voltage" output (0-10V, mapped)
        KnobCc,      // Knob's "cc" output (raw 0-127, unmapped)
        Const,       // Const's scalar "value" output
        Button,
        UpDownButton,
        MasterVolume,
        MidiProgram, // MidiIn's "program" output: last Program Change (0xC), raw 0-127
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

    // Index of a virtual source output. MidiSplit uses it for the originating
    // zone; multi-control sources such as Knob use it for the control index.
    // The default of zero preserves old, single-output preset connections.
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
    // hardware sinks (USB / ES8311 / WM8960 / NS4168) has its own pair, driven
    // by the corresponding output widget (UsbOut/Es8311Out/Wm8960Out/Ns4168Out) in the
    // graph. A sink with no matching widget stays silent (zeros).
    struct Outputs
    {
        float *usbL = nullptr;
        float *usbR = nullptr;
        float *esL = nullptr;
        float *esR = nullptr;
        float *wmL = nullptr;
        float *wmR = nullptr;
        float *nsL = nullptr;
        float *nsR = nullptr;
    };

    void renderBlock(Outputs &out, size_t frames);
    void handleMidiEvent(const UsbMidiEvent &event);
    void recordControlDebug(uint16_t widget, uint8_t side, bool fromUi, uint8_t channel = 0,
                            uint8_t cc = 0, uint8_t raw = 0);
    // Emits at most `maxEvents` pending entries (oldest first); the rest stay
    // queued and keep coalescing until a later block has time for them.
    void flushControlDebug(MidiCcDebugSnapshot &scratch,
                          void (*emit)(const MidiCcDebugSnapshot &, void *), void *context,
                          uint8_t maxEvents);
    uint16_t activeSoundFontVoiceCount() const;
    void midiVoiceUsage(uint16_t &active, uint16_t &maximum) const;

    // Applies a live parameter patch (from a browser WS "paramUpdate"
    // message) to every instance of the widget `widgetId`, without touching topology or
    // reallocating any buffers - just field writes, matching what a full
    // rebuild would set for these same keys. Unknown widgetId or unknown
    // keys within `paramsObj` are silently ignored (logged at debug level).
    // midiCcOut (optional, see SynthEngine::MidiCcOut): fires once if this
    // update touched a MIDI-learned Knob control, mirroring the new value
    // back out to the hardware that owns that CC (so its LED ring/motorized
    // fader follows a change made from the browser instead of that knob).
    // outActivity (optional): filled in under the same condition, for the
    // caller to also light up the on-device activity display (see
    // KnobLiveChangeInfo).
    void applyLiveParams(const char *widgetId, cJSON *paramsObj,
                         void (*midiCcOut)(uint8_t channel, uint8_t cc, uint8_t value7bit) = nullptr,
                         KnobLiveChangeInfo *outActivity = nullptr);

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
    float presetBpm = -1.0f;

    // EMA-smoothed per-widget CPU load. Profiling rotates over one widget per
    // block so timer reads do not scale with graph size. Generation advances
    // after a complete sweep and lets SynthEngine publish only fresh snapshots.
    // widgetLoadPercent covers only that widget's own render() calls (node
    // DSP); widgetEdgeLoadPercent separately covers summing its outgoing
    // edges into downstream instances - previously entirely unmeasured (see
    // documentation/native-synth-optimization-plan-2026-09-09.md section 7:
    // "Eingangspuffer vorbereiten und ausgehende Verbindungen summieren
    // liegen auÃŸerhalb"). Same rotation, same O(1)-per-block cost - just two
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
    ControlDebugEvent controlDebugEvents[32] = {};
    uint8_t controlDebugEventCount = 0;
    uint32_t controlDebugDropped = 0;

    uint16_t renderOrder[synth::kMaxWidgets];
    uint16_t renderOrderCount = 0;

    // Indices of the (at most one each) output widgets per hardware sink.
    // -1 = no widget for that sink (renders silence).
    int16_t usbOutWidget = -1;
    int16_t es8311OutWidget = -1;
    int16_t wm8960OutWidget = -1;
    int16_t ns4168OutWidget = -1;
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
inline void pushControlRouteValue(Graph &graph, uint16_t sourceWidget, ControlRoute::Kind kind, float value,
                                  uint8_t sourceSide = 0)
{
    const WidgetSlot &source = graph.widgets[sourceWidget];
    const uint16_t routeEnd = source.routeStart + source.routeCount;
    for (uint16_t r = source.routeStart; r < routeEnd; ++r)
    {
        const ControlRoute &route = graph.controlRoutes[r];
        if (route.kind != kind || route.side != sourceSide)
        {
            continue;
        }
        // Param-name checks don't depend on the voice - resolve once per route.
        const bool bendToTune = kind == ControlRoute::Kind::MidiPitchBend && strcmp(route.paramName, "tune") == 0;
        const bool rateParam = kind != ControlRoute::Kind::KnobCc && strcmp(route.paramName, "rate") == 0;
        const WidgetSlot &targetWidget = graph.widgets[route.targetWidget];
        for (uint16_t v = 0; v < targetWidget.instanceCount; ++v)
        {
            NodeInstance &target = graph.instances[targetWidget.instanceBase + v];
            float targetValue = value;
            if (bendToTune &&
                (target.type == NodeType::Oscillator || target.type == NodeType::FmOscillator ||
                 target.type == NodeType::Hammond || target.type == NodeType::Fm8))
            {
                // Bipolar Pitch Bend feeds the existing 5V-centered Tune parameter.
                targetValue += cv::kMaxVolts / 2.0f;
            }
            // LFO Rate is configured in Hz, but its connected CV input is
            // 0-10 V. Map that voltage to the LFO's 0-50 Hz range. A raw
            // Knob CC route intentionally stays raw and is clamped by the
            // LFO parameter setter itself.
            if (rateParam && (target.type == NodeType::Lfo || target.type == NodeType::Wavetable))
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

inline void updateMasterVolumeControlRoutes(Graph &graph)
{
    const float volts = synth::clampDeviceVolume(synth::deviceSettings().volume) * cv::kMaxVolts;
    for (uint16_t w = 0; w < graph.widgetCount; ++w)
    {
        const WidgetSlot &widget = graph.widgets[w];
        if (widget.type != NodeType::MasterVolume) continue;
        auto &previous = graph.instances[widget.instanceBase].controlValue[0];
        if (previous == volts) continue;
        previous = volts;
        pushControlRouteValue(graph, w, ControlRoute::Kind::MasterVolume, volts);
    }
}

inline void copyMidiCcDebugText(char *destination, size_t destinationSize, const char *source)
{
    if (destination == nullptr || destinationSize == 0)
    {
        return;
    }
    size_t length = 0;
    if (source != nullptr)
    {
        while (length + 1 < destinationSize && source[length] != '\0')
        {
            destination[length] = source[length];
            ++length;
        }
    }
    destination[length] = '\0';
}

// Delivers one note-on/off event's control routes from `sourceWidget` to
// every target it feeds. Used both for a real midi-in's own routes (side=-1:
// deliver every route regardless of ControlRoute::side) AND, recursively,
// for a midi-split widget forwarding to its own lower/upper targets (side=0
// or 1: only deliver routes tagged for that side - see the MidiGate case
// below). Also the shared body behind handleMidiEvent's own note on/off
// delivery and releaseVoiceGate's sustain-pedal-up replay, so both paths stay
// byte-identical instead of duplicating this switch.
inline bool samplerPlayingInVoice(const Graph &graph, uint8_t voiceIdx)
{
    for (uint16_t w = 0; w < graph.widgetCount; ++w)
    {
        const WidgetSlot &slot = graph.widgets[w];
        if (slot.type != NodeType::Sampler) continue;
        const uint16_t v = slot.voiced ? voiceIdx : 0;
        if (v < slot.instanceCount && graph.instances[slot.instanceBase + v].sampler.playing) return true;
    }
    return false;
}

inline void deliverNoteControlRoutes(Graph &graph, uint16_t sourceWidget, uint8_t voiceIdx, uint8_t note, bool noteOn,
                               uint8_t velocity, int side)
{
    // Note-on for a voice whose Sampler still plays (stolen at the voice limit, or a long
    // release re-struck): its ADSRs restart from silence instead of re-attacking from their
    // current level - otherwise every new note lacks its attack and the level creeps up note
    // after note. The Sampler crossfades its old position out meanwhile (SamplerNode.h).
    const bool restartEnvelopes = noteOn && samplerPlayingInVoice(graph, voiceIdx);
    const WidgetSlot &source = graph.widgets[sourceWidget];
    const uint16_t routeEnd = source.routeStart + source.routeCount;
    for (uint16_t r = source.routeStart; r < routeEnd; ++r)
    {
        const ControlRoute &route = graph.controlRoutes[r];
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
                    target.osc.feedbackSample = 0.0f;
                }
                target.osc.gatePrev = noteOn;
            }
            else if (target.type == NodeType::FmOscillator)
            {
                if (noteOn && target.fmOsc.wave != WaveType::Noise && target.fmOsc.wave != WaveType::PinkNoise)
                {
                    target.fmOsc.phaseGen.resetPhase();
                    target.fmOsc.feedbackSample = 0.0f;
                }
                target.fmOsc.gatePrev = noteOn;
            }
            else if (target.type == NodeType::Fm8)
            {
                if (target.fm8 != nullptr)
                {
                    if (noteOn)
                    {
                        target.fm8->pendingOn = true;
                        target.fm8->pendingNote = note;
                        target.fm8->idle = false;
                    }
                    else
                    {
                        nodes::fm8::gateOff(*target.fm8);
                    }
                }
            }
            else if (target.type == NodeType::Adsr)
            {
                // Idle voices may not have rendered since the last Knob update.
                // Configure before scheduling this note's attack/release.
                nodes::adsr::updateParameters(target);
                if (restartEnvelopes)
                    target.adsr->ramp.restart();
                else
                    target.adsr->ramp.gate(noteOn);
                target.adsr->gatePrev = noteOn;
            }
            else if (target.type == NodeType::DxEnvelope)
            {
                nodes::dxenvelope::updateParameters(target);
                noteOn ? target.dxEnvelope->scheduler.gateOn() : target.dxEnvelope->scheduler.gateOff();
                target.dxEnvelope->gatePrev = noteOn;
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
            else if (target.type == NodeType::Sampler)
            {
                // Event-driven retrigger, same reasoning as WavePlayer above.
                if (noteOn) nodes::sampler::trigger(target.sampler);
                target.sampler.gatePrev = noteOn;
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
            else if (target.type == NodeType::MdaPiano)
            {
                target.mdaPiano.currentNote = note;
                if (noteOn) target.mdaPiano.pendingNoteOn = true;
                else nodes::mdapiano::noteOff(target.mdaPiano, note);
                target.mdaPiano.gatePrev = noteOn;
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
            else if (target.type == NodeType::FmOscillator)
            {
                target.fmOsc.amplitude = velGain;
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
            else if (target.type == NodeType::DxEnvelope)
            {
                target.dxEnvelope->velocity = velGain;
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
                const auto &source = graph.widgets[sourceWidget];
                const auto &sourceInst = graph.instances[source.instanceBase];
                const MidiInState defaults;
                const MidiInState &midi = source.type == NodeType::MidiIn ? sourceInst.midiIn :
                    source.type == NodeType::MidiFilter ? sourceInst.midiFilter->midi : defaults;
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
        case ControlRoute::Kind::MidiAftertouch:
        case ControlRoute::Kind::MidiPitchBend:
            // Delivered directly from handleMidiEvent's 0xD/0xE branches via
            // pushControlRouteValue() - event-driven, independent of note
            // timing, updates every active voice instance of the target at
            // once. Never reached here.
            break;
        case ControlRoute::Kind::Knob:
        case ControlRoute::Kind::KnobCc:
        case ControlRoute::Kind::Button:
        case ControlRoute::Kind::UpDownButton:
        case ControlRoute::Kind::MasterVolume:
        case ControlRoute::Kind::MidiProgram:
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
    updateMasterVolumeControlRoutes(*this);
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
    // envelope of that voice has not truly reached 0 yet. An unconnected
    // envelope must not silence the rest of the signal path. If there is no
    // connected envelope, all voices stay active.
    for (bool &a : voiceStillSounding)
    {
        a = false;
    }
    bool hasEnvelope = false;
    for (uint16_t w = 0; w < widgetCount; ++w)
    {
        if ((widgets[w].type != NodeType::Adsr && widgets[w].type != NodeType::DxEnvelope &&
             widgets[w].type != NodeType::Fm8 && widgets[w].type != NodeType::Sampler) ||
            widgets[w].outgoingEdgeCount == 0)
        {
            continue;
        }
        hasEnvelope = true;
        // "gate" (Adsr port 4 / DxEnvelope port 8 - see PortTables.h) can
        // only detect its own rising/falling edges by actually running
        // render() every block (see AdsrNode.h's/DxEnvelopeNode.h's gateFed
        // check) - unlike the MIDI-ControlRoute gate path
        // (Graph::deliverNoteControlRoutes), which wakes an idle envelope
        // directly/out-of-band, no render() needed. Without this, a
        // genuinely idle envelope whose ONLY gate source is a wired cable
        // would never be rendered again once idle - voiceStillSounding[v]
        // would never observe the very edge that should wake it, a
        // permanent deadlock. Force the voice live whenever that port is
        // actually wired, regardless of the envelope's current idle state.
        const NodeType type = widgets[w].type;
        const int gatePort = type == NodeType::Adsr ? 4
                           : type == NodeType::Fm8 ? 1
                           : type == NodeType::Sampler ? nodes::sampler::kGatePort
                                                       : 8;
        for (uint16_t v = 0; v < widgets[w].instanceCount; ++v)
        {
            NodeInstance &envelope = instances[widgets[w].instanceBase + v];
            const bool idle = type == NodeType::Adsr    ? envelope.adsr->ramp.isIdle()
                            : type == NodeType::Fm8     ? (envelope.fm8->idle && !envelope.fm8->pendingOn)
                            : type == NodeType::Sampler ? !envelope.sampler.playing
                                                        : envelope.dxEnvelope->scheduler.isIdle();
            const bool gateWired = (envelope.audioConnectedPortMask & synth::portBit(gatePort)) != 0;
            if (!idle || gateWired)
            {
                voiceStillSounding[v] = true;
            }
        }
    }
    if (!hasEnvelope)
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
        inst.constantPortMask = 0;
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
        if ((w.type == NodeType::Lfo || w.type == NodeType::Wavetable) && w.outgoingEdgeCount == 0)
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
        // A widget-global Lfo (see LfoState::global / nodes::lfo::globalUpdate)
        // is shared by the whole chord: voice 0 is only the technical master
        // every other voice mirrors, so it must render whenever ANY voice
        // sounds (not only while its own note is active), and it is
        // (re)started by the first note - later notes just join.
        bool forceLfoPrimaryActive = false;
        if ((w.type == NodeType::Lfo || w.type == NodeType::Wavetable) && w.voiced && w.instanceCount > 1)
        {
            bool anySounding = false;
            for (uint16_t v = 0; v < w.instanceCount && !anySounding; ++v)
            {
                anySounding = voiceStillSounding[v];
            }
            forceLfoPrimaryActive = nodes::lfo::globalUpdate(&instances[w.instanceBase], w.instanceCount, anySounding);
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
                // Same tag-copy, for "src's output is one repeated value
                // this block" instead of "src's output is a ramp" - see
                // NodeInstance.h's constantPortMask comment (fm-plan.md
                // section 3, P3). Same safety argument as the ramp copy
                // above: view-eligibility already proved this is the sole
                // edge into this port, so no second edge can invalidate
                // this tag later in this same loop.
                else if (src.outIsConstant)
                {
                    const uint8_t slot = static_cast<uint8_t>(__builtin_popcount(dst.constantPortMask));
                    if (slot < NodeInstance::kMaxConstantPorts)
                    {
                        dst.constDesc[slot] = {edge.toPort, src.outConstantValue};
                        dst.constantPortMask |= synth::portBit(edge.toPort);
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
    fillOutput(ns4168OutWidget, out.nsL, out.nsR);
    {
        const float outputPercent =
            100.0f * static_cast<float>(esp_timer_get_time() - phaseOutputStartUs) / kGraphBlockBudgetUs;
        phaseOutputPercent += kGraphWidgetLoadEmaAlpha * (outputPercent - phaseOutputPercent);
    }
}

inline void deliverRawMidi(Graph &graph, uint16_t source, const UsbMidiEvent &event,
                           bool *filtered, UsbMidiEvent *filterEvents, bool *path)
{
    if (path[source]) return;
    path[source] = true;
    const bool isNote = event.codeIndex == 8 || event.codeIndex == 9 || event.codeIndex == 10;
    const WidgetSlot &sourceSlot = graph.widgets[source];
    const uint16_t routeEnd = sourceSlot.routeStart + sourceSlot.routeCount;
    for (uint16_t r = sourceSlot.routeStart; r < routeEnd; ++r)
    {
        const auto &route = graph.controlRoutes[r];
        const auto &target = graph.widgets[route.targetWidget];
        if (route.kind != ControlRoute::Kind::MidiRaw &&
            !(route.kind == ControlRoute::Kind::MidiGate && target.type == NodeType::MidiSplit)) continue;
        UsbMidiEvent forwarded = event;
        if (graph.widgets[source].type == NodeType::MidiSplit)
        {
            const auto &split = graph.instances[graph.widgets[source].instanceBase].midiSplit;
            if (route.side >= split.zoneCount) continue;
            const auto &zone = split.zones[route.side];
            if (isNote)
            {
                if (event.data1 < zone.fromNote || event.data1 > zone.toNote) continue;
                int note = event.data1 + zone.octaveTranspose * 12;
                forwarded.data1 = static_cast<uint8_t>(note < 0 ? 0 : note > 127 ? 127 : note);
            }
        }
        auto &inst = graph.instances[target.instanceBase];
        if (target.type == NodeType::MidiFilter)
        {
            if (filtered[route.targetWidget] || path[route.targetWidget]) continue;
            if (!inst.midiFilter->accepts(forwarded.status, forwarded.data1, forwarded.data2)) continue;
            filtered[route.targetWidget] = true;
            filterEvents[route.targetWidget] = forwarded;
            deliverRawMidi(graph, route.targetWidget, forwarded, filtered, filterEvents, path);
        }
        else if (target.type == NodeType::MidiSplit)
            deliverRawMidi(graph, route.targetWidget, forwarded, filtered, filterEvents, path);
        else if (target.type == NodeType::SoundFontPlayer)
            soundfont::handleMidi(inst.soundFontPlayer, forwarded);
        else if (target.type == NodeType::MdaPiano)
            nodes::mdapiano::handleMidi(inst.mdaPiano, forwarded);
    }
    path[source] = false;
}

inline void Graph::recordControlDebug(uint16_t widget, uint8_t side, bool fromUi,
                                     uint8_t channel, uint8_t cc, uint8_t raw)
{
    // Coalesce successive positions of the same control until it is flushed
    // (possibly several blocks later - see flushControlDebug's maxEvents).
    for (uint8_t i = 0; i < controlDebugEventCount; ++i)
    {
        auto &event = controlDebugEvents[i];
        if (event.widget == widget && event.side == side && event.fromUi == fromUi)
        {
            event = {widget, side, channel, cc, raw, fromUi};
            return;
        }
    }
    if (controlDebugEventCount < 32)
        controlDebugEvents[controlDebugEventCount++] = {widget, side, channel, cc, raw, fromUi};
    else
        ++controlDebugDropped;
}

inline void Graph::flushControlDebug(MidiCcDebugSnapshot &snapshot,
                                     void (*emit)(const MidiCcDebugSnapshot &, void *), void *context,
                                     uint8_t maxEvents)
{
    const uint8_t flushCount = controlDebugEventCount < maxEvents ? controlDebugEventCount : maxEvents;
    for (uint8_t i = 0; i < flushCount; ++i)
    {
        const auto &event = controlDebugEvents[i];
        const auto &source = widgets[event.widget];
        const auto &inst = instances[source.instanceBase];
        if ((source.type == NodeType::Knob && !inst.knob->controls.contains(event.side)) ||
            (source.type == NodeType::Button && !inst.button->controls.contains(event.side))) continue;
        snapshot = {};
        snapshot.fromUi = event.fromUi;
        snapshot.channel = event.channel;
        snapshot.cc = event.cc;
        snapshot.rawValue = event.raw;
        copyMidiCcDebugText(snapshot.sourceId, sizeof(snapshot.sourceId), source.id);
        snapshot.controlIndex = event.side;
        snapshot.isKnob = source.type == NodeType::Knob;
        const char *label = source.label[0] ? source.label : source.id;
        const char *kind = source.type == NodeType::Knob ? "CC-Slider" : source.type == NodeType::Button ? "CC Button" :
                           source.type == NodeType::Switch ? "Switch" : "Output Switch";
        if (snapshot.isKnob)
        {
            const auto &control = inst.knob->controls[event.side];
            const float raw = std::isfinite(control.raw) ? control.raw : 0.0f;
            snapshot.rawValue = static_cast<uint8_t>(raw < 0.0f ? 0.0f : raw > 127.0f ? 127.0f : raw + 0.5f);
            if (control.label[0])
                copyMidiCcDebugText(snapshot.knobName, sizeof(snapshot.knobName), control.label);
            else
                snprintf(snapshot.knobName, sizeof(snapshot.knobName), "%s %u", label,
                         static_cast<unsigned>(event.side % synth::kKnobControlMax + 1));
        }
        snapshot.isButton = source.type == NodeType::Button;
        if (snapshot.isButton)
        {
            const auto &control = inst.button->controls[event.side];
            if (control.label[0])
                copyMidiCcDebugText(snapshot.knobName, sizeof(snapshot.knobName), control.label);
            else
                snprintf(snapshot.knobName, sizeof(snapshot.knobName), "%s %u", label,
                         static_cast<unsigned>(event.side % synth::kButtonControlMax + 1));
        }
        if (source.type == NodeType::Knob || source.type == NodeType::Button)
        {
            const uint8_t width = source.type == NodeType::Knob ? synth::kKnobControlMax : synth::kButtonControlMax;
            snprintf(snapshot.sourceLabel, sizeof(snapshot.sourceLabel), "%s [%s] %s %u Layer %u", label,
                     source.id, kind, event.side % width + 1, event.side / width + 1);
            snapshot.sourceValue = source.type == NodeType::Knob ? inst.knob->controls[event.side].currentValue :
                                   inst.button->controls[event.side].currentValue;
        }
        else
        {
            const bool selected = source.type == NodeType::Switch ? inst.switchState.selectIn2 : inst.outputSwitch.selectOut2;
            snprintf(snapshot.sourceLabel, sizeof(snapshot.sourceLabel), "%s [%s] %s %s %u", label, source.id, kind,
                     source.type == NodeType::Switch ? "In" : "Out", selected ? 2 : 1);
            const float *buffer = outputPortBuffer(const_cast<NodeInstance &>(inst), selected && source.type == NodeType::OutputSwitch ? 1 : 0);
            snapshot.sourceValue = buffer ? buffer[synth::kBlockSize - 1] : 0.0f;
        }

        bool reported[synth::kMaxWidgets][synth::kMaxAudioInPorts] = {};
        auto append = [&](uint16_t targetWidget, int port, const char *paramName, float value, bool raw)
        {
            if (port >= 0 && reported[targetWidget][port]) return;
            if (port >= 0) reported[targetWidget][port] = true;
            if (snapshot.targetCount == kMaxMidiCcDebugTargets)
            {
                emit(snapshot, context);
                snapshot.targetCount = 0;
                snapshot.continuation = true;
            }
            auto &entry = snapshot.targets[snapshot.targetCount++];
            const auto &target = widgets[targetWidget];
            copyMidiCcDebugText(entry.targetId, sizeof(entry.targetId), target.id);
            copyMidiCcDebugText(entry.targetLabel, sizeof(entry.targetLabel), target.label[0] ? target.label : target.id);
            copyMidiCcDebugText(entry.paramName, sizeof(entry.paramName), paramName);
            entry.targetType = target.type;
            entry.value = value;
            entry.raw = raw;
            // "layer" is a discrete selector: report the resulting 1-based layer, not the input volts.
            if (strcmp(paramName, "layer") == 0 && (target.type == NodeType::Knob || target.type == NodeType::Button))
            {
                const auto &targetInst = instances[target.instanceBase];
                entry.value = 1.0f + (target.type == NodeType::Knob ? targetInst.knob->activeLayer : targetInst.button->activeLayer);
                entry.raw = true;
            }
        };
        const uint16_t routeEnd = source.routeStart + source.routeCount;
        for (uint16_t r = source.routeStart; r < routeEnd; ++r)
        {
            const auto &route = controlRoutes[r];
            if (route.side != event.side) continue;
            if (route.kind != ControlRoute::Kind::Knob && route.kind != ControlRoute::Kind::KnobCc &&
                route.kind != ControlRoute::Kind::Button) continue;
            const auto &target = instances[widgets[route.targetWidget].instanceBase];
            float value = snapshot.sourceValue;
            const bool raw = route.kind == ControlRoute::Kind::KnobCc;
            if (raw) value = inst.knob->controls[route.side].raw;
            if (route.targetPort >= 0)
                value = widgets[route.targetWidget].needed &&
                        (target.audioConnectedPortMask & synth::portBit(route.targetPort)) != 0 ?
                        noderender::portSample(target, route.targetPort, synth::kBlockSize - 1) :
                        target.controlValue[route.targetPort];
            append(route.targetWidget, route.targetPort, route.paramName, value, raw);
        }
        // Edges are grouped by source widget (GraphBuilder's outgoingEdgeStart) - no need
        // to scan every edge of a 16-voice graph on each flush.
        const uint16_t edgeEnd = source.outgoingEdgeStart + source.outgoingEdgeCount;
        for (uint16_t e = source.outgoingEdgeStart; e < edgeEnd; ++e)
        {
            const auto &edge = edges[e];
            // Both outputs of Output Switch change, including the now-zero branch.
            const auto &targetInst = instances[edge.toInstance];
            const uint16_t targetWidget = targetInst.widgetIndex;
            const auto &target = widgets[targetWidget];
            const uint16_t voice = target.voiced && monitorVoiceIndex < target.instanceCount ? monitorVoiceIndex : 0;
            if (edge.toInstance != target.instanceBase + voice) continue;
            append(targetWidget, edge.toPort, edge.paramName,
                   noderender::portSample(targetInst, edge.toPort, synth::kBlockSize - 1), false);
        }
        emit(snapshot, context);
    }
    controlDebugEventCount = static_cast<uint8_t>(controlDebugEventCount - flushCount);
    memmove(controlDebugEvents, controlDebugEvents + flushCount, controlDebugEventCount * sizeof(controlDebugEvents[0]));
}

inline void Graph::handleMidiEvent(const UsbMidiEvent &event)
{
    bool filtered[synth::kMaxWidgets] = {};
    UsbMidiEvent filterEvents[synth::kMaxWidgets] = {};
    bool path[synth::kMaxWidgets] = {};
    for (uint16_t w = 0; w < widgetCount; ++w)
    {
        if (widgets[w].type != NodeType::MidiIn) continue;
        const auto &m = instances[widgets[w].instanceBase].midiIn;
        if (event.status < 0xF0 && m.channel >= 0 && m.channel != (event.status & 15)) continue;
        // Key filter: note-ons on keys this Midi-In ignores are dropped (note-offs always pass).
        if (event.codeIndex == 0x9 && event.data2 > 0 && !m.noteAllowed(event.data1)) continue;
        deliverRawMidi(*this, w, event, filtered, filterEvents, path);
    }

    // Button MIDI learn: CC, Note On/Off or Program Change (ButtonControl::midiType;
    // its `cc` field holds the CC/note/program number). CC >= 64 and Note On
    // (velocity > 0) press, CC < 64 and Note Off release. Program Change has no
    // release: each matching program toggles the button or advances its list.
    // Other programs do not affect the binding.
    uint8_t buttonMidiType = 0xFF;
    switch (event.codeIndex)
    {
    case 0xB: buttonMidiType = nodes::button::kButtonMidiCc; break;
    case 0x8:
    case 0x9: buttonMidiType = nodes::button::kButtonMidiNote; break;
    case 0xC: buttonMidiType = nodes::button::kButtonMidiProgram; break;
    default: break;
    }
    if (buttonMidiType != 0xFF && event.data1 <= 127)
    {
        const uint8_t channel0 = event.status & 0x0F;
        const uint8_t number = event.data1;
        for (uint16_t w = 0; w < widgetCount; ++w)
        {
            if (widgets[w].type != NodeType::Button) continue;
            ButtonState &b = *instances[widgets[w].instanceBase].button;
            for (uint8_t buttonIndex = 0; buttonIndex < b.count; ++buttonIndex)
            {
                const ButtonControl &binding = b.controls[buttonIndex];
                if (binding.midiType != buttonMidiType || binding.cc < 0 ||
                    binding.channel < 0 || static_cast<uint8_t>(binding.channel) != channel0)
                    continue;
                const bool matched = static_cast<uint8_t>(binding.cc) == number;
                if (!matched) continue;
                const uint8_t controlIndex = ButtonState::controlIndex(buttonIndex, b.activeLayer);
                ButtonControl &control = b.controls[controlIndex];
                bool above = matched;
                if (buttonMidiType == nodes::button::kButtonMidiCc) above = event.data2 >= 64;
                else if (buttonMidiType == nodes::button::kButtonMidiNote) above = event.codeIndex == 0x9 && event.data2 > 0;

                if (binding.switchMode || buttonMidiType == nodes::button::kButtonMidiProgram)
                {
                    if (above)
                    {
                        if (control.valueListCount >= 2)
                            control.listIndex = static_cast<uint8_t>((control.listIndex + 1) % control.valueListCount);
                        else
                            control.wasAbove = !control.wasAbove;
                    }
                }
                else
                {
                    if (control.valueListCount >= 2 && above && !control.wasAbove)
                        control.listIndex = static_cast<uint8_t>((control.listIndex + 1) % control.valueListCount);
                    control.wasAbove = above;
                }
                control.currentValue = control.currentOutput();

                // Incoming hardware events already carry the controller's value.
                // Do not feed them back and create MIDI echo loops.
                control.midiFeedback = (static_cast<uint32_t>(control.channel) << 16) |
                                       (static_cast<uint32_t>(control.cc) << 8) | control.feedbackValue();
                pushControlRouteValue(*this, w, ControlRoute::Kind::Button, control.currentValue, controlIndex);
                recordControlDebug(w, controlIndex, false, channel0, number,
                                   buttonMidiType == nodes::button::kButtonMidiProgram ? (above ? 127 : 0) : event.data2);
            }
        }
    }

    if (event.codeIndex == 0xC && event.data1 <= 127)
    {
        for (uint16_t w = 0; w < widgetCount; ++w)
        {
            if (widgets[w].type != NodeType::MidiIn) continue;
            MidiInState &m = instances[widgets[w].instanceBase].midiIn;
            if (m.channel >= 0 && m.channel != (event.status & 0x0F)) continue;
            m.program = event.data1;
            pushControlRouteValue(*this, w, ControlRoute::Kind::MidiProgram, m.program);
        }
    }

    // Relative CC and Program Change each count as one up/down tick.
    if ((event.codeIndex == 0xB || event.codeIndex == 0xC) && event.data1 <= 127)
    {
        for (uint16_t w = 0; w < widgetCount; ++w)
        {
            if (widgets[w].type != NodeType::UpDownButton) continue;
            NodeInstance &udInst = instances[widgets[w].instanceBase];
            UpDownButtonState &ud = *udInst.upDownButton;
            bool matchedUp = ud.midiTypeUp == buttonMidiType && ud.ccUp >= 0 && static_cast<uint8_t>(ud.ccUp) == event.data1 &&
                              ud.channelUp >= 0 && static_cast<uint8_t>(ud.channelUp) == (event.status & 0x0F);
            bool matchedDown = ud.midiTypeDown == buttonMidiType && ud.ccDown >= 0 && static_cast<uint8_t>(ud.ccDown) == event.data1 &&
                                ud.channelDown >= 0 && static_cast<uint8_t>(ud.channelDown) == (event.status & 0x0F);
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
                for (uint8_t knobIndex = 0; knobIndex < knobInst.knob->count; ++knobIndex)
                {
                    const KnobControl &binding = knobInst.knob->controls[knobIndex];
                    if (binding.cc < 0 || static_cast<uint8_t>(binding.cc) != ccNumber ||
                        binding.channel < 0 || static_cast<uint8_t>(binding.channel) != ccChannel0)
                    {
                        continue;
                    }
                    const uint8_t controlIndex = KnobState::controlIndex(knobIndex, knobInst.knob->activeLayer);
                    KnobControl &k = knobInst.knob->controls[controlIndex];
                    k.raw = static_cast<float>(ccValue);
                    k.currentValue = k.mappedValue();
                    k.midiFeedback = (static_cast<uint32_t>(k.channel) << 16) |
                                     (static_cast<uint32_t>(k.cc) << 8) | ccValue;
                    pushControlRouteValue(*this, w, ControlRoute::Kind::Knob, k.currentValue, controlIndex);
                    pushControlRouteValue(*this, w, ControlRoute::Kind::KnobCc, k.raw, controlIndex);
                    recordControlDebug(w, controlIndex, false, ccChannel0, ccNumber, ccValue);
                }
            }
            else if (widgets[w].type == NodeType::Fm8)
            {
                // FM8 MIDI learn: the shared patch (owned by the primary voice) holds the bindings.
                NodeInstance &fmInst = instances[widgets[w].instanceBase];
                if (fmInst.fm8 != nullptr && fmInst.primary == &fmInst && fmInst.fm8->patch != nullptr)
                    fmInst.fm8->patch->applyMidiCc(ccNumber, ccChannel0, ccValue);
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
                recordControlDebug(w, 0, false, ccChannel0, ccNumber, ccValue);
            }
            else if (widgets[w].type == NodeType::OutputSwitch)
            {
                NodeInstance &osInst = instances[widgets[w].instanceBase];
                OutputSwitchState &os = osInst.outputSwitch;
                if (os.cc < 0 || static_cast<uint8_t>(os.cc) != ccNumber)
                {
                    continue;
                }
                if (os.channel < 0 || static_cast<uint8_t>(os.channel) != ccChannel0)
                {
                    continue;
                }
                // Same direct CC-to-output mapping as Switch's own
                // selectIn2 above, just picking the output side instead.
                os.selectOut2 = ccValue >= 64;
                recordControlDebug(w, 0, false, ccChannel0, ccNumber, ccValue);
            }
            else if (widgets[w].type == NodeType::MidiIn ||
                     (widgets[w].type == NodeType::MidiFilter && filtered[w]))
            {
                if (widgets[w].type == NodeType::MidiFilter && (ccNumber == 120 || ccNumber == 123))
                {
                    auto &m = instances[widgets[w].instanceBase].midiFilter->midi;
                    auto &allocator = voiceAllocators[w];
                    for (uint8_t v = 0; v < allocator.voiceCount(); ++v)
                    {
                        const int16_t heldNote = allocator.currentNote(v);
                        if (heldNote >= 0) releaseVoiceGate(*this, w, v, static_cast<uint8_t>(heldNote));
                    }
                    allocator.configure(m.voices, m.overflowMode);
                    m.sustainPedalDown = false;
                    continue;
                }
                // Sustain pedal, CC64: >=64 down / <64 up, same convention
                // already used by Button's CC-learn above. Always active per
                // MidiIn, no enable/disable toggle (see plan).
                if (ccNumber != 64)
                {
                    continue;
                }
                NodeInstance &midiInst = instances[widgets[w].instanceBase];
                MidiInState &m = widgets[w].type == NodeType::MidiFilter ? midiInst.midiFilter->midi : midiInst.midiIn;
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

    // Channel-pressure (Aftertouch, CIN 0xD): event-driven and independent
    // of note timing - unlike deliverNoteControlRoutes (which only fires ON
    // note-on/off for ONE resolved voice), this must reach EVERY currently
    // active voice instance of whatever's connected downstream, because a
    // real channel-pressure message affects the whole MIDI channel, not one
    // note. Fixed linear 0-10V mapping, same convention as MidiVelocity
    // (cv::velocityToVoltage) - not configurable per widget, so computed
    // once outside the per-widget loop. pushControlRouteValue() already
    // fans out to every ControlRoute from this source AND every instance of
    // each route's target (Graph.h:303).
    if (event.codeIndex == 0xD)
    {
        const uint8_t eventChannel0 = event.status & 0x0F;
        const float atVolts = cv::velocityToVoltage(static_cast<float>(event.data1));
        for (uint16_t w = 0; w < widgetCount; ++w)
        {
            if (widgets[w].type != NodeType::MidiIn) continue;
            const MidiInState &m = instances[widgets[w].instanceBase].midiIn;
            if (m.channel >= 0 && static_cast<uint8_t>(m.channel) != eventChannel0) continue;
            pushControlRouteValue(*this, w, ControlRoute::Kind::MidiAftertouch, atVolts);
        }
        return;
    }

    // Pitch bend (CIN 0xE): same event-driven/all-instances delivery as
    // aftertouch above, but the bend RANGE is per-MidiIn-widget config
    // (MidiInState::pitchBendRangeCents), so the volts computation happens
    // INSIDE the loop - each matching source widget may have a different
    // configured range, unlike aftertouch's fixed mapping. Deliberately a
    // SEPARATE output, not blended into pitchCv - 0V = no bend. The
    // persisted cents range remains compatible with existing presets;
    // the settings dialog displays it in volts (1200 cents/volt).
    if (event.codeIndex == 0xE)
    {
        const uint8_t eventChannel0 = event.status & 0x0F;
        const int16_t bend = static_cast<int16_t>(((event.data2 & 0x7F) << 7 | (event.data1 & 0x7F)) - 8192);
        for (uint16_t w = 0; w < widgetCount; ++w)
        {
            if (widgets[w].type != NodeType::MidiIn) continue;
            const MidiInState &m = instances[widgets[w].instanceBase].midiIn;
            if (m.channel >= 0 && static_cast<uint8_t>(m.channel) != eventChannel0) continue;
            const float rangeVolts = m.pitchBendRangeCents / 1200.0f;
            float bendVolts = (static_cast<float>(bend) / (bend < 0 ? 8192.0f : 8191.0f)) * rangeVolts;
            if (m.pitchBendQuantizeCents)
            {
                // Snap to the nearest whole cent (1/1200 V) so the output
                // always lands on a voltage matching an exact pitch step,
                // instead of a smooth in-between value.
                const float limitCents = floorf(m.pitchBendRangeCents);
                bendVolts = std::fmax(-limitCents, std::fmin(limitCents, roundf(bendVolts * 1200.0f))) / 1200.0f;
            }
            pushControlRouteValue(*this, w, ControlRoute::Kind::MidiPitchBend, bendVolts);
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
        if (widgets[w].type != NodeType::MidiIn && !(widgets[w].type == NodeType::MidiFilter && filtered[w]))
        {
            continue;
        }
        NodeInstance &midiInst = instances[widgets[w].instanceBase];
        MidiInState &midi = widgets[w].type == NodeType::MidiFilter ? midiInst.midiFilter->midi : midiInst.midiIn;
        const uint8_t sourceNote = widgets[w].type == NodeType::MidiFilter ? filterEvents[w].data1 : note;
        const int8_t midiChannel = midi.channel;
        if (midiChannel >= 0 && midiChannel != static_cast<int8_t>(eventChannel0))
        {
            continue;
        }
        if (noteOn && widgets[w].type == NodeType::MidiIn && !midi.noteAllowed(sourceNote))
        {
            continue;
        }

        VoiceAllocator &alloc = voiceAllocators[w];
        const int16_t previousNote = alloc.currentNote(0);
        const int8_t voiceIdx =
            noteOn ? alloc.noteOn(sourceNote, voiceStillSounding) : alloc.noteOff(sourceNote, midi.sustainPedalDown);
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
                deliverNoteControlRoutes(*this, w, 0, sourceNote, false, velocity, /*side=*/-1);
            }
            continue;
        }

        deliverNoteControlRoutes(*this, w, static_cast<uint8_t>(voiceIdx), sourceNote, noteOn, velocity, /*side=*/-1);
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

inline void Graph::applyLiveParams(const char *widgetId, cJSON *paramsObj,
                                   void (*midiCcOut)(uint8_t channel, uint8_t cc, uint8_t value7bit),
                                   KnobLiveChangeInfo *outActivity)
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
    uint8_t knobControlIndex = 0;
    uint8_t buttonControlIndex = 0;
    if (slot.type == NodeType::Knob)
    {
        cJSON *indexItem = cJSON_GetObjectItemCaseSensitive(paramsObj, "knobIndex");
        if (cJSON_IsNumber(indexItem) && indexItem->valueint > 0 &&
            indexItem->valueint <= static_cast<int>(synth::kKnobOutputMax))
        {
            knobControlIndex = static_cast<uint8_t>(indexItem->valueint - 1);
        }
        cJSON *countItem = cJSON_GetObjectItemCaseSensitive(paramsObj, "knobCount");
        if (cJSON_IsNumber(countItem))
        {
            const int count = countItem->valueint;
            const uint8_t clamped = static_cast<uint8_t>(count < 1 ? 1 :
                (count > static_cast<int>(synth::kKnobControlMax) ? synth::kKnobControlMax : count));
            for (uint16_t v = 0; v < slot.instanceCount; ++v)
            {
                KnobState *knob = instances[slot.instanceBase + v].knob;
                if (!knob->resizeControls(clamped, knob->layerCount)) return;
            }
        }
        cJSON *layerCountItem = cJSON_GetObjectItemCaseSensitive(paramsObj, "layerCount");
        if (cJSON_IsNumber(layerCountItem))
        {
            const int count = layerCountItem->valueint;
            const uint8_t clamped = static_cast<uint8_t>(count < 1 ? 1 :
                (count > static_cast<int>(synth::kKnobLayerMax) ? synth::kKnobLayerMax : count));
            for (uint16_t v = 0; v < slot.instanceCount; ++v)
            {
                KnobState *knob = instances[slot.instanceBase + v].knob;
                if (!knob->resizeControls(knob->count, clamped)) return;
            }
        }
        for (uint16_t v = 0; v < slot.instanceCount; ++v)
            if (!instances[slot.instanceBase + v].knob->controls.contains(knobControlIndex)) return;
        cJSON *activeLayerItem = cJSON_GetObjectItemCaseSensitive(paramsObj, "activeLayer");
        if (cJSON_IsNumber(activeLayerItem))
        {
            for (uint16_t v = 0; v < slot.instanceCount; ++v)
            {
                nodes::knob::setParam(instances[slot.instanceBase + v], "activeLayer",
                                      static_cast<float>(activeLayerItem->valuedouble));
            }
        }
    }
    else if (slot.type == NodeType::Button)
    {
        cJSON *indexItem = cJSON_GetObjectItemCaseSensitive(paramsObj, "buttonIndex");
        if (cJSON_IsNumber(indexItem) && indexItem->valueint > 0 &&
            indexItem->valueint <= static_cast<int>(synth::kButtonOutputMax))
            buttonControlIndex = static_cast<uint8_t>(indexItem->valueint - 1);
        cJSON *countItem = cJSON_GetObjectItemCaseSensitive(paramsObj, "buttonCount");
        if (cJSON_IsNumber(countItem))
        {
            const int count = countItem->valueint;
            const uint8_t clamped = static_cast<uint8_t>(count < 1 ? 1 :
                (count > static_cast<int>(synth::kButtonControlMax) ? synth::kButtonControlMax : count));
            for (uint16_t v = 0; v < slot.instanceCount; ++v) {
                ButtonState *button = instances[slot.instanceBase + v].button;
                if (!button->resizeControls(clamped, button->layerCount)) return;
            }
        }
        cJSON *layerCountItem = cJSON_GetObjectItemCaseSensitive(paramsObj, "layerCount");
        if (cJSON_IsNumber(layerCountItem))
        {
            const int count = layerCountItem->valueint;
            const uint8_t clamped = static_cast<uint8_t>(count < 1 ? 1 :
                (count > static_cast<int>(synth::kButtonLayerMax) ? synth::kButtonLayerMax : count));
            for (uint16_t v = 0; v < slot.instanceCount; ++v)
            {
                ButtonState *button = instances[slot.instanceBase + v].button;
                if (!button->resizeControls(button->count, clamped)) return;
            }
        }
        for (uint16_t v = 0; v < slot.instanceCount; ++v)
            if (!instances[slot.instanceBase + v].button->controls.contains(buttonControlIndex)) return;
        cJSON *activeLayerItem = cJSON_GetObjectItemCaseSensitive(paramsObj, "activeLayer");
        if (cJSON_IsNumber(activeLayerItem))
        {
            for (uint16_t v = 0; v < slot.instanceCount; ++v)
                nodes::button::setParamButton(instances[slot.instanceBase + v], "activeLayer",
                                              static_cast<float>(activeLayerItem->valuedouble));
        }
    }

    cJSON *item = nullptr;
    cJSON_ArrayForEach(item, paramsObj)
    {
        const char *key = item->string;
        if (key == nullptr)
        {
            continue;
        }
        if (slot.type == NodeType::Knob &&
            (strcmp(key, "knobIndex") == 0 || strcmp(key, "knobCount") == 0 ||
             strcmp(key, "layerCount") == 0 || strcmp(key, "activeLayer") == 0))
        {
            continue;
        }
        if (slot.type == NodeType::Button &&
            (strcmp(key, "buttonIndex") == 0 || strcmp(key, "buttonCount") == 0 ||
             strcmp(key, "layerCount") == 0 || strcmp(key, "activeLayer") == 0))
            continue;

        if (slot.type == NodeType::Knob && strcmp(key, "label") == 0 && cJSON_IsString(item))
        {
            for (uint16_t v = 0; v < slot.instanceCount; ++v)
            {
                auto &control = instances[slot.instanceBase + v].knob->controls[knobControlIndex];
                snprintf(control.label, sizeof(control.label), "%s", item->valuestring);
            }
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
        if (slot.type == NodeType::MidiIn && strcmp(key, "pitchBendQuantizeCents") == 0 && cJSON_IsBool(item))
        {
            instances[slot.instanceBase].midiIn.pitchBendQuantizeCents = cJSON_IsTrue(item);
            continue;
        }
        if (slot.type == NodeType::MidiIn && strcmp(key, "keyMask") == 0 && cJSON_IsString(item))
        {
            for (uint16_t v = 0; v < slot.instanceCount; ++v)
                parseKeyMaskHex(item->valuestring, instances[slot.instanceBase + v].midiIn.keyMask);
            continue;
        }

        if (slot.type == NodeType::DrumSequencer && strcmp(key, "trackPulseMs") == 0 && cJSON_IsArray(item))
        {
            for (uint16_t v = 0; v < slot.instanceCount; ++v)
                nodes::drumsequencer::applyTrackPulseMs(item, instances[slot.instanceBase + v].drumSequencer->trackPulseMs);
            continue;
        }

        if (cJSON_IsNumber(item))
        {
            const float value = static_cast<float>(item->valuedouble);
            bool handled = false;
            for (uint16_t v = 0; v < slot.instanceCount; ++v)
            {
                if (slot.type == NodeType::Knob)
                {
                    handled = nodes::knob::setParam(instances[slot.instanceBase + v], key, value, knobControlIndex) || handled;
                }
                else if (slot.type == NodeType::Button)
                {
                    handled = nodes::button::setParamButton(instances[slot.instanceBase + v], key, value, buttonControlIndex) || handled;
                }
                else
                {
                    handled = setFloatFieldByName(instances[slot.instanceBase + v], slot.type, key, value) || handled;
                }
            }
            if (handled)
            {
                continue;
            }
        }

        const bool soundFontBool = slot.type == NodeType::SoundFontPlayer &&
            (strcmp(key, "enableCc") == 0 || strcmp(key, "enableModWheel") == 0 ||
             strcmp(key, "enablePitchWheel") == 0);
        const bool mdaPianoBool = slot.type == NodeType::MdaPiano &&
            (strcmp(key, "enableModWheel") == 0 || strcmp(key, "enablePitchWheel") == 0);
        if ((soundFontBool || mdaPianoBool) && cJSON_IsBool(item))
        {
            setFloatFieldByName(instances[slot.instanceBase], slot.type, key, cJSON_IsTrue(item) ? 1.0f : 0.0f);
            continue;
        }

        // Non-float / special-cased fields that setFloatFieldByName doesn't
        // cover (strings, nullable ints, bools).
        if ((slot.type == NodeType::Oscillator || slot.type == NodeType::Lfo || slot.type == NodeType::FmOscillator) &&
            strcmp(key, "waveType") == 0 && cJSON_IsString(item))
        {
            const WaveType wave = waveTypeFromString(item->valuestring, slot.type == NodeType::FmOscillator,
                                                     slot.type == NodeType::Lfo);
            for (uint16_t v = 0; v < slot.instanceCount; ++v)
            {
                NodeInstance &inst = instances[slot.instanceBase + v];
                // Picking a linear wave starts a fresh ramp (phase 0 / linearPos 0).
                if (slot.type == NodeType::Oscillator)
                {
                    inst.osc.wave = wave;
                    if (isLinearWave(wave))
                        inst.osc.phaseGen.resetPhase();
                }
                else if (slot.type == NodeType::FmOscillator)
                {
                    inst.fmOsc.wave = wave;
                    if (isLinearWave(wave))
                        inst.fmOsc.phaseGen.resetPhase();
                }
                else
                {
                    inst.lfo.wave = wave;
                    inst.lfo.linearPos = 0.0f;
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
            const FilterKind kind = curtisLegacyFilterKind(item->valuestring);
            for (uint16_t v = 0; v < slot.instanceCount; ++v)
                nodes::curtisfilter::setKind(instances[slot.instanceBase + v], kind);
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
        else if (slot.type == NodeType::DxEnvelope && strncmp(key, "time", 4) == 0 &&
                 key[4] >= '1' && key[4] <= '4' && strcmp(key + 5, "Curve") == 0 && cJSON_IsString(item))
        {
            const bool log = strcmp(item->valuestring, "log") == 0;
            const int index = key[4] - '1';
            for (uint16_t v = 0; v < slot.instanceCount; ++v)
            {
                instances[slot.instanceBase + v].dxEnvelope->timeLog[index] = log;
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
        else if (slot.type == NodeType::MidiIn && strcmp(key, "pitchBendRangeCents") == 0 && cJSON_IsNumber(item))
        {
            const float value = static_cast<float>(item->valuedouble);
            const float clamped = std::isfinite(value) ? std::fmax(0.0f, std::fmin(6000.0f, value)) : 200.0f;
            for (uint16_t v = 0; v < slot.instanceCount; ++v)
            {
                instances[slot.instanceBase + v].midiIn.pitchBendRangeCents = clamped;
            }
        }
        else if (slot.type == NodeType::Knob && strcmp(key, "cc") == 0)
        {
            const int16_t cc = cJSON_IsNumber(item) ? static_cast<int16_t>(item->valueint) : -1;
            for (uint16_t v = 0; v < slot.instanceCount; ++v)
            {
                KnobState *knob = instances[slot.instanceBase + v].knob;
                const uint8_t knobIndex = knobControlIndex % synth::kKnobControlMax;
                for (uint8_t layerIndex = 0; layerIndex < knob->layerCount; ++layerIndex)
                    knob->controls[KnobState::controlIndex(knobIndex, layerIndex)].cc = cc;
            }
        }
        else if (slot.type == NodeType::Knob && strcmp(key, "channel") == 0)
        {
            const int8_t ch = cJSON_IsNumber(item) ? static_cast<int8_t>(item->valueint) : -1;
            for (uint16_t v = 0; v < slot.instanceCount; ++v)
            {
                KnobState *knob = instances[slot.instanceBase + v].knob;
                const uint8_t knobIndex = knobControlIndex % synth::kKnobControlMax;
                for (uint8_t layerIndex = 0; layerIndex < knob->layerCount; ++layerIndex)
                    knob->controls[KnobState::controlIndex(knobIndex, layerIndex)].channel = ch;
            }
        }
        else if (slot.type == NodeType::Knob && strcmp(key, "constraintMin") == 0)
        {
            const bool has = cJSON_IsNumber(item);
            const float val = has ? static_cast<float>(item->valuedouble) : 0.0f;
            for (uint16_t v = 0; v < slot.instanceCount; ++v)
            {
                KnobControl &control = instances[slot.instanceBase + v].knob->controls[knobControlIndex];
                control.hasMin = has;
                control.minVal = val;
                control.currentValue = control.mappedValue();
            }
        }
        else if (slot.type == NodeType::Knob && strcmp(key, "constraintMax") == 0)
        {
            const bool has = cJSON_IsNumber(item);
            const float val = has ? static_cast<float>(item->valuedouble) : 0.0f;
            for (uint16_t v = 0; v < slot.instanceCount; ++v)
            {
                KnobControl &control = instances[slot.instanceBase + v].knob->controls[knobControlIndex];
                control.hasMax = has;
                control.maxVal = val;
                control.currentValue = control.mappedValue();
            }
        }
        else if (slot.type == NodeType::Knob && strcmp(key, "valueList") == 0)
        {
            for (uint16_t v = 0; v < slot.instanceCount; ++v)
            {
                KnobControl &k = instances[slot.instanceBase + v].knob->controls[knobControlIndex];
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
                ButtonState *button = instances[slot.instanceBase + v].button;
                const uint8_t buttonIndex = buttonControlIndex % synth::kButtonControlMax;
                for (uint8_t layerIndex = 0; layerIndex < button->layerCount; ++layerIndex)
                    button->controls[ButtonState::controlIndex(buttonIndex, layerIndex)].cc = cc;
            }
        }
        else if (slot.type == NodeType::Button && strcmp(key, "channel") == 0)
        {
            const int8_t ch = cJSON_IsNumber(item) ? static_cast<int8_t>(item->valueint) : -1;
            for (uint16_t v = 0; v < slot.instanceCount; ++v)
            {
                ButtonState *button = instances[slot.instanceBase + v].button;
                const uint8_t buttonIndex = buttonControlIndex % synth::kButtonControlMax;
                for (uint8_t layerIndex = 0; layerIndex < button->layerCount; ++layerIndex)
                    button->controls[ButtonState::controlIndex(buttonIndex, layerIndex)].channel = ch;
            }
        }
        else if (slot.type == NodeType::Button && strcmp(key, "midiType") == 0 && cJSON_IsString(item))
        {
            const uint8_t midiType = nodes::button::buttonMidiTypeFromString(item->valuestring);
            for (uint16_t v = 0; v < slot.instanceCount; ++v)
            {
                ButtonState *button = instances[slot.instanceBase + v].button;
                const uint8_t buttonIndex = buttonControlIndex % synth::kButtonControlMax;
                for (uint8_t layerIndex = 0; layerIndex < button->layerCount; ++layerIndex)
                    button->controls[ButtonState::controlIndex(buttonIndex, layerIndex)].midiType = midiType;
            }
        }
        else if (slot.type == NodeType::Button && strcmp(key, "switchMode") == 0 && cJSON_IsBool(item))
        {
            const bool enabled = cJSON_IsTrue(item);
            for (uint16_t v = 0; v < slot.instanceCount; ++v)
            {
                ButtonState *button = instances[slot.instanceBase + v].button;
                const uint8_t buttonIndex = buttonControlIndex % synth::kButtonControlMax;
                for (uint8_t layerIndex = 0; layerIndex < button->layerCount; ++layerIndex)
                    button->controls[ButtonState::controlIndex(buttonIndex, layerIndex)].switchMode = enabled;
            }
        }
        else if (slot.type == NodeType::Button && strcmp(key, "valueList") == 0)
        {
            for (uint16_t v = 0; v < slot.instanceCount; ++v)
            {
                ButtonControl &control = instances[slot.instanceBase + v].button->controls[buttonControlIndex];
                control.valueListCount = applyFloatArrayItem(item, control.valueList, static_cast<uint8_t>(synth::kValueListMax));
                control.listIndex = 0;
                control.currentValue = control.currentOutput();
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
        else if (slot.type == NodeType::UpDownButton &&
                 (strcmp(key, "midiTypeUp") == 0 || strcmp(key, "midiTypeDown") == 0))
        {
            const uint8_t type = cJSON_IsString(item) && strcmp(item->valuestring, "program") == 0
                ? nodes::button::kButtonMidiProgram : nodes::button::kButtonMidiCc;
            for (uint16_t v = 0; v < slot.instanceCount; ++v)
            {
                auto &ud = *instances[slot.instanceBase + v].upDownButton;
                if (strcmp(key, "midiTypeUp") == 0) ud.midiTypeUp = type;
                else ud.midiTypeDown = type;
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
        else if (slot.type == NodeType::OutputSwitch && strcmp(key, "cc") == 0)
        {
            const int16_t cc = cJSON_IsNumber(item) ? static_cast<int16_t>(item->valueint) : -1;
            for (uint16_t v = 0; v < slot.instanceCount; ++v)
            {
                instances[slot.instanceBase + v].outputSwitch.cc = cc;
            }
        }
        else if (slot.type == NodeType::OutputSwitch && strcmp(key, "channel") == 0)
        {
            const int8_t ch = cJSON_IsNumber(item) ? static_cast<int8_t>(item->valueint) : -1;
            for (uint16_t v = 0; v < slot.instanceCount; ++v)
            {
                instances[slot.instanceBase + v].outputSwitch.channel = ch;
            }
        }
        else if (slot.type == NodeType::OutputSwitch && strcmp(key, "selectOut2") == 0 && cJSON_IsBool(item))
        {
            const bool sel = cJSON_IsTrue(item);
            for (uint16_t v = 0; v < slot.instanceCount; ++v)
            {
                instances[slot.instanceBase + v].outputSwitch.selectOut2 = sel;
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
        else if ((slot.type == NodeType::Lfo || slot.type == NodeType::Wavetable) && strcmp(key, "oneShot") == 0 &&
                 cJSON_IsBool(item))
        {
            const bool oneShot = cJSON_IsTrue(item);
            for (uint16_t v = 0; v < slot.instanceCount; ++v)
            {
                instances[slot.instanceBase + v].lfo.oneShot = oneShot;
            }
        }
        else if ((slot.type == NodeType::Lfo || slot.type == NodeType::Wavetable) && strcmp(key, "global") == 0 &&
                 cJSON_IsBool(item))
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
        else if (slot.type == NodeType::ReverbLight && strcmp(key, "bypass") == 0 && cJSON_IsBool(item))
        {
            const bool bypass = cJSON_IsTrue(item);
            for (uint16_t v = 0; v < slot.instanceCount; ++v)
            {
                instances[slot.instanceBase + v].reverbLight->bypass = bypass;
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
        else if (slot.type == NodeType::Bitcrusher && strcmp(key, "bypass") == 0 && cJSON_IsBool(item))
        {
            const bool bypass = cJSON_IsTrue(item);
            for (uint16_t v = 0; v < slot.instanceCount; ++v)
            {
                instances[slot.instanceBase + v].bitcrusher.bypass = bypass;
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
        else if (slot.type == NodeType::GraphicEq && strcmp(key, "bypass") == 0 && cJSON_IsBool(item))
        {
            const bool bypass = cJSON_IsTrue(item);
            for (uint16_t v = 0; v < slot.instanceCount; ++v)
            {
                instances[slot.instanceBase + v].graphicEq->bypass = bypass;
            }
        }
        else if ((slot.type == NodeType::AudioOut || slot.type == NodeType::UsbOut ||
                  slot.type == NodeType::Es8311Out || slot.type == NodeType::Wm8960Out ||
                  slot.type == NodeType::Ns4168Out) &&
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
        const KnobState *knob = instances[slot.instanceBase].knob;
        const KnobControl &control = knob->controls[knobControlIndex];
        const bool activeControl = knobControlIndex / synth::kKnobControlMax == knob->activeLayer;
        const bool valueChanged = cJSON_GetObjectItemCaseSensitive(paramsObj, "raw") != nullptr ||
                                  cJSON_GetObjectItemCaseSensitive(paramsObj, "point1In") != nullptr ||
                                  cJSON_GetObjectItemCaseSensitive(paramsObj, "point1Out") != nullptr ||
                                  cJSON_GetObjectItemCaseSensitive(paramsObj, "point2In") != nullptr ||
                                  cJSON_GetObjectItemCaseSensitive(paramsObj, "point2Out") != nullptr ||
                                  cJSON_GetObjectItemCaseSensitive(paramsObj, "constraintMin") != nullptr ||
                                  cJSON_GetObjectItemCaseSensitive(paramsObj, "constraintMax") != nullptr ||
                                  cJSON_GetObjectItemCaseSensitive(paramsObj, "valueList") != nullptr;
        if (activeControl && valueChanged)
        {
            pushControlRouteValue(*this, static_cast<uint16_t>(widgetIdx), ControlRoute::Kind::Knob,
                                  control.currentValue, knobControlIndex);
            pushControlRouteValue(*this, static_cast<uint16_t>(widgetIdx), ControlRoute::Kind::KnobCc,
                                  control.raw, knobControlIndex);
            recordControlDebug(static_cast<uint16_t>(widgetIdx), knobControlIndex, true);
        }
        // Mirror this control's new value back to whatever hardware CC it's
        // learned to (see SynthEngine::MidiCcOut) - a change made from the
        // browser should still move that controller's LED ring/motorized
        // fader, not just the on-screen knob. Same condition also lights up
        // the on-device activity display (outActivity) - a physical CC
        // message already does that (see SynthEngine::
        // publishMidiControlActivity), a browser-driven change previously
        // didn't.
        if (activeControl && valueChanged && (midiCcOut != nullptr || outActivity != nullptr) &&
            control.cc >= 0 && control.channel >= 0)
        {
            const float rawClamped = control.raw < 0.0f ? 0.0f : (control.raw > 127.0f ? 127.0f : control.raw);
            const uint8_t channel8 = static_cast<uint8_t>(control.channel);
            const uint8_t cc8 = static_cast<uint8_t>(control.cc);
            const uint8_t raw8 = static_cast<uint8_t>(rawClamped + 0.5f);
            if (midiCcOut != nullptr)
            {
                midiCcOut(channel8, cc8, raw8);
            }
            if (outActivity != nullptr)
            {
                outActivity->changed = true;
                outActivity->label = slot.label[0] != '\0' ? slot.label : slot.id;
                outActivity->channel = channel8;
                outActivity->cc = cc8;
                outActivity->raw = raw8;
                outActivity->value = control.currentValue;
            }
        }
    }
    else if (slot.type == NodeType::Button)
    {
        const ButtonState *button = instances[slot.instanceBase].button;
        const ButtonControl &control = button->controls[buttonControlIndex];
        const bool activeControl = buttonControlIndex / synth::kButtonControlMax == button->activeLayer;
        const bool valueChanged = cJSON_GetObjectItemCaseSensitive(paramsObj, "value0") != nullptr ||
                                  cJSON_GetObjectItemCaseSensitive(paramsObj, "value127") != nullptr ||
                                  cJSON_GetObjectItemCaseSensitive(paramsObj, "listIndex") != nullptr ||
                                  cJSON_GetObjectItemCaseSensitive(paramsObj, "wasAbove") != nullptr ||
                                  cJSON_GetObjectItemCaseSensitive(paramsObj, "valueList") != nullptr;
        if (activeControl && valueChanged)
        {
            pushControlRouteValue(*this, static_cast<uint16_t>(widgetIdx), ControlRoute::Kind::Button,
                                  control.currentValue, buttonControlIndex);
            recordControlDebug(static_cast<uint16_t>(widgetIdx), buttonControlIndex, true);
        }
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
    else if ((slot.type == NodeType::Switch && cJSON_IsBool(cJSON_GetObjectItemCaseSensitive(paramsObj, "selectIn2"))) ||
             (slot.type == NodeType::OutputSwitch && cJSON_IsBool(cJSON_GetObjectItemCaseSensitive(paramsObj, "selectOut2"))))
    {
        recordControlDebug(static_cast<uint16_t>(widgetIdx), 0, true);
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
        case NodeType::GraphicEq:
            if (inst.graphicEq != nullptr)
            {
                destroyGraphicEq(*inst.graphicEq);
                heap_caps_free(inst.graphicEq);
                inst.graphicEq = nullptr;
            }
            break;
        case NodeType::Chorus:
            nodes::chorus::destroy(inst);
            break;
        case NodeType::MidiFilter:
            heap_caps_free(inst.midiFilter);
            inst.midiFilter = nullptr;
            break;
        case NodeType::CombFilter:
            nodes::combfilter::destroy(inst);
            break;
        case NodeType::Delay:
            nodes::delay::destroy(inst);
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
        case NodeType::MdaPiano:
            nodes::mdapiano::destroy(inst);
            break;
        case NodeType::Adsr:
            nodes::adsr::destroy(inst);
            break;
        case NodeType::DxEnvelope:
            nodes::dxenvelope::destroy(inst);
            break;
        case NodeType::Wavetable:
            nodes::wavetable::destroy(inst);
            break;
        case NodeType::Lfo:
            nodes::lfo::destroy(inst);
            break;
        case NodeType::FmOscillator:
            nodes::fmoscillator::destroy(inst);
            break;
        case NodeType::Fm8:
            nodes::fm8::destroy(inst);
            break;
        case NodeType::StepArpeggiator:
            nodes::steparp::destroy(inst);
            break;
        case NodeType::DrumSequencer:
            nodes::drumsequencer::destroy(inst);
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
        case NodeType::AudioOutputLink:
        case NodeType::AudioInputLink:
        case NodeType::LinkedInput:
        case NodeType::LinkedOutput:
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
            break;
        case NodeType::OutputMultiplexer:
            if (inst.outputMux != nullptr)
            {
                for (float *&buf : inst.outputMux->outPorts)
                {
                    heap_caps_free(buf);
                    buf = nullptr;
                }
                heap_caps_free(inst.outputMux);
                inst.outputMux = nullptr;
            }
            break;
        default:
            break;
        }
        heap_caps_free(inst.outL);
        heap_caps_free(inst.outR);
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
