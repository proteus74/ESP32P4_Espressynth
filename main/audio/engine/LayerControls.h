#pragma once

#include <cstddef>
#include <cstdint>
#include <new>
#include "esp_heap_caps.h"

// Keep the preset's fixed 16-slot layer indices while storing only used slots.
template <typename Control>
class LayerControls
{
    Control *data_ = nullptr;
    uint8_t count_ = 0, layers_ = 0;

public:
    LayerControls() { resize(1, 1); }
    LayerControls(const LayerControls &) = delete;
    LayerControls &operator=(const LayerControls &) = delete;
    ~LayerControls() { heap_caps_free(data_); }

    bool resize(uint8_t count, uint8_t layers)
    {
        if (count == 0 || count > 16 || layers == 0 || layers > 8) return false;
        if (count == count_ && layers == layers_) return true;
        const size_t bytes = static_cast<size_t>(count) * layers * sizeof(Control);
        void *raw = heap_caps_malloc(bytes, MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT);
        if (raw == nullptr) raw = heap_caps_malloc(bytes, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
        if (raw == nullptr) return false; // Preserve the current layout and values.
        auto *next = static_cast<Control *>(raw);
        for (uint8_t layer = 0; layer < layers; ++layer)
            for (uint8_t control = 0; control < count; ++control) {
                auto *slot = new (&next[layer * count + control]) Control();
                if (layer < layers_ && control < count_)
                    *slot = data_[layer * count_ + control];
            }
        heap_caps_free(data_);
        data_ = next;
        count_ = count;
        layers_ = layers;
        return true;
    }

    bool contains(uint8_t index) const
    {
        return index / 16 < layers_ && index % 16 < count_;
    }
    Control &operator[](uint8_t index) { return data_[(index / 16) * count_ + index % 16]; }
    const Control &operator[](uint8_t index) const { return data_[(index / 16) * count_ + index % 16]; }
};
