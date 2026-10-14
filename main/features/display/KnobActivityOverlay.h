#pragma once

#include <algorithm>
#include <cstdio>
#include <cstring>
#include <vector>

#include "lvgl.h"
#include "screens.h"
#include "features/synth/KnobActivity.h"
#include "features/display/PortDisplayLabels.h"
#include "esp_log.h"
#include "features/display/MidiButtonCard.h"

class KnobActivityOverlay
{
    struct Card
    {
        KnobActivity activity;
        lv_obj_t *root;
        lv_obj_t *bar; // nullptr on MidiButton cards
        lv_obj_t *value;
        lv_obj_t *device;
        lv_obj_t *port;
        lv_obj_t *name;
        lv_obj_t *nodeValue;
        lv_obj_t *chart = nullptr;
        lv_chart_series_t *series = nullptr;
        bool dirty = false;
    };
    lv_obj_t *panel_ = nullptr;
    std::vector<Card> cards_;
    std::vector<KnobActivity> pending_;
    std::vector<Card> spare_;
    lv_obj_t *sparePanel_ = nullptr;
    bool layoutDirty_ = false;

    static void refresh(Card &card)
    {
        const auto &activity = card.activity;
        if (activity.isEnvelope)
        {
            if (strcmp(lv_label_get_text(card.name), activity.name) != 0)
                lv_label_set_text(card.name, activity.name);
            for (uint32_t i = 0; i < 6; ++i)
                lv_chart_set_series_value_by_id2(card.chart, card.series, i, activity.envelope[i].x, activity.envelope[i].y);
            lv_chart_refresh(card.chart);
            card.dirty = false;
            return;
        }
        char value[24];
        if (activity.isButton)
            snprintf(value, sizeof(value), "%.4g", static_cast<double>(activity.sourceValue));
        else
        {
            if (lv_bar_get_value(card.bar) != activity.raw) lv_bar_set_value(card.bar, activity.raw, LV_ANIM_OFF);
            snprintf(value, sizeof(value), "%u", static_cast<unsigned>(activity.raw));
        }
        auto label = [](lv_obj_t *obj, const char *text)
        {
            if (strcmp(lv_label_get_text(obj), text) != 0) lv_label_set_text(obj, text);
        };
        label(card.value, value);
        label(card.name, activity.name);
        label(card.device, activity.device[0] ? activity.device : "Nicht verbunden");
        label(card.port, portlabels::display(static_cast<NodeType>(activity.targetType), activity.port));
        char nodeValue[24] = "";
        if (activity.targetId[0])
        {
            if (activity.targetValueRaw)
                snprintf(nodeValue, sizeof(nodeValue), "%.0f", static_cast<double>(activity.targetValue));
            else
                snprintf(nodeValue, sizeof(nodeValue), "%.2f V", static_cast<double>(activity.targetValue));
        }
        label(card.nodeValue, nodeValue);
        card.dirty = false;
    }

    void layout()
    {
        if (panel_ == nullptr) return;
        const int availableWidth = lv_display_get_horizontal_resolution(nullptr) - 32;
        const int availableHeight = lv_display_get_vertical_resolution(nullptr) - 32;
        const bool hasEnvelope = std::any_of(cards_.begin(), cards_.end(), [](const Card &card) { return card.activity.isEnvelope; });
        const int cellWidth = hasEnvelope ? 206 : 161;
        const bool hasButtonOrEnvelope = std::any_of(cards_.begin(), cards_.end(), [](const Card &card) { return card.activity.isButton || card.activity.isEnvelope; });
        const int rowHeight = hasButtonOrEnvelope ? 150 : 95;
        const int columns = std::max(1, (availableWidth - 4) / (cellWidth + 12));
        const int count = static_cast<int>(cards_.size());
        const int usedColumns = std::min(count, columns);
        const int rows = (count + columns - 1) / columns;
        lv_obj_set_size(panel_, usedColumns * cellWidth + (usedColumns - 1) * 12 + 16,
                        std::min(availableHeight, rows * rowHeight + (rows - 1) * 12 + 16));
        lv_obj_center(panel_);
    }

    void apply(const KnobActivity &activity, bool defer)
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
            const auto reuse = std::find_if(spare_.begin(), spare_.end(), [&](const Card &candidate) {
                return candidate.activity.isEnvelope == activity.isEnvelope && candidate.activity.isButton == activity.isButton;
            });
            if (reuse != spare_.end()) {
                card = *reuse;
                spare_.erase(reuse);
                lv_obj_set_parent(card.root, panel_);
            } else {
            card.root = lv_obj_create(panel_);
            lv_obj_set_size(card.root, activity.isEnvelope ? 200 : (activity.isButton ? 150 : 155), (activity.isButton || activity.isEnvelope) ? 150 : 95);
            lv_obj_set_style_margin_hor(card.root, 3, 0);
            lv_obj_set_style_pad_all(card.root, 0, 0);
            lv_obj_set_style_border_width(card.root, 0, 0);
            lv_obj_set_style_bg_opa(card.root, LV_OPA_TRANSP, 0);
            lv_obj_clear_flag(card.root, static_cast<lv_obj_flag_t>(LV_OBJ_FLAG_SCROLLABLE | LV_OBJ_FLAG_CLICKABLE));
            // EEZ's reusable factories write their handles into objects[].
            // Capture them under the LVGL lock and immediately restore screen handles.
            const objects_t screenObjects = objects;
            auto **handles = reinterpret_cast<lv_obj_t **>(&objects);
            if (activity.isEnvelope)
            {
                create_user_widget_envelope(card.root, 0);
                card.name = handles[0];
                card.chart = handles[1];
                lv_obj_clear_flag(card.chart, static_cast<lv_obj_flag_t>(LV_OBJ_FLAG_SCROLLABLE | LV_OBJ_FLAG_CLICKABLE));
                lv_chart_set_type(card.chart, LV_CHART_TYPE_SCATTER);
                lv_chart_set_point_count(card.chart, 6);
                lv_chart_set_range(card.chart, LV_CHART_AXIS_PRIMARY_X, 0, 1000);
                lv_chart_set_range(card.chart, LV_CHART_AXIS_PRIMARY_Y, 0, 1000);
                lv_chart_set_div_line_count(card.chart, 4, 0);
                lv_obj_set_style_pad_all(card.chart, 8, 0);
                lv_obj_set_style_line_width(card.chart, 2, LV_PART_ITEMS);
                lv_obj_set_style_size(card.chart, 4, 4, LV_PART_INDICATOR);
                card.series = lv_chart_add_series(card.chart, lv_color_hex(0x32bbed), LV_CHART_AXIS_PRIMARY_Y);
            }
            else if (activity.isButton)
            {
                create_user_widget_midi_button(card.root, 0);
                card.bar = nullptr;
                card.value = handles[0];
                card.device = handles[1];
                card.port = handles[2];
                card.name = handles[3];
                card.nodeValue = handles[4];
                // Display only, not pressable.
                if (lv_obj_t *button = midiButtonOf(card.root)) lv_obj_clear_flag(button, LV_OBJ_FLAG_CLICKABLE);
            }
            else
            {
                create_user_widget_knob(card.root, 0);
                const KnobHandles knob = KnobHandles::from(handles);
                if (!knob.valid())
                {
                    objects = screenObjects;
                    ESP_LOGE("KnobOverlay", "Knob widget handle order does not match firmware (screens.c re-exported?)");
                    lv_obj_delete(card.root);
                    return;
                }
                card.bar = knob.bar;
                card.value = knob.value;
                card.device = knob.device;
                card.port = knob.port;
                card.name = knob.name;
                card.nodeValue = knob.nodeValue;
                lv_bar_set_range(card.bar, 0, 127);
                lv_obj_clear_flag(card.bar, LV_OBJ_FLAG_CLICKABLE);
            }
            objects = screenObjects;
            }
            card.activity = activity;
            cards_.push_back(card);
            found = cards_.end() - 1;
            layoutDirty_ = true;
            if (!defer) { layout(); layoutDirty_ = false; }
        }
        found->activity = activity;
        found->dirty = true;
        if (!defer) refresh(*found);
    }

    void release(const Card &card)
    {
        if (sparePanel_ != nullptr && !card.activity.isEnvelope && spare_.size() < 16) {
            lv_obj_set_parent(card.root, sparePanel_);
            spare_.push_back(card);
        } else lv_obj_delete(card.root);
    }

public:
    ~KnobActivityOverlay()
    {
        clear();
        if (sparePanel_ != nullptr) lv_obj_delete(sparePanel_);
    }

    void clear()
    {
        for (auto &card : cards_) release(card);
        if (panel_ != nullptr) lv_obj_delete(panel_);
        panel_ = nullptr;
        cards_.clear();
        pending_.clear();
        layoutDirty_ = false;
    }

    // Prepare the reusable EEZ widgets behind the startup splash, before first CC.
    void warmup()
    {
        if (sparePanel_ != nullptr) return;
        sparePanel_ = lv_obj_create(lv_layer_top());
        lv_obj_add_flag(sparePanel_, LV_OBJ_FLAG_HIDDEN);
        for (int i = 0; i < 8; ++i) {
            KnobActivity activity{};
            activity.controlIndex = i;
            activity.isButton = i >= 6;
            apply(activity, true);
        }
        clear();
    }

    void update(const KnobActivity &activity, bool defer = false)
    {
        if (!defer) { apply(activity, false); return; }
        // Drain the notification queue without creating widgets. Only the latest
        // state for each source/target pair reaches the expensive LVGL path.
        const auto found = std::find_if(pending_.begin(), pending_.end(), [&](const KnobActivity &entry) {
            return entry.controlIndex == activity.controlIndex && strcmp(entry.sourceId, activity.sourceId) == 0 &&
                   strcmp(entry.targetId, activity.targetId) == 0 && strcmp(entry.port, activity.port) == 0;
        });
        if (found == pending_.end()) pending_.push_back(activity);
        else *found = activity;
    }

    void tick(int64_t nowUs)
    {
        for (const auto &activity : pending_)
            if (nowUs - activity.updatedUs < activity.durationUs()) apply(activity, true);
        pending_.clear();
        const size_t previousCount = cards_.size();
        cards_.erase(std::remove_if(cards_.begin(), cards_.end(), [&](const Card &card)
        {
            if (nowUs - card.activity.updatedUs < card.activity.durationUs()) return false;
            release(card);
            return true;
        }), cards_.end());
        if (cards_.empty()) clear();
        else if (layoutDirty_ || cards_.size() != previousCount) { layout(); layoutDirty_ = false; }
        for (auto &card : cards_)
            if (card.dirty) refresh(card);
    }
};
