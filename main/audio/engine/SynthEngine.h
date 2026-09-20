#pragma once

#include <atomic>
#include <cstdint>
#include <cstddef>
#include <cstdio>
#include <cstring>
#include <new>

#include "cJSON.h"
#include "driver/i2s_std.h"
#include "esp_codec_dev.h"
#include "esp_heap_caps.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/queue.h"
#include "freertos/task.h"

#include "CvConventions.h"
#include "DeviceSettings.h"
#include "DspPrimitives.h"
#include "EngineStats.h"
#include "Graph.h"
#include "GraphBuilder.h"
#include "LiveParamMsg.h"
#include "MasterClock.h"
#include "MemoryDiagnostics.h"
#include "PresetChangedMsg.h"
#include "PresetFill.h"
#include "PresetParser.h"
#include "SynthConfig.h"
#include "UsbMidiEvent.h"

// Owns the render task (pinned to core 1) and the live Graph, plus a second,
// low-priority loader task (core 0) that does the actual JSON-parse +
// GraphBuilder::build for a preset change off the real-time path - see
// loaderTaskLoop(). MIDI events and the loader task's finished Graph* are
// both funneled through queues drained by the render task itself -
// liveGraph_ is therefore only ever touched by that one task, so graph
// swap and MIDI-driven mutation of live node state never race against each
// other or against rendering, with no locks/atomics needed.
class SynthEngine
{
public:
    // `codecHandle` must already be open (Feature::Audio::ES8311::init()).
    // `secondaryI2sTxHandle` is an optional second, already-enabled raw I2S TX
    // channel (Feature::Audio::WM8960::get_tx_handle()) that receives the same
    // audio stream in parallel; pass nullptr if no secondary codec is
    // configured. Unlike the primary sink, the WM8960 path writes PCM directly
    // via i2s_channel_write() rather than esp_codec_dev - this project's WM8960
    // support (features/audio/wm8960_manager.hpp) is a hand-rolled raw-I2S
    // driver, not wrapped in esp_codec_dev.
    // `presetChangedQueue` and `liveParamQueue` are the same queues
    // Feature::Web's synth API routes were given. `initialPresetPath` is loaded
    // once at startup before the first preset-changed notification (if any)
    // arrives.
    void begin(esp_codec_dev_handle_t codecHandle, i2s_chan_handle_t secondaryI2sTxHandle,
               QueueHandle_t presetChangedQueue, QueueHandle_t liveParamQueue, const char *initialPresetPath);

    // Enqueues a USB-MIDI event for the render task to apply before its next
    // block. Non-blocking; drops the event if the queue is full rather than
    // stalling the caller (matches the drop-on-backpressure pattern already
    // used by UsbMidiClassDriver's own transfer-complete path). Safe to call
    // from any task.
    bool onMidiEvent(const UsbMidiEvent &event);

    // Audio-core utilization (percent, EMA-smoothed): processing time of a
    // block (queues + renderBlock + PCM conversion, WITHOUT the blocking
    // codec write operation) relative to the real-time budget
    // kBlockSize/kSampleRate. >=100% means the render task needs longer
    // than it is allotted per block (underrun risk). The only writer is the
    // render task itself; std::atomic makes the value safely readable by any
    // reading task (e.g. the periodic WS broadcast in main.cpp), without the
    // render task ever having to block.
    float audioCoreLoadPercent() const { return audioCoreLoadPercent_.load(std::memory_order_relaxed); }

    // Per-widget CPU-load debug display (see Graph::widgetLoadPercent): a
    // snapshot that the render task itself copies out of the
    // (render-task-owned, non-atomic) liveGraph_ after each renderBlock() -
    // see taskLoop(). widgetLoadIds_/widgetLoadPercent_/widgetLoadCount_ are
    // therefore safely readable by any other task (e.g. main.cpp's periodic
    // WS broadcast), without ever touching liveGraph_ itself (which stays
    // strictly render-task-only, see class comment above). idOutSize should
    // be >= sizeof(WidgetSlot::id) (24); idOut is always null-terminated.
    // Returns false (idOut/percentOut unchanged) for i >= widgetLoadCount().
    uint16_t widgetLoadCount() const { return widgetLoadCount_.load(std::memory_order_relaxed); }
    bool widgetLoadAt(uint16_t i, char *idOut, size_t idOutSize, float *percentOut) const
    {
        if (i >= widgetLoadCount_.load(std::memory_order_relaxed) || idOut == nullptr || idOutSize == 0 ||
            percentOut == nullptr)
        {
            return false;
        }
        snprintf(idOut, idOutSize, "%s", widgetLoadIds_[i]);
        *percentOut = widgetLoadPercent_[i].load(std::memory_order_relaxed);
        return true;
    }

    // Edge-summing counterpart to widgetLoadAt() above - same widget index
    // space (0..widgetLoadCount()-1), just the outgoing-edge-summing share of
    // that widget's cost instead of its own render() calls (see
    // Graph::widgetEdgeLoadPercent).
    float widgetEdgeLoadPercentAt(uint16_t i) const
    {
        return i < widgetLoadCount_.load(std::memory_order_relaxed)
                   ? widgetEdgeLoadPercent_[i].load(std::memory_order_relaxed)
                   : 0.0f;
    }

    // CV In/Out "device responding" status AND Digital In/Out "pin configured" status +
    // Digital In's live HIGH/LOW level, for the web UI's missing-device badge and Digital In's
    // on-widget level readout (see synth_web_api.hpp's cvStatus broadcast). Deliberately its
    // OWN scan in taskLoop() below,
    // NOT piggybacked on the widgetLoad* profiler above: that one rotates one widget per
    // block (fine for a slow debug percentage), far too slow for a badge that should appear
    // within a poll cycle or two. A full scan of every widget every block is cheap here (no
    // esp_timer_get_time() calls, just a handful of comparisons/copies at kMaxWidgets=64).
    uint16_t cvStatusCount() const { return cvStatusCount_.load(std::memory_order_relaxed); }
    bool cvStatusAt(uint16_t i, char *idOut, size_t idOutSize, bool *foundOut,
                    bool *isInputOut, float *voltsOut) const
    {
        if (i >= cvStatusCount_.load(std::memory_order_relaxed) || idOut == nullptr || idOutSize == 0 ||
            foundOut == nullptr || isInputOut == nullptr || voltsOut == nullptr)
        {
            return false;
        }
        snprintf(idOut, idOutSize, "%s", cvStatusIds_[i]);
        *foundOut = cvStatusFound_[i].load(std::memory_order_relaxed);
        *isInputOut = cvStatusIsInput_[i].load(std::memory_order_relaxed);
        *voltsOut = cvStatusVolts_[i].load(std::memory_order_relaxed);
        return true;
    }

    // P0 baseline diagnostics (documentation/native-synth-optimization-plan-
    // 2026-09-09.md, sections 7/8): the block-level phase breakdown inside
    // Graph::renderBlock (percent of block budget, EMA-smoothed) that the
    // per-widget profiler above can't see, plus the fast-path/instance
    // counters from the most recently completed block. Also logged natively
    // (no browser/JS involved, see taskLoop()) every couple seconds at
    // ESP_LOGI, matching the plan's "native C++ benchmark mode" request.
    struct PhaseSnapshot
    {
        float activityPercent = 0.0f; // voice-activity calc + input buffer clear/fill
        float renderPercent = 0.0f;   // whole renderOrder sweep: node DSP + edge-summing, every widget
        float outputPercent = 0.0f;   // final per-sink output copy
    };
    PhaseSnapshot phaseSnapshot() const
    {
        return {phaseActivityPercent_.load(std::memory_order_relaxed),
                phaseRenderPercent_.load(std::memory_order_relaxed),
                phaseOutputPercent_.load(std::memory_order_relaxed)};
    }
    struct BlockStatsSnapshot
    {
        uint32_t oscPitchConstant = 0;
        uint32_t oscPitchAudioRate = 0;
        uint32_t oscTuneConstant = 0;
        uint32_t oscTuneAudioRate = 0;
        uint32_t oscAmpConstant = 0;
        uint32_t oscAmpAudioRate = 0;
        uint32_t oscSilentSkipped = 0;
        uint32_t oscGainConverged = 0;
        // Mirrors enginestats::BlockCounters' fm* fields - see EngineStats.h.
        uint32_t fmPitchConstant = 0;
        uint32_t fmPitchAudioRate = 0;
        uint32_t fmTuneConstant = 0;
        uint32_t fmTuneAudioRate = 0;
        uint32_t fmAmpConstant = 0;
        uint32_t fmAmpAudioRate = 0;
        uint32_t fmAmpConstantZero = 0;
        uint32_t fmSilentSkipped = 0;
        uint32_t fmGainConverged = 0;
        uint32_t fmModActive = 0;
        uint32_t fmFeedbackActive = 0;
        uint32_t fmSuperSawActive = 0;
        uint32_t dxHolding = 0;
        uint32_t dxRamping = 0;
        uint32_t dxBoundary = 0;
        uint32_t dxShapeConfigured = 0;
        uint32_t instancesTotal = 0;
        uint32_t instancesRendered = 0;
        uint32_t instancesRenderedWhileIdle = 0;
    };
    BlockStatsSnapshot blockStatsSnapshot() const
    {
        BlockStatsSnapshot s;
        s.oscPitchConstant = oscPitchConstant_.load(std::memory_order_relaxed);
        s.oscPitchAudioRate = oscPitchAudioRate_.load(std::memory_order_relaxed);
        s.oscTuneConstant = oscTuneConstant_.load(std::memory_order_relaxed);
        s.oscTuneAudioRate = oscTuneAudioRate_.load(std::memory_order_relaxed);
        s.oscAmpConstant = oscAmpConstant_.load(std::memory_order_relaxed);
        s.oscAmpAudioRate = oscAmpAudioRate_.load(std::memory_order_relaxed);
        s.oscSilentSkipped = oscSilentSkipped_.load(std::memory_order_relaxed);
        s.oscGainConverged = oscGainConverged_.load(std::memory_order_relaxed);
        s.fmPitchConstant = fmPitchConstant_.load(std::memory_order_relaxed);
        s.fmPitchAudioRate = fmPitchAudioRate_.load(std::memory_order_relaxed);
        s.fmTuneConstant = fmTuneConstant_.load(std::memory_order_relaxed);
        s.fmTuneAudioRate = fmTuneAudioRate_.load(std::memory_order_relaxed);
        s.fmAmpConstant = fmAmpConstant_.load(std::memory_order_relaxed);
        s.fmAmpAudioRate = fmAmpAudioRate_.load(std::memory_order_relaxed);
        s.fmAmpConstantZero = fmAmpConstantZero_.load(std::memory_order_relaxed);
        s.fmSilentSkipped = fmSilentSkipped_.load(std::memory_order_relaxed);
        s.fmGainConverged = fmGainConverged_.load(std::memory_order_relaxed);
        s.fmModActive = fmModActive_.load(std::memory_order_relaxed);
        s.fmFeedbackActive = fmFeedbackActive_.load(std::memory_order_relaxed);
        s.fmSuperSawActive = fmSuperSawActive_.load(std::memory_order_relaxed);
        s.dxHolding = dxHolding_.load(std::memory_order_relaxed);
        s.dxRamping = dxRamping_.load(std::memory_order_relaxed);
        s.dxBoundary = dxBoundary_.load(std::memory_order_relaxed);
        s.dxShapeConfigured = dxShapeConfigured_.load(std::memory_order_relaxed);
        s.instancesTotal = instancesTotal_.load(std::memory_order_relaxed);
        s.instancesRendered = instancesRendered_.load(std::memory_order_relaxed);
        s.instancesRenderedWhileIdle = instancesRenderedWhileIdle_.load(std::memory_order_relaxed);
        return s;
    }

    struct TimingSnapshot
    {
        uint32_t sampleCount = 0;
        uint32_t p50Us = 0;
        uint32_t p95Us = 0;
        uint32_t p99Us = 0;
        uint32_t peakUs = 0;
    };
    TimingSnapshot consumeTimingSnapshot();
    uint32_t renderOverrunCount() const { return renderOverrunCount_.load(std::memory_order_relaxed); }
    uint32_t midiDropCount() const { return midiDropCount_.load(std::memory_order_relaxed); }
    uint16_t activeSoundFontVoices() const { return activeSoundFontVoices_.load(std::memory_order_relaxed); }
    bool presetLoadActive() const { return presetLoadActive_.load(std::memory_order_acquire); }
    uint8_t presetLoadProgress() const { return presetLoadProgress_.load(std::memory_order_relaxed); }
    void beginPresetLoad()
    {
        presetLoadProgress_.store(5, std::memory_order_relaxed);
        presetLoadActive_.store(true, std::memory_order_release);
    }
    void cancelPresetLoad() { presetLoadActive_.store(false, std::memory_order_release); }
    struct MidiVoiceUsageSnapshot
    {
        uint16_t active = 0;
        uint16_t maximum = 0;
    };
    MidiVoiceUsageSnapshot midiVoiceUsage() const
    {
        const uint32_t packed = midiVoiceUsagePacked_.load(std::memory_order_acquire);
        return {static_cast<uint16_t>(packed & 0xFFFFu), static_cast<uint16_t>(packed >> 16)};
    }
    uint32_t presetChangeSequence() const { return presetChangeSequence_.load(std::memory_order_acquire); }
    // Bumped once per real preset reload (a new/re-saved file, see loaderTaskLoop's `gotMsg`)
    // or Panic - deliberately NOT on a cpuReloadRequested_ overload-watchdog reload, since that
    // one reloads *because of* an overload that may itself be producing real DMA underruns, and
    // resetting right then would erase the evidence. Callers (Feature::Synth::dma_underrun_tick
    // in synth_manager.hpp) poll this to know when to zero the true DMA-underrun counters.
    uint32_t underrunResetSequence() const { return underrunResetSequence_.load(std::memory_order_relaxed); }
    void readCurrentPresetName(char *out, size_t outSize) const
    {
        if (out == nullptr || outSize == 0) return;
        size_t length = 0;
        while (length + 1 < outSize && length + 1 < sizeof(currentPresetName_) &&
               currentPresetName_[length] != '\0')
        {
            ++length;
        }
        memcpy(out, currentPresetName_, length);
        out[length] = '\0';
    }

    struct MidiControlSnapshot
    {
        enum class Kind : uint8_t
        {
            Knob = 0,
            Button = 1,
            UpDownButton = 2,
            Switch = 3,
        };

        uint32_t sequence = 0;
        char widgetLabel[48] = {};
        Kind kind = Kind::Knob;
        uint8_t direction = 0; // UpDownButton: 1=up, 2=down
        uint8_t channel = 0;
        uint8_t cc = 0;
        float raw = 0.0f;
        float value = 0.0f;
    };

    bool readMidiControlSnapshot(MidiControlSnapshot &out) const
    {
        const uint32_t sequence = midiControlSequence_.load(std::memory_order_acquire);
        if (sequence == 0) return false;
        memcpy(out.widgetLabel, midiControlWidgetLabel_, sizeof(out.widgetLabel));
        out.kind = static_cast<MidiControlSnapshot::Kind>(midiControlKind_.load(std::memory_order_relaxed));
        out.direction = midiControlDirection_.load(std::memory_order_relaxed);
        out.channel = midiControlChannel_.load(std::memory_order_relaxed);
        out.cc = midiControlCc_.load(std::memory_order_relaxed);
        out.raw = midiControlRaw_.load(std::memory_order_relaxed);
        out.value = midiControlValue_.load(std::memory_order_relaxed);
        out.sequence = sequence;
        return true;
    }

    // Set whenever a build (see buildFromFile) had to use fewer voices than
    // requested - either because the requested count wouldn't fit in
    // internal RAM alone (see GraphBuilder::build's retry-with-fewer-voices
    // comment; voicesCapReason() 0), or because the render task detected the
    // *previous* voice count sustainably exceeding the real-time budget and
    // asked for one fewer (see taskLoop's overload watchdog; reason 1).
    // voicesCapEventId() increments on every such occurrence (including
    // repeats of the same requested/effective pair) so a poller (main.cpp's
    // loop()) can tell "there's a new one to report" apart from "still the
    // same one already broadcast" - a plain latest-value pair alone can't
    // distinguish those two cases.
    uint32_t voicesCapEventId() const { return voicesCapEventId_.load(std::memory_order_relaxed); }
    uint16_t voicesCapRequested() const { return voicesCapRequested_.load(std::memory_order_relaxed); }
    uint16_t voicesCapEffective() const { return voicesCapEffective_.load(std::memory_order_relaxed); }
    uint8_t voicesCapReason() const { return voicesCapReason_.load(std::memory_order_relaxed); }

    void requestPanic()
    {
        panicClearMidiRequested_.store(true, std::memory_order_relaxed);
        panicReloadRequested_.store(true, std::memory_order_relaxed);
    }

    // Optional USB-audio tap: submitOut() hands the produced block over as
    // interleaved stereo PCM (device->host), mixHostIn() mixes received host
    // audio (host->device) into the codec block. Both as plain function
    // pointers, to avoid coupling the audio engine to the USB layer. main.cpp
    // sets these to UsbAudioDevice calls.
    using TapSubmit = void (*)(const int16_t *interleaved, size_t samples);
    using TapMix = void (*)(float *blockL, float *blockR, size_t frames);
    void setUsbAudioTap(TapSubmit submitOut, TapMix mixHostIn)
    {
        submitOut_ = submitOut;
        mixHostIn_ = mixHostIn;
    }

    // Optional MIDI-CC-out sink: fires whenever a MIDI-learned Knob's value
    // changes so a controller with LED rings/motorized faders can mirror it
    // back (e.g. a preset loaded with different values than the hardware is
    // currently showing). Same "plain function pointer" reasoning as
    // TapSubmit/TapMix above - synth_manager.hpp sets this to a wrapper that
    // reaches into the USB-host/BLE MIDI transports, the engine itself stays
    // unaware of which transport(s) are even active.
    using MidiCcOut = void (*)(uint8_t channel, uint8_t cc, uint8_t value7bit);
    void setMidiCcOutCallback(MidiCcOut cb) { midiCcOut_ = cb; }
    MidiCcOut midiCcOutCallback() const { return midiCcOut_; }

    // Public entry point for a live (browser-driven) Knob change to also
    // light up the on-device activity overlay - the same one a real
    // incoming MIDI CC message already lights up (see
    // publishMidiControlActivity, private below). Called from
    // applyLiveParamMessage with the KnobLiveChangeInfo Graph::
    // applyLiveParams reports back.
    void publishKnobActivityFromUi(const char *label, uint8_t channel, uint8_t cc, uint8_t raw, float value)
    {
        publishMidiControlSnapshot(label, MidiControlSnapshot::Kind::Knob, 0, channel, cc, raw, value);
    }

    // Per-sink output routing. The master stream (graph's AudioOut) is
    // weighted per output target (USB-mic-IN, ES8311, WM8960) individually
    // via gain/mute. Only read/written on the render task (ws "paramUpdate"
    // is wired there) - hence no locks needed.
    struct SinkParams
    {
        float gain = 1.0f;
        bool muted = false;
    };
    // index: 0=USB, 1=ES8311, 2=WM8960
    const SinkParams &sink(size_t index) const { return sink_[index]; }

    // Applies an already-parsed reserved "@out:*" paramUpdate to a hardware
    // output sink. Only ever called on the render task.
    bool applyOutputParam(const char *widgetId, cJSON *params);

private:
    void publishMidiVoiceUsage()
    {
        uint16_t active = 0;
        uint16_t maximum = 0;
        if (liveGraph_ != nullptr)
        {
            liveGraph_->midiVoiceUsage(active, maximum);
        }
        midiVoiceUsagePacked_.store((static_cast<uint32_t>(maximum) << 16) | active,
                                    std::memory_order_release);
    }

    // Mirrors every MIDI-learned Knob's current value back out to a
    // connected controller (see setMidiCcOutCallback) - called once right
    // after a preset finishes loading, so a controller with LED rings/
    // motorized faders shows what the just-loaded preset actually set,
    // not whatever position it was left at before the reload.
    void publishLearnedKnobsToMidiOut()
    {
        if (midiCcOut_ == nullptr || liveGraph_ == nullptr)
        {
            return;
        }
        for (uint16_t w = 0; w < liveGraph_->widgetCount; ++w)
        {
            const WidgetSlot &widget = liveGraph_->widgets[w];
            if (widget.type != NodeType::Knob)
            {
                continue;
            }
            const KnobState &knob = *liveGraph_->instances[widget.instanceBase].knob;
            for (uint8_t i = 0; i < knob.count; ++i)
            {
                const KnobControl &control = knob.controls[i];
                if (control.cc < 0 || control.channel < 0)
                {
                    continue;
                }
                const float raw = control.raw < 0.0f ? 0.0f : (control.raw > 127.0f ? 127.0f : control.raw);
                midiCcOut_(static_cast<uint8_t>(control.channel), static_cast<uint8_t>(control.cc),
                           static_cast<uint8_t>(raw + 0.5f));
            }
        }
    }

    void setCurrentPresetNameFromPath(const char *path)
    {
        currentPresetName_[0] = '\0';
        if (path == nullptr || strcmp(path, presetpaths::unnamedPath()) == 0) return;
        const char *base = strrchr(path, '/');
        base = base != nullptr ? base + 1 : path;
        size_t length = strlen(base);
        if (length >= 5 && strcmp(base + length - 5, ".json") == 0) length -= 5;
        if (length >= sizeof(currentPresetName_)) length = sizeof(currentPresetName_) - 1;
        memcpy(currentPresetName_, base, length);
        currentPresetName_[length] = '\0';
    }

    void publishMidiControlActivity(const UsbMidiEvent &event)
    {
        if (liveGraph_ == nullptr || event.codeIndex != 0xB) return;
        const uint8_t channel = event.status & 0x0F;
        for (uint16_t w = 0; w < liveGraph_->widgetCount; ++w)
        {
            const WidgetSlot &widget = liveGraph_->widgets[w];
            NodeInstance &instance = liveGraph_->instances[widget.instanceBase];
            const char *label = widget.label[0] != '\0' ? widget.label : widget.id;

            if (widget.type == NodeType::Knob && instance.knob != nullptr)
            {
                // A Knob widget can bundle several physical controls (see
                // KnobNode.h's count/controls[]) - check every control's
                // (cc, channel) pair, not just the first, or turning any
                // pot but the first would never light up the activity panel.
                for (uint8_t controlIndex = 0; controlIndex < instance.knob->count; ++controlIndex)
                {
                    const KnobControl &binding = instance.knob->controls[controlIndex];
                    if (binding.cc >= 0 && static_cast<uint8_t>(binding.cc) == event.data1 &&
                        binding.channel >= 0 && static_cast<uint8_t>(binding.channel) == channel)
                    {
                        const KnobControl &control = instance.knob->controls[
                            KnobState::controlIndex(controlIndex, instance.knob->activeLayer)];
                        publishMidiControlSnapshot(label, MidiControlSnapshot::Kind::Knob, controlIndex, channel,
                                                   event.data1, event.data2, control.currentValue);
                        break;
                    }
                }
            }
            else if (widget.type == NodeType::Button && instance.button != nullptr)
            {
                for (uint8_t buttonIndex = 0; buttonIndex < instance.button->count; ++buttonIndex)
                {
                    const ButtonControl &binding = instance.button->controls[buttonIndex];
                    if (binding.cc < 0 || static_cast<uint8_t>(binding.cc) != event.data1 ||
                        binding.channel < 0 || static_cast<uint8_t>(binding.channel) != channel)
                        continue;
                    const ButtonControl &control = instance.button->controls[
                        ButtonState::controlIndex(buttonIndex, instance.button->activeLayer)];
                    publishMidiControlSnapshot(label, MidiControlSnapshot::Kind::Button, buttonIndex, channel,
                                               event.data1, event.data2, control.currentValue);
                    break;
                }
            }
            else if (widget.type == NodeType::UpDownButton && instance.upDownButton != nullptr)
            {
                auto &upDown = *instance.upDownButton;
                const bool matchedUp = upDown.ccUp >= 0 && static_cast<uint8_t>(upDown.ccUp) == event.data1 &&
                                       upDown.channelUp >= 0 && static_cast<uint8_t>(upDown.channelUp) == channel;
                const bool matchedDown = upDown.ccDown >= 0 && static_cast<uint8_t>(upDown.ccDown) == event.data1 &&
                                         upDown.channelDown >= 0 && static_cast<uint8_t>(upDown.channelDown) == channel;
                if (matchedUp)
                {
                    publishMidiControlSnapshot(label, MidiControlSnapshot::Kind::UpDownButton, 1, channel,
                                               event.data1, event.data2, upDown.currentOutput());
                }
                if (matchedDown)
                {
                    publishMidiControlSnapshot(label, MidiControlSnapshot::Kind::UpDownButton, 2, channel,
                                               event.data1, event.data2, upDown.currentOutput());
                }
            }
            else if (widget.type == NodeType::Switch &&
                     instance.switchState.cc >= 0 && static_cast<uint8_t>(instance.switchState.cc) == event.data1 &&
                     instance.switchState.channel >= 0 && static_cast<uint8_t>(instance.switchState.channel) == channel)
            {
                publishMidiControlSnapshot(label, MidiControlSnapshot::Kind::Switch, 0, channel,
                                           event.data1, event.data2, instance.switchState.selectIn2 ? 1.0f : 0.0f);
            }
        }
    }

    void publishMidiControlSnapshot(const char *label, MidiControlSnapshot::Kind kind, uint8_t direction,
                                    uint8_t channel, uint8_t cc, uint8_t raw, float value)
    {
        snprintf(midiControlWidgetLabel_, sizeof(midiControlWidgetLabel_), "%s", label != nullptr ? label : "");
        midiControlKind_.store(static_cast<uint8_t>(kind), std::memory_order_relaxed);
        midiControlDirection_.store(direction, std::memory_order_relaxed);
        midiControlChannel_.store(channel, std::memory_order_relaxed);
        midiControlCc_.store(cc, std::memory_order_relaxed);
        midiControlRaw_.store(static_cast<float>(raw), std::memory_order_relaxed);
        midiControlValue_.store(value, std::memory_order_relaxed);
        midiControlSequence_.fetch_add(1, std::memory_order_release);
    }

    static void taskEntry(void *arg);
    void taskLoop();
    // voiceCeiling caps the voice count regardless of what the preset's
    // MidiIn widget(s) request - default SYNTH_VOICE_COUNT means "no
    // additional restriction beyond the preset itself and GraphBuilder's own
    // internal-RAM-fit retry". Only the overload-watchdog reload path (see
    // taskLoop/loaderTaskLoop) passes a tighter one.
    // allowPreFree: whether this rebuild may ask the render task to free
    // liveGraph_ first (see preFreeRequested_ comment). Must be
    // false for taskLoop()'s own initial boot call - that call runs
    // synchronously ON the render task itself, before its while(true) loop
    // (the only place that services preFreeRequested_) has even started, so
    // the request could never be answered. Harmless either way at boot
    // anyway: liveGraph_ there is already just the trivial Graph::allocate()
    // placeholder, so there is no old-graph RAM to free.
    Graph *buildFromFile(const char *path, uint16_t voiceCeiling = SYNTH_VOICE_COUNT, bool allowPreFree = true,
                         bool forcePreFree = false);

    // JSON parse + GraphBuilder::build can "easily take tens of ms" (see the
    // overrun log comment below) - far above the ~2.7ms block budget at
    // 128/48000. If that ran in the render task, even a muted block would
    // arrive far too late at the codec and let its DMA ring run dry - a
    // click that depends purely on timing and is NOT fixed by the loudness
    // fade alone. Hence a separate, low-priority task (on core 0, never core
    // 1) parses/builds the new graph; the render task only receives the
    // finished Graph* (cheap pointer swap) via builtGraphQueue_.
    static void loaderTaskEntry(void *arg);
    void loaderTaskLoop();

    struct BuiltGraphMsg
    {
        Graph *graph = nullptr;
        char path[300] = {};
    };

    esp_codec_dev_handle_t codecHandle_ = nullptr;
    i2s_chan_handle_t secondaryI2sTxHandle_ = nullptr;
    QueueHandle_t presetChangedQueue_ = nullptr;
    QueueHandle_t liveParamQueue_ = nullptr;
    QueueHandle_t midiEventQueue_ = nullptr;
    QueueHandle_t builtGraphQueue_ = nullptr; // length 1; loader task -> render task, fully-built Graph* ready to swap in
    QueueHandle_t retiredGraphQueue_ = nullptr; // render task -> loader task; heap destruction stays off the real-time core
    TaskHandle_t taskHandle_ = nullptr;
    TaskHandle_t loaderTaskHandle_ = nullptr;
    char currentPresetPath_[300] = {}; // matches PresetChangedMsg::path - see its comment for the sizing rationale; render-task-owned (only ever written at swap time in taskLoop), see loaderKnownPresetPath_ below for the loader task's own copy
    char currentPresetName_[presetpaths::kMaxNameLen] = {};
    char loaderKnownPresetPath_[300] = {}; // loader-task-owned twin of currentPresetPath_ - same initial value (set once in begin(), before either task runs), kept in sync independently within loaderTaskLoop as it processes each PresetChangedMsg. Exists solely so the overload-watchdog reload (see taskLoop) has a path to rebuild from without the loader task reading currentPresetPath_ across tasks (a plain char[300], not atomic - the render task can be mid-write to it right as a reload starts)

    Graph *liveGraph_ = nullptr; // owned by, and only ever accessed on, the render task

    TapSubmit submitOut_ = nullptr;
    TapMix mixHostIn_ = nullptr;
    MidiCcOut midiCcOut_ = nullptr;

    SinkParams sink_[3];

    // Master-output fade, multiplied across ALL sinks over a preset reload
    // (in addition to their own gain), to avoid the click that a hard graph
    // swap would otherwise cause: instead of fading the new graph in at full
    // loudness, first fade to silence, then swap the graph, then fade back in.
    enum class OutputFadeState : uint8_t
    {
        Idle,
        FadingOut,
        FadingIn,
    };
    OutputFadeState outputFadeState_ = OutputFadeState::Idle;
    float outputFadeMul_ = 1.0f; // currently effective multiplier, ramped sample-accurately
    Graph *pendingGraph_ = nullptr; // fully built by the loader task, waits for the swap after fade-out
    char pendingPresetPath_[300] = {}; // path of pendingGraph_, for currentPresetPath_ after the swap

    // Pre-free handshake (see buildFromFile()): a rebuild briefly needs room
    // for OLD+NEW simultaneously (the old graph still renders while the
    // loader task builds the new one) - more internal RAM peak than boot ever
    // needed (there the "old" graph is only an empty placeholder).
    // preFreeRequested_ is the loader task's request to the
    // render task to free liveGraph_ NOW (via fade-out, then placeholder),
    // BEFORE building at all; preFreeDone_ is the confirmation back.
    // preFreeInFlight_ is a pure render-task-owned state flag (no atomic
    // needed, like liveGraph_ itself) and distinguishes in the fade-swap code
    // block "this fade-out is a pre-free" from "this fade-out is a normal
    // pendingGraph_ swap". Same relaxed-atomic pattern as cpuReloadRequested_/
    // cpuReloadPending_ below - no new sync style.
    bool preFreeInFlight_ = false;
    std::atomic<bool> preFreeRequested_{false};
    std::atomic<bool> preFreeDone_{false};

    // Overload-Watchdog (render task only, taskLoop): tracks how long
    // audioCoreLoadPercent_ has been continuously above kSynthEngineOverloadPercentThreshold
    // (see SynthEngine.cpp) - 0 means "not currently in a streak". A single
    // spike (a preset reload, a MIDI chord burst, a flurry of live-param
    // updates - see the Audio-Overrun log comment) decays out of the EMA
    // within its ~53ms time constant and never reaches the sustained
    // threshold; only a graph that's genuinely too heavy stays elevated long
    // enough to trigger a reduced-voice reload.
    int64_t overloadStartUs_ = 0;

    std::atomic<float> audioCoreLoadPercent_{0.0f};

    // Per-widget CPU-load snapshot (see widgetLoadAt() above) - IDs are pure
    // char arrays (only overwritten by the render task, very rarely on a
    // preset reload; a torn read by another task would at most be a briefly
    // garbled debug display, no crash, self-corrects on the next snapshot),
    // while the percent values are real atomics (change every block).
    char widgetLoadIds_[synth::kMaxWidgets][24] = {};
    std::atomic<float> widgetLoadPercent_[synth::kMaxWidgets] = {};
    std::atomic<float> widgetEdgeLoadPercent_[synth::kMaxWidgets] = {};
    std::atomic<uint16_t> widgetLoadCount_{0};
    const Graph *widgetLoadGraph_ = nullptr;
    uint32_t widgetLoadGeneration_ = 0;

    char cvStatusIds_[synth::kMaxWidgets][24] = {};
    std::atomic<bool> cvStatusFound_[synth::kMaxWidgets] = {};
    std::atomic<bool> cvStatusIsInput_[synth::kMaxWidgets] = {};
    std::atomic<float> cvStatusVolts_[synth::kMaxWidgets] = {};
    std::atomic<uint16_t> cvStatusCount_{0};

    // P0 baseline diagnostics snapshots (see phaseSnapshot()/
    // blockStatsSnapshot() above) - published once per block from the render
    // task alongside the widget-load sweep, read by any task.
    std::atomic<float> phaseActivityPercent_{0.0f};
    std::atomic<float> phaseRenderPercent_{0.0f};
    std::atomic<float> phaseOutputPercent_{0.0f};
    std::atomic<uint32_t> oscPitchConstant_{0};
    std::atomic<uint32_t> oscPitchAudioRate_{0};
    std::atomic<uint32_t> oscTuneConstant_{0};
    std::atomic<uint32_t> oscTuneAudioRate_{0};
    std::atomic<uint32_t> oscAmpConstant_{0};
    std::atomic<uint32_t> oscAmpAudioRate_{0};
    std::atomic<uint32_t> oscSilentSkipped_{0};
    std::atomic<uint32_t> oscGainConverged_{0};
    std::atomic<uint32_t> fmPitchConstant_{0};
    std::atomic<uint32_t> fmPitchAudioRate_{0};
    std::atomic<uint32_t> fmTuneConstant_{0};
    std::atomic<uint32_t> fmTuneAudioRate_{0};
    std::atomic<uint32_t> fmAmpConstant_{0};
    std::atomic<uint32_t> fmAmpAudioRate_{0};
    std::atomic<uint32_t> fmAmpConstantZero_{0};
    std::atomic<uint32_t> fmSilentSkipped_{0};
    std::atomic<uint32_t> fmGainConverged_{0};
    std::atomic<uint32_t> fmModActive_{0};
    std::atomic<uint32_t> fmFeedbackActive_{0};
    std::atomic<uint32_t> fmSuperSawActive_{0};
    std::atomic<uint32_t> dxHolding_{0};
    std::atomic<uint32_t> dxRamping_{0};
    std::atomic<uint32_t> dxBoundary_{0};
    std::atomic<uint32_t> dxShapeConfigured_{0};
    std::atomic<uint32_t> instancesTotal_{0};
    std::atomic<uint32_t> instancesRendered_{0};
    std::atomic<uint32_t> instancesRenderedWhileIdle_{0};

    std::atomic<uint32_t> renderPeakUs_{0};
    static constexpr uint32_t kTimingHistogramBinWidthUs = 64;
    static constexpr size_t kTimingHistogramBinCount = 128;
    std::atomic<uint32_t> timingHistogram_[kTimingHistogramBinCount] = {};
    std::atomic<uint32_t> renderOverrunCount_{0};
    std::atomic<uint32_t> midiDropCount_{0};
    std::atomic<uint16_t> activeSoundFontVoices_{0};
    // Loader stages, not byte-exact file progress: SoundFont/WAV parsing has
    // no useful byte callback. Read only by the display task.
    std::atomic<bool> presetLoadActive_{false};
    std::atomic<uint8_t> presetLoadProgress_{0};
    std::atomic<uint32_t> midiVoiceUsagePacked_{0};
    std::atomic<uint32_t> presetChangeSequence_{0};
    std::atomic<uint32_t> underrunResetSequence_{0};
    char midiControlWidgetLabel_[48] = {};
    std::atomic<uint8_t> midiControlKind_{0};
    std::atomic<uint8_t> midiControlDirection_{0};
    std::atomic<uint8_t> midiControlChannel_{0};
    std::atomic<uint8_t> midiControlCc_{0};
    std::atomic<float> midiControlRaw_{0.0f};
    std::atomic<float> midiControlValue_{0.0f};
    std::atomic<uint32_t> midiControlSequence_{0};
    std::atomic<uint32_t> voicesCapEventId_{0};
    std::atomic<uint16_t> voicesCapRequested_{0};
    std::atomic<uint16_t> voicesCapEffective_{0};
    std::atomic<uint8_t> voicesCapReason_{0}; // 0 = psram (GraphBuilder's own internal-RAM-fit retry), 1 = cpu (overload watchdog)

    // currentVoiceCount_: the most recently built graph's voice count,
    // updated at the end of every buildFromFile() call (loader task
    // normally, or the render task itself for the very first boot build) -
    // read by the overload watchdog to know what to request one fewer of.
    // Not necessarily the *live* graph's count if a different reload is
    // still fading in, but that's an acceptable proxy here: the watchdog
    // only fires after several continuous seconds of overload, by which
    // point an unrelated concurrent reload is rare, and if it does happen
    // the watchdog simply corrects itself again on its next cycle if still
    // overloaded. cpuReloadRequested_/cpuReloadCeiling_ are the watchdog's
    // one-shot request to the loader task (polled in loaderTaskLoop
    // alongside presetChangedQueue_); cpuReloadPending_ blocks the watchdog
    // from firing again while a previously requested reload hasn't swapped
    // in yet - cleared at swap time in taskLoop.
    std::atomic<uint16_t> currentVoiceCount_{1};
    std::atomic<bool> cpuReloadRequested_{false};
    std::atomic<bool> cpuReloadPending_{false};
    std::atomic<uint16_t> cpuReloadCeiling_{0};
    std::atomic<bool> panicClearMidiRequested_{false};
    std::atomic<bool> panicReloadRequested_{false};
};

namespace
{
constexpr const char *kSynthEngineTag = "synth_engine";
constexpr UBaseType_t kSynthEngineMidiQueueDepth = 32;
constexpr uint32_t kSynthEngineTaskStackSize = 32768; // reverb comb-summing + port-summing are the deepest call paths; the USB-audio mix (mixHostAudioInto) additionally allocates temporary buffers
constexpr uint32_t kSynthEngineLoaderTaskStackSize = 8192; // only JSON parse + GraphBuilder::build, no DSP buffers like the render task

// Real-time budget of one block in microseconds - the time actually
// available between two codec write operations.
constexpr float kSynthEngineBlockBudgetUs = 1000000.0f * static_cast<float>(synth::kBlockSize) / static_cast<float>(synth::kSampleRate);
// EMA smoothing of the CPU-load display: time constant ~ kSynthEngineBlockBudgetUs/kSynthEngineLoadEmaAlpha
// (~53ms at 128/48000), smooths block-to-block jitter for a calmly
// readable percentage display in the browser, without smearing real load
// spikes.
constexpr float kSynthEngineLoadEmaAlpha = 0.05f;

// Overload watchdog (see SynthEngine::taskLoop, overloadStartUs_): how far
// above budget audioCoreLoadPercent_ must sit, and for how long continuously,
// before it's treated as "this graph is genuinely too heavy" rather than a
// transient spike (preset reload, MIDI chord burst, live-param flurry - see
// the Audio-Overrun log comment below for the usual causes of those). 130%
// leaves headroom above the ~100% mark real-time playback already tolerates
// briefly; 2s is roughly 38x the EMA's own ~53ms time constant, long enough
// that only sustained load keeps it there.
constexpr float kSynthEngineOverloadPercentThreshold = 130.0f;
constexpr int64_t kSynthEngineOverloadSustainedUs = 2000000;

// Duration of the master-output fade in/out around a preset reload (see
// SynthEngine::outputFadeState_). 8ms is short enough not to be noticeable
// on a preset switch, but long enough (~3 blocks at 128/48000) to smear any
// click artifact from the hard graph swap.
constexpr float kSynthEngineOutputFadeMs = 8.0f;
constexpr size_t kSynthEngineOutputFadeSamples =
    static_cast<size_t>(synth::kSampleRate * kSynthEngineOutputFadeMs / 1000.0f);
constexpr float kSynthEngineOutputFadeStep = 1.0f / static_cast<float>(kSynthEngineOutputFadeSamples);

// Master Clock's own MIDI-CC-learn/mapping - same "runs natively on the ESP
// regardless of whether a browser is even connected" reasoning as Knob's CC
// branch in Graph::handleMidiEvent, but engine-global (not a graph widget) so
// it keeps working across a preset switch/rebuild. Only updates the live bpm
// in RAM - see MasterClock.h's saveMasterClockToFs() comment for why this
// path never touches flash.
inline void handleMasterClockMidi(const UsbMidiEvent &event)
{
    if (event.codeIndex != 0xB) return; // Control Change
    auto &mc = synth::masterClock();
    if (mc.midiCc < 0 || static_cast<uint8_t>(mc.midiCc) != event.data1) return;
    if (mc.midiChannel < 0 || static_cast<uint8_t>(mc.midiChannel) != (event.status & 0x0F)) return;
    mc.bpm = (static_cast<float>(event.data2) / 127.0f) * mc.maxBpm;
}

// Parse each queued frame once, then route the resolved fields either to the
// engine's hardware sinks or to the live graph.
void applyLiveParamMessage(SynthEngine &engine, Graph &graph, const char *json)
{
    cJSON *root = cJSON_Parse(json);
    if (root == nullptr)
    {
        return;
    }
    cJSON *type = cJSON_GetObjectItemCaseSensitive(root, "type");
    cJSON *widgetId = cJSON_GetObjectItemCaseSensitive(root, "widgetId");
    cJSON *params = cJSON_GetObjectItemCaseSensitive(root, "params");
    if (cJSON_IsString(type) && strcmp(type->valuestring, "panic") == 0)
    {
        engine.requestPanic();
    }
    else if (cJSON_IsString(type) && strcmp(type->valuestring, "paramUpdate") == 0 &&
             cJSON_IsString(widgetId) && cJSON_IsObject(params))
    {
        if (!engine.applyOutputParam(widgetId->valuestring, params))
        {
            KnobLiveChangeInfo activity;
            graph.applyLiveParams(widgetId->valuestring, params, engine.midiCcOutCallback(), &activity);
            if (activity.changed)
            {
                engine.publishKnobActivityFromUi(activity.label, activity.channel, activity.cc, activity.raw,
                                                 activity.value);
            }
        }
    }
    cJSON_Delete(root);
}

void destroyPresetDoc(PresetDoc *doc)
{
    if (doc != nullptr)
    {
        doc->~PresetDoc();
        heap_caps_free(doc);
    }
}

PresetDoc *readPresetFile(const char *path, uint32_t reloadGeneration)
{
    FILE *f = fopen(path, "rb");
    if (f == nullptr)
    {
        return nullptr;
    }
    fseek(f, 0, SEEK_END);
    const long size = ftell(f);
    fseek(f, 0, SEEK_SET);
    if (size <= 0 || static_cast<size_t>(size) > synth::kMaxPresetBytes)
    {
        fclose(f);
        return nullptr;
    }

    const size_t bufferSize = static_cast<size_t>(size);
    char *buffer = static_cast<char *>(heap_caps_malloc(bufferSize, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT));
    if (buffer == nullptr)
    {
        buffer = static_cast<char *>(heap_caps_malloc(bufferSize, MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT));
    }
    if (buffer == nullptr)
    {
        fclose(f);
        return nullptr;
    }
    const size_t readBytes = fread(buffer, 1, static_cast<size_t>(size), f);
    fclose(f);
    if (readBytes != static_cast<size_t>(size))
    {
        heap_caps_free(buffer);
        return nullptr;
    }
    memorydiag::captureReloadPhase(reloadGeneration, memorydiag::ReloadPhase::FileBufferReady);

    void *docStorage = heap_caps_malloc(sizeof(PresetDoc), MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (docStorage == nullptr)
    {
        docStorage = heap_caps_malloc(sizeof(PresetDoc), MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT);
    }
    if (docStorage == nullptr)
    {
        heap_caps_free(buffer);
        return nullptr;
    }
    PresetDoc *doc = new (docStorage) PresetDoc();
    memorydiag::captureReloadPhase(reloadGeneration, memorydiag::ReloadPhase::PresetDocReady);
    const bool ok = parsePreset(buffer, readBytes, *doc);
    memorydiag::captureReloadPhase(reloadGeneration, memorydiag::ReloadPhase::Parsed);
    heap_caps_free(buffer);
    memorydiag::captureReloadPhase(reloadGeneration, memorydiag::ReloadPhase::FileBufferFreed);
    if (!ok)
    {
        destroyPresetDoc(doc);
        return nullptr;
    }
    return doc;
}
} // namespace

inline SynthEngine::TimingSnapshot SynthEngine::consumeTimingSnapshot()
{
    TimingSnapshot snapshot;
    uint32_t counts[kTimingHistogramBinCount];
    for (size_t i = 0; i < kTimingHistogramBinCount; ++i)
    {
        counts[i] = timingHistogram_[i].exchange(0, std::memory_order_relaxed);
        snapshot.sampleCount += counts[i];
    }
    snapshot.peakUs = renderPeakUs_.exchange(0, std::memory_order_relaxed);
    if (snapshot.sampleCount == 0)
    {
        return snapshot;
    }

    const auto percentileUs = [&](uint32_t percentile) {
        const uint32_t target = (snapshot.sampleCount * percentile + 99) / 100;
        uint32_t cumulative = 0;
        for (size_t i = 0; i < kTimingHistogramBinCount; ++i)
        {
            cumulative += counts[i];
            if (cumulative >= target)
            {
                return i + 1 == kTimingHistogramBinCount
                           ? snapshot.peakUs
                           : static_cast<uint32_t>((i + 1) * kTimingHistogramBinWidthUs);
            }
        }
        return snapshot.peakUs;
    };
    snapshot.p50Us = percentileUs(50);
    snapshot.p95Us = percentileUs(95);
    snapshot.p99Us = percentileUs(99);
    return snapshot;
}

inline void SynthEngine::begin(esp_codec_dev_handle_t codecHandle, i2s_chan_handle_t secondaryI2sTxHandle,
                                QueueHandle_t presetChangedQueue, QueueHandle_t liveParamQueue,
                                const char *initialPresetPath)
{
    dsp::initSineLut();    // once, here - never recomputed once the render task starts
    dsp::initVcaExpLut();  // same pattern, see DspPrimitives.h
    dsp::initPow2Lut();    // same pattern, see DspPrimitives.h
    dsp::initTanhLut();    // nonlinear saturation without tanhf() in the audio hot path

    codecHandle_ = codecHandle;
    secondaryI2sTxHandle_ = secondaryI2sTxHandle;
    presetChangedQueue_ = presetChangedQueue;
    liveParamQueue_ = liveParamQueue;
    snprintf(currentPresetPath_, sizeof(currentPresetPath_), "%s", initialPresetPath);
    setCurrentPresetNameFromPath(initialPresetPath);
    snprintf(loaderKnownPresetPath_, sizeof(loaderKnownPresetPath_), "%s", initialPresetPath);

    midiEventQueue_ = xQueueCreate(kSynthEngineMidiQueueDepth, sizeof(UsbMidiEvent));
    builtGraphQueue_ = xQueueCreate(1, sizeof(BuiltGraphMsg));
    retiredGraphQueue_ = xQueueCreate(2, sizeof(Graph *));

    // Pinned to core 1: this is the only task allowed on that core, so it
    // never contends with Ethernet/USB/httpd/etc for CPU time.
    xTaskCreatePinnedToCore(&SynthEngine::taskEntry, "synth_engine", kSynthEngineTaskStackSize, this,
                             configMAX_PRIORITIES - 2, &taskHandle_, 1);

    // Core 0, low priority (like sd_detect) - pure background parsing, must
    // never touch core 1 (render task), see loaderTaskLoop() comment in the
    // header.
    xTaskCreatePinnedToCore(&SynthEngine::loaderTaskEntry, "preset_loader", kSynthEngineLoaderTaskStackSize, this,
                             3, &loaderTaskHandle_, 0);
}

inline bool SynthEngine::onMidiEvent(const UsbMidiEvent &event)
{
    if (midiEventQueue_ != nullptr && xQueueSend(midiEventQueue_, &event, 0) == pdTRUE)
    {
        return true;
    }
    midiDropCount_.fetch_add(1, std::memory_order_relaxed);
    return false;
}

inline bool SynthEngine::applyOutputParam(const char *widgetId, cJSON *params)
{
    // "@masterClock" is intercepted earlier, in synth_web_api.hpp's
    // websocket_handler, entirely off this (real-time render) task - see
    // that branch's comment for why.
    size_t idx;
    if (strcmp(widgetId, "@out:usb") == 0) idx = 0;
    else if (strcmp(widgetId, "@out:es8311") == 0) idx = 1;
    else if (strcmp(widgetId, "@out:wm8960") == 0) idx = 2;
    else return false;

    if (cJSON_IsObject(params))
    {
        cJSON *gain = cJSON_GetObjectItemCaseSensitive(params, "gain");
        if (cJSON_IsNumber(gain))
        {
            float g = static_cast<float>(gain->valuedouble);
            if (g < 0.0f) g = 0.0f;
            if (g > 1.0f) g = 1.0f;
            sink_[idx].gain = g;
        }
        cJSON *mute = cJSON_GetObjectItemCaseSensitive(params, "muted");
        if (cJSON_IsBool(mute))
        {
            sink_[idx].muted = cJSON_IsTrue(mute);
        }
    }
    return true;
}

inline Graph *SynthEngine::buildFromFile(const char *path, uint16_t voiceCeiling, bool allowPreFree, bool forcePreFree)
{
    presetLoadProgress_.store(15, std::memory_order_relaxed); // reading preset JSON
    const uint32_t reloadGeneration = memorydiag::beginReload();
    PresetDoc *doc = readPresetFile(path, reloadGeneration);
    if (doc == nullptr)
    {
        memorydiag::finishReload(reloadGeneration, false);
        ESP_LOGW(kSynthEngineTag, "Preset JSON invalid (%s), keeping current graph", path);
        return nullptr;
    }

    // A rebuild otherwise briefly needs room for OLD+NEW simultaneously
    // (liveGraph_ still renders while this is being built) - more internal
    // RAM peak than boot ever needed, where the "old" graph is only an empty
    // placeholder. The caller uses the pre-free path for every reload, so
    // topology edits cannot push the new graph's hot buffers into PSRAM.
    // currentVoiceCount_ is written exclusively by the loader task itself
    // (this task here), so the read is the same task, no cross-thread risk.
    const uint16_t requestedVoicesPeek = GraphBuilder::peekRequestedVoices(*doc);
    if (allowPreFree &&
        (forcePreFree || requestedVoicesPeek > currentVoiceCount_.load(std::memory_order_relaxed)))
    {
        ESP_LOGI(kSynthEngineTag, "Preparing %u-voice rebuild - freeing old graph first (brief audio dropout)",
                 static_cast<unsigned>(requestedVoicesPeek));
        preFreeRequested_.store(true, std::memory_order_relaxed);
        // Bounded wait (not forever): the fade-out itself only takes
        // kSynthEngineOutputFadeMs (8ms) + max. one block (~2.7ms) until the render task
        // reacts - 300ms is generous. A timeout simply lets the flow continue
        // unchanged (old graph may still be active, GraphBuilder::build()'s
        // own retry catches a still-too-tight build as before) - safe
        // fallback, no hang.
        int waitedMs = 0;
        while (!preFreeDone_.load(std::memory_order_relaxed) && waitedMs < 300)
        {
            vTaskDelay(pdMS_TO_TICKS(2));
            waitedMs += 2;
        }
        preFreeDone_.store(false, std::memory_order_relaxed);
        memorydiag::captureReloadPhase(reloadGeneration, memorydiag::ReloadPhase::OldGraphDestroyed);
    }

    uint16_t requestedVoices = 0;
    uint16_t effectiveVoices = 0;
    presetLoadProgress_.store(35, std::memory_order_relaxed); // graph allocation, SoundFont/WAV loading
    Graph *newGraph = GraphBuilder::build(*doc, &requestedVoices, &effectiveVoices, voiceCeiling);
    memorydiag::captureReloadPhase(reloadGeneration, memorydiag::ReloadPhase::GraphBuilt);
    ESP_LOGI(kSynthEngineTag, "Preset loaded: %u widgets, %u connections", doc->widgetCount, doc->connectionCount);
    currentVoiceCount_.store(effectiveVoices, std::memory_order_relaxed);
    if (effectiveVoices < requestedVoices)
    {
        // The watchdog reload always asks for exactly voiceCeiling ==
        // currentVoiceCount_-1 (see taskLoop) and GraphBuilder::build starts
        // its own retry AT that ceiling - so effectiveVoices landing exactly
        // on voiceCeiling (rather than below it) means the ceiling itself
        // was the binding constraint, not a PSRAM-fit issue GraphBuilder hit
        // on its own. The rare case where both apply (ceiling reduces it,
        // *then* it still doesn't fit) is reported as psram, since that's
        // the reason it stopped at the value it actually settled on.
        const bool cpuBound = voiceCeiling < requestedVoices && effectiveVoices == voiceCeiling;
        ESP_LOGW(kSynthEngineTag, "Voice count reduced from %u to %u (%s)", static_cast<unsigned>(requestedVoices),
                 static_cast<unsigned>(effectiveVoices), cpuBound ? "real-time budget" : "did not fit in internal RAM");
        voicesCapRequested_.store(requestedVoices, std::memory_order_relaxed);
        voicesCapEffective_.store(effectiveVoices, std::memory_order_relaxed);
        voicesCapReason_.store(cpuBound ? 1 : 0, std::memory_order_relaxed);
        voicesCapEventId_.fetch_add(1, std::memory_order_relaxed);
    }
    destroyPresetDoc(doc);
    memorydiag::captureReloadPhase(reloadGeneration, memorydiag::ReloadPhase::PresetDocFreed);
    memorydiag::finishReload(reloadGeneration, newGraph != nullptr);
    if (newGraph != nullptr) presetLoadProgress_.store(90, std::memory_order_relaxed);
    return newGraph;
}

inline void SynthEngine::loaderTaskEntry(void *arg)
{
    static_cast<SynthEngine *>(arg)->loaderTaskLoop();
}

inline void SynthEngine::loaderTaskLoop()
{
    for (;;)
    {
        Graph *retiredGraph = nullptr;
        while (retiredGraphQueue_ != nullptr && xQueueReceive(retiredGraphQueue_, &retiredGraph, 0) == pdTRUE)
        {
            retiredGraph->destroy();
            Graph::release(retiredGraph);
        }

        PresetChangedMsg msg;
        // Bounded wait (not portMAX_DELAY) so this loop also notices a
        // cpuReloadRequested_ overload-watchdog request (see taskLoop)
        // promptly instead of only whenever the next real preset save
        // happens to arrive. 200ms is plenty responsive for a reload that's
        // reacting to a multi-second-sustained condition, and this task has
        // nothing else to do while idle anyway.
        const bool gotMsg = xQueueReceive(presetChangedQueue_, &msg, pdMS_TO_TICKS(200)) == pdTRUE;
        while (retiredGraphQueue_ != nullptr && xQueueReceive(retiredGraphQueue_, &retiredGraph, 0) == pdTRUE)
        {
            retiredGraph->destroy();
            Graph::release(retiredGraph);
        }
        const bool panicReload = panicReloadRequested_.exchange(false, std::memory_order_relaxed);
        const bool cpuReload = cpuReloadRequested_.exchange(false, std::memory_order_acquire);
        if (!gotMsg && !panicReload && !cpuReload)
        {
            continue;
        }
        if (gotMsg || panicReload)
        {
            underrunResetSequence_.fetch_add(1, std::memory_order_relaxed);
        }

        uint16_t voiceCeiling = SYNTH_VOICE_COUNT;
        const char *path = loaderKnownPresetPath_;
        if (gotMsg)
        {
            path = msg.path;
            snprintf(loaderKnownPresetPath_, sizeof(loaderKnownPresetPath_), "%s", msg.path);
            // A real preset edit (new file, or the same file re-saved with
            // different content) supersedes any pending overload-driven
            // ceiling from a previous, different graph - that ceiling was
            // never about this new content, and GraphBuilder's own
            // internal-RAM-fit retry re-protects memory regardless.
        }
        else if (cpuReload && !panicReload)
        {
            voiceCeiling = cpuReloadCeiling_.load(std::memory_order_relaxed);
        }
        // Every reload must use the same pre-free path as Panic. A topology
        // edit can increase the graph's block-buffer footprint even when the
        // configured voice count stays unchanged. Building beside the old
        // graph then pushes the new graph's hot buffers into PSRAM, which can
        // overload Core 1 until the next Panic reloads from a clean heap.
        Graph *newGraph = buildFromFile(path, voiceCeiling, true, true);
        if (newGraph == nullptr)
        {
            // buildFromFile() already wrote the warning log; the old graph
            // stays untouched, no fade needed.
            if (!gotMsg)
            {
                cpuReloadPending_.store(false, std::memory_order_relaxed);
            }
            if (gotMsg) cancelPresetLoad();
            continue;
        }

        BuiltGraphMsg built;
        built.graph = newGraph;
        snprintf(built.path, sizeof(built.path), "%s", path);
        // Blocking send with full timeout: if the render task still holds a
        // previous, unconsumed graph in the queue (fade still running), we
        // wait here rather than silently leaking the freshly built graph via
        // overwrite.
        xQueueSend(builtGraphQueue_, &built, portMAX_DELAY);
    }
}

inline void SynthEngine::taskEntry(void *arg)
{
    static_cast<SynthEngine *>(arg)->taskLoop();
}

inline void SynthEngine::taskLoop()
{
    memorydiag::registerSynthTask(xTaskGetCurrentTaskHandle());
    liveGraph_ = Graph::allocate(); // start silent, never render through a null graph
    if (Graph *initial = buildFromFile(currentPresetPath_, SYNTH_VOICE_COUNT, /*allowPreFree=*/false))
    {
        liveGraph_->destroy();
        Graph::release(liveGraph_);
        liveGraph_ = initial;
    }
    publishMidiVoiceUsage();

    static float fadeMulBlock[synth::kBlockSize];
    static float usbL[synth::kBlockSize];
    static float usbR[synth::kBlockSize];
    static float esL[synth::kBlockSize];
    static float esR[synth::kBlockSize];
    static float wmL[synth::kBlockSize];
    static float wmR[synth::kBlockSize];
    static int16_t usbChunk[synth::kBlockSize * 2];
    static int16_t esChunk[synth::kBlockSize * 2];
    static int16_t wmChunk[synth::kBlockSize * 2];

    while (true)
    {
        const int64_t iterStartUs = esp_timer_get_time();

        // Per-phase times/counters of this iteration - only needed for the
        // overrun diagnosis below (see its comment), otherwise unused.
        // Practically free (a few registers/stack variables,
        // esp_timer_get_time() is already used everywhere else in the loop).
        int64_t presetReloadUs = 0;
        uint32_t midiEventCount = 0;
        int64_t midiUs = 0;
        uint32_t liveParamCount = 0;
        int64_t liveParamUs = 0;

        // The loader task (loaderTaskLoop()) has already fully done the JSON
        // parse + GraphBuilder::build before anything happens here - this
        // task only receives the finished Graph*. The actual swap (further
        // down) additionally waits until outputFadeMul_ has been faded out
        // to 0, so it happens in silence instead of audibly clicking. If the
        // next finished graph already arrives during a running fade, it
        // stays in the queue and is only picked up after the current fade
        // completes.
        BuiltGraphMsg builtMsg;
        if (outputFadeState_ == OutputFadeState::Idle && builtGraphQueue_ != nullptr &&
            xQueueReceive(builtGraphQueue_, &builtMsg, 0) == pdTRUE)
        {
            pendingGraph_ = builtMsg.graph;
            snprintf(pendingPresetPath_, sizeof(pendingPresetPath_), "%s", builtMsg.path);
            outputFadeState_ = OutputFadeState::FadingOut;
        }
        // Pre-free request from the loader task (see buildFromFile() and the
        // preFreeRequested_ comment in the header) - temporally exclusive with
        // the pendingGraph_ acceptance above: a pre-free always completes
        // BEFORE the loader task even starts building, so a pendingGraph_
        // can never be pending at the same time.
        else if (outputFadeState_ == OutputFadeState::Idle && preFreeRequested_.load(std::memory_order_relaxed))
        {
            preFreeInFlight_ = true;
            outputFadeState_ = OutputFadeState::FadingOut;
        }

        UsbMidiEvent event;
        {
            const int64_t midiStartUs = esp_timer_get_time();
            if (panicClearMidiRequested_.exchange(false, std::memory_order_relaxed) && midiEventQueue_ != nullptr)
            {
                xQueueReset(midiEventQueue_);
            }
            while (midiEventQueue_ != nullptr && xQueueReceive(midiEventQueue_, &event, 0) == pdTRUE)
            {
                liveGraph_->handleMidiEvent(event);
                handleMasterClockMidi(event);
                publishMidiControlActivity(event);
                ++midiEventCount;
            }
            if (midiEventCount != 0)
            {
                publishMidiVoiceUsage();
            }
            midiUs = esp_timer_get_time() - midiStartUs;
        }

        LiveParamMsg paramMsg;
        {
            const int64_t liveParamStartUs = esp_timer_get_time();
            while (liveParamQueue_ != nullptr && xQueueReceive(liveParamQueue_, &paramMsg, 0) == pdTRUE)
            {
                applyLiveParamMessage(*this, *liveGraph_, paramMsg.json);
                ++liveParamCount;
            }
            liveParamUs = esp_timer_get_time() - liveParamStartUs;
        }

        const int64_t renderStartUs = esp_timer_get_time();
        Graph::Outputs gout;
        gout.usbL = usbL; gout.usbR = usbR;
        gout.esL = esL; gout.esR = esR;
        gout.wmL = wmL; gout.wmR = wmR;
        liveGraph_->renderBlock(gout, synth::kBlockSize);
        activeSoundFontVoices_.store(liveGraph_->activeSoundFontVoiceCount(), std::memory_order_relaxed);
        const int64_t renderUs = esp_timer_get_time() - renderStartUs;

        // Publish once per completed profiling sweep instead of copying every
        // widget ID and atomic value on every audio block.
        const bool widgetLoadGraphChanged = widgetLoadGraph_ != liveGraph_;
        const bool widgetLoadSweepReady = widgetLoadGeneration_ != liveGraph_->widgetLoadGeneration;
        if (widgetLoadGraphChanged || widgetLoadSweepReady)
        {
            const uint16_t widgetLoadN = liveGraph_->widgetCount;
            for (uint16_t i = 0; i < widgetLoadN; ++i)
            {
                memcpy(widgetLoadIds_[i], liveGraph_->widgets[i].id, sizeof(widgetLoadIds_[i]));
                widgetLoadPercent_[i].store(liveGraph_->widgetLoadPercent[i], std::memory_order_relaxed);
                widgetEdgeLoadPercent_[i].store(liveGraph_->widgetEdgeLoadPercent[i], std::memory_order_relaxed);
            }
            widgetLoadCount_.store(widgetLoadN, std::memory_order_relaxed);
            widgetLoadGraph_ = liveGraph_;
            widgetLoadGeneration_ = liveGraph_->widgetLoadGeneration;
        }

        // CV In/Out device status for the web UI's missing-device badge - unconditional every
        // block (unlike the profiler sweep above), so a device dropping out shows up within a
        // poll cycle or two rather than waiting for the next full profiler rotation.
        {
            uint16_t cvN = 0;
            for (uint16_t i = 0; i < liveGraph_->widgetCount && cvN < synth::kMaxWidgets; ++i)
            {
                const NodeType t = liveGraph_->widgets[i].type;
                if (t != NodeType::CvIn && t != NodeType::CvOut &&
                    t != NodeType::DigitalIn && t != NodeType::DigitalOut) continue;
                memcpy(cvStatusIds_[cvN], liveGraph_->widgets[i].id, sizeof(cvStatusIds_[cvN]));
                const NodeInstance &inst = liveGraph_->instances[liveGraph_->widgets[i].instanceBase];
                cvStatusFound_[cvN].store(inst.ioDeviceFound, std::memory_order_relaxed);
                cvStatusIsInput_[cvN].store(t == NodeType::CvIn || t == NodeType::DigitalIn,
                                            std::memory_order_relaxed);
                // DigitalIn's live level is reported through the same "volts" slot as CV In
                // (0/cv::kMaxVolts = LOW/HIGH) so the browser's existing cvStatus consumer needs
                // no new wire format - see synth_web_api.hpp's broadcast_cv_status_tick().
                float volts = 0.0f;
                if (t == NodeType::CvIn) volts = inst.cvIn.lastVolts;
                else if (t == NodeType::DigitalIn) volts = inst.digitalIn.lastLevel ? cv::kMaxVolts : 0.0f;
                cvStatusVolts_[cvN].store(volts, std::memory_order_relaxed);
                ++cvN;
            }
            cvStatusCount_.store(cvN, std::memory_order_relaxed);
        }

        // P0 baseline diagnostics: published every block (a handful of plain
        // stores, same cost class as the phase timing itself) - see
        // phaseSnapshot()/blockStatsSnapshot() above. enginestats::lastBlock
        // reflects the block Graph::renderBlock() just finished (its
        // beginBlock() call runs at the very start of that renderBlock, see
        // Graph.h), so this is always one block behind, same staleness as
        // every other render-task-owned diagnostic here.
        phaseActivityPercent_.store(liveGraph_->phaseActivityPercent, std::memory_order_relaxed);
        phaseRenderPercent_.store(liveGraph_->phaseRenderPercent, std::memory_order_relaxed);
        phaseOutputPercent_.store(liveGraph_->phaseOutputPercent, std::memory_order_relaxed);
        const enginestats::BlockCounters &blockStats = enginestats::lastBlock;
        oscPitchConstant_.store(blockStats.oscPitchConstant, std::memory_order_relaxed);
        oscPitchAudioRate_.store(blockStats.oscPitchAudioRate, std::memory_order_relaxed);
        oscTuneConstant_.store(blockStats.oscTuneConstant, std::memory_order_relaxed);
        oscTuneAudioRate_.store(blockStats.oscTuneAudioRate, std::memory_order_relaxed);
        oscAmpConstant_.store(blockStats.oscAmpConstant, std::memory_order_relaxed);
        oscAmpAudioRate_.store(blockStats.oscAmpAudioRate, std::memory_order_relaxed);
        oscSilentSkipped_.store(blockStats.oscSilentSkipped, std::memory_order_relaxed);
        oscGainConverged_.store(blockStats.oscGainConverged, std::memory_order_relaxed);
        fmPitchConstant_.store(blockStats.fmPitchConstant, std::memory_order_relaxed);
        fmPitchAudioRate_.store(blockStats.fmPitchAudioRate, std::memory_order_relaxed);
        fmTuneConstant_.store(blockStats.fmTuneConstant, std::memory_order_relaxed);
        fmTuneAudioRate_.store(blockStats.fmTuneAudioRate, std::memory_order_relaxed);
        fmAmpConstant_.store(blockStats.fmAmpConstant, std::memory_order_relaxed);
        fmAmpAudioRate_.store(blockStats.fmAmpAudioRate, std::memory_order_relaxed);
        fmAmpConstantZero_.store(blockStats.fmAmpConstantZero, std::memory_order_relaxed);
        fmSilentSkipped_.store(blockStats.fmSilentSkipped, std::memory_order_relaxed);
        fmGainConverged_.store(blockStats.fmGainConverged, std::memory_order_relaxed);
        fmModActive_.store(blockStats.fmModActive, std::memory_order_relaxed);
        fmFeedbackActive_.store(blockStats.fmFeedbackActive, std::memory_order_relaxed);
        fmSuperSawActive_.store(blockStats.fmSuperSawActive, std::memory_order_relaxed);
        dxHolding_.store(blockStats.dxHolding, std::memory_order_relaxed);
        dxRamping_.store(blockStats.dxRamping, std::memory_order_relaxed);
        dxBoundary_.store(blockStats.dxBoundary, std::memory_order_relaxed);
        dxShapeConfigured_.store(blockStats.dxShapeConfigured, std::memory_order_relaxed);
        instancesTotal_.store(blockStats.instancesTotal, std::memory_order_relaxed);
        instancesRendered_.store(blockStats.instancesRendered, std::memory_order_relaxed);
        instancesRenderedWhileIdle_.store(blockStats.instancesRenderedWhileIdle, std::memory_order_relaxed);
        // The actual ESP_LOGI print deliberately does NOT happen here: a
        // real hardware run showed this render task's own periodic log call
        // blocking long enough (~2ms, UART write) to single-handedly cause
        // "Audio-Overrun" on the very block it logged from - the exact
        // failure mode plan section 7 already warned about for a different
        // diagnostic ("Logging außerhalb des Audiotasks ausführen"), which
        // this diagnostic itself violated at first. Fixed by moving the
        // actual logging to Feature::Synth::log_p0_baseline_tick()
        // (synth_manager.hpp), polled from app_main's own task (main.cpp) -
        // this render task only ever publishes plain atomic stores above.

        // Host audio (USB speaker-OUT) arrives as its own streaming ring;
        // mix it into the ES8311 master block here (host plays on its own
        // speakers). The three output widgets independently control WHICH
        // signal stream goes out per sink (see below).
        if (mixHostIn_ != nullptr)
        {
            mixHostIn_(esL, esR, synth::kBlockSize);
        }

        // Sample-accurate fade multiplier for this block, see
        // outputFadeState_ declaration. Multiplied below with sink_[idx].gain
        // before converting to PCM - so the actual sink gain/mute path stays
        // unchanged and the fade only layers additively on top.
        for (size_t i = 0; i < synth::kBlockSize; ++i)
        {
            switch (outputFadeState_)
            {
            case OutputFadeState::FadingOut:
                outputFadeMul_ -= kSynthEngineOutputFadeStep;
                if (outputFadeMul_ < 0.0f) outputFadeMul_ = 0.0f;
                break;
            case OutputFadeState::FadingIn:
                outputFadeMul_ += kSynthEngineOutputFadeStep;
                if (outputFadeMul_ > 1.0f) outputFadeMul_ = 1.0f;
                break;
            case OutputFadeState::Idle:
                break;
            }
            fadeMulBlock[i] = outputFadeMul_;
        }

        // Fade-out done (output at zero) -> only now perform the swap.
        // pendingGraph_ is already fully parsed/built by the loader task (see
        // loaderTaskLoop()) - only releasing the old graph plus a pointer
        // write remains here, both fast enough for the block budget.
        if (outputFadeState_ == OutputFadeState::FadingOut && outputFadeMul_ <= 0.0f && preFreeInFlight_)
        {
            // Pre-free instead of a normal swap: no new graph is ready,
            // liveGraph_ is only reset to the empty boot placeholder, so the
            // loader task can start the subsequent rebuild with maximum free
            // internal RAM (see preFreeRequested_ comment in the header and
            // buildFromFile()). Deliberately no fading in - the placeholder
            // is silent anyway, the next real pendingGraph_ swap threads
            // itself back in normally.
            liveGraph_->destroy();
            Graph::release(liveGraph_);
            liveGraph_ = Graph::allocate();
            publishMidiVoiceUsage();
            preFreeInFlight_ = false;
            preFreeRequested_.store(false, std::memory_order_relaxed);
            preFreeDone_.store(true, std::memory_order_relaxed);
            outputFadeState_ = OutputFadeState::Idle;
        }
        else if (outputFadeState_ == OutputFadeState::FadingOut && outputFadeMul_ <= 0.0f)
        {
            const int64_t swapStartUs = esp_timer_get_time();
            Graph *retiredGraph = liveGraph_;
            liveGraph_ = pendingGraph_;
            pendingGraph_ = nullptr;
            publishMidiVoiceUsage();
            publishLearnedKnobsToMidiOut();
            // A preset save/reload is a full runtime reset. Drop queued MIDI
            // and stale live-param messages, same cleanup effect as Panic.
            if (midiEventQueue_ != nullptr)
            {
                xQueueReset(midiEventQueue_);
            }
            if (liveParamQueue_ != nullptr)
            {
                xQueueReset(liveParamQueue_);
            }
            if (xQueueSend(retiredGraphQueue_, &retiredGraph, 0) != pdTRUE)
            {
                // Queue depth two is sufficient because the loader serializes
                // builds. Keep a leak impossible if that invariant changes.
                retiredGraph->destroy();
                Graph::release(retiredGraph);
            }
            snprintf(currentPresetPath_, sizeof(currentPresetPath_), "%s", pendingPresetPath_);
            setCurrentPresetNameFromPath(currentPresetPath_);
            presetChangeSequence_.fetch_add(1, std::memory_order_release);
            presetLoadProgress_.store(100, std::memory_order_relaxed);
            presetLoadActive_.store(false, std::memory_order_release);
            presetReloadUs = esp_timer_get_time() - swapStartUs;
            outputFadeState_ = OutputFadeState::FadingIn;
            // A freshly swapped-in graph deserves a fresh read on whether it
            // overloads - reset the watchdog's streak timer regardless of
            // whether this swap was itself the watchdog's own doing (clearing
            // cpuReloadPending_ either way: if it wasn't pending, this is a
            // harmless no-op store).
            overloadStartUs_ = 0;
            cpuReloadPending_.store(false, std::memory_order_relaxed);
        }
        else if (outputFadeState_ == OutputFadeState::FadingIn && outputFadeMul_ >= 1.0f)
        {
            outputFadeState_ = OutputFadeState::Idle;
        }

        // --- USB-Out-Widget (Device->Host, "USB-Mikrofon") ---
        const SinkParams &usbSink = sink_[0];
        if (submitOut_ != nullptr && !usbSink.muted && usbSink.gain > 0.0f)
        {
            for (size_t i = 0; i < synth::kBlockSize; ++i)
            {
                const float gain = usbSink.gain * fadeMulBlock[i];
                float l = usbL[i] * gain, r = usbR[i] * gain;
                if (l > 1.0f) l = 1.0f; else if (l < -1.0f) l = -1.0f;
                if (r > 1.0f) r = 1.0f; else if (r < -1.0f) r = -1.0f;
                usbChunk[i * 2 + 0] = static_cast<int16_t>(l * 32767.0f);
                usbChunk[i * 2 + 1] = static_cast<int16_t>(r * 32767.0f);
            }
            submitOut_(usbChunk, sizeof(usbChunk) / sizeof(usbChunk[0]));
        }

        const int64_t preWriteProcessEndUs = esp_timer_get_time();
        const uint32_t preWriteProcessUs = static_cast<uint32_t>(preWriteProcessEndUs - iterStartUs);
        const int64_t postRenderUs = preWriteProcessEndUs - renderStartUs - renderUs;

        // esp_codec_dev_write() blocks until the codec driver has DMA space
        // again - under healthy operation this is the actual clocking wait
        // (keeps the block rhythm, see comment above) and therefore
        // deliberately NOT part of processUs/the overrun above. But if a
        // SINGLE write call itself becomes many times slower than the budget
        // than usual, that is a real, so far blind spot here: the time would
        // not be logged anywhere, even though exactly there (while the render
        // task waits for the next DMA-done interrupt) a system-wide stall -
        // e.g. a flash-cache lock through an NVS write operation, or an
        // interrupt storm/a long ISR elsewhere (candidate: Ethernet link
        // change) - can very well let the codec run dry. Separate log branch
        // below, independent of the processUs overrun above.
        int64_t esConvertUs = 0;
        int64_t esWriteUs = 0;
        if (codecHandle_ != nullptr)
        {
            const int64_t esConvertStartUs = esp_timer_get_time();
            const SinkParams &esSyn = sink_[1];
            static int16_t s_lastEsL = 0;
            static int64_t s_lastEsClickLogUs = 0;
            int32_t esMaxStep = 0;
            int16_t esPrev = s_lastEsL;
            if (esSyn.muted || esSyn.gain <= 0.0f)
            {
                memset(esChunk, 0, sizeof(esChunk));
                esMaxStep = esPrev < 0 ? -static_cast<int32_t>(esPrev) : static_cast<int32_t>(esPrev);
                esPrev = 0;
            }
            else
            {
                for (size_t i = 0; i < synth::kBlockSize; ++i)
                {
                    const float gain = esSyn.gain * fadeMulBlock[i];
                    float l = esL[i] * gain, r = esR[i] * gain;
                    if (l > 1.0f) l = 1.0f; else if (l < -1.0f) l = -1.0f;
                    if (r > 1.0f) r = 1.0f; else if (r < -1.0f) r = -1.0f;
                    esChunk[i * 2 + 0] = static_cast<int16_t>(l * 32767.0f);
                    esChunk[i * 2 + 1] = static_cast<int16_t>(r * 32767.0f);
                    const int32_t step = static_cast<int32_t>(esChunk[i * 2]) - static_cast<int32_t>(esPrev);
                    const int32_t absStep = step < 0 ? -step : step;
                    if (absStep > esMaxStep) esMaxStep = absStep;
                    esPrev = esChunk[i * 2];
                }
            }
            s_lastEsL = esPrev;
            const int64_t esNowUs = esp_timer_get_time();
            if (esMaxStep > 20000 && esNowUs - s_lastEsClickLogUs > 100000) {
                s_lastEsClickLogUs = esNowUs;
                ESP_LOGW(kSynthEngineTag, "Signal-Discontinuity ES8311: maxStep=%ld", static_cast<long>(esMaxStep));
            }
            esConvertUs = esp_timer_get_time() - esConvertStartUs;
            const int64_t esWriteStartUs = esp_timer_get_time();
            esp_codec_dev_write(codecHandle_, esChunk, sizeof(esChunk));
            esWriteUs = esp_timer_get_time() - esWriteStartUs;
        }
        int64_t wmConvertUs = 0;
        int64_t wmWriteUs = 0;
        if (secondaryI2sTxHandle_ != nullptr)
        {
            const int64_t wmConvertStartUs = esp_timer_get_time();
            const SinkParams &wmSyn = sink_[2];
            static int16_t s_lastWmL = 0;
            static int64_t s_lastWmClickLogUs = 0;
            int32_t wmMaxStep = 0;
            int16_t wmPrev = s_lastWmL;
            if (wmSyn.muted || wmSyn.gain <= 0.0f)
            {
                memset(wmChunk, 0, sizeof(wmChunk));
                wmMaxStep = wmPrev < 0 ? -static_cast<int32_t>(wmPrev) : static_cast<int32_t>(wmPrev);
                wmPrev = 0;
            }
            else
            {
                for (size_t i = 0; i < synth::kBlockSize; ++i)
                {
                    const float gain = wmSyn.gain * fadeMulBlock[i];
                    float l = wmL[i] * gain, r = wmR[i] * gain;
                    if (l > 1.0f) l = 1.0f; else if (l < -1.0f) l = -1.0f;
                    if (r > 1.0f) r = 1.0f; else if (r < -1.0f) r = -1.0f;
                    wmChunk[i * 2 + 0] = static_cast<int16_t>(l * 32767.0f);
                    wmChunk[i * 2 + 1] = static_cast<int16_t>(r * 32767.0f);
                    const int32_t step = static_cast<int32_t>(wmChunk[i * 2]) - static_cast<int32_t>(wmPrev);
                    const int32_t absStep = step < 0 ? -step : step;
                    if (absStep > wmMaxStep) wmMaxStep = absStep;
                    wmPrev = wmChunk[i * 2];
                }
            }
            s_lastWmL = wmPrev;
            const int64_t wmNowUs = esp_timer_get_time();
            if (wmMaxStep > 20000 && wmNowUs - s_lastWmClickLogUs > 100000) {
                s_lastWmClickLogUs = wmNowUs;
                ESP_LOGW(kSynthEngineTag, "Signal-Discontinuity WM8960: maxStep=%ld", static_cast<long>(wmMaxStep));
            }
            wmConvertUs = esp_timer_get_time() - wmConvertStartUs;
            const int64_t wmWriteStartUs = esp_timer_get_time();
            size_t wmBytesWritten = 0;
            i2s_channel_write(secondaryI2sTxHandle_, wmChunk, sizeof(wmChunk), &wmBytesWritten, portMAX_DELAY);
            wmWriteUs = esp_timer_get_time() - wmWriteStartUs;
        }

        // CPU load excludes only the blocking DMA waits. Both codec conversion
        // paths and their signal-discontinuity scans are real Core-1 work and
        // therefore belong in the render budget and overload watchdog.
        const uint32_t processUs = static_cast<uint32_t>(
            static_cast<int64_t>(preWriteProcessUs) + esConvertUs + wmConvertUs);
        uint32_t peak = renderPeakUs_.load(std::memory_order_relaxed);
        if (processUs > peak) renderPeakUs_.store(processUs, std::memory_order_relaxed);
        size_t timingBin = processUs / kTimingHistogramBinWidthUs;
        if (timingBin >= kTimingHistogramBinCount) timingBin = kTimingHistogramBinCount - 1;
        timingHistogram_[timingBin].fetch_add(1, std::memory_order_relaxed);
        const int64_t timingNowUs = esp_timer_get_time();
        if (static_cast<float>(processUs) > kSynthEngineBlockBudgetUs)
        {
            renderOverrunCount_.fetch_add(1, std::memory_order_relaxed);
            static int64_t s_lastOverrunLogUs = 0;
            if (timingNowUs - s_lastOverrunLogUs > 100000)
            {
                s_lastOverrunLogUs = timingNowUs;
                ESP_LOGW(kSynthEngineTag,
                         "Audio-Overrun: %lu us > %lu us Budget (preset=%lld us, midi=%lu Ev/%lld us, "
                         "liveParams=%lu/%lld us, render=%lld us, postRender=%lld us, esConvert=%lld us, "
                         "wmConvert=%lld us)",
                         static_cast<unsigned long>(processUs), static_cast<unsigned long>(kSynthEngineBlockBudgetUs),
                         static_cast<long long>(presetReloadUs), static_cast<unsigned long>(midiEventCount),
                         static_cast<long long>(midiUs), static_cast<unsigned long>(liveParamCount),
                         static_cast<long long>(liveParamUs), static_cast<long long>(renderUs),
                         static_cast<long long>(postRenderUs), static_cast<long long>(esConvertUs),
                         static_cast<long long>(wmConvertUs));
            }
        }
        const float instLoadPercent = 100.0f * static_cast<float>(processUs) / kSynthEngineBlockBudgetUs;
        const float prevLoad = audioCoreLoadPercent_.load(std::memory_order_relaxed);
        const float smoothedLoad = prevLoad + kSynthEngineLoadEmaAlpha * (instLoadPercent - prevLoad);
        audioCoreLoadPercent_.store(smoothedLoad, std::memory_order_relaxed);

        const uint16_t currentVoices = currentVoiceCount_.load(std::memory_order_relaxed);
        if (smoothedLoad >= kSynthEngineOverloadPercentThreshold && currentVoices > 1 &&
            !cpuReloadPending_.load(std::memory_order_relaxed))
        {
            if (overloadStartUs_ == 0)
            {
                overloadStartUs_ = timingNowUs;
            }
            else if (timingNowUs - overloadStartUs_ >= kSynthEngineOverloadSustainedUs)
            {
                cpuReloadCeiling_.store(static_cast<uint16_t>(currentVoices - 1), std::memory_order_relaxed);
                cpuReloadPending_.store(true, std::memory_order_relaxed);
                cpuReloadRequested_.store(true, std::memory_order_release);
                overloadStartUs_ = 0;
            }
        }
        else
        {
            overloadStartUs_ = 0;
        }

        // >2x budget for the ENTIRE iteration (processing + both blocking
        // write calls) cannot arise under normal DMA buffering from merely
        // waiting for the next buffer - points to a real system stall during
        // a write call (see comment above). "else if": no double log if the
        // processUs overrun above already fired.
        const int64_t totalIterUs = static_cast<int64_t>(processUs) + esWriteUs + wmWriteUs;
        // Rate-limited to >=100ms between log lines: at the default 115200 baud, one of these
        // ~250-char lines takes ~20ms to transmit over UART/USB-CDC - blocking, since the
        // ESP_LOGW call itself doesn't return until the driver has queued/sent the bytes. Left
        // unthrottled, a single genuine stall's log line becomes its own next stall (the next
        // iteration's write calls queue up behind the still-draining previous log line), which
        // then logs again, forever - a self-sustaining feedback loop indistinguishable in the
        // log from a real recurring hardware stall. Confirmed live: unthrottled, this fired on
        // 1889 of ~1900 blocks in an 8s window with esWrite pinned at ~8ms (log-transmission
        // time), not the periodic burst a real intermittent stall would show.
        static int64_t s_lastStallLogUs = 0;
        if (!(static_cast<float>(processUs) > kSynthEngineBlockBudgetUs) &&
            static_cast<float>(totalIterUs) > 2.0f * kSynthEngineBlockBudgetUs &&
            timingNowUs - s_lastStallLogUs > 100000)
        {
            s_lastStallLogUs = timingNowUs;
            ESP_LOGW(kSynthEngineTag,
                     "Codec-Write-Stall: Iteration gesamt %lld us > %.0f us (2x Budget) trotz process=%lu us - "
                     "esWrite=%lld us, wmWrite=%lld us (haengt vermutlich mit einem System-weiten Stall waehrend "
                     "des Wartens auf DMA zusammen, z.B. Flash-Cache-Sperre oder eine lange ISR anderswo)",
                     static_cast<long long>(totalIterUs), static_cast<double>(2.0f * kSynthEngineBlockBudgetUs),
                     static_cast<unsigned long>(processUs), static_cast<long long>(esWriteUs),
                     static_cast<long long>(wmWriteUs));
        }
    }
}
