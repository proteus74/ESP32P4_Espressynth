#pragma once

#include <stddef.h>
#include <stdint.h>
#include "features/synth/KnobActivity.h"

#define EEZ_PRESET_CAPACITY 32

typedef struct EezSynthStatus {
    float audio_core_load;
    float master_clock_bpm;
    uint16_t active_voices;
    uint16_t maximum_voices;
    uint16_t preset_number;
    uint16_t preset_count;
    uint32_t underruns;
    uint32_t preset_change_sequence;
    char preset_name[64];
    char midi_name[32];
} EezSynthStatus;

typedef struct EezPresetEntry {
    char file_name[64];
    char display_name[64];
    char description[96];
    char author[48];
} EezPresetEntry;

typedef struct EezPresetList {
    uint16_t count;
    EezPresetEntry entries[EEZ_PRESET_CAPACITY];
} EezPresetList;

#define EEZ_TAB_CAPACITY 65

// Page tabs of the active preset, in the browser's tab-bar order.
typedef struct EezTabList {
    uint16_t count;
    char ids[EEZ_TAB_CAPACITY][32];
    char titles[EEZ_TAB_CAPACITY][64];
} EezTabList;

#define EEZ_PAGE_CONTROL_CAPACITY 64

// One Knob/Button control (active layer only) of a preset page, as shown on the touch screen.
typedef struct EezPageControl {
    char widget_id[24];
    char widget_label[48];
    char name[48];
    uint8_t control_index; // layer-adjusted index, as the engine's knobIndex/buttonIndex - 1
    bool is_button;
    bool switch_mode;      // Button: toggles instead of momentary
    bool was_above;        // Button: pressed/on state
    uint8_t list_index;    // Button: current value-list entry
    uint8_t list_count;    // Button: value-list length (>= 2 = list mode)
    uint8_t raw;           // Knob: 0..127
    float value;           // Knob: mapped value, Button: current output
    int32_t ui_x;
    int32_t ui_y;
} EezPageControl;

typedef struct EezPageControls {
    uint16_t count;
    EezPageControl controls[EEZ_PAGE_CONTROL_CAPACITY];
} EezPageControls;

#ifdef __cplusplus
extern "C" {
#endif

bool eez_synth_status(EezSynthStatus *status);
bool eez_list_tabs(EezTabList *list);
bool eez_list_page_controls(const char *page_id, EezPageControls *out);
bool eez_send_control_param(const char *widget_id, bool is_button, uint8_t control_index, const char *key, float value);
bool eez_read_knob_activity(KnobActivity *activity);
void eez_network_ip(char *out, size_t out_size);
bool eez_list_presets(EezPresetList *list);
bool eez_synth_load_preset(const char *name);
void eez_synth_panic(void);
void eez_synth_step_preset(int direction);

#ifdef __cplusplus
}
#endif

namespace Feature::EEZUI {
void init();
void start();
void show_preset_list();
}
