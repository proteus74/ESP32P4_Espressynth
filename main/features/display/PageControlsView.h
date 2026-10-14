#pragma once

#include <algorithm>
#include <cstdio>
#include <cstring>
#include <vector>
#include <utility>

#include "lvgl.h"
#include "screens.h"
#include "eez_ui_bridge.hpp"
#include "esp_log.h"
#include "features/display/MidiButtonCard.h"
#include "features/synth/KnobActivity.h"

// The Knob/Button controls of one preset page as touch-editable cards inside wndmain.
// A touch sends the same paramUpdate a browser drag sends (eez_send_control_param), so
// routes, MIDI-CC-out and the activity display behave exactly like any other live change.
class PageControlsView
{
    struct Card
    {
        EezPageControl control;
        lv_obj_t *root;
        lv_obj_t *bar;    // nullptr on button cards
        lv_obj_t *button; // nullptr on knob cards
        lv_obj_t *value;
        lv_obj_t *nodeValue;
        bool dirty = false;
    };
    lv_obj_t *panel_ = nullptr;
    std::vector<Card> cards_;
    struct CachedView {
        char pageId[32];
        lv_obj_t *panel;
        std::vector<Card> cards;
    };
    std::vector<CachedView> cached_;
    char pageId_[32] = {};
    static constexpr size_t kCachedViews = 8;

    void saveCurrent()
    {
        if (panel_ == nullptr) return;
        lv_obj_add_flag(panel_, LV_OBJ_FLAG_HIDDEN);
        if (cached_.size() >= kCachedViews) {
            lv_obj_delete(cached_.front().panel);
            cached_.erase(cached_.begin());
        }
        CachedView saved{};
        snprintf(saved.pageId, sizeof(saved.pageId), "%s", pageId_);
        saved.panel = panel_;
        saved.cards = std::move(cards_); // Event user-data pointers stay at the same addresses.
        cached_.push_back(std::move(saved));
        panel_ = nullptr;
        pageId_[0] = '\0';
    }

    static void setText(lv_obj_t *label, const char *text)
    {
        if (strcmp(lv_label_get_text(label), text) != 0) lv_label_set_text(label, text);
    }

    static void refresh(Card &card)
    {
        card.dirty = false;
        char text[24];
        if (card.control.is_button)
        {
            snprintf(text, sizeof(text), "%.4g", static_cast<double>(card.control.value));
            setText(card.value, text);
            if (card.button != nullptr && card.control.switch_mode && card.control.list_count < 2)
            {
                if (card.control.was_above) lv_obj_add_state(card.button, LV_STATE_CHECKED);
                else lv_obj_remove_state(card.button, LV_STATE_CHECKED);
            }
            return;
        }
        if (lv_bar_get_value(card.bar) != card.control.raw) lv_bar_set_value(card.bar, card.control.raw, LV_ANIM_OFF);
        snprintf(text, sizeof(text), "%u", static_cast<unsigned>(card.control.raw));
        setText(card.value, text);
        snprintf(text, sizeof(text), "%.2f", static_cast<double>(card.control.value));
        setText(card.nodeValue, text);
    }

    // Finger position on the bar sets the raw value (0..127), same paramUpdate as a browser drag.
    static void onBarTouch(lv_event_t *e)
    {
        Card &card = *static_cast<Card *>(lv_event_get_user_data(e));
        lv_point_t point;
        lv_indev_get_point(lv_indev_get_act(), &point);
        lv_area_t area;
        lv_obj_get_coords(card.bar, &area);
        const int32_t width = lv_area_get_width(&area);
        int32_t raw = (point.x - area.x1) * 127 / (width - 1);
        raw = std::clamp<int32_t>(raw, 0, 127);
        if (raw == card.control.raw) return;
        card.control.raw = static_cast<uint8_t>(raw);
        eez_send_control_param(card.control.widget_id, false, card.control.control_index, "raw", static_cast<float>(raw));
        refresh(card);
    }

    // Mirrors Graph::handleMidiEvent's button press/release handling.
    static void onButton(lv_event_t *e)
    {
        Card &card = *static_cast<Card *>(lv_event_get_user_data(e));
        EezPageControl &c = card.control;
        const bool pressed = lv_event_get_code(e) == LV_EVENT_PRESSED;
        if (c.switch_mode)
        {
            if (!pressed) return;
            if (c.list_count >= 2)
            {
                c.list_index = static_cast<uint8_t>((c.list_index + 1) % c.list_count);
                eez_send_control_param(c.widget_id, true, c.control_index, "listIndex", c.list_index);
            }
            else
            {
                c.was_above = !c.was_above;
                eez_send_control_param(c.widget_id, true, c.control_index, "wasAbove", c.was_above ? 1.0f : 0.0f);
            }
        }
        else
        {
            if (pressed && c.list_count >= 2 && !c.was_above)
            {
                c.list_index = static_cast<uint8_t>((c.list_index + 1) % c.list_count);
                eez_send_control_param(c.widget_id, true, c.control_index, "listIndex", c.list_index);
            }
            c.was_above = pressed;
            eez_send_control_param(c.widget_id, true, c.control_index, "wasAbove", pressed ? 1.0f : 0.0f);
        }
        refresh(card);
    }

public:
    void clear()
    {
        if (panel_ != nullptr) lv_obj_delete(panel_);
        panel_ = nullptr;
        cards_.clear();
        for (auto &view : cached_) lv_obj_delete(view.panel);
        cached_.clear();
        pageId_[0] = '\0';
    }

    void show(lv_obj_t *parent, const EezPageControls &controls, const char *pageId = "")
    {
        if (pageId[0] != '\0') {
            if (panel_ != nullptr && strcmp(pageId_, pageId) == 0) {
                setHidden(false);
                tick();
                return;
            }
            saveCurrent();
            for (auto it = cached_.begin(); it != cached_.end(); ++it) {
                if (strcmp(it->pageId, pageId) != 0) continue;
                panel_ = it->panel;
                cards_ = std::move(it->cards);
                cached_.erase(it);
                snprintf(pageId_, sizeof(pageId_), "%s", pageId);
                setHidden(false);
                tick();
                return;
            }
        } else clear();
        snprintf(pageId_, sizeof(pageId_), "%s", pageId);
        panel_ = lv_obj_create(parent);
        lv_obj_set_size(panel_, LV_PCT(100), LV_PCT(100));
        lv_obj_set_style_pad_all(panel_, 8, 0);
        lv_obj_set_style_pad_row(panel_, 12, 0);
        lv_obj_set_style_pad_column(panel_, 12, 0);
        lv_obj_set_style_border_width(panel_, 0, 0);
        lv_obj_set_style_radius(panel_, 0, 0);
        lv_obj_set_flex_flow(panel_, LV_FLEX_FLOW_ROW_WRAP);
        lv_obj_set_scroll_dir(panel_, LV_DIR_VER);

        // Card addresses are event user data: reserve up front so push_back never moves them.
        cards_.reserve(controls.count);
        for (uint16_t i = 0; i < controls.count; ++i)
        {
            Card card{};
            card.control = controls.controls[i];
            card.root = lv_obj_create(panel_);
            lv_obj_set_size(card.root, card.control.is_button ? 150 : 155, card.control.is_button ? 150 : 95);
            lv_obj_set_style_pad_all(card.root, 0, 0);
            lv_obj_set_style_border_width(card.root, 0, 0);
            lv_obj_set_style_bg_opa(card.root, LV_OPA_TRANSP, 0);
            lv_obj_clear_flag(card.root, static_cast<lv_obj_flag_t>(LV_OBJ_FLAG_SCROLLABLE | LV_OBJ_FLAG_CLICKABLE));
            // EEZ's reusable factories write their handles into objects[] - capture them and
            // restore the screen handles right away (same as KnobActivityOverlay).
            const objects_t screenObjects = objects;
            auto **handles = reinterpret_cast<lv_obj_t **>(&objects);
            lv_obj_t *device;
            lv_obj_t *port;
            lv_obj_t *name;
            if (card.control.is_button)
            {
                create_user_widget_midi_button(card.root, 0);
                card.value = handles[0];
                device = handles[1];
                port = handles[2];
                name = handles[3];
                card.nodeValue = handles[4];
                card.button = midiButtonOf(card.root);
            }
            else
            {
                create_user_widget_knob(card.root, 0);
                const KnobHandles knob = KnobHandles::from(handles);
                if (!knob.valid())
                {
                    objects = screenObjects;
                    ESP_LOGE("PageControls", "Knob widget handle order does not match firmware (screens.c re-exported?)");
                    lv_obj_delete(card.root);
                    continue;
                }
                card.bar = knob.bar;
                card.value = knob.value;
                device = knob.device;
                port = knob.port;
                name = knob.name;
                card.nodeValue = knob.nodeValue;
                lv_bar_set_range(card.bar, 0, 127);
            }
            objects = screenObjects;
            lv_label_set_text(name, card.control.name);
            lv_label_set_text(device, card.control.widget_label);
            lv_label_set_text(port, "");
            lv_label_set_text(card.nodeValue, "");
            cards_.push_back(card);
            Card &stored = cards_.back();
            if (stored.control.is_button)
            {
                if (stored.button != nullptr) lv_obj_add_event_cb(stored.button, onButton, LV_EVENT_PRESSED, &stored);
                if (stored.button != nullptr) lv_obj_add_event_cb(stored.button, onButton, LV_EVENT_RELEASED, &stored);
                if (stored.button != nullptr) lv_obj_add_event_cb(stored.button, onButton, LV_EVENT_PRESS_LOST, &stored);
            }
            else
            {
                lv_obj_add_flag(stored.bar, LV_OBJ_FLAG_CLICKABLE);
                lv_obj_add_event_cb(stored.bar, onBarTouch, LV_EVENT_PRESSED, &stored);
                lv_obj_add_event_cb(stored.bar, onBarTouch, LV_EVENT_PRESSING, &stored);
            }
            refresh(stored);
        }
    }

    // Build a hidden view during prefetch, keeping the visible page unchanged.
    void prepare(lv_obj_t *parent, const EezPageControls &controls, const char *pageId)
    {
        if (strcmp(pageId_, pageId) == 0 || cached_.size() >= kCachedViews - 1) return;
        for (const auto &view : cached_)
            if (strcmp(view.pageId, pageId) == 0) return;
        char previous[sizeof(pageId_)];
        snprintf(previous, sizeof(previous), "%s", pageId_);
        const bool hidden = panel_ != nullptr && lv_obj_has_flag(panel_, LV_OBJ_FLAG_HIDDEN);
        show(parent, controls, pageId);
        setHidden(true);
        if (previous[0] != '\0') {
            show(parent, controls, previous);
            setHidden(hidden);
        } else saveCurrent();
    }

    void setHidden(bool hidden)
    {
        if (panel_ == nullptr) return;
        if (hidden) lv_obj_add_flag(panel_, LV_OBJ_FLAG_HIDDEN);
        else lv_obj_remove_flag(panel_, LV_OBJ_FLAG_HIDDEN);
    }

    // Takes over a value change from MIDI, the browser or this view's own touches.
    // Returns true when the control is on this page (so the activity overlay can stay away).
    bool apply(const KnobActivity &activity, bool defer = false)
    {
        bool shown = false;
        for (auto &card : cards_)
        {
            if (card.control.control_index != activity.controlIndex ||
                strcmp(card.control.widget_id, activity.sourceId) != 0)
                continue;
            shown = true;
            // While a finger is on the bar, the touch is the newest value - don't jump back.
            if (!card.control.is_button && !lv_obj_has_state(card.bar, LV_STATE_PRESSED))
                card.control.raw = activity.raw;
            card.control.value = activity.sourceValue;
            if (defer) card.dirty = true;
            else refresh(card);
        }
        // Hidden cached pages track live state without formatting or invalidating pixels.
        for (auto &view : cached_)
            for (auto &card : view.cards)
                if (card.control.control_index == activity.controlIndex &&
                    strcmp(card.control.widget_id, activity.sourceId) == 0) {
                    if (!card.control.is_button) card.control.raw = activity.raw;
                    card.control.value = activity.sourceValue;
                    card.dirty = true;
                }
        return shown;
    }

    void tick()
    {
        for (auto &card : cards_)
            if (card.dirty) refresh(card);
    }
};
