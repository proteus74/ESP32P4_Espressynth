#pragma once

#include <cstddef>
#include <cstdint>
#include <cstring>

#include "cJSON.h"

#include "NodeTypes.h"
#include "SynthConfig.h"

// One drum-sequencer pattern grid (see WidgetParams::drumPatterns below).
struct DrumSequencerPatternParams
{
    uint8_t active[(synth::kDrumSequencerMaxCells + 7) / 8] = {};
    uint8_t velocity[synth::kDrumSequencerMaxCells] = {};
};

// Every widget-type-specific param this engine understands, flattened into
// one struct (only the fields relevant to a widget's actual type are
// populated/used - see GraphBuilder). Mirrors the "params" object per
// widget in the preset JSON.
struct WidgetParams
{
    // oscillator - tuneVolts is the Tune slider's value (0-10V pitch-CV
    // domain, 5V = no transpose - see CvConventions.h/OscillatorState)
    WaveType waveType = WaveType::Sine;
    float tuneVolts = 5.0f;
    float detune = 0.0f;
    float amplitude = 1.0f;
    // "gain" (0-10V CV, 10V = unchanged - see OscillatorState::gainCv)
    // shares the "gain" field further down with AudioOut/UsbOut/Es8311Out/
    // Wm8960Out/SoundFontPlayer (same 10.0f default convention there).
    // fm-oscillator - fmIndex is FmOscillatorState::fmIndexCv (0-10V,
    // 0V = no FM, see FmOscillatorNode.h). Reuses waveType/tuneVolts/
    // amplitude/gain above (same fields/defaults as oscillator).
    float fmIndex = 0.0f;
    // WaveGen/FM-operator internal one-sample feedback, 0-10V. Shared field
    // because both oscillator types expose the same "feedback" parameter.
    float oscillatorFeedback = 0.0f;
    // fm-oscillator only - FmOscillatorState::dutyCycleCv (0-10V, 5V =
    // classic 50% square, only audible when waveType==Square).
    float dutyCycle = 5.0f;
    float superSawDetune = 5.0f;
    float superSawMix = 5.0f;
    // lfo - amplitude is 0-10V peak swing (see LfoState); zeroPointFixed
    // selects fixed (zeroPoint) vs auto (amplitude/2) center; oneShot plays
    // one cycle per reset edge then freezes (see NodeInstance's Lfo case).
    // lfoGlobal is the "global" port's UI-toggle fallback (see LfoState::
    // global), only consulted when the port itself has no DSP-edge/
    // ControlRoute wired.
    float lfoRate = 2.0f;
    float lfoMaxRate = 50.0f;
    float lfoAmplitude = 10.0f;
    float lfoPhaseDeg = 0.0f;
    bool lfoZeroPointFixed = false;
    float lfoZeroPoint = 5.0f;
    bool lfoOneShot = false;
    bool lfoGlobal = false;
    // detune - amount is 0-10V, 5V = 0 offset (see DetuneState in
    // NodeState.h). maxOffsetCents is the settings-dialog range; stepCents
    // is the rounding grid (100 = semitone steps, 1 = cent steps).
    float detuneAmount = 5.0f;
    float detuneMaxOffsetCents = 1200.0f;
    float detuneStepCents = 100.0f;
    // frequency-divider
    float frequencyDivider = 2.0f;
    // adsr - attack/decay/sustain/release are the final seconds/level value
    // (what ramp.configure() uses directly); attackMaxSeconds/attackLog etc.
    // mirror the settings dialog (main.js) and are only consulted when the
    // matching CV port has a real audio-rate edge (see AdsrState).
    float attack = 0.1f, decay = 0.1f, sustain = 0.7f, release = 0.2f;
    float attackMaxSeconds = 4.0f, decayMaxSeconds = 4.0f, releaseMaxSeconds = 4.0f;
    bool attackLog = false, decayLog = false, releaseLog = false;
    // dx-envelope - four target levels in the 0-10V CV domain and four
    // segment durations. Gate-on runs L1/T1 -> L2/T2 -> L3/T3 and holds;
    // gate-off runs from the current level to L4 over T4.
    float dxLevel[4] = {10.0f, 8.0f, 7.0f, 0.0f};
    float dxTime[4] = {0.1f, 0.25f, 0.4f, 0.6f};
    float dxTimeMaxSeconds[4] = {4.0f, 4.0f, 4.0f, 4.0f};
    bool dxTimeLog[4] = {false, false, false, false};
    // filter - "cutoff"/"resonance" are raw 0-10V CV (see CvConventions.h),
    // mapped to Hz/Q via the min/max/log fields below at DSP time.
    FilterKind filterType = FilterKind::LowPass;
    float cutoffCv = 5.0f, cutoffMinHz = 20.0f, cutoffMaxHz = 20000.0f;
    bool cutoffLog = true;
    float resonanceCv = 5.0f, resonanceMinQ = 0.1f, resonanceMaxQ = 20.0f;
    bool resonanceLog = true;
    // How often (in samples) the biquad coefficients get recomputed under a
    // genuine audio-rate cutoff/resonance edge - 1-16, default 4 (see
    // FilterState::coeffStride in NodeState.h).
    uint8_t coeffStride = 4;
    // 12 or 24 dB per octave - 24 cascades a second identical biquad stage
    // (see FilterState::slopeDb).
    uint8_t filterSlopeDb = 12;
    // filterGlobal is the "global" port's UI-toggle fallback (see
    // FilterState::global), same pattern as gainGlobal.
    bool filterGlobal = false;
    // esp-eq: one stereo band, filter type switchable (peak/shelf/hi-lo-pass)
    EqFilterKind eqFilterType = EqFilterKind::Peak;
    float eqFrequency = 1000.0f;
    float eqQ = 1.0f;
    float eqGainDb = 0.0f;
    // eq-only bypass (see EspEqState::bypass).
    bool eqBypass = false;
    // gain - "amount" (Output Gain) and "curve" are raw CV (0-10V, see
    // CvConventions.h): amount 10V = unity gain default, curve 5V = unity
    // default (see GainState/cv::curveGain). gainGlobal is the "global"
    // port's UI-toggle fallback (see GainState::global), same pattern as
    // lfoGlobal.
    float amount = 10.0f;
    float curve = 5.0f;
    bool gainGlobal = false;
    // bitcrusher - 0..10 combines 16..2-bit quantization with 1..32-sample hold.
    float bitcrusherCrush = 5.0f;
    bool bitcrusherBypass = false;
    // wave-folder - all four controls are raw 0-10V CV values.
    float waveFolderDrive = 0.0f, waveFolderSymmetry = 5.0f;
    float waveFolderGain = 10.0f, waveFolderMix = 10.0f;
    // pan - single CV, centre (5V) = hard center.
    float panCv = 5.0f;
    // mixer/multiplier/adder
    uint8_t inputCount = 2;
    float mixerGain[8] = {1, 1, 1, 1, 1, 1, 1, 1};
    float outputGain = 1.0f;
    // multiplexer - reuses inputCount above. multiplexerSelect is the
    // widget's own vertical-slider override (0-based, see
    // MultiplexerState::manualSelect).
    uint8_t multiplexerSelect = 0;
    float multiplexerSoftFadeMs = 5.0f;
    // output-multiplexer - mirror of multiplexer, but the CV/slider picks
    // which OUTPUT (of outputMuxCount) receives the single audio input;
    // every other output holds its own last value (persisted per-output in
    // outputMuxHeld - see OutputMultiplexerState/nodes/OutputMultiplexerNode.h).
    uint8_t outputMuxCount = 2;
    uint8_t outputMuxSelect = 0;
    float outputMuxHeld[16] = {}; // NodeState.h's kOutputMultiplexerMaxOutputs
    // chorus
    float rate = 1.5f, offset = 0.02f, depth = 0.003f;
    bool chorusBypass = false;
    // delay (always stereo, "echo" was only ever delay-with-feedback) -
    // time/feedback/mixMin/Max/Log mirror main.js's Settings-Dialog
    // (openEffectCvSettings/EFFECT_CV_PARAMS), only consulted when the
    // matching CV port has a real audio-rate edge (see DelayState).
    float delayTimeMs = 300.0f, delayFeedback = 0.3f, delayMix = 0.35f;
    float timeMin = 0.0f, timeMax = 2000.0f, feedbackMin = 0.0f, feedbackMax = 0.95f, mixMin = 0.0f, mixMax = 1.0f;
    bool timeLog = false, feedbackLog = false, mixLog = false;
    bool delayBypass = false;
    // reverb-light (always stereo) - param semantics follow esp_ae_reverb_para_t
    // (see nodes/ReverbNode.h's ReverbLightState). roomSize/damping/wetLevel/
    // dryLevel likewise mirror the settings dialog.
    float reverbRoomSize = 0.5f, reverbDamping = 0.5f;
    float reverbWetLevelDb = -6.0f, reverbDryLevelDb = 0.0f;
    bool reverbBypass = false;
    float roomSizeMin = 0.0f, roomSizeMax = 1.0f, dampingMin = 0.0f, dampingMax = 1.0f;
    float wetLevelMin = -96.0f, wetLevelMax = 0.0f, dryLevelMin = -96.0f, dryLevelMax = 0.0f;
    bool roomSizeLog = false, dampingLog = false, wetLevelLog = false, dryLevelLog = false;
    // audio-out
    bool muted = false;
    float left = 1.0f, right = 1.0f, gain = 10.0f;
    // Audio Outputs page-link: eight 0-10V per-bus level controls.
    float linkGain[synth::kLinkBusCount] = {10, 10, 10, 10, 10, 10, 10, 10};
    // linked-input / linked-output: freely-placeable, linkId-keyed links -
    // NOT the fixed 8-bus audio-output-link/audio-input-link mechanism above.
    char linkId[24] = {};
    char linkName[48] = {};
    char sourceLinkId[24] = {};
    // clock - pulse width in ms; tempo itself comes from the device-wide
    // synth::masterClock() (MasterClock.h), not from the preset.
    float clockPulseMs = 100.0f;
    // cv-in / cv-out (External Hardware) - which I2C ADC/DAC channel this widget talks to,
    // chosen once via the settings modal from the live boot-time scan
    // (Feature::IO::I2CDeviceRegistry), never live-updated (see nodes/CvInNode.h/
    // CvOutNode.h - structural, set only in build()). -1 = placed but never configured.
    int8_t cvIoPort = -1;
    uint8_t cvIoAddress = 0;
    uint8_t cvIoChannel = 0;
    // digital-in / digital-out (External Hardware) - which configured GPIO
    // (Feature::IO::gpioSettings()) this widget talks to, chosen via the settings modal,
    // never live-updated (structural, set only in build()). -1 = placed but never configured.
    int8_t gpioPin = -1;
    // sample-hold - manual threshold, overridden by a connected Threshold CV.
    float sampleHoldThreshold = 5.0f;
    // midi-in
    int8_t channel = -1;
    uint8_t voices = 1;
    OverflowMode overflowMode = OverflowMode::StealOldest;
    bool glide = false;
    float glideTime = 0.0f;
    float midiInPitchBendRangeCents = 200.0f;
    bool midiInPitchBendQuantizeCents = false;
    // midi-split
    uint8_t midiSplitZoneCount = 0; // starts empty - no zone until the user adds one
    int8_t midiSplitZoneFrom[4] = {0, 0, 0, 0};
    int8_t midiSplitZoneTo[4] = {127, 127, 127, 127};
    int8_t midiSplitZoneOctave[4] = {0, 0, 0, 0};
    // const
    float constValue = 0.0f;
    // knob
    struct KnobControlParams
    {
        int16_t cc = -1;
        int8_t channel = -1;
        float raw = 0.0f;
        float point1In = 0.0f, point1Out = 0.0f;
        float point2In = 127.0f, point2Out = 10.0f;
        bool hasConstraintMin = false, hasConstraintMax = false;
        float constraintMin = 0.0f, constraintMax = 0.0f;
        float valueList[synth::kValueListMax] = {};
        uint8_t valueListCount = 0;
    };
    uint8_t knobCount = 1;
    uint8_t knobLayerCount = 1;
    uint8_t knobActiveLayer = 0;
    KnobControlParams knobs[synth::kKnobOutputMax] = {};
    uint8_t knobOutputSide = 1; // top=0, bottom=1, left=2, right=3; browser layout only
    int16_t knobCc = -1;
    int8_t knobChannel = -1;
    float knobRaw = 0.0f;
    float point1In = 0.0f, point1Out = 0.0f;
    float point2In = 127.0f, point2Out = 127.0f;
    bool hasConstraintMin = false, hasConstraintMax = false;
    float constraintMin = 0.0f, constraintMax = 0.0f;
    // knob/button "value list" - shared storage, only one type ever
    // populates it for a given widget instance. Wins automatically over
    // linear/fixed once it has >=2 entries (see KnobState/ButtonState).
    float valueList[synth::kValueListMax] = {};
    uint8_t valueListCount = 0;
    // button
    struct ButtonControlParams
    {
        int16_t cc = -1;
        int8_t channel = -1;
        float value0 = 0.0f;
        float value127 = 10.0f;
        float valueList[synth::kValueListMax] = {};
        uint8_t valueListCount = 0;
        uint8_t listIndex = 0;
        bool wasAbove = false;
    };
    uint8_t buttonCount = 1;
    uint8_t buttonLayerCount = 1;
    uint8_t buttonActiveLayer = 0;
    ButtonControlParams buttons[synth::kButtonOutputMax] = {};
    // up-down-button - shares valueList/valueListCount above
    int16_t upDownCcUp = -1;
    int8_t upDownChannelUp = -1;
    int16_t upDownCcDown = -1;
    int8_t upDownChannelDown = -1;
    float upDownMin = 0.0f;
    float upDownMax = 10.0f;
    float upDownStep = 1.0f;
    uint8_t upDownListIndex = 0;
    float upDownCurrentValue = 0.0f;
    // switch
    int16_t switchCc = -1;
    int8_t switchChannel = -1;
    bool switchSelectIn2 = false;
    float switchIn1Default = 0.0f;
    float switchIn2Default = 0.0f;
    // output-switch - mirror of switch, but selectOut2 picks which OUTPUT
    // receives the single audio input; the other output holds its own last
    // value (persisted in outputSwitchHeld1/2 - see OutputSwitchState/
    // nodes/OutputSwitchNode.h), not a fixed configured fallback.
    int16_t outputSwitchCc = -1;
    int8_t outputSwitchChannel = -1;
    bool outputSwitchSelectOut2 = false;
    float outputSwitchHeld1 = 0.0f;
    float outputSwitchHeld2 = 0.0f;
    // wave-player
    char sampleFile[40] = {};
    int rootNoteMidi = 60; // C4 (scientific pitch notation, A4=69)
    // soundfont-player
    char soundFontVolume[8] = "flash";
    char soundFontPath[160] = {};
    int soundFontBank = 0;
    int soundFontProgram = 0;
    int soundFontMaxVoices = 32;
    bool soundFontEnableCc = false;
    bool soundFontEnableModWheel = false;
    bool soundFontEnablePitchWheel = false;
    // mda-piano - all controls use the same 0-10V domain as their ports.
    int mdaPianoMaxVoices = 32;
    bool mdaPianoEnableModWheel = false;
    bool mdaPianoEnablePitchWheel = false;
    float mdaPianoVoiceType = 0.0f;
    float mdaPianoDrive = 0.0f;
    float mdaPianoTremWahMode = 0.0f;
    float mdaPianoTremWahDepth = 0.0f;
    float mdaPianoTremWahRate = 5.0f;
    float mdaPianoChoPhaMode = 0.0f;
    float mdaPianoChoPhaDepth = 0.0f;
    float mdaPianoChoPhaSpeed = 5.0f;
    float mdaPianoPreGain = 10.0f;
    // hammond - drawbar LEVELS (0-10V CV, default 888000000) and their
    // harmonic RATIOS (settings-dialog-only, default the classic Hammond
    // footages). Two independent 9-element arrays - see nodes/HammondNode.h.
    // tuneVolts/amplitude/gain above are reused, same convention as Oscillator.
    float hammondDrawbar[9] = {10, 10, 10, 0, 0, 0, 0, 0, 0};
    float hammondRatio[9] = {0.5f, 1.5f, 1.0f, 2.0f, 3.0f, 4.0f, 5.0f, 6.0f, 8.0f};
    uint8_t hammondModulationStride = 4;
    // step-arpeggiator - sixteen per-step pitch-CV knob values (0-10V,
    // C4/5V default - see CvConventions.h), only used while the matching
    // "stepN" port has no DSP edge (see nodes/StepArpeggiatorNode.h).
    // stepCount is 8 or 16 (the "8/16 steps" switch); values beyond it stay
    // stored but unplayed. pulseMs is gateOut's pulse width, same convention
    // as Clock's pulseMs.
    float stepArpValue[16] = {5, 5, 5, 5, 5, 5, 5, 5, 5, 5, 5, 5, 5, 5, 5, 5};
    uint8_t stepArpStepCount = 16;
    // Local knob fallback (0-10V) for the "pulseTime" port - only consulted
    // while unwired (see StepArpeggiatorNode.h); already in the final,
    // real-world unit (ms) here, same "local value pre-mapped, wired CV
    // mapped fresh every time" split as Delay's own delayTimeMs/timeMin/
    // timeMax. pulseTimeMinSec/MaxSec are the settings-dialog range that
    // 0-10V maps to (in seconds) whenever the port IS wired.
    float stepArpPulseMs = 10.0f;
    float stepArpPulseTimeMinSec = 0.001f, stepArpPulseTimeMaxSec = 2.0f;
    // Settings-dialog cap (semitones) on a step knob's pitch-CV offset when
    // "pitch" is wired - see nodes/StepArpeggiatorNode.h's StepArpState::
    // maxSemitoneOffset comment. Default 60 keeps the previous, uncapped
    // 5-octave behavior.
    float stepArpMaxSemitoneOffset = 60.0f;
    // Local knob fallback (0-10V) for the "direction" port - only consulted
    // while unwired. Default 0V = Forward (see directionFromCv()).
    float stepArpDirectionVolts = 0.0f;
    // Settings-dialog toggle - see nodes/StepArpeggiatorNode.h's
    // StepArpState::retriggerGateOut comment.
    bool stepArpRetriggerGateOut = false;
    // Internal fallback clock generator (Port-Index 21, "rate") - used only
    // while the "clock" port itself is unwired (see
    // nodes/StepArpeggiatorNode.h's render()). Same cvVolts/maxBpm/roundBpm
    // shape as Clock's own WidgetParams fields above; minBpm is always 0
    // here, no separate field needed.
    float stepArpClockCvVolts = 0.0f;
    float stepArpClockMaxBpm = 120.0f;
    bool stepArpClockRoundBpm = false;
    // drum-sequencer - one 4/8/12/16-step bar, up to eight trigger/velocity
    // output pairs, and up to kDrumSequencerMaxPatterns independent pattern
    // grids sharing that same step/track shape. Active flags are packed to
    // keep PresetDoc bounded; velocity bytes map linearly to 0-10V.
    uint8_t drumStepCount = 16;
    uint8_t drumTrackCount = 1;
    DrumSequencerPatternParams drumPatterns[synth::kDrumSequencerMaxPatterns];
    uint8_t drumPatternCount = 1;
    uint8_t drumActivePatternIndex = 0;
    bool drumSwitchAtEnd = false;
    float drumPulseMs = 10.0f;
    float drumClockCvVolts = 0.0f;
    float drumClockMaxBpm = 120.0f;
    bool drumClockRoundBpm = false;
};

struct WidgetDesc
{
    char id[24] = {};
    char label[48] = {};
    NodeType type = NodeType::Unknown;
    // Pages are normally presentation-only. FX MIDI-In is exception: it is
    // deliberately mono, so its downstream graph is rendered once.
    bool isFxPage = false;
    WidgetParams params;
};

struct ConnectionDesc
{
    char fromWidget[24] = {};
    char fromPort[16] = {};
    char toWidget[24] = {};
    char toPort[16] = {};
};

// Flat intermediate representation: generator+fx pages concatenated (the
// page split is a pure UI concept the engine doesn't need - see
// GraphBuilder for how audio-output-link/audio-input-link recover the
// "boundary between pages" semantics structurally).
struct PresetDoc
{
    WidgetDesc widgets[synth::kMaxWidgets];
    uint16_t widgetCount = 0;
    ConnectionDesc connections[synth::kMaxConnections];
    uint16_t connectionCount = 0;
};

// parsePreset() itself lives in PresetFill.h, not here - it needs every
// nodes/*.h's full definition, but every nodes/*.h needs WidgetParams
// (declared above) first. See PresetFill.h.

// Small per-widget-params JSON accessors shared by every node type's own
// parsePreset(cJSON*, WidgetParams&) (see nodes/*.h) - kept here (not
// duplicated per node header) since every one of them needs cJSON anyway.
namespace presetparser
{
inline float getFloat(cJSON *obj, const char *key, float def)
{
    if (obj == nullptr)
        return def;
    cJSON *v = cJSON_GetObjectItemCaseSensitive(obj, key);
    return cJSON_IsNumber(v) ? static_cast<float>(v->valuedouble) : def;
}

inline const char *getString(cJSON *obj, const char *key, const char *def)
{
    if (obj == nullptr)
        return def;
    cJSON *v = cJSON_GetObjectItemCaseSensitive(obj, key);
    return cJSON_IsString(v) ? v->valuestring : def;
}

inline bool getBool(cJSON *obj, const char *key, bool def)
{
    if (obj == nullptr)
        return def;
    cJSON *v = cJSON_GetObjectItemCaseSensitive(obj, key);
    if (v == nullptr)
        return def;
    return cJSON_IsBool(v) ? cJSON_IsTrue(v) : def;
}

inline int getInt(cJSON *obj, const char *key, int def)
{
    if (obj == nullptr)
        return def;
    cJSON *v = cJSON_GetObjectItemCaseSensitive(obj, key);
    return cJSON_IsNumber(v) ? v->valueint : def;
}

// Parses a JSON array of numbers into `out` (up to maxCount entries),
// returning how many were written. Used by Knob/Button's "valueList" - the
// comma-separated string main.js's parseValueList() validates client-side
// arrives here already split into a proper JSON number array.
inline uint8_t getFloatArray(cJSON *obj, const char *key, float *out, uint8_t maxCount)
{
    if (obj == nullptr)
        return 0;
    cJSON *arr = cJSON_GetObjectItemCaseSensitive(obj, key);
    if (!cJSON_IsArray(arr))
        return 0;
    uint8_t count = 0;
    cJSON *item = nullptr;
    cJSON_ArrayForEach(item, arr)
    {
        if (count >= maxCount)
            break;
        out[count] = cJSON_IsNumber(item) ? static_cast<float>(item->valuedouble) : 0.0f;
        ++count;
    }
    return count;
}
} // namespace presetparser
