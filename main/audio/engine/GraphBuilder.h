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
#include "nodes/ChorusNode.h"
#include "nodes/ClockNode.h"
#include "nodes/ConstNode.h"
#include "nodes/DelayNode.h"
#include "nodes/DetuneNode.h"
#include "nodes/FilterNode.h"
#include "nodes/GainNode.h"
#include "nodes/HammondNode.h"
#include "nodes/KnobNode.h"
#include "nodes/LfoNode.h"
#include "nodes/MidiInNode.h"
#include "nodes/MidiSplitNode.h"
#include "nodes/MixerNode.h"
#include "nodes/MultiplexerNode.h"
#include "nodes/OscillatorNode.h"
#include "nodes/OutputNodes.h"
#include "nodes/ReverbNode.h"
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
           type == NodeType::SoundFontPlayer || type == NodeType::AudioOutputLink || type == NodeType::AudioInputLink;
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
        if (doc.widgets[i].type == NodeType::MidiIn && doc.widgets[i].params.voices > 1)
        {
            visited[i] = true; // midi-in itself is never instanced/voiced, just a BFS seed
            queue[qTail++] = i;
        }
    }

    while (qHead < qTail)
    {
        const uint16_t cur = queue[qHead++];
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

inline void applyParams(NodeInstance &inst, NodeType type, const WidgetParams &p, bool psram)
{
    switch (type)
    {
    case NodeType::Oscillator:
        nodes::oscillator::build(inst, p);
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
    case NodeType::Filter:
        nodes::filter::build(inst, p);
        break;
    case NodeType::EspEq:
        initEspEq(*inst.espEq, p.eqFilterType, p.eqFrequency, p.eqQ, p.eqGainDb);
        break;
    case NodeType::EspAlc: initEspEffect(*inst.espEffect, EspEffectKind::Alc, p.espParam, 2); break;
    case NodeType::EspChannelConvert: initEspEffect(*inst.espEffect, EspEffectKind::ChannelConvert, p.espParam, 2); break;
    case NodeType::EspMixer: initEspEffect(*inst.espEffect, EspEffectKind::Mixer, p.espParam, 4); break;
    case NodeType::EspSonic: initEspEffect(*inst.espEffect, EspEffectKind::Sonic, p.espParam, 2); break;
    case NodeType::EspDrc: initEspEffect(*inst.espEffect, EspEffectKind::Drc, p.espParam, 6); break;
    case NodeType::EspMbc: initEspEffect(*inst.espEffect, EspEffectKind::Mbc, p.espParam, 3); break;
    case NodeType::Gain:
        nodes::gain::build(inst, p);
        break;
    case NodeType::Clock:
        nodes::clock::build(inst, p);
        break;
    case NodeType::Mixer:
        nodes::mixer::build(inst, p);
        break;
    case NodeType::Multiplexer:
        nodes::multiplexer::build(inst, p);
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
        nodes::reverb::buildReverbLight(inst, p);
        break;
    case NodeType::AudioOut:
    case NodeType::UsbOut:
    case NodeType::Es8311Out:
    case NodeType::Wm8960Out:
        nodes::output::build(inst, p);
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
            const uint16_t configured = doc.widgets[i].params.voices < 1 ? 1 : doc.widgets[i].params.voices;
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
            case NodeType::Filter:
                inst.filter = allocateNodeState<FilterState>(outputPsramFallbacks);
                stateAllocFailed = (inst.filter == nullptr);
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
            applyParams(inst, w.type, doc.widgets[i].params, kPsram);
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
            g->voiceAllocators[i].configure(voiceCount, doc.widgets[i].params.overflowMode);
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
            ControlRoute &route = g->controlRoutes[g->controlRouteCount++];
            route.sourceWidget = static_cast<uint16_t>(fromIdx);
            route.targetWidget = static_cast<uint16_t>(toIdx);
            // "cc" = raw 0-127 dial/MIDI-CC value; "voltage" (and any legacy
            // preset still using the old single "value" port name) = the
            // 2-point-mapped 0-10V output - see Graph.h's ControlRoute::Kind.
            route.kind = (strcmp(cd.fromPort, "cc") == 0) ? ControlRoute::Kind::KnobCc : ControlRoute::Kind::Knob;
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
        if (route.targetPort < 0)
        {
            continue;
        }
        const WidgetSlot &targetWidget = g->widgets[route.targetWidget];
        float initial = 0.0f;
        if (route.kind == ControlRoute::Kind::Knob)
        {
            initial = g->instances[g->widgets[route.sourceWidget].instanceBase].knob->currentValue;
        }
        else if (route.kind == ControlRoute::Kind::KnobCc)
        {
            initial = g->instances[g->widgets[route.sourceWidget].instanceBase].knob->raw;
        }
        else if (route.kind == ControlRoute::Kind::Button)
        {
            initial = g->instances[g->widgets[route.sourceWidget].instanceBase].button->currentValue;
        }
        else if (route.kind == ControlRoute::Kind::UpDownButton)
        {
            initial = g->instances[g->widgets[route.sourceWidget].instanceBase].upDownButton->currentOutput();
        }
        else
        {
            continue;
        }
        for (uint16_t v = 0; v < targetWidget.instanceCount; ++v)
        {
            g->instances[targetWidget.instanceBase + v].controlValue[route.targetPort] = initial;
        }
    }

    if (outputLinkIdx >= 0 && inputLinkIdx >= 0)
    {
        addEdge(*g, g->widgets[outputLinkIdx].instanceBase, 0, g->widgets[inputLinkIdx].instanceBase, 0, 1.0f);
        addEdge(*g, g->widgets[outputLinkIdx].instanceBase, 1, g->widgets[inputLinkIdx].instanceBase, 1, 1.0f);
        g->instances[g->widgets[inputLinkIdx].instanceBase].audioConnectedPortMask |= 0x03u;
        markDependency(static_cast<uint16_t>(outputLinkIdx), static_cast<uint16_t>(inputLinkIdx));
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
            bool midiPitchCvRoute = false;
            for (uint16_t r = 0; r < g->controlRouteCount; ++r)
            {
                const ControlRoute &route = g->controlRoutes[r];
                if (route.kind == ControlRoute::Kind::MidiPitchCv && route.targetWidget == inst.widgetIndex &&
                    route.targetPort == p)
                {
                    midiPitchCvRoute = true;
                    break;
                }
            }
            if (!audioConnected && !requireAllInputs && !midiPitchCvRoute)
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

    const size_t internalFreeAfter = heap_caps_get_free_size(MALLOC_CAP_INTERNAL);
    const size_t internalLargestAfter = heap_caps_get_largest_free_block(MALLOC_CAP_INTERNAL);
    memorydiag::setGraphStats(g->instanceCount, sizeof(NodeInstance), blockBufferCount,
                              blockBufferCount * synth::kBlockSize * sizeof(float), instancesInternal);
    ESP_LOGI(kGraphBuilderTag, "%u instances, NodeInstance %u B (%s), output-PSRAM fallbacks %u, internal heap free %u -> %u B, largest block %u -> %u B",
             static_cast<unsigned>(g->instanceCount), static_cast<unsigned>(sizeof(NodeInstance)),
             instancesInternal ? "internal" : "PSRAM", static_cast<unsigned>(outputPsramFallbacks),
             static_cast<unsigned>(internalFreeBefore), static_cast<unsigned>(internalFreeAfter),
             static_cast<unsigned>(internalLargestBefore), static_cast<unsigned>(internalLargestAfter));

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
            const uint16_t configured = doc.widgets[i].params.voices < 1 ? 1 : doc.widgets[i].params.voices;
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
