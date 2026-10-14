#pragma once

#include "lvgl.h"

// The EEZ MidiButton widget no longer nests its value label inside the lv_button, so the
// button has no handle and is no parent of anything. Find it by class among the card's children.
inline lv_obj_t *midiButtonOf(lv_obj_t *cardRoot)
{
    const uint32_t count = lv_obj_get_child_count(cardRoot);
    for (uint32_t i = 0; i < count; ++i)
    {
        lv_obj_t *child = lv_obj_get_child(cardRoot, i);
        if (lv_obj_check_type(child, &lv_button_class)) return child;
    }
    return nullptr;
}

// Handle order of create_user_widget_knob() follows the widget creation order in EEZ Studio and
// changes whenever widgets are re-ordered there. Writing a label API call to a bar corrupts
// memory, so check the types once after creation and drop the card instead of crashing.
struct KnobHandles
{
    lv_obj_t *bar, *value, *device, *port, *name, *nodeValue;

    static KnobHandles from(lv_obj_t *const *h) { return {h[0], h[1], h[2], h[3], h[5], h[4]}; }

    bool valid() const
    {
        return bar != nullptr && lv_obj_check_type(bar, &lv_bar_class) &&
               lv_obj_check_type(value, &lv_label_class) && lv_obj_check_type(device, &lv_label_class) &&
               lv_obj_check_type(port, &lv_label_class) && lv_obj_check_type(name, &lv_label_class) &&
               lv_obj_check_type(nodeValue, &lv_label_class);
    }
};
