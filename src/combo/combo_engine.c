/*
 * Copyright (c) 2020 The ZMK Contributors
 *
 * SPDX-License-Identifier: MIT
 *
 * ZMK's app/src/combo.c (cormoran/zmk at 1fc72aaf), with the combo table built at runtime from
 * the dax_combo settings instead of the zmk,combos devicetree node. Apart from the table, the
 * changes are: require_prior_idle_ms is 32 bits, the listener is named dax_combo, and the file
 * scope is static so this can sit next to ZMK's own combos. Keep the rest identical, so that a
 * ZMK update can be diffed against it.
 */

#include <zephyr/device.h>
#include <zephyr/logging/log.h>
#include <zephyr/sys/dlist.h>
#include <zephyr/sys/util.h>
#include <zephyr/kernel.h>

#include <drivers/behavior.h>

#include <zmk/behavior.h>
#include <zmk/event_manager.h>
#include <zmk/events/position_state_changed.h>
#include <zmk/events/keycode_state_changed.h>
#include <zmk/hid.h>
#include <zmk/matrix.h>
#include <zmk/keymap.h>
#include <zmk/virtual_key_position.h>

#include <dax/combo.h>

LOG_MODULE_DECLARE(zmk, CONFIG_ZMK_LOG_LEVEL);

#define MAX_COMBO_KEYS CONFIG_ZMK_DAX_COMBO_MAX_KEYS_PER_COMBO

struct combo_cfg {
    int32_t key_positions[MAX_COMBO_KEYS];
    int16_t key_position_len;
    int32_t require_prior_idle_ms;
    int32_t timeout_ms;
    uint32_t layer_mask;
    struct zmk_behavior_binding behavior;
    // if slow release is set, the combo releases when the last key is released.
    // otherwise, the combo releases when the first key is released.
    bool slow_release;
};

struct active_combo {
    uint16_t combo_idx;
    // key_positions_pressed is filled with key_positions when the combo is pressed.
    // The keys are removed from this array when they are released.
    // Once this array is empty, the behavior is released.
    uint16_t key_positions_pressed_count;
    struct zmk_position_state_changed_event key_positions_pressed[MAX_COMBO_KEYS];
};

// Sorted shortest key positions list first, as ZMK's devicetree table is, so that the bitmasks
// find the shorter combos first. Built by rebuild_combos() below.
static struct combo_cfg combos[CONFIG_ZMK_DAX_COMBO_MAX_COMBOS];
static size_t combos_len;

// We need at least 4 bytes to avoid alignment issues
#define BYTES_FOR_COMBOS_MASK DIV_ROUND_UP(CONFIG_ZMK_DAX_COMBO_MAX_COMBOS, 32)

static uint8_t pressed_keys_count = 0;
// set of keys pressed
static struct zmk_position_state_changed_event pressed_keys[MAX_COMBO_KEYS] = {};
// the set of candidate combos based on the currently pressed_keys
static uint32_t candidates[BYTES_FOR_COMBOS_MASK];
// the last candidate that was completely pressed
static int16_t fully_pressed_combo = INT16_MAX;
// a lookup dict that maps a key position to all combos on that position
static uint32_t combo_lookup[ZMK_KEYMAP_LEN][BYTES_FOR_COMBOS_MASK] = {};
// combos that have been activated and still have (some) keys pressed
// this array is always contiguous from 0.
static struct active_combo active_combos[CONFIG_ZMK_COMBO_MAX_PRESSED_COMBOS] = {};
static uint8_t active_combo_count = 0;

static struct k_work_delayable timeout_task;
static int64_t timeout_task_timeout_at;

// this keeps track of the last non-combo, non-mod key tap
static int64_t last_tapped_timestamp = INT32_MIN;
// this keeps track of the last time a combo was pressed
static int64_t last_combo_timestamp = INT32_MIN;

static void store_last_tapped(int64_t timestamp) {
    if (timestamp > last_combo_timestamp) {
        last_tapped_timestamp = timestamp;
    }
}

// Store the combo key pointer in the combos array, one pointer for each key position
// The combos are sorted shortest-first, then by virtual-key-position.
static int initialize_combo(size_t index) {
    const struct combo_cfg *new_combo = &combos[index];

    for (size_t kp = 0; kp < new_combo->key_position_len; kp++) {
        sys_bitfield_set_bit((mem_addr_t)&combo_lookup[new_combo->key_positions[kp]], index);
    }

    return 0;
}

static bool combo_active_on_layer(const struct combo_cfg *combo, uint8_t layer) {
    if (!combo->layer_mask) {
        return true;
    }

    return combo->layer_mask & BIT(layer);
}

static bool is_quick_tap(const struct combo_cfg *combo, int64_t timestamp) {
    return (last_tapped_timestamp + combo->require_prior_idle_ms) > timestamp;
}

static int setup_candidates_for_first_keypress(int32_t position, int64_t timestamp) {
    int number_of_combo_candidates = 0;
    uint8_t highest_active_layer = zmk_keymap_highest_layer_active();

    for (size_t i = 0; i < combos_len; i++) {
        if (sys_bitfield_test_bit((mem_addr_t)&combo_lookup[position], i)) {
            const struct combo_cfg *combo = &combos[i];
            if (combo_active_on_layer(combo, highest_active_layer) &&
                !is_quick_tap(combo, timestamp)) {
                sys_bitfield_set_bit((mem_addr_t)&candidates, i);
                number_of_combo_candidates++;
            }
            // LOG_DBG("combo timeout %d %d %d", position, i, candidates[i].timeout_at);
        }
    }

    return number_of_combo_candidates;
}

static inline uint8_t zero_one_or_more_bits(uint32_t field) {
    if (field == 0) {
        return 0;
    }
    if ((field & (field - 1)) == 0) {
        return 1;
    }
    return 2;
}

static int filter_candidates(int32_t position) {
    int matches = 0;
    for (int i = 0; i < BYTES_FOR_COMBOS_MASK; i++) {
        candidates[i] &= combo_lookup[position][i];
        if (matches < 2) {
            matches += zero_one_or_more_bits(candidates[i]);
        }
    }

    LOG_DBG("combo matches after filter %d", matches);
    return matches;
}

static int64_t first_candidate_timeout() {
    if (pressed_keys_count == 0) {
        return LONG_MAX;
    }

    int64_t first_timeout = LONG_MAX;
    for (int i = 0; i < combos_len; i++) {
        if (sys_bitfield_test_bit((mem_addr_t)&candidates, i)) {
            first_timeout = MIN(first_timeout, combos[i].timeout_ms);
        }
    }

    return pressed_keys[0].data.timestamp + first_timeout;
}

static inline bool candidate_is_completely_pressed(const struct combo_cfg *candidate) {
    // this code assumes set(pressed_keys) <= set(candidate->key_positions)
    // this invariant is enforced by filter_candidates
    // since events may have been reraised after clearing one or more slots at
    // the start of pressed_keys (see: release_pressed_keys), we have to check
    // that each key needed to trigger the combo was pressed, not just the last.
    return candidate->key_position_len == pressed_keys_count;
}

static int cleanup();

static int filter_timed_out_candidates(int64_t timestamp) {
    __ASSERT(pressed_keys_count > 0, "Searching for a candidate timeout with no keys pressed");

    int remaining_candidates = 0;
    for (int i = 0; i < combos_len; i++) {
        if (sys_bitfield_test_bit((mem_addr_t)&candidates, i)) {

            if (pressed_keys[0].data.timestamp + combos[i].timeout_ms > timestamp) {
                remaining_candidates++;
            } else {
                sys_bitfield_clear_bit((mem_addr_t)&candidates, i);
            }
        }
    }

    LOG_DBG(
        "after filtering out timed out combo candidates: remaining_candidates=%d timestamp=%lld",
        remaining_candidates, timestamp);

    return remaining_candidates;
}

static int capture_pressed_key(const struct zmk_position_state_changed *ev) {
    if (pressed_keys_count == MAX_COMBO_KEYS) {
        return ZMK_EV_EVENT_BUBBLE;
    }

    pressed_keys[pressed_keys_count++] = copy_raised_zmk_position_state_changed(ev);
    return ZMK_EV_EVENT_CAPTURED;
}

const struct zmk_listener zmk_listener_dax_combo;

static int release_pressed_keys() {
    uint8_t count = pressed_keys_count;
    pressed_keys_count = 0;
    for (int i = 0; i < count; i++) {
        struct zmk_position_state_changed_event *ev = &pressed_keys[i];
        if (i == 0) {
            LOG_DBG("combo: releasing position event %d", ev->data.position);
            ZMK_EVENT_RELEASE(*ev);
        } else {
            // reprocess events (see tests/combo/fully-overlapping-combos-3 for why this is needed)
            LOG_DBG("combo: reraising position event %d", ev->data.position);
            ZMK_EVENT_RAISE(*ev);
        }
    }

    return count;
}

static inline int press_combo_behavior(int combo_idx, const struct combo_cfg *combo,
                                       int32_t timestamp) {
    struct zmk_behavior_binding_event event = {
        .position = ZMK_VIRTUAL_KEY_POSITION_COMBO(combo_idx),
        .timestamp = timestamp,
#if IS_ENABLED(CONFIG_ZMK_SPLIT)
        .source = ZMK_POSITION_STATE_CHANGE_SOURCE_LOCAL,
#endif
    };

    last_combo_timestamp = timestamp;

    return zmk_behavior_invoke_binding(&combo->behavior, event, true);
}

static inline int release_combo_behavior(int combo_idx, const struct combo_cfg *combo,
                                         int32_t timestamp) {
    struct zmk_behavior_binding_event event = {
        .position = ZMK_VIRTUAL_KEY_POSITION_COMBO(combo_idx),
        .timestamp = timestamp,
#if IS_ENABLED(CONFIG_ZMK_SPLIT)
        .source = ZMK_POSITION_STATE_CHANGE_SOURCE_LOCAL,
#endif
    };

    return zmk_behavior_invoke_binding(&combo->behavior, event, false);
}

static void move_pressed_keys_to_active_combo(struct active_combo *active_combo) {

    int combo_length = MIN(pressed_keys_count, combos[active_combo->combo_idx].key_position_len);
    for (int i = 0; i < combo_length; i++) {
        active_combo->key_positions_pressed[i] = pressed_keys[i];
    }
    active_combo->key_positions_pressed_count = combo_length;

    // move any other pressed keys up
    for (int i = 0; i + combo_length < pressed_keys_count; i++) {
        pressed_keys[i] = pressed_keys[i + combo_length];
    }

    pressed_keys_count -= combo_length;
}

static struct active_combo *store_active_combo(int32_t combo_idx) {
    for (int i = 0; i < CONFIG_ZMK_COMBO_MAX_PRESSED_COMBOS; i++) {
        if (active_combos[i].combo_idx == UINT16_MAX) {
            active_combos[i].combo_idx = combo_idx;
            active_combo_count++;
            return &active_combos[i];
        }
    }
    LOG_ERR("Unable to store combo; already %d active. Increase "
            "CONFIG_ZMK_COMBO_MAX_PRESSED_COMBOS",
            CONFIG_ZMK_COMBO_MAX_PRESSED_COMBOS);
    return NULL;
}

static void activate_combo(int combo_idx) {
    struct active_combo *active_combo = store_active_combo(combo_idx);
    if (active_combo == NULL) {
        // unable to store combo
        release_pressed_keys();
        return;
    }
    move_pressed_keys_to_active_combo(active_combo);
    press_combo_behavior(combo_idx, &combos[combo_idx],
                         active_combo->key_positions_pressed[0].data.timestamp);
}

static void deactivate_combo(int active_combo_index) {
    active_combo_count--;
    if (active_combo_index != active_combo_count) {
        memcpy(&active_combos[active_combo_index], &active_combos[active_combo_count],
               sizeof(struct active_combo));
    }
    active_combos[active_combo_count] = (struct active_combo){0};
    active_combos[active_combo_count].combo_idx = UINT16_MAX;
}

/* returns true if a key was released. */
static bool release_combo_key(int32_t position, int64_t timestamp) {
    for (int combo_idx = 0; combo_idx < active_combo_count; combo_idx++) {
        struct active_combo *active_combo = &active_combos[combo_idx];

        bool key_released = false;
        bool all_keys_pressed = active_combo->key_positions_pressed_count ==
                                combos[active_combo->combo_idx].key_position_len;
        bool all_keys_released = true;
        for (int i = 0; i < active_combo->key_positions_pressed_count; i++) {
            if (key_released) {
                active_combo->key_positions_pressed[i - 1] = active_combo->key_positions_pressed[i];
                all_keys_released = false;
            } else if (active_combo->key_positions_pressed[i].data.position != position) {
                all_keys_released = false;
            } else { // position matches
                key_released = true;
            }
        }

        if (key_released) {
            active_combo->key_positions_pressed_count--;
            const struct combo_cfg *c = &combos[active_combo->combo_idx];
            if ((c->slow_release && all_keys_released) || (!c->slow_release && all_keys_pressed)) {
                release_combo_behavior(active_combo->combo_idx, c, timestamp);
            }
            if (all_keys_released) {
                deactivate_combo(combo_idx);
            }
            return true;
        }
    }
    return false;
}

static int cleanup() {
    k_work_cancel_delayable(&timeout_task);
    memset(candidates, 0, BYTES_FOR_COMBOS_MASK * sizeof(uint32_t));
    if (fully_pressed_combo != INT16_MAX) {
        activate_combo(fully_pressed_combo);
        fully_pressed_combo = INT16_MAX;
    }
    return release_pressed_keys();
}

static void update_timeout_task() {
    int64_t first_timeout = first_candidate_timeout();
    if (timeout_task_timeout_at == first_timeout) {
        return;
    }
    if (first_timeout == LLONG_MAX) {
        timeout_task_timeout_at = 0;
        k_work_cancel_delayable(&timeout_task);
        return;
    }
    if (k_work_schedule(&timeout_task, K_MSEC(first_timeout - k_uptime_get())) >= 0) {
        timeout_task_timeout_at = first_timeout;
    }
}

static int position_state_down(const zmk_event_t *ev, struct zmk_position_state_changed *data) {
    int num_candidates;
    if (!pressed_keys_count) {
        num_candidates = setup_candidates_for_first_keypress(data->position, data->timestamp);
        if (num_candidates == 0) {
            return ZMK_EV_EVENT_BUBBLE;
        }
    } else {
        filter_timed_out_candidates(data->timestamp);
        num_candidates = filter_candidates(data->position);
    }

    LOG_DBG("combo: capturing position event %d", data->position);
    int ret = capture_pressed_key(data);
    update_timeout_task();

    if (num_candidates) {
        for (int i = 0; i < combos_len; i++) {
            if (sys_bitfield_test_bit((mem_addr_t)&candidates, i)) {
                const struct combo_cfg *candidate_combo = &combos[i];
                if (candidate_is_completely_pressed(candidate_combo)) {
                    fully_pressed_combo = i;
                    if (num_candidates == 1) {
                        cleanup();
                    }
                }

                return ret;
            }
        }
    } else {
        cleanup();
        return ret;
    }

    return -EINVAL;
}

static int position_state_up(const zmk_event_t *ev, struct zmk_position_state_changed *data) {
    int released_keys = cleanup();
    if (release_combo_key(data->position, data->timestamp)) {
        return ZMK_EV_EVENT_HANDLED;
    }
    if (released_keys > 1) {
        // The second and further key down events are re-raised. To preserve
        // correct order for e.g. hold-taps, reraise the key up event too.
        struct zmk_position_state_changed_event dupe_ev =
            copy_raised_zmk_position_state_changed(data);
        ZMK_EVENT_RAISE(dupe_ev);
        return ZMK_EV_EVENT_CAPTURED;
    }
    return ZMK_EV_EVENT_BUBBLE;
}

static void combo_timeout_handler(struct k_work *item) {
    if (timeout_task_timeout_at == 0 || k_uptime_get() < timeout_task_timeout_at) {
        // timer was cancelled or rescheduled.
        return;
    }
    if (filter_timed_out_candidates(timeout_task_timeout_at) == 0) {
        LOG_DBG("CLEANUP!");
        cleanup();
    }

    LOG_DBG("ABOUT TO UPDATE IN TIMEOUT");
    update_timeout_task();
}

static int position_state_changed_listener(const zmk_event_t *ev) {
    struct zmk_position_state_changed *data = as_zmk_position_state_changed(ev);
    if (data == NULL) {
        return ZMK_EV_EVENT_BUBBLE;
    }

    if (data->state) { // keydown
        return position_state_down(ev, data);
    } else { // keyup
        return position_state_up(ev, data);
    }
}

static int keycode_state_changed_listener(const zmk_event_t *eh) {
    struct zmk_keycode_state_changed *ev = as_zmk_keycode_state_changed(eh);
    if (ev->state && !is_mod(ev->usage_page, ev->keycode)) {
        store_last_tapped(ev->timestamp);
    }
    return ZMK_EV_EVENT_BUBBLE;
}

static int behavior_combo_listener(const zmk_event_t *eh) {
    if (as_zmk_position_state_changed(eh) != NULL) {
        return position_state_changed_listener(eh);
    } else if (as_zmk_keycode_state_changed(eh) != NULL) {
        return keycode_state_changed_listener(eh);
    }
    return ZMK_EV_EVENT_BUBBLE;
}

static bool combo_keys_held(void) { return pressed_keys_count > 0 || active_combo_count > 0; }

static bool rebuild_pending;

static void rebuild_work_handler(struct k_work *work);

static K_WORK_DEFINE(rebuild_work, rebuild_work_handler);

void dax_combo_engine_request_rebuild(void) { k_work_submit(&rebuild_work); }

// Combo indexes and the lookup table change with a rebuild, so it waits until every combo key is
// up. The retry goes through the work queue rather than running here: release_pressed_keys()
// raises events that re-enter the listener while the outer call is still using the table.
static void submit_pending_rebuild(void) {
    if (rebuild_pending && !combo_keys_held()) {
        k_work_submit(&rebuild_work);
    }
}

static bool load_combo(uint32_t slot, const struct dax_combo_global_settings *global,
                       struct combo_cfg *cfg) {
    struct dax_combo_config combo;
    int ret = dax_combo_read(slot, &combo);
    if (ret == -ENODEV) {
        // A combo being written over RPC is briefly read back before its behavior is.
        LOG_DBG("combo slot %u: behavior not resolved", slot);
        return false;
    }
    if (ret == -ENOENT) {
        return false;
    }
    if (ret < 0) {
        LOG_WRN("combo slot %u: unreadable (%d), skipped", slot, ret);
        return false;
    }
    if (!combo.enabled) {
        return false;
    }
    if (combo.key_position_len == 0 || combo.key_position_len > MAX_COMBO_KEYS) {
        LOG_WRN("combo slot %u: %u key positions, skipped", slot, combo.key_position_len);
        return false;
    }
    for (int i = 0; i < combo.key_position_len; i++) {
        // combo_lookup is indexed by key position.
        if (combo.key_positions[i] < 0 || combo.key_positions[i] >= ZMK_KEYMAP_LEN) {
            LOG_WRN("combo slot %u: key position %d is outside the keymap, skipped", slot,
                    combo.key_positions[i]);
            return false;
        }
    }

    uint16_t require_prior_idle_ms =
        combo.require_prior_idle_ms ? combo.require_prior_idle_ms : global->require_prior_idle_ms;
    *cfg = (struct combo_cfg){
        .key_position_len = combo.key_position_len,
        .timeout_ms = combo.timeout_ms ? combo.timeout_ms : global->timeout_ms,
        // -1 is the zmk,combos default for "no prior idle required".
        .require_prior_idle_ms = require_prior_idle_ms ? require_prior_idle_ms : -1,
        .layer_mask = combo.layer_mask,
        .behavior = combo.behavior,
        .slow_release = combo.slow_release_override == DAX_COMBO_SLOW_RELEASE_INHERIT
                            ? global->slow_release
                            : combo.slow_release_override == DAX_COMBO_SLOW_RELEASE_ON,
    };
    memcpy(cfg->key_positions, combo.key_positions, sizeof(cfg->key_positions));
    return true;
}

static void rebuild_combos(void) {
    struct dax_combo_global_settings global;
    if (dax_combo_read_global_settings(&global) < 0) {
        LOG_ERR("Unable to read the combo settings; keeping the current combos");
        return;
    }

    combos_len = 0;
    for (uint32_t slot = 0; slot < CONFIG_ZMK_DAX_COMBO_MAX_COMBOS; slot++) {
        struct combo_cfg cfg;
        if (!load_combo(slot, &global, &cfg)) {
            continue;
        }
        // Insertion sort, stable, so equal lengths keep slot order.
        size_t i = combos_len++;
        while (i > 0 && combos[i - 1].key_position_len > cfg.key_position_len) {
            combos[i] = combos[i - 1];
            i--;
        }
        combos[i] = cfg;
    }

    memset(combo_lookup, 0, sizeof(combo_lookup));
    for (int i = 0; i < combos_len; i++) {
        initialize_combo(i);
    }
    LOG_INF("Have %d combos", (int)combos_len);
}

static void rebuild_work_handler(struct k_work *work) {
    ARG_UNUSED(work);

    rebuild_pending = combo_keys_held();
    if (!rebuild_pending) {
        rebuild_combos();
    }
}

static int dax_combo_listener(const zmk_event_t *eh) {
    int ret = behavior_combo_listener(eh);
    submit_pending_rebuild();
    return ret;
}

static void dax_combo_timeout_handler(struct k_work *item) {
    combo_timeout_handler(item);
    submit_pending_rebuild();
}

ZMK_LISTENER(dax_combo, dax_combo_listener);
ZMK_SUBSCRIPTION(dax_combo, zmk_position_state_changed);
ZMK_SUBSCRIPTION(dax_combo, zmk_keycode_state_changed);

static int combo_init(void) {
    for (size_t i = 0; i < CONFIG_ZMK_COMBO_MAX_PRESSED_COMBOS; i++) {
        active_combos[i].combo_idx = UINT16_MAX;
    }

    k_work_init_delayable(&timeout_task, dax_combo_timeout_handler);
    return 0;
}

SYS_INIT(combo_init, APPLICATION, CONFIG_KERNEL_INIT_PRIORITY_DEFAULT);
