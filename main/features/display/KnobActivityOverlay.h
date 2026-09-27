#pragma once

#include <algorithm>
#include <cstdio>
#include <cstring>
#include <vector>

#include "lvgl.h"
#include "screens.h"
#include "features/synth/KnobActivity.h"

class KnobActivityOverlay
{
    struct Card
    {
        KnobActivity activity;
        lv_obj_t *root;
        lv_obj_t *arc;
        lv_obj_t *value;
        lv_obj_t *device;
        lv_obj_t *port;
        lv_obj_t *name;
        bool dirty = false;
    };
    lv_obj_t *panel_ = nullptr;
    std::vector<Card> cards_;

    static void refresh(Card &card)
    {
        const auto &activity = card.activity;
        lv_arc_set_value(card.arc, activity.raw);
        char value[4];
        snprintf(value, sizeof(value), "%u", static_cast<unsigned>(activity.raw));
        auto label = [](lv_obj_t *obj, const char *text)
        {
            if (strcmp(lv_label_get_text(obj), text) != 0) lv_label_set_text(obj, text);
        };
        label(card.value, value);
        label(card.name, activity.name);
        label(card.device, activity.device[0] ? activity.device : "Nicht verbunden");
        label(card.port, activity.port[0] ? portLabel(activity.port) : "--");
        card.dirty = false;
    }

    static const char *portLabel(const char *port)
    {
        if (strcmp(port, "pitchCv") == 0) return "Pitch CV";
        if (strcmp(port, "clock") == 0) return "Trigger";
        if (strcmp(port, "pulseTime") == 0) return "Gate-Out-Time";
        if (strcmp(port, "switchAtEnd") == 0) return "Switch at End";
        return port;
    }

    void layout()
    {
        if (panel_ == nullptr) return;
        const int availableWidth = lv_display_get_horizontal_resolution(nullptr) - 32;
        const int availableHeight = lv_display_get_vertical_resolution(nullptr) - 32;
        const int columns = std::max(1, availableWidth / 162);
        const int count = static_cast<int>(cards_.size());
        const int usedColumns = std::min(count, columns);
        const int rows = (count + columns - 1) / columns;
        lv_obj_set_size(panel_, usedColumns * 150 + (usedColumns - 1) * 12 + 16,
                        std::min(availableHeight, rows * 194 + (rows - 1) * 12 + 16));
        lv_obj_center(panel_);
    }

public:
    void clear()
    {
        if (panel_ != nullptr) lv_obj_delete(panel_);
        panel_ = nullptr;
        cards_.clear();
    }

    void update(const KnobActivity &activity, bool defer = false)
    {
        auto found = std::find_if(cards_.begin(), cards_.end(), [&](const Card &card)
        {
            return card.activity.controlIndex == activity.controlIndex &&
                   strcmp(card.activity.sourceId, activity.sourceId) == 0 &&
                   strcmp(card.activity.targetId, activity.targetId) == 0 &&
                   strcmp(card.activity.port, activity.port) == 0;
        });
        if (found == cards_.end())
        {
            if (panel_ == nullptr)
            {
                panel_ = lv_obj_create(lv_layer_top());
                lv_obj_set_style_pad_all(panel_, 8, 0);
                lv_obj_set_style_pad_row(panel_, 12, 0);
                lv_obj_set_style_pad_column(panel_, 12, 0);
                lv_obj_set_style_border_width(panel_, 0, 0);
                lv_obj_set_flex_flow(panel_, LV_FLEX_FLOW_ROW_WRAP);
                lv_obj_clear_flag(panel_, LV_OBJ_FLAG_CLICKABLE);
                lv_obj_set_scroll_dir(panel_, LV_DIR_VER);
            }
            Card card{};
            card.root = lv_obj_create(panel_);
            lv_obj_set_size(card.root, 150, 194);
            lv_obj_set_style_pad_all(card.root, 0, 0);
            lv_obj_set_style_border_width(card.root, 0, 0);
            lv_obj_set_style_bg_opa(card.root, LV_OPA_TRANSP, 0);
            lv_obj_clear_flag(card.root, static_cast<lv_obj_flag_t>(LV_OBJ_FLAG_SCROLLABLE | LV_OBJ_FLAG_CLICKABLE));
            // EEZ's reusable factory writes its five handles into objects[].
            // Capture them under the LVGL lock and immediately restore screen handles.
            const objects_t screenObjects = objects;
            create_user_widget_knob(card.root, 0);
            auto **handles = reinterpret_cast<lv_obj_t **>(&objects);
            card.arc = handles[0];
            card.value = handles[1];
            card.device = handles[2];
            card.port = handles[3];
            card.name = handles[4];
            objects = screenObjects;
            lv_arc_set_range(card.arc, 0, 127);
            lv_obj_clear_flag(card.arc, LV_OBJ_FLAG_CLICKABLE);
            cards_.push_back(card);
            found = cards_.end() - 1;
            layout();
        }
        found->activity = activity;
        found->dirty = true;
        if (!defer) refresh(*found);
    }

    void tick(int64_t nowUs)
    {
        const size_t previousCount = cards_.size();
        cards_.erase(std::remove_if(cards_.begin(), cards_.end(), [&](const Card &card)
        {
            if (nowUs - card.activity.updatedUs < 1000000) return false;
            lv_obj_delete(card.root);
            return true;
        }), cards_.end());
        if (cards_.empty()) clear();
        else if (cards_.size() != previousCount) layout();
        for (auto &card : cards_)
            if (card.dirty) refresh(card);
    }
};
