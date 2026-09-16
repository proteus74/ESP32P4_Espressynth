#pragma once

#include <cstddef>
#include <cstdio>
#include <cstring>

#include "cJSON.h"
#include "CvConventions.h"

#include "EspAdditionalEffects.h"
#include "EspEqEffect.h"
#include "PresetParser.h"
#include "SoundFontPlayer.h"

#include "nodes/AdsrNode.h"
#include "nodes/ArithmeticNodes.h"
#include "nodes/ButtonNode.h"
#include "nodes/BitcrusherNode.h"
#include "nodes/WaveFolderNode.h"
#include "nodes/PanNode.h"
#include "nodes/ChorusNode.h"
#include "nodes/ClockNode.h"
#include "nodes/CvInNode.h"
#include "nodes/CvOutNode.h"
#include "nodes/SampleHoldNode.h"
#include "nodes/CurtisFilterNode.h"
#include "nodes/ConstNode.h"
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
#include "nodes/LinkedIONodes.h"
#include "nodes/WavePlayerNode.h"

// Parses the /api/preset JSON schema into a PresetDoc, aggregating every
// node type's own parsePreset() (see nodes/*.h) into one dispatcher. Kept
// out of PresetParser.h: it needs every nodes/*.h's full definition, but
// every nodes/*.h needs WidgetParams (declared in PresetParser.h) first -
// putting the dispatcher here (downstream of both) avoids that include
// cycle, same pattern as NodeRender.h/NodeInstance.h.
namespace
{
const char *presetFillGetString(cJSON *obj, const char *key, const char *def)
{
    if (obj == nullptr)
        return def;
    cJSON *v = cJSON_GetObjectItemCaseSensitive(obj, key);
    return cJSON_IsString(v) ? v->valuestring : def;
}

void fillParams(NodeType type, cJSON *p, WidgetParams &out)
{
    switch (type)
    {
    case NodeType::Oscillator:
        nodes::oscillator::parsePreset(p, out);
        break;
    case NodeType::FmOscillator:
        nodes::fmoscillator::parsePreset(p, out);
        break;
    case NodeType::Hammond:
        nodes::hammond::parsePreset(p, out);
        break;
    case NodeType::Lfo:
        nodes::lfo::parsePreset(p, out);
        break;
    case NodeType::Detune:
        nodes::detune::parsePreset(p, out);
        break;
    case NodeType::FrequencyDivider:
        nodes::arithmetic::parsePresetFrequencyDivider(p, out);
        break;
    case NodeType::Adsr:
        nodes::adsr::parsePreset(p, out);
        break;
    case NodeType::DxEnvelope:
        nodes::dxenvelope::parsePreset(p, out);
        break;
    case NodeType::StepArpeggiator:
        nodes::steparp::parsePreset(p, out);
        break;
    case NodeType::Filter:
        nodes::filter::parsePreset(p, out);
        break;
    case NodeType::CurtisFilter:
        nodes::curtisfilter::parsePreset(p, out);
        break;
    case NodeType::EspEq:
        parsePresetEspEq(p, out);
        break;
    case NodeType::EspAlc:
        parsePresetEspAlc(p, out);
        break;
    case NodeType::EspChannelConvert:
        parsePresetEspChannelConvert(p, out);
        break;
    case NodeType::EspMixer:
        parsePresetEspMixer(p, out);
        break;
    case NodeType::EspSonic:
        parsePresetEspSonic(p, out);
        break;
    case NodeType::EspDrc:
        parsePresetEspDrc(p, out);
        break;
    case NodeType::EspMbc:
        parsePresetEspMbc(p, out);
        break;
    case NodeType::Gain:
        nodes::gain::parsePreset(p, out);
        break;
    case NodeType::Clock:
        nodes::clock::parsePreset(p, out);
        break;
    case NodeType::CvIn:
        nodes::cvin::parsePreset(p, out);
        break;
    case NodeType::CvOut:
        nodes::cvout::parsePreset(p, out);
        break;
    case NodeType::SampleHold:
        nodes::samplehold::parsePreset(p, out);
        break;
    case NodeType::Mixer:
        nodes::mixer::parsePreset(p, out);
        break;
    case NodeType::Multiplexer:
        nodes::multiplexer::parsePreset(p, out);
        break;
    case NodeType::OutputMultiplexer:
        nodes::outputmultiplexer::parsePreset(p, out);
        break;
    case NodeType::Multiplier:
    case NodeType::Adder:
        nodes::arithmetic::parsePresetMultiplierOrAdder(p, out);
        break;
    case NodeType::Bitcrusher:
        nodes::bitcrusher::parsePreset(p, out);
        break;
    case NodeType::WaveFolder:
        nodes::wavefolder::parsePreset(p, out);
        break;
    case NodeType::Pan:
        nodes::pan::parsePreset(p, out);
        break;
    case NodeType::Chorus:
        nodes::chorus::parsePreset(p, out);
        break;
    case NodeType::Delay:
        nodes::delay::parsePreset(p, out);
        break;
    case NodeType::Reverb:
        nodes::reverb::parsePresetReverb(p, out);
        break;
    case NodeType::ReverbLight:
        nodes::reverb::parsePresetReverbLight(p, out);
        break;
    case NodeType::AudioInputLink:
        nodes::link::parsePreset(p, out);
        break;
    case NodeType::LinkedInput:
        nodes::linkedio::parseLinkDefinition(p, out);
        break;
    case NodeType::LinkedOutput:
        nodes::linkedio::parseLinkSelection(p, out);
        break;
    case NodeType::AudioOut:
    case NodeType::UsbOut:
    case NodeType::Es8311Out:
    case NodeType::Wm8960Out:
        nodes::output::parsePreset(p, out);
        break;
    case NodeType::MidiIn:
        nodes::midiin::parsePreset(p, out);
        break;
    case NodeType::MidiSplit:
        nodes::midisplit::parsePreset(p, out);
        break;
    case NodeType::Const:
        nodes::constant::parsePreset(p, out);
        break;
    case NodeType::Knob:
        nodes::knob::parsePreset(p, out);
        break;
    case NodeType::ValueMapper:
        nodes::valuemapper::parsePreset(p, out);
        break;
    case NodeType::Button:
        nodes::button::parsePresetButton(p, out);
        break;
    case NodeType::UpDownButton:
        nodes::button::parsePresetUpDownButton(p, out);
        break;
    case NodeType::Switch:
        nodes::switchnode::parsePreset(p, out);
        break;
    case NodeType::OutputSwitch:
        nodes::outputswitch::parsePreset(p, out);
        break;
    case NodeType::WavePlayer:
        nodes::waveplayer::parsePreset(p, out);
        break;
    case NodeType::SoundFontPlayer:
        soundfont::parsePreset(p, out);
        break;
    default:
        break;
    }
}

void parseWidgetsArray(cJSON *widgetsArr, PresetDoc &doc, bool isFxPage)
{
    cJSON *w;
    cJSON_ArrayForEach(w, widgetsArr)
    {
        if (doc.widgetCount >= synth::kMaxWidgets)
        {
            break;
        }
        const char *id = presetFillGetString(w, "id", nullptr);
        const char *typeStr = presetFillGetString(w, "type", nullptr);
        if (id == nullptr || typeStr == nullptr)
        {
            continue;
        }
        const NodeType type = nodeTypeFromString(typeStr);
        if (type == NodeType::Unknown)
        {
            continue; // matches _buildNode()'s default case (unrecognized type -> ignored)
        }
        WidgetDesc &wd = doc.widgets[doc.widgetCount];
        snprintf(wd.id, sizeof(wd.id), "%s", id);
        const char *label = presetFillGetString(w, "label", typeStr);
        snprintf(wd.label, sizeof(wd.label), "%s", label);
        wd.type = type;
        wd.isFxPage = isFxPage;
        cJSON *params = cJSON_GetObjectItemCaseSensitive(w, "params");
        fillParams(type, params, wd.params);
        ++doc.widgetCount;
    }
}

void parseConnectionsArray(cJSON *connsArr, PresetDoc &doc)
{
    cJSON *c;
    cJSON_ArrayForEach(c, connsArr)
    {
        if (doc.connectionCount >= synth::kMaxConnections)
        {
            break;
        }
        cJSON *from = cJSON_GetObjectItemCaseSensitive(c, "from");
        cJSON *to = cJSON_GetObjectItemCaseSensitive(c, "to");
        if (!cJSON_IsObject(from) || !cJSON_IsObject(to))
        {
            continue;
        }
        const char *fromWidget = presetFillGetString(from, "widgetId", nullptr);
        const char *fromPort = presetFillGetString(from, "portId", nullptr);
        const char *toWidget = presetFillGetString(to, "widgetId", nullptr);
        const char *toPort = presetFillGetString(to, "portId", nullptr);
        if (fromWidget == nullptr || fromPort == nullptr || toWidget == nullptr || toPort == nullptr)
        {
            continue;
        }
        ConnectionDesc &cd = doc.connections[doc.connectionCount];
        snprintf(cd.fromWidget, sizeof(cd.fromWidget), "%s", fromWidget);
        snprintf(cd.fromPort, sizeof(cd.fromPort), "%s", fromPort);
        snprintf(cd.toWidget, sizeof(cd.toWidget), "%s", toWidget);
        snprintf(cd.toPort, sizeof(cd.toPort), "%s", toPort);
        ++doc.connectionCount;
    }
}
} // namespace

// Parses the /api/preset JSON schema into `out`. Returns false only on
// malformed JSON / oversized input; a well-formed-but-empty or
// unexpected-shape document still returns true with an empty PresetDoc
// (produces a silent graph, matches the GET fallback shape).
inline bool parsePreset(const char *json, size_t len, PresetDoc &out)
{
    out.widgetCount = 0;
    out.connectionCount = 0;

    if (json == nullptr || len == 0 || len > synth::kMaxPresetBytes)
    {
        return false;
    }

    cJSON *root = cJSON_ParseWithLength(json, len);
    if (root == nullptr)
    {
        return false;
    }

    cJSON *pages = cJSON_GetObjectItemCaseSensitive(root, "pages");
    if (cJSON_IsObject(pages))
    {
        cJSON *pageNames[2] = {
            cJSON_GetObjectItemCaseSensitive(pages, "generator"),
            cJSON_GetObjectItemCaseSensitive(pages, "fx"),
        };
        for (size_t pageIndex = 0; pageIndex < 2; ++pageIndex)
        {
            cJSON *page = pageNames[pageIndex];
            if (!cJSON_IsObject(page))
            {
                continue;
            }
            cJSON *widgetsArr = cJSON_GetObjectItemCaseSensitive(page, "widgets");
            if (cJSON_IsArray(widgetsArr))
            {
                parseWidgetsArray(widgetsArr, out, pageIndex == 1);
            }
            cJSON *connsArr = cJSON_GetObjectItemCaseSensitive(page, "connections");
            if (cJSON_IsArray(connsArr))
            {
                parseConnectionsArray(connsArr, out);
            }
        }
    }

    cJSON_Delete(root);
    return true;
}
