#include <cassert>
#include <cstdio>
#include <initializer_list>
#include <limits>
#include <vector>
#include "nodes/CounterNode.h"
#include "nodes/ClockNode.h"
#include "nodes/MonoflopNode.h"
#include "nodes/StepArpeggiatorNode.h"
#include "nodes/DrumSequencerNode.h"

static void check(NodeInstance &inst, std::initializer_list<float> count,
                  std::initializer_list<float> reset, std::initializer_list<float> expected)
{
    assert(count.size() == reset.size() && count.size() == expected.size());
    float output[64] = {};
    assert(count.size() <= 64);
    inst.inPorts[0] = count.begin();
    inst.inPorts[1] = reset.begin();
    inst.outL = output;
    nodes::counter::render(inst, count.size());
    size_t i = 0;
    for (float value : expected)
        assert(output[i++] == value);
    inst.inPorts[0] = inst.inPorts[1] = nullptr;
}

int main()
{
    NodeInstance inst;
    check(inst, {0, 4.99f, 5, 10, 0, 5}, {0, 0, 0, 0, 0, 0}, {0, 0, 1, 1, 1, 2});
    check(inst, {10, 0, 10}, {0, 0, 0}, {2, 2, 3});
    check(inst, {0, 10, 0, 10, 10, 0, 10}, {0, 5, 5, 5, 0, 0, 0}, {3, 0, 0, 0, 0, 0, 1});
    float count[24] = {}, reset[24] = {}, output[24] = {};
    for (size_t i = 0; i < 24; ++i)
        count[i] = i % 2 ? 10.0f : 0.0f;
    inst.inPorts[0] = count;
    inst.inPorts[1] = reset;
    inst.outL = output;
    nodes::counter::render(inst, 24);
    assert(inst.counter.count == 13 && output[23] == 13.0f);

    NodeInstance other;
    check(other, {0, 5}, {0, 0}, {0, 1});
    assert(inst.counter.count == 13);

    // The real portSample() also falls back to control values for MIDI/Button routes.
    other.outL = output;
    other.controlValue[0] = 0;
    nodes::counter::render(other, 1);
    other.controlValue[0] = 10;
    nodes::counter::render(other, 1);
    assert(output[0] == 2);
    other.controlValue[1] = 5;
    nodes::counter::render(other, 1);
    assert(output[0] == 0);
    other.controlValue[1] = 0;
    nodes::counter::render(other, 1);
    assert(output[0] == 0);

    other.counter.count = UINT32_MAX;
    other.counter.countWasHigh = false;
    nodes::counter::render(other, 1);
    assert(other.counter.count == UINT32_MAX);
    assert(output[0] == static_cast<float>(UINT32_MAX));
    std::puts("PASS native Counter: edges, threshold, held reset, simultaneous signals, block continuity, raw count, independent state, control ports, saturation");
    for (float bpm : {60.0f, 120.0f, 240.0f})
    {
        for (float pulseMs : {10.0f, 100.0f, 1000.0f})
        {
            synth::masterClock().bpm = bpm;
            NodeInstance clock, counter;
            clock.clock.pulseMs = pulseMs;
            float trigger[128], frequency[128], counts[128];
            clock.outL = trigger;
            clock.outR = frequency;
            counter.inPorts[0] = trigger;
            counter.outL = counts;
            const size_t beatSamples = static_cast<size_t>(synth::kSampleRate * 60.0f / bpm);
            for (size_t remaining = beatSamples; remaining > 0;)
            {
                const size_t frames = remaining < 128 ? remaining : 128;
                nodes::clock::render(clock, frames);
                nodes::counter::render(counter, frames);
                for (size_t i = 0; i < frames; ++i)
                    assert(frequency[i] == bpm);
                remaining -= frames;
            }
            assert(counter.counter.count == 4);
            synth::masterClock().bpm = 0;
            nodes::clock::render(clock, 128);
            for (float value : trigger) assert(value == 0);
        }
    }
    std::puts("PASS native Clock to Counter: four rising edges per beat at 60/120/240 BPM, long trigger separation, direct BPM output, stopped output");
    for (float pulseMs : {1.0f, 10.0f, 25.5f, 1000.0f})
    {
        NodeInstance mono;
        WidgetParams params;
        params.monoflopPulseMs = pulseMs;
        nodes::monoflop::build(mono, params);
        const size_t length = static_cast<size_t>(ceilf(pulseMs * 48.0f));
        std::vector<float> input(length + 257, 10), output(input.size());
        mono.inPorts[0] = input.data();
        mono.outL = output.data();
        nodes::monoflop::render(mono, 17);
        mono.inPorts[0] += 17;
        mono.outL += 17;
        nodes::monoflop::render(mono, input.size() - 17);
        for (size_t i = 0; i < output.size(); ++i) assert(output[i] == (i < length ? 10 : 0));
    }
    NodeInstance mono;
    nodes::monoflop::setParam(mono, "pulseMs", 1);
    float input[112] = {}, monoOutput[112] = {};
    input[0] = 5; // Equality does not exceed the threshold.
    input[1] = 5.01f;
    input[10] = 10; // Crossing while busy must not extend the pulse.
    input[60] = 10;
    mono.inPorts[0] = input;
    mono.outL = monoOutput;
    nodes::monoflop::render(mono, 112);
    for (size_t i = 0; i < 112; ++i)
        assert(monoOutput[i] == ((i >= 1 && i < 49) || (i >= 60 && i < 108) ? 10 : 0));

    NodeInstance controlled;
    controlled.outL = monoOutput;
    controlled.controlValue[0] = 4;
    nodes::monoflop::render(controlled, 1);
    assert(monoOutput[0] == 0);
    controlled.controlConnectedPortMask = synth::portBit(1);
    controlled.controlValue[1] = 3;
    nodes::monoflop::render(controlled, 1);
    assert(monoOutput[0] == 10);
    controlled.monoflop.samplesLeft = 0;
    controlled.monoflop.wasAbove = false;
    controlled.controlConnectedPortMask = 0;
    nodes::monoflop::render(controlled, 1);
    assert(monoOutput[0] == 0); // Local threshold restored after disconnect.
    float cvInput[] = {5, 5, 5, 5}, threshold[] = {10, 5, 4, -1};
    controlled.inPorts[0] = cvInput;
    controlled.inPorts[1] = threshold;
    controlled.audioConnectedPortMask = synth::portBit(1);
    nodes::monoflop::render(controlled, 4);
    assert(monoOutput[0] == 0 && monoOutput[1] == 0 && monoOutput[2] == 10 && monoOutput[3] == 10);
    assert(nodes::monoflop::clampThreshold(-10) == 0 && nodes::monoflop::clampThreshold(20) == 10);
    assert(nodes::monoflop::clampThreshold(std::numeric_limits<float>::quiet_NaN()) == 5);
    assert(nodes::monoflop::clampPulseMs(0) == 1 && nodes::monoflop::clampPulseMs(100000) == 60000);
    assert(!nodes::monoflop::setParam(controlled, "unknown", 1));
    WidgetParams defaults;
    nodes::monoflop::parsePreset(nullptr, defaults);
    assert(defaults.monoflopThreshold == 5 && defaults.monoflopPulseMs == 10);
    std::puts("PASS native Monoflop: exact durations, held high, block continuity, strict threshold, short input, busy crossings, rearming, control/audio threshold, fallback, validation");
    for (bool drum : {false, true})
    {
        for (float bpm : {0.0f, 60.0f, 73.5f, 90.0f, 120.0f, 240.0f, 300.0f})
        {
            for (int mode : {0, 1, 2}) // Master fallback, control BPM, audio BPM.
            {
                synth::masterClock().bpm = mode == 0 ? bpm : 37;
                NodeInstance sequencer, counter;
                nodes::steparp::StepArpState arp;
                DrumSequencerState drums;
                sequencer.stepArp = &arp;
                sequencer.drumSequencer = &drums;
                arp.gatePrev = drums.gatePrev = true;
                arp.internalClockRoundBpm = drums.internalClockRoundBpm = true; // Legacy settings cannot alter direct BPM.
                drums.trackPulseMs[0] = 10;
                memset(drums.patterns[0].active, 0xff, sizeof(drums.patterns[0].active));
                const int ratePort = drum ? 3 : 21;
                sequencer.controlValue[1] = 10;
                sequencer.controlValue[ratePort] = bpm;
                if (mode == 1) sequencer.controlConnectedPortMask = synth::portBit(ratePort);
                if (mode == 2) sequencer.audioConnectedPortMask = synth::portBit(ratePort);
                float left[128], right[128], tempo[128], counts[128];
                for (float &value : tempo) value = bpm;
                sequencer.outL = left;
                sequencer.outR = right;
                if (mode == 2) sequencer.inPorts[ratePort] = tempo;
                counter.inPorts[0] = drum ? left : right;
                counter.outL = counts;
                for (size_t remaining = 48480; remaining > 0;)
                {
                    const size_t frames = remaining < 128 ? remaining : 128;
                    if (drum) nodes::drumsequencer::render(sequencer, frames);
                    else nodes::steparp::render(sequencer, frames);
                    nodes::counter::render(counter, frames);
                    remaining -= frames;
                }
                assert(counter.counter.count == static_cast<uint32_t>(bpm / 15));
            }
        }
    }
    std::puts("PASS native Sequencers: raw BPM 0/60/90/120/240/300, Master Clock fallback, control/audio input, four steps per beat");
    {
        NodeInstance inst;
        nodes::steparp::StepArpState arp;
        assert(!arp.skipOnC0);
        inst.stepArp = &arp;
        inst.controlConnectedPortMask = synth::portBit(0);
        inst.controlValue[1] = 10;
        float cv[8], gate[8];
        inst.outL = cv;
        inst.outR = gate;
        arp.pulseMs = 1000;
        arp.stepVolts[0][0] = 6;
        arp.stepVolts[0][1] = 0;
        arp.stepVolts[0][2] = 7;
        const auto tick = [&]() {
            inst.controlValue[0] = 0;
            nodes::steparp::render(inst, 1);
            inst.controlValue[0] = 10;
            nodes::steparp::render(inst, 1);
        };
        arp.gatePrev = true;
        tick();
        assert(cv[0] == 6 && gate[0] == 10 && arp.currentStep == 1);
        tick(); // Disabled option: zero is played normally.
        assert(cv[0] == 0 && gate[0] == 10 && arp.currentStep == 2);
        arp.skipOnC0 = true;
        arp.currentStep = 0;
        tick();
        assert(cv[0] == 6 && gate[0] == 10);
        arp.pendingPulseSamples = 100;
        arp.retriggerGapRemaining = 4;
        tick();
        assert(cv[0] == 6 && gate[0] == 0 && arp.currentStep == 2);
        assert(arp.pulseSamplesRemaining == 0 && arp.pendingPulseSamples == 0 && arp.retriggerGapRemaining == 0);
        nodes::steparp::render(inst, 8);
        for (size_t i = 0; i < 8; ++i) assert(cv[i] == 6 && gate[i] == 0);
        tick();
        assert(cv[0] == 7 && gate[0] == 10 && arp.currentStep == 3);
        // An external step CV of zero wins over a nonzero knob, before Pitch.
        arp.stepVolts[0][3] = 9;
        inst.controlConnectedPortMask |= synth::portBit(7) | synth::portBit(2);
        inst.controlValue[7] = 0;
        inst.controlValue[2] = 9;
        tick();
        assert(cv[0] == 7 && gate[0] == 0 && arp.currentStep == 4);
        // An all-zero pattern advances normally, with no gate or CV changes.
        for (float &value : arp.stepVolts[0]) value = 0;
        inst.controlConnectedPortMask = synth::portBit(0);
        for (int i = 0; i < 16; ++i) {
            tick();
            assert(cv[0] == 7 && gate[0] == 0);
        }
        assert(arp.currentStep == 4);
        // Gate reset also respects a skipped first step.
        inst.controlValue[1] = 0;
        nodes::steparp::render(inst, 1);
        inst.controlValue[1] = 10;
        nodes::steparp::render(inst, 1);
        assert(cv[0] == 7 && gate[0] == 0 && arp.currentStep == 1);
        for (float direction : {3.0f, 6.0f, 9.0f}) {
            inst.controlValue[3] = direction;
            arp.currentStep = 3;
            bool forward = arp.pingPongForward;
            uint32_t rng = arp.rngState;
            const auto expected = nodes::steparp::nextStepIndex(3, arp.stepCount, nodes::steparp::directionFromCv(direction), forward, rng);
            tick();
            assert(arp.currentStep == expected && cv[0] == 7 && gate[0] == 0);
        }
    }
    std::puts("PASS native Skip on C0: default off, zero rests, CV hold, gate/pending pulse cancellation, external step CV, Pitch, all-zero patterns, Gate reset, directions");
    {
        NodeInstance inst;
        nodes::steparp::StepArpState arp;
        inst.stepArp = &arp;
        WidgetParams params;
        nodes::steparp::parsePreset(nullptr, params);
        assert(!params.stepArpSnapToSemitones);
        cJSON preset;
        preset.snapToSemitones = true;
        nodes::steparp::parsePreset(&preset, params);
        nodes::steparp::build(inst, params);
        assert(arp.snapToSemitones);
        float cv[1], gate[1];
        inst.outL = cv;
        inst.outR = gate;
        inst.controlConnectedPortMask = synth::portBit(0) | synth::portBit(4);
        inst.controlValue[1] = 10;
        arp.gatePrev = true;
        const auto trigger = [&]() {
            arp.currentStep = 0;
            inst.controlValue[0] = 0;
            nodes::steparp::render(inst, 1);
            inst.controlValue[0] = 10;
            nodes::steparp::render(inst, 1);
            return cv[0];
        };
        // Exhaust every MIDI CC value delivered as the default knob's 0-10 V.
        for (int cc = 0; cc <= 127; ++cc) {
            const float raw = static_cast<float>(cc) * 10.0f / 127.0f;
            inst.controlValue[4] = raw;
            const float expected = floorf(raw * 12.0f + 0.5f) / 12.0f;
            assert(fabsf(trigger() - expected) < 1e-5f);
        }
        // Audio/CV connections obey the same rule without a browser.
        float raw = 5.123f;
        inst.inPorts[4] = &raw;
        inst.audioConnectedPortMask = synth::portBit(4);
        assert(fabsf(trigger() - 5.083333f) < 1e-5f);
        assert(nodes::steparp::setParam(inst, "snapToSemitones", 0));
        assert(fabsf(trigger() - raw) < 1e-5f);
        nodes::steparp::setParam(inst, "snapToSemitones", 1);
        inst.inPorts[4] = nullptr;
        inst.audioConnectedPortMask = 0;
        inst.controlConnectedPortMask |= synth::portBit(2);
        inst.controlValue[2] = 6;
        for (float offset : {0.0f, 12.0f, 24.0f, 60.0f}) {
            nodes::steparp::setParam(inst, "maxSemitoneOffset", offset);
            for (int cc = 0; cc <= 127; ++cc) {
                const float value = static_cast<float>(cc) * 10.0f / 127.0f;
                inst.controlValue[4] = value;
                const float expected = 6 + floorf((value - 5) * offset / 5 + 0.5f) / 12.0f;
                assert(fabsf(trigger() - expected) < 1e-5f);
            }
        }
        // Local live parameter updates and existing stored values are quantized too.
        inst.controlConnectedPortMask = synth::portBit(0);
        nodes::steparp::setParam(inst, "step1", 5.123f);
        assert(fabsf(trigger() - 5.083333f) < 1e-5f);
        nodes::steparp::setParam(inst, "skipOnC0", 1);
        nodes::steparp::setParam(inst, "step1", 0.02f);
        const float previous = arp.heldCv;
        assert(trigger() == previous && gate[0] == 0);
    }
    std::puts("PASS native Snap to Semitones: persisted flag/build/live toggle, all 128 MIDI values, audio/CV control, local values, Pitch ranges, free mode, Skip on C0; no JavaScript");
    {
        NodeInstance inst;
        nodes::steparp::StepArpState arp;
        inst.stepArp = &arp;
        float cv[1], gate[1];
        inst.outL = cv;
        inst.outR = gate;
        arp.patternCount = 2;
        arp.stepCount = 8;
        for (int i = 0; i < 16; ++i) arp.stepVolts[1][i] = 8;
        inst.controlConnectedPortMask = synth::portBit(0);
        const auto tick = [&]() {
            inst.controlValue[0] = 0;
            nodes::steparp::render(inst, 1);
            inst.controlValue[0] = 10;
            nodes::steparp::render(inst, 1);
        };
        // Gate low: a new selection applies at once, and step edits go to it.
        nodes::steparp::setParam(inst, "activePatternIndex", 1);
        nodes::steparp::render(inst, 1);
        assert(arp.activePattern == 1);
        nodes::steparp::setParam(inst, "step1", 9);
        assert(arp.stepVolts[1][0] == 9 && arp.stepVolts[0][0] == 5);
        nodes::steparp::setParam(inst, "activePatternIndex", 0);
        // Switch at End while running: pattern 2 waits until step 8 has played.
        nodes::steparp::setParam(inst, "switchAtEnd", 1);
        inst.controlValue[1] = 10; // gate high plays step 1
        nodes::steparp::render(inst, 1);
        assert(arp.activePattern == 0 && cv[0] == 5);
        nodes::steparp::setParam(inst, "activePatternIndex", 1);
        assert(arp.activePattern == 0 && arp.pendingPattern == 1);
        for (int step = 2; step <= 7; ++step) tick();
        assert(arp.activePattern == 0 && cv[0] == 5);
        tick(); // step 8, back at step 1
        assert(arp.activePattern == 1 && arp.pendingPattern == nodes::steparp::kNoPendingPattern);
        tick();
        assert(cv[0] == 9);
        // Wired "pattern" CV (control-only) picks by equal bands and wins over the local choice.
        nodes::steparp::setParam(inst, "switchAtEnd", 0);
        inst.controlConnectedPortMask |= synth::portBit(nodes::steparp::kPatternPort);
        inst.controlValue[nodes::steparp::kPatternPort] = 2;
        nodes::steparp::render(inst, 1);
        assert(arp.activePattern == 0);
    }
    std::puts("PASS native Step Arp patterns: immediate switch, per-pattern edits, Switch at End on the sequence end, wired Pattern CV");
}
