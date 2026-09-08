#pragma once

#include <cstdint>
#include <cstdio>

#include "esp_log.h"

// Parses and logs 4-byte USB-MIDI Event Packets (USB Class Definition for
// MIDI Devices 1.0). Deliberately free of any USB-Host/FreeRTOS dependency
// so the wire-format decoding stays independent of the transport.
struct UsbMidiEvent
{
    uint8_t cable;
    uint8_t codeIndex;
    uint8_t status;
    uint8_t data1;
    uint8_t data2;

    // USB device address this event originated from. Not part of the wire
    // packet itself - set by the caller (UsbMidiClassDriver) after parse(),
    // so multi-device setups can be told apart downstream (e.g. in the /ws
    // JSON payload).
    uint8_t sourceAddress = 0;

    using Callback = void (*)(const UsbMidiEvent &event);

    // 1-based MIDI channel (1-16) decoded from the status byte's low nibble.
    uint8_t channel() const { return static_cast<uint8_t>((status & 0x0F) + 1); }

    // Decodes a single 4-byte USB-MIDI Event Packet. Returns false for
    // reserved/unused Code Index Numbers (e.g. padding packets).
    static bool parse(const uint8_t packet[4], UsbMidiEvent &out)
    {
        const uint8_t codeIndex = packet[0] & 0x0F;
        if (codeIndex == 0x00 || codeIndex == 0x01)
        {
            return false; // reserved / reserved-for-future-use CINs
        }

        out.cable = (packet[0] >> 4) & 0x0F;
        out.codeIndex = codeIndex;
        out.status = packet[1];
        out.data1 = packet[2];
        out.data2 = packet[3];
        return true;
    }

    void logTo(const char *tag) const
    {
        // Every label spelled out (ch=/note=/cc=/val=/...), deliberately not
        // "C %u" for channel next to a "CC" event-name prefix - that
        // shorthand reads as if the channel number were the CC number at a
        // glance (e.g. old format "CC 3 C 74 V 100" for channel 3, CC 74,
        // value 100 - easy to misread "CC 3" as "CC number 3").
        const uint8_t channel = this->channel();
        switch (codeIndex)
        {
        case 0x8:
            //ESP_LOGI(tag, "note_off ch=%u note=%u vel=%u", channel, data1, data2);
            break;
        case 0x9:
            if (data2 == 0)
            {
                //ESP_LOGI(tag, "note_off ch=%u note=%u vel=%u", channel, data1, data2);
            }
            else
            {
                //ESP_LOGI(tag, "note_on ch=%u note=%u vel=%u", channel, data1, data2);
            }
            break;
        case 0xA:
            ESP_LOGI(tag, "pa %u n=%u p=%u", channel, data1, data2);
            break;
        case 0xB:
            ESP_LOGI(tag, "cc %u cc=%u val=%u", channel, data1, data2);
            break;
        case 0xC:
            ESP_LOGI(tag, "pc %u p=%u", channel, data1);
            break;
        case 0xD:
            ESP_LOGI(tag, "af %u p=%u", channel, data1);
            break;
        case 0xE:
        {
            const int16_t bend = static_cast<int16_t>(((data2 & 0x7F) << 7) | (data1 & 0x7F)) - 8192;
            ESP_LOGI(tag, "pw ch=%u v=%d", channel, bend);
            break;
        }
        default:
            ESP_LOGI(tag, "event cin=0x%X status=0x%02X data=%u,%u", codeIndex, status, data1, data2);
            break;
        }
    }

    // Serializes to a compact JSON object mirroring the {type:"midi", ...}
    // payload shape the web UI expects over /ws (see data/app.py's
    // midi_payload(), which forwards mido's message-type names verbatim -
    // "note_on"/"note_off"/"control_change"/etc. - and a 0-based channel;
    // both are matched exactly here since the frontend (routeMidiToWidgets(),
    // routeMidiToKnobs() in static/js/main.js) compares against those exact
    // strings/range, not this driver's 1-based channel(). `uptimeMs` and
    // `portLabel` are supplied by the caller (main.cpp) rather than read
    // here, keeping this struct free of any ESP-IDF/FreeRTOS dependency for
    // wall-clock/device-naming concerns that belong at a higher layer - they
    // fill the "time"/"port" fields the frontend's log line
    // (`appendLog(\`[${data.time}] ${data.port}: ${data.message}\`)`)
    // expects, which otherwise render as "undefined". Returns what snprintf
    // returns (bytes that would have been written) - compare against
    // bufferSize to detect truncation.
    size_t toJson(char *buffer, size_t bufferSize, uint32_t uptimeMs, const char *portLabel) const
    {
        const uint8_t ch0 = status & 0x0F; // 0-based channel, matches mido/frontend convention
        switch (codeIndex)
        {
        case 0x8:
            return snprintf(buffer, bufferSize,
                             "{\"type\":\"midi\",\"address\":%u,\"midiType\":\"note_off\",\"channel\":%u,\"note\":%u,"
                             "\"velocity\":%u,\"time\":%lu,\"port\":\"%s\",\"message\":\"note_off channel=%u "
                             "note=%u velocity=%u\"}",
                             sourceAddress, ch0, data1, data2, static_cast<unsigned long>(uptimeMs), portLabel, ch0,
                             data1, data2);
        case 0x9:
        {
            const char *midiType = data2 == 0 ? "note_off" : "note_on";
            return snprintf(buffer, bufferSize,
                             "{\"type\":\"midi\",\"address\":%u,\"midiType\":\"%s\",\"channel\":%u,\"note\":%u,"
                             "\"velocity\":%u,\"time\":%lu,\"port\":\"%s\",\"message\":\"%s channel=%u note=%u "
                             "velocity=%u\"}",
                             sourceAddress, midiType, ch0, data1, data2, static_cast<unsigned long>(uptimeMs),
                             portLabel, midiType, ch0, data1, data2);
        }
        case 0xA:
            return snprintf(buffer, bufferSize,
                             "{\"type\":\"midi\",\"address\":%u,\"midiType\":\"polytouch\",\"channel\":%u,"
                             "\"note\":%u,\"value\":%u,\"time\":%lu,\"port\":\"%s\",\"message\":\"polytouch "
                             "channel=%u note=%u value=%u\"}",
                             sourceAddress, ch0, data1, data2, static_cast<unsigned long>(uptimeMs), portLabel, ch0,
                             data1, data2);
        case 0xB:
            return snprintf(buffer, bufferSize,
                             "{\"type\":\"midi\",\"address\":%u,\"midiType\":\"control_change\",\"channel\":%u,"
                             "\"control\":%u,\"value\":%u,\"time\":%lu,\"port\":\"%s\",\"message\":\""
                             "control_change channel=%u control=%u value=%u\"}",
                             sourceAddress, ch0, data1, data2, static_cast<unsigned long>(uptimeMs), portLabel, ch0,
                             data1, data2);
        case 0xC:
            return snprintf(buffer, bufferSize,
                             "{\"type\":\"midi\",\"address\":%u,\"midiType\":\"program_change\",\"channel\":%u,"
                             "\"program\":%u,\"time\":%lu,\"port\":\"%s\",\"message\":\"program_change channel=%u "
                             "program=%u\"}",
                             sourceAddress, ch0, data1, static_cast<unsigned long>(uptimeMs), portLabel, ch0, data1);
        case 0xD:
            return snprintf(buffer, bufferSize,
                             "{\"type\":\"midi\",\"address\":%u,\"midiType\":\"aftertouch\",\"channel\":%u,"
                             "\"value\":%u,\"time\":%lu,\"port\":\"%s\",\"message\":\"aftertouch channel=%u "
                             "value=%u\"}",
                             sourceAddress, ch0, data1, static_cast<unsigned long>(uptimeMs), portLabel, ch0, data1);
        case 0xE:
        {
            const int16_t bend = static_cast<int16_t>(((data2 & 0x7F) << 7) | (data1 & 0x7F)) - 8192;
            return snprintf(buffer, bufferSize,
                             "{\"type\":\"midi\",\"address\":%u,\"midiType\":\"pitchwheel\",\"channel\":%u,"
                             "\"value\":%d,\"time\":%lu,\"port\":\"%s\",\"message\":\"pitchwheel channel=%u "
                             "value=%d\"}",
                             sourceAddress, ch0, bend, static_cast<unsigned long>(uptimeMs), portLabel, ch0, bend);
        }
        default:
            return snprintf(buffer, bufferSize,
                             "{\"type\":\"midi\",\"address\":%u,\"midiType\":\"other\",\"channel\":%u,\"cin\":%u,"
                             "\"time\":%lu,\"port\":\"%s\",\"message\":\"other cin=0x%X status=0x%02X\"}",
                             sourceAddress, ch0, codeIndex, static_cast<unsigned long>(uptimeMs), portLabel,
                             codeIndex, status);
        }
    }
};
