#pragma once

#include <stddef.h>
#include <stdint.h>

#define EEZ_PRESET_CAPACITY 32

typedef struct EezSynthStatus {
    float audio_core_load;
    uint16_t active_voices;
    uint16_t maximum_voices;
    uint16_t preset_number;
    uint16_t preset_count;
    uint8_t preset_load_progress;
    bool preset_load_active;
    uint32_t underruns;
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

#ifdef __cplusplus
extern "C" {
#endif

bool eez_synth_status(EezSynthStatus *status);
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
