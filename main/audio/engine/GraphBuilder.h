#pragma once

#include <cmath>
#include <cstdio>
#include <cstring>
#include <new>

#include "esp_heap_caps.h"
#include "esp_log.h"

#include "EspEqEffect.h"
#include "EspAdditionalEffects.h"
#include "Graph.h"
#include "MemoryDiagnostics.h"
#include "PortTables.h"
#include "PresetParser.h"
#include "SoundFontPlayer.h"
#include "WavLoader.h"

#include "nodes/AdsrNode.h"
#include "nodes/ArithmeticNodes.h"
#include "nodes/ButtonNode.h"
#include "nodes/BitcrusherNode.h"
#include "nodes/WaveFolderNode.h"
#include "nodes/PanNode.h"
#include "nodes/ChorusNode.h"
#include "nodes/ClockNode.h"
#include "nodes/SampleHoldNode.h"
#include "nodes/ConstNode.h"
#include "nodes/CurtisFilterNode.h"
#include "nodes/DelayNode.h"
#include "nodes/DetuneNode.h"
#include "nodes/DxEnvelopeNode.h"
#include "nodes/FilterNode.h"
#include "nodes/FmOscillatorNode.h"
#include "nodes/GainNode.h"
#include "nodes/HammondNode.h"
#include "nodes/KnobNode.h"
#include "nodes/LfoNode.h"
#include "nodes/LinkNodes.h"
#include "nodes/MidiInNode.h"
#include "nodes/MidiSplitNode.h"
#include "nodes/MixerNode.h"
#include "nodes/MultiplexerNode.h"
#include "nodes/OscillatorNode.h"
#include "nodes/OutputMultiplexerNode.h"
#include "nodes/OutputNodes.h"
#include "nodes/OutputSwitchNode.h"
#include "nodes/ReverbNode.h"
#include "nodes/StepArpeggiatorNode.h"
#include "nodes/SwitchNode.h"
#include "nodes/ValueMapperNode.h"

// Builds a fully-wired, ready-to-render Graph from a parsed preset. Never
// mutates an existing Graph - always constructs a brand new one (caller
// swaps the live pointer and destroys the old Graph via Graph::destroy()).
class GraphBuilder
{
public:
    // outRequestedVoices/outEffectiveVoices (both optional, pass nullptr to
    // ignore) report the voice count the preset's MidiIn widget(s) actually
    // asked for vs. the one this build ended up using. They differ when the
    // requested count wouldn't fit entirely in internal RAM (build() retries
    // with fewer voices - see the .cpp - rather than silently landing the
    // per-voice hot-path state in PSRAM, which measurably tanks render
    // performance well before it would actually run out of memory) or when
    // voiceCeiling below restricts it further regardless of what fits. The
    // caller (SynthEngine::buildFromFile) uses a mismatch here to warn the
    // browser and let it correct its own "voices" setting back down.
    //
    // voiceCeiling: a hard cap applied on top of the preset's own request,
    // independent of what would actually fit - default SYNTH_VOICE_COUNT
    // means "no additional restriction". Only SynthEngine's overload
    // watchdog passes a tighter one, when a *previous* build's voice count
    // sustainably exceeded the real-time render budget (a purely CPU-bound
    // limit GraphBuilder has no way to predict or detect on its own - it
    // only ever sees allocation success/failure, never render timing).
    static Graph *build(const PresetDoc &doc, uint16_t *outRequestedVoices = nullptr,
                         uint16_t *outEffectiveVoices = nullptr, uint16_t voiceCeiling = SYNTH_VOICE_COUNT);

    // Cheap standalone peek at what a preset's MidiIn widget(s) ask for
    // (clamped to SYNTH_VOICE_COUNT), without allocating a Graph - build()
    // uses this internally, and SynthEngine::buildFromFile() uses it to
    // decide up front whether a reload needs the old live graph freed
    // before this one is built (see its call site for why).
    static uint16_t peekRequestedVoices(const PresetDoc &doc);
};

namespace
{
constexpr char kGraphBuilderTag[] = "GraphBuilder";

// Used for every per-instance block buffer (output L/R, input ports) -
// internal RAM first by default, PSRAM only for WavePlayer/SoundFontPlayer
// (see preferPsram below). Tried PSRAM-first for every type once (frees a
// lot of internal RAM) but that stacks each type's per-sample block-buffer
// traffic onto the same PSRAM bus/L2-cache that WavePlayer's own sample-data
// streaming already leans on hard - audible glitches showed up starting
// around 12 voices with two WavePlayer widgets active per voice. Back to
// internal-first until that's resolved differently (e.g. only widening this
// for graphs that don't use WavePlayer/SoundFontPlayer at all).
inline float *allocateBlockBuffer(bool preferPsram, uint16_t &psramFallbacks)
{
    constexpr size_t bytes = synth::kBlockSize * sizeof(float);
    const uint32_t primaryCaps = preferPsram ? (MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT) : (MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT);
    const uint32_t fallbackCaps = preferPsram ? (MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT) : (MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    float *buffer = static_cast<float *>(heap_caps_calloc(1, bytes, primaryCaps));
    if (buffer == nullptr)
    {
        buffer = static_cast<float *>(heap_caps_calloc(1, bytes, fallbackCaps));
        if (buffer != nullptr && !preferPsram)
        {
            ++psramFallbacks; // only meaningful as "unexpectedly landed in PSRAM"
        }
    }
    return buffer;
}

// Allocates one instance's type-specific big-state payload (see
// NodeInstance.h's chorus/delay/reverb/reverbLight/espEffect comment) -
// internal RAM first, PSRAM fallback, same policy/counter as
// allocateBlockBuffer above. Default-constructed via placement-new (matches
// how these were built as plain value members before this split).
template <typename T>
T *allocateNodeState(uint16_t &psramFallbacks)
{
    void *raw = heap_caps_malloc(sizeof(T), MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT);
    if (raw == nullptr)
    {
        raw = heap_caps_malloc(sizeof(T), MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
        if (raw != nullptr)
        {
            ++psramFallbacks;
        }
    }
    if (raw == nullptr)
    {
        return nullptr;
    }
    return new (raw) T();
}

inline bool hasStereoOutput(NodeType type)
{
    return type == NodeType::Chorus || type == NodeType::EspEq || ports::isGlobalOnly(type) ||
           type == NodeType::AudioOut || type == NodeType::UsbOut || type == NodeType::Es8311Out ||
           type == NodeType::Wm8960Out || type == NodeType::Detune || type == NodeType::WavePlayer ||
           type == NodeType::SoundFontPlayer || type == NodeType::AudioOutputLink || type == NodeType::AudioInputLink ||
           type == NodeType::StepArpeggiator || type == NodeType::Pan || type == NodeType::OutputSwitch ||
           type == NodeType::OutputMultiplexer;
}

inline bool requiresInputBlockBuffers(NodeType type)
{
    return type == NodeType::EspEq || type == NodeType::EspAlc || type == NodeType::EspChannelConvert ||
           type == NodeType::EspMixer || type == NodeType::EspSonic || type == NodeType::EspDrc ||
           type == NodeType::EspMbc;
}

// Parses "zone<1-4><suffix>" (e.g. "zone2Gate") into a 0-based zone index
// and ControlRoute::Kind - midi-split's dynamic-zone-count counterpart to
// PortTables.h's indexedInPort() ("in1".."in16" for Multiplexer), kept locally scoped since
// midi-split's virtual ports only ever get resolved right here.
inline bool parseZonePort(const char *portName, int &zoneIdx, ControlRoute::Kind &kind)
{
    if (portName == nullptr || strncmp(portName, "zone", 4) != 0)
    {
        return false;
    }
    const char *numPart = portName + 4;
    if (numPart[0] < '1' || numPart[0] > '4')
    {
        return false;
    }
    zoneIdx = numPart[0] - '1';
    const char *suffix = numPart + 1;
    // "Midi" and "Gate" both resolve to the same Kind - deliverNoteControlRoutes's
    // MidiGate case already drives every target type a zone output can
    // reasonably feed (Oscillator/Adsr/WavePlayer/SoundFontPlayer's note-on/
    // off, or a generic Gate-CV target via controlValue). Two port names
    // exist only so a zone's port set visually/structurally mirrors MidiIn's
    // own (MIDI/Pitch/Note/Velocity/Gate) - "Midi" is the conventional target
    // for MIDI-consuming widgets (e.g. a second SoundFontPlayer per zone),
    // "Gate" for a plain 0/10V Gate-CV consumer.
    if (strcmp(suffix, "Midi") == 0)
        kind = ControlRoute::Kind::MidiGate;
    else if (strcmp(suffix, "Pitch") == 0)
        kind = ControlRoute::Kind::MidiPitchCv;
    else if (strcmp(suffix, "Note") == 0)
        kind = ControlRoute::Kind::MidiNote;
    else if (strcmp(suffix, "Gate") == 0)
        kind = ControlRoute::Kind::MidiGate;
    else if (strcmp(suffix, "Velocity") == 0)
        kind = ControlRoute::Kind::MidiVelocity;
    else
        return false;
    return true;
}

inline int findWidgetIndex(const PresetDoc &doc, const char *id)
{
    for (uint16_t i = 0; i < doc.widgetCount; ++i)
    {
        if (strcmp(doc.widgets[i].id, id) == 0)
        {
            return static_cast<int>(i);
        }
    }
    return -1;
}

inline uint16_t requestedMidiVoices(const WidgetDesc &widget)
{
    if (widget.isFxPage)
    {
        return 1;
    }
    return widget.params.voices < 1 ? 1 : widget.params.voices;
}

// Voiced/global BFS classification - port of main.js's traverseVoiceSet().
// Boundary types (link widgets, no-output widgets) are never voiced and
// stop traversal; everything else reachable from a >1-voice midi-in is
// voiced. Global inputs fan out into voiced nodes; voices are summed only
// when they reach a true global boundary such as the output link.
inline void classifyVoiced(const PresetDoc &doc, bool *voiced)
{
    bool visited[synth::kMaxWidgets] = {};
    uint16_t queue[synth::kMaxWidgets];
    uint16_t qHead = 0, qTail = 0;

    for (uint16_t i = 0; i < doc.widgetCount; ++i)
    {
        if (doc.widgets[i].type == NodeType::MidiIn && requestedMidiVoices(doc.widgets[i]) > 1)
        {
            visited[i] = true; // midi-in itself is never instanced/voiced, just a BFS seed
            queue[qTail++] = i;
        }
    }

    while (qHead < qTail)
    {
        const uint16_t cur = queue[qHead++];

        // Freely-placeable linked-io pairing: a voiced LinkedInput must
        // propagate voicedness to every LinkedOutput sharing its linkId,
        // even though that pairing is never a doc.connections[] entry for
        // the loop below to find (GraphBuilder resolves the actual hidden
        // edge later, by matching linkId - see the "Linked-IO resolution"
        // block further down). Without this, a per-voice signal (e.g.
        // MidiIn's "gate") routed through a link would arrive at a single
        // global LinkedOutput instance instead of one per voice - every
        // voice's target (e.g. every Adsr instance) would then read that
        // same one global value instead of its own voice's.
        if (doc.widgets[cur].type == NodeType::LinkedInput)
        {
            const char *linkId = doc.widgets[cur].params.linkId;
            if (linkId[0] != '\0')
            {
                for (uint16_t i = 0; i < doc.widgetCount; ++i)
                {
                    if (doc.widgets[i].type != NodeType::LinkedOutput || visited[i])
                    {
                        continue;
                    }
                    if (strcmp(doc.widgets[i].params.sourceLinkId, linkId) == 0)
                    {
                        visited[i] = true;
                        voiced[i] = true;
                        queue[qTail++] = i;
                    }
                }
            }
        }

        for (uint16_t c = 0; c < doc.connectionCount; ++c)
        {
            if (strcmp(doc.connections[c].fromWidget, doc.widgets[cur].id) != 0)
            {
                continue;
            }
            const int targetIdx = findWidgetIndex(doc, doc.connections[c].toWidget);
            if (targetIdx < 0 || visited[targetIdx])
            {
                continue;
            }
            const NodeType targetType = doc.widgets[targetIdx].type;
            const bool boundary = ports::isLinkType(targetType) || ports::hasNoOutputs(targetType) ||
                                  ports::isGlobalOnly(targetType);

            visited[targetIdx] = true;
            if (!boundary)
            {
                voiced[targetIdx] = true;
                queue[qTail++] = targetIdx;
            }
        }
    }
}

inline void classifyIdleRendered(const PresetDoc &doc, const bool *voiced, bool *renderWhenIdle)
{
    bool changed;
    do
    {
        changed = false;
        for (uint16_t c = 0; c < doc.connectionCount; ++c)
        {
            const ConnectionDesc &edge = doc.connections[c];
            const int sourceIdx = findWidgetIndex(doc, edge.fromWidget);
            const int targetIdx = findWidgetIndex(doc, edge.toWidget);
            if (sourceIdx < 0 || targetIdx < 0 || !voiced[targetIdx] || renderWhenIdle[targetIdx])
            {
                continue;
            }
            if (ports::audioOutIndex(doc.widgets[sourceIdx].type, edge.fromPort) < 0 ||
                ports::audioInIndex(doc.widgets[targetIdx].type, edge.toPort) < 0)
            {
                continue;
            }
            const int targetPort = ports::audioInIndex(doc.widgets[targetIdx].type, edge.toPort);
            if (ports::isControlOnlyInput(doc.widgets[targetIdx].type, targetPort))
            {
                continue;
            }
            if (!voiced[sourceIdx] || renderWhenIdle[sourceIdx])
            {
                renderWhenIdle[targetIdx] = true;
                changed = true;
            }
        }
    } while (changed);
}

inline void addEdge(Graph &g, uint16_t fromInstance, uint8_t fromPort, uint16_t toInstance, uint8_t toPort, float scale,
             bool monitorTap = false)
{
    if (g.edgeCount >= synth::kMaxRuntimeEdges)
    {
        ESP_LOGE(kGraphBuilderTag, "Runtime edge capacity exceeded");
        return;
    }
    Edge &e = g.edges[g.edgeCount++];
    e.fromInstance = fromInstance;
    e.fromPort = fromPort;
    e.toInstance = toInstance;
    e.toPort = toPort;
    e.scale = scale;
    e.monitorTap = monitorTap;
}

// Expands one logical (widget,port)->(widget,port) connection into the
// concrete instance-level edges per the voiced/global fan-in/fan-out rules
// (see plan). The 1/sqrt(N) fan-in compensation is resolved once here.
inline void wireConnection(Graph &g, uint16_t fromWidgetIdx, int fromPort, uint16_t toWidgetIdx, int toPort)
{
    const WidgetSlot &fromW = g.widgets[fromWidgetIdx];
    const WidgetSlot &toW = g.widgets[toWidgetIdx];

    if (fromW.voiced && toW.voiced)
    {
        for (uint16_t v = 0; v < fromW.instanceCount; ++v)
        {
            addEdge(g, fromW.instanceBase + v, static_cast<uint8_t>(fromPort), toW.instanceBase + v,
                    static_cast<uint8_t>(toPort), 1.0f);
        }
    }
    else if (fromW.voiced && !toW.voiced)
    {
        const bool monitorTap =
            toW.type == NodeType::VuMeter || toW.type == NodeType::Oscilloscope || toW.type == NodeType::Spectrum;
        const float scale = monitorTap ? 1.0f : 1.0f / sqrtf(static_cast<float>(fromW.instanceCount));
        for (uint16_t v = 0; v < fromW.instanceCount; ++v)
        {
            addEdge(g, fromW.instanceBase + v, static_cast<uint8_t>(fromPort), toW.instanceBase,
                    static_cast<uint8_t>(toPort), scale, monitorTap);
        }
    }
    else if (!fromW.voiced && toW.voiced)
    {
        for (uint16_t v = 0; v < toW.instanceCount; ++v)
        {
            addEdge(g, fromW.instanceBase, static_cast<uint8_t>(fromPort), toW.instanceBase + v,
                    static_cast<uint8_t>(toPort), 1.0f);
        }
    }
    else
    {
        addEdge(g, fromW.instanceBase, static_cast<uint8_t>(fromPort), toW.instanceBase, static_cast<uint8_t>(toPort),
                1.0f);
    }
}

inline void groupEdgesBySourceWidget(Graph &g)
{
    uint16_t cursor = 0;
    for (uint16_t w = 0; w < g.widgetCount; ++w)
    {
        WidgetSlot &slot = g.widgets[w];
        slot.outgoingEdgeStart = cursor;
        for (uint16_t e = cursor; e < g.edgeCount; ++e)
        {
            if (g.instances[g.edges[e].fromInstance].widgetIndex != w)
            {
                continue;
            }
            if (e != cursor)
            {
                const Edge tmp = g.edges[cursor];
                g.edges[cursor] = g.edges[e];
                g.edges[e] = tmp;
            }
            ++cursor;
        }
        slot.outgoingEdgeCount = cursor - slot.outgoingEdgeStart;
    }
}

// Resolves widgetIndex's WAV file at most once per Graph build (see
// WaveSampleCache), then copies the resulting pointer/metadata into this one
// voice instance. Called once per voice instance (up to SYNTH_VOICE_COUNT
// times for the same widget) from the per-instance init loop below, but
// wavloader::load() only ever runs on the first call for a given
// widgetIndex - later calls just reuse the cached result (success or
// failure). The decoded buffer itself is owned by WavLoader's own
// process-wide cache (see WavLoader.h), not this Graph - it can outlive
// this Graph and be reused by the next one if the widget's sampleFile is
// unchanged.
inline void initWavePlayer(Graph &g, uint16_t widgetIndex, NodeInstance &inst, const WidgetParams &p)
{
    inst.wavePlayer.rootNoteMidi = p.rootNoteMidi;
    inst.wavePlayer.position = 0.0f;
    inst.wavePlayer.playing = false;
    inst.wavePlayer.gatePrev = false;

    WaveSampleCache &cache = g.waveSampleCache[widgetIndex];
    if (!cache.attempted)
    {
        cache.attempted = true;
        const int16_t *data = nullptr;
        uint32_t frameCount = 0;
        uint8_t channels = 0;
        if (wavloader::load(p.sampleFile, data, frameCount, channels))
        {
            cache.data = data;
            cache.frameCount = frameCount;
            cache.channels = channels;
        }
    }
    inst.wavePlayer.sampleData = cache.data;
    inst.wavePlayer.frameCount = cache.frameCount;
    inst.wavePlayer.channels = cache.channels;
}

inline void initSoundFontPlayer(Graph &g, uint16_t widgetIndex, NodeInstance &inst, const WidgetParams &p, bool firstInstance)
{
    SoundFontCache &cache = g.soundFontCache[widgetIndex];
    if (!cache.attempted)
    {
        cache.attempted = true;
        soundfont::LoadConfig cfg = {p.soundFontVolume, p.soundFontPath, p.soundFontBank,
                                     p.soundFontProgram, p.soundFontMaxVoices};
        cache.source = soundfont::load(cfg, cache.presetIndex);
    }
    inst.soundFontPlayer.presetIndex = cache.presetIndex;
    inst.soundFontPlayer.synth = firstInstance ? cache.source : soundfont::copy(cache.source, p.soundFontMaxVoices);
    inst.soundFontPlayer.bank = p.soundFontBank;
    inst.soundFontPlayer.program = p.soundFontProgram;
    inst.soundFontPlayer.enableCc = p.soundFontEnableCc;
    inst.soundFontPlayer.enableModWheel = p.soundFontEnableModWheel;
    inst.soundFontPlayer.enablePitchWheel = p.soundFontEnablePitchWheel;
    inst.soundFontPlayer.currentNote = 60;
    inst.soundFontPlayer.gatePrev = false;
    inst.soundFontPlayer.pendingNoteOn = false;
    inst.soundFontPlayer.gainCv = p.gain;
    // Same cutoff/reason as Oscillator's gainSmooth - see NodeState.h.
    inst.soundFontPlayer.gainSmooth.setCutoff(300.0f, static_cast<float>(synth::kSampleRate));
}

inline void applyParams(NodeInstance &inst, NodeType type, const WidgetParams &p, bool psram, uint16_t &psramFallbacks)
{
    switch (type)
    {
    case NodeType::Oscillator:
        nodes::oscillator::build(inst, p);
        break;
    case NodeType::FmOscillator:
        nodes::fmoscillator::build(inst, p);
        break;
    case NodeType::Hammond:
        nodes::hammond::build(inst, p);
        break;
    case NodeType::Lfo:
        nodes::lfo::build(inst, p);
        break;
    case NodeType::Detune:
        nodes::detune::build(inst, p);
        break;
    case NodeType::FrequencyDivider:
        nodes::arithmetic::buildFrequencyDivider(inst, p);
        break;
    case NodeType::Adsr:
        nodes::adsr::build(inst, p);
        break;
    case NodeType::DxEnvelope:
        nodes::dxenvelope::build(inst, p);
        break;
    case NodeType::StepArpeggiator:
        nodes::steparp::build(inst, p);
        break;
    case NodeType::Filter:
        nodes::filter::build(inst, p);
        break;
    case NodeType::CurtisFilter:
        nodes::curtisfilter::build(inst, p);
        break;
    case NodeType::EspEq:
        initEspEq(*inst.espEq, p.eqFilterType, p.eqFrequency, p.eqQ, p.eqGainDb);
        inst.espEq->bypass = p.eqBypass;
        break;
    case NodeType::EspAlc: initEspEffect(*inst.espEffect, EspEffectKind::Alc, p.espParam, 2); inst.espEffect->bypass = p.espBypass; break;
    case NodeType::EspChannelConvert: initEspEffect(*inst.espEffect, EspEffectKind::ChannelConvert, p.espParam, 2); inst.espEffect->bypass = p.espBypass; break;
    // EspMixer deliberately excluded - a 2-input mixer has no single dry
    // signal to bypass to (see plan); s.bypass stays permanently false.
    case NodeType::EspMixer: initEspEffect(*inst.espEffect, EspEffectKind::Mixer, p.espParam, 4); break;
    case NodeType::EspSonic: initEspEffect(*inst.espEffect, EspEffectKind::Sonic, p.espParam, 2); inst.espEffect->bypass = p.espBypass; break;
    case NodeType::EspDrc: initEspEffect(*inst.espEffect, EspEffectKind::Drc, p.espParam, 6); inst.espEffect->bypass = p.espBypass; break;
    case NodeType::EspMbc: initEspEffect(*inst.espEffect, EspEffectKind::Mbc, p.espParam, 3); inst.espEffect->bypass = p.espBypass; break;
    case NodeType::Gain:
        nodes::gain::build(inst, p);
        break;
    case NodeType::Clock:
        nodes::clock::build(inst, p);
        break;
    case NodeType::SampleHold:
        nodes::samplehold::build(inst, p);
        break;
    case NodeType::Mixer:
        nodes::mixer::build(inst, p);
        break;
    case NodeType::Multiplexer:
        nodes::multiplexer::build(inst, p);
        break;
    case NodeType::OutputMultiplexer:
        nodes::outputmultiplexer::build(inst, p);
        break;
    case NodeType::Bitcrusher:
        nodes::bitcrusher::build(inst, p);
        break;
    case NodeType::WaveFolder:
        nodes::wavefolder::build(inst, p);
        break;
    case NodeType::Pan:
        nodes::pan::build(inst, p);
        break;
    case NodeType::Chorus:
        nodes::chorus::build(inst, p, psram);
        break;
    case NodeType::Delay:
        nodes::delay::build(inst, p, psram);
        break;
    case NodeType::Reverb:
        nodes::reverb::buildReverb(inst, p, psram);
        break;
    case NodeType::ReverbLight:
        nodes::reverb::buildReverbLight(inst, p, psramFallbacks);
        break;
    case NodeType::AudioOut:
    case NodeType::UsbOut:
    case NodeType::Es8311Out:
    case NodeType::Wm8960Out:
        nodes::output::build(inst, p);
        break;
    case NodeType::AudioInputLink:
        nodes::link::build(inst, p);
        break;
    case NodeType::MidiIn:
        nodes::midiin::build(inst, p);
        break;
    case NodeType::MidiSplit:
        nodes::midisplit::build(inst, p);
        break;
    case NodeType::Const:
        nodes::constant::build(inst, p);
        break;
    case NodeType::Knob:
        nodes::knob::build(inst, p);
        break;
    case NodeType::ValueMapper:
        nodes::valuemapper::build(inst, p);
        break;
    case NodeType::Button:
        nodes::button::buildButton(inst, p);
        break;
    case NodeType::UpDownButton:
        nodes::button::buildUpDownButton(inst, p);
        break;
    case NodeType::Switch:
        nodes::switchnode::build(inst, p);
        break;
    case NodeType::OutputSwitch:
        nodes::outputswitch::build(inst, p);
        break;
    default:
        break;
    }
}

// Does one full build attempt capped at voiceCountCap voices (regardless of
// what the preset's MidiIn widget(s) actually asked for - GraphBuilder::build
// below handles the request-vs-cap difference). Sets fitInternal true only
// on a fully successful build where nothing unexpectedly landed in PSRAM
// (the compact NodeInstance array itself, or any per-instance block buffer/
// big-state payload - see allocateBlockBuffer/allocateNodeState above);
// left at its caller-supplied default everywhere else (hard allocation
// failure, or the silent-empty-graph fallback), which GraphBuilder::build's
// retry loop treats the same as "didn't fit" and retries smaller.
inline Graph *buildWithVoiceCap(const PresetDoc &doc, uint16_t voiceCountCap, bool &fitInternal)
{
    // Release any cached WAV buffer no WavePlayer widget in THIS preset
    // references anymore, before loading anything new - keeps the cache
    // bounded even when many samples are swapped out in one edit (see
    // WavLoader.h's sweepCache()). Sweeping first (rather than after)
    // avoids ever needing cache slots for both the outgoing and incoming set
    // of files at once.
    {
        const char *keepSampleFiles[synth::kMaxWidgets];
        size_t keepCount = 0;
        for (uint16_t i = 0; i < doc.widgetCount && keepCount < synth::kMaxWidgets; ++i)
        {
            if (doc.widgets[i].type == NodeType::WavePlayer)
            {
                keepSampleFiles[keepCount++] = doc.widgets[i].params.sampleFile;
            }
        }
        wavloader::sweepCache(keepSampleFiles, keepCount);
    }

    // Same reasoning/pattern as the WavePlayer sweep above, for SoundFont
    // Player's (volume, relativePath) file cache (see SoundFontPlayer.h's
    // sweepFileCache()) - a preset can now have multiple SoundFontPlayer
    // widgets, possibly sharing the same file.
    {
        soundfont::FileKey keepSoundFonts[synth::kMaxWidgets];
        size_t keepCount = 0;
        for (uint16_t i = 0; i < doc.widgetCount && keepCount < synth::kMaxWidgets; ++i)
        {
            if (doc.widgets[i].type == NodeType::SoundFontPlayer)
            {
                keepSoundFonts[keepCount].volume = doc.widgets[i].params.soundFontVolume;
                keepSoundFonts[keepCount].relativePath = doc.widgets[i].params.soundFontPath;
                ++keepCount;
            }
        }
        soundfont::sweepFileCache(keepSoundFonts, keepCount);
    }

    Graph *g = Graph::allocate();
    const size_t internalFreeBefore = heap_caps_get_free_size(MALLOC_CAP_INTERNAL);
    const size_t internalLargestBefore = heap_caps_get_largest_free_block(MALLOC_CAP_INTERNAL);
    uint16_t outputPsramFallbacks = 0;
    uint32_t blockBufferCount = 0;

    bool voiced[synth::kMaxWidgets] = {};
    classifyVoiced(doc, voiced);
    bool renderWhenIdle[synth::kMaxWidgets] = {};
    classifyIdleRendered(doc, voiced, renderWhenIdle);

    uint16_t voiceCount = 1;
    for (uint16_t i = 0; i < doc.widgetCount; ++i)
    {
        if (doc.widgets[i].type == NodeType::MidiIn)
        {
            const uint16_t configured = requestedMidiVoices(doc.widgets[i]);
            if (configured > voiceCount)
            {
                voiceCount = configured;
            }
        }
    }
    if (voiceCount > voiceCountCap)
    {
        voiceCount = voiceCountCap;
    }

    // Widget slots + instance-count/base planning.
    uint16_t total = 0;
    for (uint16_t i = 0; i < doc.widgetCount; ++i)
    {
        WidgetSlot &w = g->widgets[i];
        snprintf(w.id, sizeof(w.id), "%s", doc.widgets[i].id);
        snprintf(w.label, sizeof(w.label), "%s", doc.widgets[i].label[0] != '\0' ? doc.widgets[i].label : doc.widgets[i].id);
        w.type = doc.widgets[i].type;
        w.voiced = voiced[i];
        w.renderWhenIdle = renderWhenIdle[i];
        w.instanceCount = voiced[i] ? voiceCount : 1;
        w.instanceBase = total;
        total += w.instanceCount;
        if (w.type == NodeType::UsbOut && g->usbOutWidget < 0)
        {
            g->usbOutWidget = static_cast<int16_t>(i);
        }
        else if (w.type == NodeType::Es8311Out && g->es8311OutWidget < 0)
        {
            g->es8311OutWidget = static_cast<int16_t>(i);
        }
        else if (w.type == NodeType::Wm8960Out && g->wm8960OutWidget < 0)
        {
            g->wm8960OutWidget = static_cast<int16_t>(i);
        }
    }
    g->widgetCount = doc.widgetCount;

    // Allocate + initialize every instance. The compact state array and
    // per-block output/port buffers prefer internal RAM (PSRAM for
    // WavePlayer specifically, see allocateBlockBuffer); long effect rings
    // stay in PSRAM via applyParams() below. PSRAM-first for this array was
    // tried too (the single largest internal-RAM win in the engine) but
    // combined with the block-buffer change above it overloaded PSRAM
    // bandwidth/L2-cache enough to cause audible glitches at ~12+ voices
    // with WavePlayer in the graph - reverted pending a less all-or-nothing
    // approach.
    const uint16_t allocCount = total > 0 ? total : 1;
    const size_t instanceBytes = static_cast<size_t>(allocCount) * sizeof(NodeInstance);
    bool instancesInternal = true;
    void *rawInstances = heap_caps_malloc(instanceBytes, MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT);
    if (rawInstances == nullptr)
    {
        instancesInternal = false;
        rawInstances = heap_caps_malloc(instanceBytes, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    }
    if (rawInstances == nullptr)
    {
        // Keep a silent graph rather than crashing on a null placement-new target.
        g->widgetCount = 0;
        g->usbOutWidget = g->es8311OutWidget = g->wm8960OutWidget = -1;
        return g;
    }
    g->instances = static_cast<NodeInstance *>(rawInstances);
    // Per-element placement-new, not array-new: array-new may reserve extra
    // space for a size "cookie" ahead of the returned pointer, which would
    // write outside this exact-sized allocation. Per-element placement-new
    // has no such cookie and matches the buffer 1:1.
    for (uint16_t i = 0; i < allocCount; ++i)
    {
        new (&g->instances[i]) NodeInstance();
    }
    g->instanceCount = total;
    constexpr bool kPsram = true; // delay/chorus/reverb ring buffers always PSRAM-backed
    for (uint16_t i = 0; i < doc.widgetCount; ++i)
    {
        WidgetSlot &w = g->widgets[i];
        for (uint16_t v = 0; v < w.instanceCount; ++v)
        {
            NodeInstance &inst = g->instances[w.instanceBase + v];
            inst.type = w.type;
            inst.constructInlineState();
            inst.widgetIndex = i;
            inst.voiceIndex = static_cast<uint8_t>(v);
            // Voice 0 of this widget - self-pointer for v==0. See
            // NodeInstance.h's `primary` doc comment (Lfo/Gain "Global" mode).
            inst.primary = &g->instances[w.instanceBase];
            inst.audioInPortCount = static_cast<uint8_t>(ports::audioInPortCount(w.type));
            if (w.type == NodeType::Mixer || w.type == NodeType::Multiplier || w.type == NodeType::Adder)
            {
                inst.audioInPortCount = doc.widgets[i].params.inputCount;
            }
            for (int p = 0; p < inst.audioInPortCount; ++p)
            {
                inst.controlValue[p] = ports::audioInDefault(w.type, p);
            }
            // WavePlayer's 3 input ports + stereo output add up to noticeably
            // more per-voice internal-RAM footprint than most node types
            // (~2.5KB/voice) - large enough at high voice counts to starve
            // internal RAM for everything else on the device (observed:
            // unrelated LittleFS/httpd allocations failing and aborting
            // shortly after a build). Its per-sample loop already reads the
            // sample data from PSRAM anyway, so PSRAM-backing its port/output
            // buffers too costs no new kind of latency, unlike most types.
            const bool preferPsram = (w.type == NodeType::WavePlayer || w.type == NodeType::SoundFontPlayer);
            inst.outL = allocateBlockBuffer(preferPsram, outputPsramFallbacks);
            if (inst.outL != nullptr) ++blockBufferCount;
            if (hasStereoOutput(w.type))
            {
                inst.outR = allocateBlockBuffer(preferPsram, outputPsramFallbacks);
                if (inst.outR != nullptr) ++blockBufferCount;
            }
            const bool linkBuses = w.type == NodeType::AudioOutputLink || w.type == NodeType::AudioInputLink;
            if (inst.outL == nullptr || (hasStereoOutput(w.type) && inst.outR == nullptr))
            {
                ESP_LOGE(kGraphBuilderTag, "Output buffer allocation failed");
                g->destroy();
                g->widgetCount = 0;
                g->usbOutWidget = g->es8311OutWidget = g->wm8960OutWidget = -1;
                return g;
            }
            // Big-state payload (see NodeInstance.h) - only the one field
            // matching this instance's actual type gets allocated; applyParams
            // below (Chorus/Delay/Reverb/ReverbLight/Esp* cases) dereferences
            // it, so this must happen first.
            bool stateAllocFailed = false;
            switch (w.type)
            {
            case NodeType::Chorus:
                inst.chorus = allocateNodeState<ChorusState>(outputPsramFallbacks);
                stateAllocFailed = (inst.chorus == nullptr);
                break;
            case NodeType::Delay:
                inst.delay = allocateNodeState<DelayState>(outputPsramFallbacks);
                stateAllocFailed = (inst.delay == nullptr);
                break;
            case NodeType::Reverb:
                inst.reverb = allocateNodeState<ReverbState>(outputPsramFallbacks);
                stateAllocFailed = (inst.reverb == nullptr);
                break;
            case NodeType::ReverbLight:
                inst.reverbLight = allocateNodeState<ReverbLightState>(outputPsramFallbacks);
                stateAllocFailed = (inst.reverbLight == nullptr);
                break;
            case NodeType::Hammond:
                inst.hammond = allocateNodeState<HammondState>(outputPsramFallbacks);
                stateAllocFailed = (inst.hammond == nullptr);
                break;
            case NodeType::EspAlc:
            case NodeType::EspChannelConvert:
            case NodeType::EspMixer:
            case NodeType::EspSonic:
            case NodeType::EspDrc:
            case NodeType::EspMbc:
                inst.espEffect = allocateNodeState<EspEffectState>(outputPsramFallbacks);
                stateAllocFailed = (inst.espEffect == nullptr);
                break;
            case NodeType::Adsr:
                inst.adsr = allocateNodeState<AdsrState>(outputPsramFallbacks);
                stateAllocFailed = (inst.adsr == nullptr);
                break;
            case NodeType::DxEnvelope:
                inst.dxEnvelope = allocateNodeState<DxEnvelopeState>(outputPsramFallbacks);
                stateAllocFailed = (inst.dxEnvelope == nullptr);
                break;
            case NodeType::StepArpeggiator:
                inst.stepArp = allocateNodeState<StepArpState>(outputPsramFallbacks);
                stateAllocFailed = (inst.stepArp == nullptr);
                break;
            case NodeType::Filter:
                inst.filter = allocateNodeState<FilterState>(outputPsramFallbacks);
                stateAllocFailed = (inst.filter == nullptr);
                break;
            case NodeType::CurtisFilter:
                inst.curtisFilter = allocateNodeState<CurtisFilterState>(outputPsramFallbacks);
                stateAllocFailed = (inst.curtisFilter == nullptr);
                break;
            case NodeType::EspEq:
                inst.espEq = allocateNodeState<EspEqState>(outputPsramFallbacks);
                stateAllocFailed = (inst.espEq == nullptr);
                break;
            case NodeType::Knob:
                inst.knob = allocateNodeState<KnobState>(outputPsramFallbacks);
                stateAllocFailed = (inst.knob == nullptr);
                break;
            case NodeType::Button:
                inst.button = allocateNodeState<ButtonState>(outputPsramFallbacks);
                stateAllocFailed = (inst.button == nullptr);
                break;
            case NodeType::UpDownButton:
                inst.upDownButton = allocateNodeState<UpDownButtonState>(outputPsramFallbacks);
                stateAllocFailed = (inst.upDownButton == nullptr);
                break;
            case NodeType::ValueMapper:
                inst.valueMapper = allocateNodeState<ValueMapperState>(outputPsramFallbacks);
                stateAllocFailed = (inst.valueMapper == nullptr);
                break;
            case NodeType::AudioOutputLink:
            case NodeType::AudioInputLink:
                inst.link = allocateNodeState<LinkState>(outputPsramFallbacks);
                stateAllocFailed = (inst.link == nullptr);
                break;
            case NodeType::OutputMultiplexer:
                inst.outputMux = allocateNodeState<OutputMultiplexerState>(outputPsramFallbacks);
                stateAllocFailed = (inst.outputMux == nullptr);
                break;
            default:
                break;
            }
            if (stateAllocFailed)
            {
                ESP_LOGE(kGraphBuilderTag, "Node-State-Allokation fehlgeschlagen");
                g->destroy();
                g->widgetCount = 0;
                g->usbOutWidget = g->es8311OutWidget = g->wm8960OutWidget = -1;
                return g;
            }
            if (linkBuses)
            {
                bool linkBusesAllocated = true;
                for (float *&bus : inst.link->busOut)
                {
                    bus = allocateBlockBuffer(preferPsram, outputPsramFallbacks);
                    if (bus != nullptr)
                    {
                        ++blockBufferCount;
                    }
                    else
                    {
                        linkBusesAllocated = false;
                    }
                }
                if (!linkBusesAllocated)
                {
                    ESP_LOGE(kGraphBuilderTag, "Link bus buffer allocation failed");
                    g->destroy();
                    g->widgetCount = 0;
                    g->usbOutWidget = g->es8311OutWidget = g->wm8960OutWidget = -1;
                    return g;
                }
            }
            if (w.type == NodeType::OutputMultiplexer)
            {
                // Ports 0/1 are outL/outR (allocated above via
                // hasStereoOutput); only ports 2..outputMuxCount-1 need their
                // own buffer here, mirroring the linkBuses block above but
                // sized from this instance's configured output count instead
                // of a fixed constant (see OutputMultiplexerState::outPorts
                // in NodeState.h).
                const uint8_t count = doc.widgets[i].params.outputMuxCount < 2
                                          ? 2
                                          : (doc.widgets[i].params.outputMuxCount > 16 ? 16 : doc.widgets[i].params.outputMuxCount);
                bool outputMuxBuffersAllocated = true;
                for (int e = 0; e < count - 2; ++e)
                {
                    inst.outputMux->outPorts[e] = allocateBlockBuffer(preferPsram, outputPsramFallbacks);
                    if (inst.outputMux->outPorts[e] != nullptr)
                    {
                        ++blockBufferCount;
                    }
                    else
                    {
                        outputMuxBuffersAllocated = false;
                    }
                }
                if (!outputMuxBuffersAllocated)
                {
                    ESP_LOGE(kGraphBuilderTag, "OutputMultiplexer buffer allocation failed");
                    g->destroy();
                    g->widgetCount = 0;
                    g->usbOutWidget = g->es8311OutWidget = g->wm8960OutWidget = -1;
                    return g;
                }
            }
            applyParams(inst, w.type, doc.widgets[i].params, kPsram, outputPsramFallbacks);
            if (w.type == NodeType::MidiIn && doc.widgets[i].isFxPage)
            {
                inst.midiIn.voices = 1;
                inst.midiIn.overflowMode = OverflowMode::Ignore;
            }
            if (w.type == NodeType::WavePlayer)
            {
                initWavePlayer(*g, i, inst, doc.widgets[i].params);
            }
            else if (w.type == NodeType::SoundFontPlayer)
            {
                initSoundFontPlayer(*g, i, inst, doc.widgets[i].params, v == 0);
            }
        }
        if (w.type == NodeType::MidiIn)
        {
            const uint16_t configured = requestedMidiVoices(doc.widgets[i]);
            const uint8_t allocatorVoices = static_cast<uint8_t>(configured < voiceCount ? configured : voiceCount);
            const OverflowMode overflowMode =
                doc.widgets[i].isFxPage ? OverflowMode::Ignore : doc.widgets[i].params.overflowMode;
            g->voiceAllocators[i].configure(allocatorVoices, overflowMode);
        }
    }

    // Implicit output-link -> input-link bridge (first of each type found).
    int16_t outputLinkIdx = -1;
    int16_t inputLinkIdx = -1;
    for (uint16_t i = 0; i < doc.widgetCount; ++i)
    {
        if (g->widgets[i].type == NodeType::AudioOutputLink && outputLinkIdx < 0)
        {
            outputLinkIdx = static_cast<int16_t>(i);
        }
        if (g->widgets[i].type == NodeType::AudioInputLink && inputLinkIdx < 0)
        {
            inputLinkIdx = static_cast<int16_t>(i);
        }
    }

    // Widget-level dependency graph for the topological render-order sort,
    // built alongside instance-level edge wiring.
    const size_t n = doc.widgetCount;
    bool *adjacency = new bool[n * n](); // zero-initialized
    uint8_t *inDegree = new uint8_t[n](); // zero-initialized

    auto markDependency = [&](uint16_t from, uint16_t to)
    {
        if (!adjacency[static_cast<size_t>(from) * n + to])
        {
            adjacency[static_cast<size_t>(from) * n + to] = true;
            ++inDegree[to];
        }
    };

    for (uint16_t c = 0; c < doc.connectionCount; ++c)
    {
        const ConnectionDesc &cd = doc.connections[c];
        const int fromIdx = findWidgetIndex(doc, cd.fromWidget);
        const int toIdx = findWidgetIndex(doc, cd.toWidget);
        if (fromIdx < 0 || toIdx < 0)
        {
            continue;
        }
        const NodeType fromType = doc.widgets[fromIdx].type;
        const NodeType toType = doc.widgets[toIdx].type;

        const int fromPort = ports::audioOutIndex(fromType, cd.fromPort);
        const int toPort = ports::audioInIndex(toType, cd.toPort);

        // Any source may feed any port that has a real audioInIndex/
        // audioOutIndex on both sides - audio and CV ports are treated
        // equally. ports::isControlOnlyInput remains, but now only controls
        // whether NodeRender.h reads the port per sample or once per block
        // (see there) - no more connection restriction.
        if (fromPort >= 0 && toPort >= 0)
        {
            wireConnection(*g, static_cast<uint16_t>(fromIdx), fromPort, static_cast<uint16_t>(toIdx), toPort);
            markDependency(static_cast<uint16_t>(fromIdx), static_cast<uint16_t>(toIdx));

            const WidgetSlot &targetSlot = g->widgets[toIdx];
            for (uint16_t v = 0; v < targetSlot.instanceCount; ++v)
            {
                g->instances[targetSlot.instanceBase + v].audioConnectedPortMask |=
                    synth::portBit(toPort);
            }
            continue;
        }

        // Narrow exception: midi-in's frequency/gate/velocity "virtual"
        // control outputs aren't audio-rate ports, but this preset
        // connection is still how they reach their target - resolve into a
        // ControlRoute instead of a DSP edge. No render-order dependency is
        // needed since midi-in isn't a rendered node.
        if (fromType == NodeType::MidiIn && g->controlRouteCount < synth::kMaxConnections)
        {
            ControlRoute::Kind kind;
            bool known = true;
            if (strcmp(cd.fromPort, "pitchCv") == 0)
                kind = ControlRoute::Kind::MidiPitchCv;
            else if (strcmp(cd.fromPort, "note") == 0)
                kind = ControlRoute::Kind::MidiNote;
            else if (strcmp(cd.fromPort, "gate") == 0)
                kind = ControlRoute::Kind::MidiGate;
            else if (strcmp(cd.fromPort, "velocity") == 0)
                kind = ControlRoute::Kind::MidiVelocity;
            else if (strcmp(cd.fromPort, "aftertouch") == 0)
                kind = ControlRoute::Kind::MidiAftertouch;
            else if (strcmp(cd.fromPort, "pitchBend") == 0)
                kind = ControlRoute::Kind::MidiPitchBend;
            else if (strcmp(cd.fromPort, "midi") == 0 && toType == NodeType::SoundFontPlayer && strcmp(cd.toPort, "midi") == 0)
                kind = ControlRoute::Kind::MidiRaw;
            // MidiSplit's single "midi" input - one wire standing in for what
            // used to be three (note/velocity/gate): the note-on/off event
            // itself is all MidiSplit's own dispatch needs (see
            // deliverNoteControlRoutes's MidiSplit case), so this resolves to
            // the exact same MidiGate delivery a "gate"-only wire always did.
            else if (strcmp(cd.fromPort, "midi") == 0 && toType == NodeType::MidiSplit && strcmp(cd.toPort, "midi") == 0)
                kind = ControlRoute::Kind::MidiGate;
            else
                known = false;

            if (known)
            {
                ControlRoute &route = g->controlRoutes[g->controlRouteCount++];
                route.sourceWidget = static_cast<uint16_t>(fromIdx);
                route.targetWidget = static_cast<uint16_t>(toIdx);
                route.kind = kind;
                if (toPort >= 0)
                {
                    route.targetPort = static_cast<int8_t>(toPort);
                    const WidgetSlot &target = g->widgets[toIdx];
                    for (uint16_t v = 0; v < target.instanceCount; ++v)
                    {
                        g->instances[target.instanceBase + v].controlConnectedPortMask |=
                            synth::portBit(toPort);
                    }
                }
            }
            continue;
        }

        // Narrow exception, midi-split's counterpart: up to 4*4=16 "virtual"
        // control outputs ("zone1Frequency".."zone4Gate", parsed by
        // parseZonePort() below), reusing the same 4 ControlRoute::Kind
        // values midi-in already uses above - zones only differ by the
        // route.side tag (0-3, the 0-based zone index), consumed by
        // Graph::deliverNoteControlRoutes's MidiSplit case to forward only
        // the matching zone's routes for a given note. No render-order
        // dependency needed, same reasoning as midi-in.
        if (fromType == NodeType::MidiSplit && g->controlRouteCount < synth::kMaxConnections)
        {
            int zoneIdx = 0;
            ControlRoute::Kind kind;
            if (parseZonePort(cd.fromPort, zoneIdx, kind))
            {
                ControlRoute &route = g->controlRoutes[g->controlRouteCount++];
                route.sourceWidget = static_cast<uint16_t>(fromIdx);
                route.targetWidget = static_cast<uint16_t>(toIdx);
                route.kind = kind;
                route.side = static_cast<uint8_t>(zoneIdx);
                if (toPort >= 0)
                {
                    route.targetPort = static_cast<int8_t>(toPort);
                    const WidgetSlot &target = g->widgets[toIdx];
                    for (uint16_t v = 0; v < target.instanceCount; ++v)
                    {
                        g->instances[target.instanceBase + v].controlConnectedPortMask |=
                            synth::portBit(toPort);
                    }
                }
            }
            continue;
        }

        // Knob's mapped-CC output: same idea, but the target param name is
        // whatever the preset's toPort already says (e.g. "cutoff", "rate",
        // "gain3") rather than one of midi-in's three fixed kinds - reused
        // directly as ControlRoute::paramName, matched against the same
        // field names setFloatFieldByName() (ParamDispatch.h) recognizes.
        if (fromType == NodeType::Knob && g->controlRouteCount < synth::kMaxConnections)
        {
            int knobIndex = 0;
            if (strncmp(cd.fromPort, "voltage", 7) == 0 && cd.fromPort[7] != '\0')
            {
                knobIndex = atoi(cd.fromPort + 7) - 1;
            }
            if (knobIndex < 0 || knobIndex >= static_cast<int>(synth::kKnobControlMax))
            {
                continue;
            }
            const KnobState *sourceKnob = g->instances[g->widgets[fromIdx].instanceBase].knob;
            if (sourceKnob == nullptr || knobIndex >= static_cast<int>(sourceKnob->count))
            {
                continue;
            }
            ControlRoute &route = g->controlRoutes[g->controlRouteCount++];
            route.sourceWidget = static_cast<uint16_t>(fromIdx);
            route.targetWidget = static_cast<uint16_t>(toIdx);
            // "cc" = raw 0-127 dial/MIDI-CC value; "voltage" (and any legacy
            // preset still using the old single "value" port name) = the
            // 2-point-mapped 0-10V output - see Graph.h's ControlRoute::Kind.
            route.kind = (strcmp(cd.fromPort, "cc") == 0) ? ControlRoute::Kind::KnobCc : ControlRoute::Kind::Knob;
            route.side = static_cast<uint8_t>(knobIndex);
            snprintf(route.paramName, sizeof(route.paramName), "%s", cd.toPort);
            if (toPort >= 0)
            {
                route.targetPort = static_cast<int8_t>(toPort);
                const WidgetSlot &target = g->widgets[toIdx];
                for (uint16_t v = 0; v < target.instanceCount; ++v)
                {
                    g->instances[target.instanceBase + v].controlConnectedPortMask |=
                        synth::portBit(toPort);
                }
            }
            continue;
        }

        // Button's mapped-CC output: same arbitrary-named-param mechanism as
        // Knob above.
        if (fromType == NodeType::Button && g->controlRouteCount < synth::kMaxConnections)
        {
            ControlRoute &route = g->controlRoutes[g->controlRouteCount++];
            route.sourceWidget = static_cast<uint16_t>(fromIdx);
            route.targetWidget = static_cast<uint16_t>(toIdx);
            route.kind = ControlRoute::Kind::Button;
            snprintf(route.paramName, sizeof(route.paramName), "%s", cd.toPort);
            if (toPort >= 0)
            {
                route.targetPort = static_cast<int8_t>(toPort);
                const WidgetSlot &target = g->widgets[toIdx];
                for (uint16_t v = 0; v < target.instanceCount; ++v)
                {
                    g->instances[target.instanceBase + v].controlConnectedPortMask |=
                        synth::portBit(toPort);
                }
            }
            continue;
        }

        // Up/Down Button's mapped-CC output: same arbitrary-named-param
        // mechanism as Knob/Button above - one route kind regardless of
        // whether the up or down button triggered the change (see
        // Graph::handleMidiEvent's CC branch).
        if (fromType == NodeType::UpDownButton && g->controlRouteCount < synth::kMaxConnections)
        {
            ControlRoute &route = g->controlRoutes[g->controlRouteCount++];
            route.sourceWidget = static_cast<uint16_t>(fromIdx);
            route.targetWidget = static_cast<uint16_t>(toIdx);
            route.kind = ControlRoute::Kind::UpDownButton;
            snprintf(route.paramName, sizeof(route.paramName), "%s", cd.toPort);
            if (toPort >= 0)
            {
                route.targetPort = static_cast<int8_t>(toPort);
                const WidgetSlot &target = g->widgets[toIdx];
                for (uint16_t v = 0; v < target.instanceCount; ++v)
                {
                    g->instances[target.instanceBase + v].controlConnectedPortMask |=
                        synth::portBit(toPort);
                }
            }
            continue;
        }

        // Const's scalar Value output is a control route. It deliberately has
        // no audio-rate output port; targets receive the numeric value in
        // their named parameter and, when applicable, controlValue buffer.
        if (fromType == NodeType::Const &&
            (strcmp(cd.fromPort, "value") == 0 || strcmp(cd.fromPort, "audio") == 0) &&
            g->controlRouteCount < synth::kMaxConnections)
        {
            ControlRoute &route = g->controlRoutes[g->controlRouteCount++];
            route.sourceWidget = static_cast<uint16_t>(fromIdx);
            route.targetWidget = static_cast<uint16_t>(toIdx);
            route.kind = ControlRoute::Kind::Const;
            snprintf(route.paramName, sizeof(route.paramName), "%s", cd.toPort);
            if (toPort >= 0)
            {
                route.targetPort = static_cast<int8_t>(toPort);
                const WidgetSlot &target = g->widgets[toIdx];
                for (uint16_t v = 0; v < target.instanceCount; ++v)
                {
                    g->instances[target.instanceBase + v].controlConnectedPortMask |=
                        synth::portBit(toPort);
                }
            }
            continue;
        }
        // Otherwise: not a recognized audio, midi-in-control, or knob-
        // control connection - silently skipped (preset connections are
        // already audio-rate-filtered by the frontend before saving, per
        // project scope).
    }

    // Seed controlValue for Knob/Button routes from the preset's own initial
    // raw/list state (already computed into knob.currentValue/button.
    // currentValue by applyParams() above) - without this, a Multiplier/
    // Adder/Divide input fed by a Knob would read as 0 until the first CC
    // message for that knob actually arrives. MidiFrequency/Gate/Velocity
    // routes need no such seeding: there is no note playing yet at build
    // time, so 0 (silence/gate-off) is already the correct initial value.
    for (uint16_t r = 0; r < g->controlRouteCount; ++r)
    {
        const ControlRoute &route = g->controlRoutes[r];
        const WidgetSlot &targetWidget = g->widgets[route.targetWidget];
        float initial = 0.0f;
        if (route.kind == ControlRoute::Kind::Knob)
        {
            // route.side is the knob's control index (see GraphBuilder's
            // knob-connection branch above) - controls[0] here would seed
            // every non-first knob's route from the wrong control, leaving
            // its target stuck on knob 1's value until that specific knob is
            // next turned live (Graph::applyLiveParams does use route.side
            // correctly - only this initial build-time seed didn't).
            initial = g->instances[g->widgets[route.sourceWidget].instanceBase].knob->controls[route.side].currentValue;
        }
        else if (route.kind == ControlRoute::Kind::KnobCc)
        {
            initial = g->instances[g->widgets[route.sourceWidget].instanceBase].knob->controls[route.side].raw;
        }
        else if (route.kind == ControlRoute::Kind::Button)
        {
            initial = g->instances[g->widgets[route.sourceWidget].instanceBase].button->currentValue;
        }
        else if (route.kind == ControlRoute::Kind::UpDownButton)
        {
            initial = g->instances[g->widgets[route.sourceWidget].instanceBase].upDownButton->currentOutput();
        }
        else if (route.kind == ControlRoute::Kind::Const)
        {
            initial = g->instances[g->widgets[route.sourceWidget].instanceBase].constant.value;
        }
        else if (route.kind == ControlRoute::Kind::MidiPitchBend)
        {
            // "no bend" = 5V (matches pitchCv's own 5V-at-note-60 center).
            // Without this a target reads fully-bent-down (0V) until the
            // first real pitch-bend message arrives.
            initial = cv::kMaxVolts / 2.0f;
        }
        else
        {
            // MidiFrequency/Gate/Velocity/Aftertouch etc. - no note is
            // playing yet at build time, so 0 (silence/gate-off) is already
            // correct.
            continue;
        }
        for (uint16_t v = 0; v < targetWidget.instanceCount; ++v)
        {
            NodeInstance &target = g->instances[targetWidget.instanceBase + v];
            float targetValue = initial;
            if (target.type == NodeType::Lfo && strcmp(route.paramName, "rate") == 0 &&
                route.kind != ControlRoute::Kind::KnobCc)
            {
                targetValue = std::fmax(0.0f, std::fmin(cv::kMaxVolts, targetValue)) *
                              target.lfo.maxRateHz / cv::kMaxVolts;
            }
            // Call for every route, not just Const: a route whose target has
            // no port index (e.g. Knob -> Adsr's "velocity", a named field
            // with no audioInIndex - see PortTables.h) previously got no
            // build-time seed at all and just sat at its struct default
            // until the next live drag happened to land on the graph
            // generation that already has this route - if a reconnect and a
            // drag race (reconnect queues an async graph rebuild, see
            // SynthEngine.h's request_current_preset/loaderTaskLoop; a drag
            // right after can still apply to the old, about-to-be-replaced
            // graph), the drag's value is lost and the field never updates.
            // Mirrors pushControlRouteValue's same unconditional call
            // (Graph.h) for the live-update path - harmless no-op here when
            // the target type doesn't recognize this field name.
            setFloatFieldByName(target, target.type, route.paramName, targetValue);
            if (route.targetPort >= 0)
            {
                target.controlValue[route.targetPort] = targetValue;
            }
        }
    }

    if (outputLinkIdx >= 0 && inputLinkIdx >= 0)
    {
        // Only wire a bus that actually carries something from the Sound
        // Generation page (plan section 3, P2: "Nur verwendete Link-Busse
        // anlegen und transportieren" - the builder-side half; the render-
        // side half, skipping AudioInputLink's gain pass for a silent bus,
        // is already in LinkNodes.h). AudioOutputLink's own
        // audioConnectedPortMask already reflects the preset's real
        // connections into it at this point (set by the general per-
        // connection loop above, well before this Link-pairing code runs),
        // so it's safe to read here as "is this bus genuinely fed". A bus
        // with no source just stays unconnected on the AudioInputLink side
        // too, exactly like any other never-wired audio input port
        // elsewhere in this engine - Graph::renderBlock fills it from
        // Link's own audioInDefault() (0.0f) instead of paying for an
        // edge + accumulator + per-sample add that would only ever produce
        // silence anyway.
        const NodeInstance &outputLinkInst = g->instances[g->widgets[outputLinkIdx].instanceBase];
        for (uint8_t bus = 0; bus < synth::kLinkBusCount; ++bus)
        {
            if ((outputLinkInst.audioConnectedPortMask & synth::portBit(bus)) == 0)
            {
                continue;
            }
            addEdge(*g, g->widgets[outputLinkIdx].instanceBase, bus, g->widgets[inputLinkIdx].instanceBase, bus, 1.0f);
            g->instances[g->widgets[inputLinkIdx].instanceBase].audioConnectedPortMask |= synth::portBit(bus);
        }
        markDependency(static_cast<uint16_t>(outputLinkIdx), static_cast<uint16_t>(inputLinkIdx));
    }

    // Linked-IO resolution: freely-placeable linked-input/linked-output
    // widgets, keyed by linkId rather than a fixed bus index - unrelated to
    // the fixed 8-bus bridge above. First-linkId-wins, deterministic (preset
    // widget order); duplicate linkId or a hidden cycle -> warn and skip,
    // never crash or silently misconnect. Must run before the input-port
    // allocation loop below (it needs audioConnectedPortMask already set)
    // and while adjacency/markDependency are still alive.
    {
        const char *liLinkId[synth::kMaxWidgets];
        uint16_t liWidgetIdx[synth::kMaxWidgets];
        size_t liCount = 0;
        for (uint16_t i = 0; i < doc.widgetCount; ++i)
        {
            if (doc.widgets[i].type != NodeType::LinkedInput)
            {
                continue;
            }
            const char *linkId = doc.widgets[i].params.linkId;
            if (linkId[0] == '\0')
            {
                continue;
            }
            bool duplicate = false;
            for (size_t k = 0; k < liCount; ++k)
            {
                if (strcmp(liLinkId[k], linkId) == 0)
                {
                    duplicate = true;
                    break;
                }
            }
            if (duplicate)
            {
                ESP_LOGW(kGraphBuilderTag,
                         "Duplicate linked-input linkId '%s' on widget '%s' - keeping first definition", linkId,
                         doc.widgets[i].id);
                continue;
            }
            liLinkId[liCount] = linkId;
            liWidgetIdx[liCount] = i;
            ++liCount;
        }

        // Can `from` already reach `to` via the widget-dependency adjacency
        // matrix built above? Used to reject a hidden linked-io edge that
        // would otherwise close a cycle, regardless of preset widget order.
        auto canReach = [&](uint16_t from, uint16_t to)
        {
            bool visited[synth::kMaxWidgets] = {};
            uint16_t queue[synth::kMaxWidgets];
            uint16_t qh = 0, qt = 0;
            visited[from] = true;
            queue[qt++] = from;
            while (qh < qt)
            {
                const uint16_t cur = queue[qh++];
                if (cur == to)
                {
                    return true;
                }
                for (uint16_t j = 0; j < n; ++j)
                {
                    if (adjacency[static_cast<size_t>(cur) * n + j] && !visited[j])
                    {
                        visited[j] = true;
                        queue[qt++] = j;
                    }
                }
            }
            return false;
        };

        for (uint16_t i = 0; i < doc.widgetCount; ++i)
        {
            if (doc.widgets[i].type != NodeType::LinkedOutput)
            {
                continue;
            }
            const char *sourceLinkId = doc.widgets[i].params.sourceLinkId;
            if (sourceLinkId[0] == '\0')
            {
                continue; // not connected - silence, no warning (by design)
            }
            int liIdx = -1;
            for (size_t k = 0; k < liCount; ++k)
            {
                if (strcmp(liLinkId[k], sourceLinkId) == 0)
                {
                    liIdx = static_cast<int>(liWidgetIdx[k]);
                    break;
                }
            }
            if (liIdx < 0)
            {
                continue; // unresolved linkId - silence, no warning (by design)
            }
            if (canReach(i, static_cast<uint16_t>(liIdx)))
            {
                ESP_LOGW(kGraphBuilderTag,
                         "Linked-input '%s' -> linked-output widget '%s' would create a hidden cycle - rejected",
                         sourceLinkId, doc.widgets[i].id);
                continue;
            }
            wireConnection(*g, static_cast<uint16_t>(liIdx), 0, i, 0);
            markDependency(static_cast<uint16_t>(liIdx), i);
            const WidgetSlot &loSlot = g->widgets[i];
            for (uint16_t v = 0; v < loSlot.instanceCount; ++v)
            {
                g->instances[loSlot.instanceBase + v].audioConnectedPortMask |= synth::portBit(0);
            }
        }
    }

    // Allocate only ports that receive sample-rate data. Scalar defaults and
    // ControlRoutes are read directly from controlValue by portSample(). ESP
    // effect adapters keep full buffers because their APIs consume raw block
    // pointers rather than portSample().
    for (uint16_t i = 0; i < g->instanceCount; ++i)
    {
        NodeInstance &inst = g->instances[i];
        const bool preferPsram = inst.type == NodeType::WavePlayer || inst.type == NodeType::SoundFontPlayer;
        const bool requireAllInputs = requiresInputBlockBuffers(inst.type);
        for (int p = 0; p < inst.audioInPortCount; ++p)
        {
            const bool audioConnected =
                (inst.audioConnectedPortMask & synth::portBit(p)) != 0;
            // Polyphonic MidiIn pitch is always a block-constant scalar:
            // deliverNoteControlRoutes disables glide whenever voices != 1,
            // and portSample() already falls back to controlValue when no
            // buffer exists. Keep a real sample buffer only for monophonic
            // MidiIn routes, where glide can be enabled live. MidiSplit is
            // kept conservative because its upstream MidiIn is indirect.
            bool midiPitchNeedsBuffer = false;
            for (uint16_t r = 0; r < g->controlRouteCount; ++r)
            {
                const ControlRoute &route = g->controlRoutes[r];
                if (route.kind == ControlRoute::Kind::MidiPitchCv && route.targetWidget == inst.widgetIndex &&
                    route.targetPort == p)
                {
                    const WidgetSlot &source = g->widgets[route.sourceWidget];
                    midiPitchNeedsBuffer = source.type != NodeType::MidiIn ||
                        g->instances[source.instanceBase].midiIn.voices == 1;
                    break;
                }
            }
            if (!audioConnected && !requireAllInputs && !midiPitchNeedsBuffer)
            {
                continue;
            }
            inst.inPorts[p] = allocateBlockBuffer(preferPsram, outputPsramFallbacks);
            if (inst.inPorts[p] == nullptr)
            {
                ESP_LOGE(kGraphBuilderTag, "Input buffer allocation failed");
                g->destroy();
                g->widgetCount = 0;
                g->usbOutWidget = g->es8311OutWidget = g->wm8960OutWidget = -1;
                delete[] adjacency;
                delete[] inDegree;
                return g;
            }
            ++blockBufferCount;
        }
    }

    groupEdgesBySourceWidget(*g);

    // Kahn's algorithm over widgets. Any widget left over (a genuine cycle
    // not routed through an opaque stateful node like delay/reverb) is
    // appended in original order as a defensive fallback rather than left
    // out of rendering entirely.
    uint16_t *queue = new uint16_t[n > 0 ? n : 1];
    uint16_t qHead = 0, qTail = 0;
    for (uint16_t i = 0; i < doc.widgetCount; ++i)
    {
        if (inDegree[i] == 0)
        {
            queue[qTail++] = i;
        }
    }
    while (qHead < qTail)
    {
        const uint16_t cur = queue[qHead++];
        g->renderOrder[g->renderOrderCount++] = cur;
        for (uint16_t j = 0; j < doc.widgetCount; ++j)
        {
            if (adjacency[static_cast<size_t>(cur) * n + j])
            {
                if (--inDegree[j] == 0)
                {
                    queue[qTail++] = j;
                }
            }
        }
    }
    // Marks where the properly topologically-sorted prefix of renderOrder
    // ends - any widget appended below (the cyclic-fallback loop) has no
    // guaranteed-safe order relative to its own dependencies. Used by the
    // buffer-view pass further down.
    const uint16_t topoCount = g->renderOrderCount;
    for (uint16_t i = 0; i < doc.widgetCount; ++i)
    {
        bool found = false;
        for (uint16_t r = 0; r < g->renderOrderCount; ++r)
        {
            if (g->renderOrder[r] == i)
            {
                found = true;
                break;
            }
        }
        if (!found)
        {
            g->renderOrder[g->renderOrderCount++] = i;
        }
    }

    delete[] queue;
    delete[] adjacency;
    delete[] inDegree;

    // Counts ports actually turned into a view below - logged alongside the
    // existing build diagnostics so this can be checked against real
    // presets, not just inferred from source (plan section 8's spirit,
    // applied to the builder itself).
    uint32_t viewPortCount = 0;
    // Buffer-view optimization (plan section 3, P2: "Bei nur einer
    // unskalierten Quelle und einem ausschließlich lesenden Ziel auf den
    // Quellpuffer verweisen"): a destination port fed by exactly one
    // unscaled, non-monitor-tap edge whose source is guaranteed to render
    // before it every block (both widgets inside the safe, non-cyclic
    // topoCount prefix above, source strictly before destination) can read
    // the source's own output buffer directly - no separate accumulator,
    // no per-block memset, no per-sample add for that edge. Deliberately a
    // separate, additive pass AFTER the normal allocation/topological-sort
    // above (not woven into it), so this can never perturb that already-
    // correct sequence - worst case it frees a just-allocated buffer and
    // re-points the pointer, a one-time build-time cost, never a per-block
    // one. See NodeInstance.h's viewPortMask and Graph::renderBlock/
    // Graph::destroy() for the read/clear/free-side handling.
    {
        uint16_t widgetRenderPos[synth::kMaxWidgets];
        for (uint16_t r = 0; r < g->renderOrderCount; ++r)
        {
            widgetRenderPos[g->renderOrder[r]] = r;
        }
        // Per-(instance,port) incoming-edge count + the one edge index when
        // exactly one - only a count of exactly one is alias-eligible; a
        // multi-source port keeps its real accumulator. Heap-allocated
        // scratch, freed at the end of this block - instanceCount can be up
        // to kMaxInstances, too large to be a comfortable stack array here.
        const size_t slotCount = static_cast<size_t>(g->instanceCount) * synth::kMaxAudioInPorts;
        uint8_t *targetEdgeCount = new uint8_t[slotCount]();
        uint16_t *targetEdgeIndex = new uint16_t[slotCount];
        for (uint16_t e = 0; e < g->edgeCount; ++e)
        {
            const Edge &edge = g->edges[e];
            const size_t key = static_cast<size_t>(edge.toInstance) * synth::kMaxAudioInPorts + edge.toPort;
            if (targetEdgeCount[key] < 255)
            {
                ++targetEdgeCount[key];
            }
            targetEdgeIndex[key] = e;
        }
        for (uint16_t i = 0; i < g->instanceCount; ++i)
        {
            NodeInstance &dst = g->instances[i];
            for (int p = 0; p < dst.audioInPortCount; ++p)
            {
                if ((dst.audioConnectedPortMask & synth::portBit(p)) == 0)
                {
                    continue;
                }
                const size_t key = static_cast<size_t>(i) * synth::kMaxAudioInPorts + p;
                if (targetEdgeCount[key] != 1)
                {
                    continue; // multi-source accumulator - keep the owned buffer
                }
                const Edge &edge = g->edges[targetEdgeIndex[key]];
                if (edge.scale != 1.0f || edge.monitorTap)
                {
                    continue;
                }
                NodeInstance &src = g->instances[edge.fromInstance];
                const uint16_t srcPos = widgetRenderPos[src.widgetIndex];
                const uint16_t dstPos = widgetRenderPos[dst.widgetIndex];
                if (srcPos >= topoCount || dstPos >= topoCount || srcPos >= dstPos)
                {
                    continue; // cyclic-fallback widget on either end, or not strictly source-before-destination
                }
                float *srcBuf = outputPortBuffer(src, edge.fromPort);
                if (srcBuf == nullptr)
                {
                    continue;
                }
                if (dst.inPorts[p] != nullptr)
                {
                    heap_caps_free(dst.inPorts[p]);
                    --blockBufferCount; // keep the memorydiag/log report below accurate
                }
                dst.inPorts[p] = srcBuf;
                dst.viewPortMask |= synth::portBit(p);
                ++viewPortCount;
            }
        }
        delete[] targetEdgeCount;
        delete[] targetEdgeIndex;
    }

    // A DX Envelope feeding only FM amplitude ports through the buffer-view
    // path need not materialize constant/ramp samples: Graph::renderBlock
    // forwards its metadata and FmOscillatorNode consumes it directly. Keep
    // this deliberately narrow. Audio-rate/gliding pitch or tune currently
    // takes an FM fallback loop that may still read the amplitude buffer.
    for (uint16_t wIdx = 0; wIdx < g->widgetCount; ++wIdx)
    {
        WidgetSlot &sourceWidget = g->widgets[wIdx];
        if (sourceWidget.type != NodeType::DxEnvelope || sourceWidget.outgoingEdgeCount == 0)
        {
            continue;
        }
        bool metadataOnly = true;
        const uint16_t edgeEnd = sourceWidget.outgoingEdgeStart + sourceWidget.outgoingEdgeCount;
        for (uint16_t e = sourceWidget.outgoingEdgeStart; e < edgeEnd; ++e)
        {
            const Edge &edge = g->edges[e];
            const NodeInstance &dst = g->instances[edge.toInstance];
            if (edge.fromPort != 0 || dst.type != NodeType::FmOscillator || edge.toPort != 0 ||
                edge.monitorTap || edge.scale != 1.0f ||
                (dst.viewPortMask & synth::portBit(0)) == 0 ||
                dst.inPorts[1] != nullptr || dst.inPorts[2] != nullptr)
            {
                metadataOnly = false;
                break;
            }
        }
        if (metadataOnly)
        {
            for (uint16_t v = 0; v < sourceWidget.instanceCount; ++v)
            {
                g->instances[sourceWidget.instanceBase + v].outputMetadataOnly = true;
            }
        }
    }

    // Subgraph pruning (plan section 3, P2: "Nur vom Ausgang oder einem
    // aktiven Monitor benötigte Teilgraphen rendern"): a widget no edge-path
    // leads from to any of the three hardware sinks or an active monitor tap
    // produces output nobody ever reads - Graph::renderBlock (see
    // WidgetSlot::needed) skips it, and its own outgoing edges, entirely.
    // Computed as reverse reachability from the sinks/monitor-tap sources
    // over widget-level edges - a per-WIDGET decision (all of a voiced
    // widget's per-voice instances share one bit), matching the granularity
    // Graph::renderBlock's own renderOrder loop already renders at. Safe
    // across feedback cycles: the fixed-point worklist below just marks
    // every widget in a cycle needed together once any one of them is
    // reached, same as any other reverse-reachability walk. Does NOT
    // interact with the Multiplexer/Switch "don't freeze an unselected
    // branch" concern (plan section 3) - an unselected input still has a
    // real edge into the (needed) Multiplexer widget in this STATIC
    // topology, so it stays needed regardless of which branch is currently
    // selected at runtime; this pass only ever prunes a widget with zero
    // path to any output, not a "currently inactive" one. A preset reload
    // always rebuilds the whole Graph from scratch (see SynthEngine's
    // live-reload path), so this can never go stale at runtime.
    {
        bool *needed = new bool[g->widgetCount]();
        if (g->usbOutWidget >= 0) needed[g->usbOutWidget] = true;
        if (g->es8311OutWidget >= 0) needed[g->es8311OutWidget] = true;
        if (g->wm8960OutWidget >= 0) needed[g->wm8960OutWidget] = true;
        for (uint16_t e = 0; e < g->edgeCount; ++e)
        {
            if (g->edges[e].monitorTap)
            {
                // Both ends: the source (so the monitored voice's signal
                // actually gets computed) and the destination (a VuMeter/
                // Oscilloscope-type widget - its own render() is a native
                // no-op today, see NodeRender.h, but the plan explicitly
                // asks to keep a "monitor"-reachable subgraph regardless,
                // and this project's web/display consumption of that
                // destination's buffer was outside this investigation's
                // scope - safer to keep it than to silently freeze a live
                // meter/scope display this pass can't see the far side of).
                needed[g->instances[g->edges[e].fromInstance].widgetIndex] = true;
                needed[g->instances[g->edges[e].toInstance].widgetIndex] = true;
            }
        }
        // widgetCount is small (<= kMaxWidgets = 64), so a plain
        // O(widgetCount * edgeCount) fixed-point worklist over the existing
        // edge list is cheap at build time and avoids needing a separate
        // reverse-adjacency structure just for this one-time pass.
        bool changed = true;
        while (changed)
        {
            changed = false;
            for (uint16_t e = 0; e < g->edgeCount; ++e)
            {
                const Edge &edge = g->edges[e];
                const uint16_t dstWidget = g->instances[edge.toInstance].widgetIndex;
                if (!needed[dstWidget])
                {
                    continue;
                }
                const uint16_t srcWidget = g->instances[edge.fromInstance].widgetIndex;
                if (!needed[srcWidget])
                {
                    needed[srcWidget] = true;
                    changed = true;
                }
            }
        }
        uint32_t prunedWidgetCount = 0;
        for (uint16_t w = 0; w < g->widgetCount; ++w)
        {
            g->widgets[w].needed = needed[w];
            if (!needed[w])
            {
                ++prunedWidgetCount;
            }
        }
        delete[] needed;
        if (prunedWidgetCount > 0)
        {
            ESP_LOGI(kGraphBuilderTag, "%u of %u widgets pruned (no path to any output/monitor)",
                     static_cast<unsigned>(prunedWidgetCount), static_cast<unsigned>(g->widgetCount));
        }
    }

    const size_t internalFreeAfter = heap_caps_get_free_size(MALLOC_CAP_INTERNAL);
    const size_t internalLargestAfter = heap_caps_get_largest_free_block(MALLOC_CAP_INTERNAL);
    memorydiag::setGraphStats(g->instanceCount, sizeof(NodeInstance), blockBufferCount,
                              blockBufferCount * synth::kBlockSize * sizeof(float), instancesInternal,
                              outputPsramFallbacks);
    ESP_LOGI(kGraphBuilderTag,
             "%u instances, NodeInstance %u B (%s), output-PSRAM fallbacks %u, internal heap free %u -> %u B, "
             "largest block %u -> %u B, buffer views %u",
             static_cast<unsigned>(g->instanceCount), static_cast<unsigned>(sizeof(NodeInstance)),
             instancesInternal ? "internal" : "PSRAM", static_cast<unsigned>(outputPsramFallbacks),
             static_cast<unsigned>(internalFreeBefore), static_cast<unsigned>(internalFreeAfter),
             static_cast<unsigned>(internalLargestBefore), static_cast<unsigned>(internalLargestAfter),
             static_cast<unsigned>(viewPortCount));

    fitInternal = instancesInternal && outputPsramFallbacks == 0;
    return g;
}
} // namespace

inline uint16_t GraphBuilder::peekRequestedVoices(const PresetDoc &doc)
{
    // Same MAX-over-MidiIn-widgets logic buildWithVoiceCap uses internally
    // for its own voiceCount, computed once here up front purely to know
    // what was actually requested (for the outRequestedVoices report and as
    // the retry loop's starting cap) - buildWithVoiceCap repeats this per
    // attempt against a shrinking cap instead of taking voiceCount directly,
    // since it also needs the natural (uncapped) MidiIn value to know
    // whether ITS cap actually bound anything. Also used standalone by
    // SynthEngine::buildFromFile() to decide - BEFORE paying for a full
    // build() - whether this reload needs the old live graph freed first
    // (see its call site).
    uint16_t requestedVoices = 1;
    for (uint16_t i = 0; i < doc.widgetCount; ++i)
    {
        if (doc.widgets[i].type == NodeType::MidiIn)
        {
            const uint16_t configured = requestedMidiVoices(doc.widgets[i]);
            if (configured > requestedVoices)
            {
                requestedVoices = configured;
            }
        }
    }
    if (requestedVoices > SYNTH_VOICE_COUNT)
    {
        requestedVoices = SYNTH_VOICE_COUNT;
    }
    return requestedVoices;
}

inline Graph *GraphBuilder::build(const PresetDoc &doc, uint16_t *outRequestedVoices, uint16_t *outEffectiveVoices,
                            uint16_t voiceCeiling)
{
    const uint16_t requestedVoices = peekRequestedVoices(doc);
    if (voiceCeiling > SYNTH_VOICE_COUNT)
    {
        voiceCeiling = SYNTH_VOICE_COUNT;
    }

    uint16_t effectiveVoices = requestedVoices < voiceCeiling ? requestedVoices : voiceCeiling;
    Graph *g = nullptr;
    while (effectiveVoices >= 1)
    {
        bool fitInternal = false;
        g = buildWithVoiceCap(doc, effectiveVoices, fitInternal);
        const bool buildSucceeded = doc.widgetCount == 0 || g->widgetCount != 0;
        if (buildSucceeded && fitInternal)
        {
            break;
        }

        g->destroy();
        Graph::release(g);
        g = nullptr;
        if (effectiveVoices == 1)
        {
            break;
        }
        --effectiveVoices;
    }

    if (outRequestedVoices != nullptr)
    {
        *outRequestedVoices = requestedVoices;
    }
    if (outEffectiveVoices != nullptr)
    {
        *outEffectiveVoices = g != nullptr ? effectiveVoices : 0;
    }
    return g;
}
