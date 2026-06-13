#include <pebble.h>

// ─── Data Model ──────────────────────────────────────────────────

#define MAX_ALARMS 5

#define DAY_SUN (1 << 0)
#define DAY_MON (1 << 1)
#define DAY_TUE (1 << 2)
#define DAY_WED (1 << 3)
#define DAY_THU (1 << 4)
#define DAY_FRI (1 << 5)
#define DAY_SAT (1 << 6)

#define DAYS_WEEKDAYS (DAY_MON|DAY_TUE|DAY_WED|DAY_THU|DAY_FRI)
#define DAYS_WEEKEND  (DAY_SAT|DAY_SUN)
#define DAYS_ALL      (0x7F)
#define DAYS_ONCE     (0)

// `configured` field removed — all 5 slots always exist.
// A slot is "inactive" simply when enabled=false.
// This is a struct layout change from the previous version:
// uninstall + reinstall from rePebble to clear stale persisted data.
typedef struct {
    uint8_t  hour;     // 0-23
    uint8_t  minute;   // 0-59
    uint8_t  days;     // bitmask; 0 = DAYS_ONCE (fire once, no repeat)
    bool     enabled;
} Alarm;

#define PERSIST_KEY_ALARM(i)   (i)   // keys 0-4
#define PERSIST_KEY_WAKEUP_ID  (5)   // int32_t

static Alarm    s_alarms[MAX_ALARMS];
static int32_t  s_active_wakeup_id = -1;

static Window    *s_main_window;
static MenuLayer *s_menu_layer;

// ─── Helper: days bitmask → human string ─────────────────────────

static void days_to_string(uint8_t days, char *buf, size_t buf_len) {
    if      (days == DAYS_ONCE)     { snprintf(buf, buf_len, "Once");     return; }
    else if (days == DAYS_ALL)      { snprintf(buf, buf_len, "Daily");    return; }
    else if (days == DAYS_WEEKDAYS) { snprintf(buf, buf_len, "Weekdays"); return; }
    else if (days == DAYS_WEEKEND)  { snprintf(buf, buf_len, "Weekend");  return; }

    // Custom mix — build "Mo We Fr" style string
    const char *abbr[7] = {"Su","Mo","Tu","We","Th","Fr","Sa"};
    int pos = 0;
    buf[0] = '\0';
    for (int i = 0; i < 7; i++) {
        if (!(days & (1 << i))) continue;
        if (pos > 0 && pos < (int)buf_len - 3) { buf[pos++] = ' '; buf[pos] = '\0'; }
        if (pos < (int)buf_len - 2) {
            snprintf(buf + pos, buf_len - pos, "%s", abbr[i]);
            pos += 2;
        }
    }
}

// ─── Persist: load & save ────────────────────────────────────────

static void alarms_load(void) {
    APP_LOG(APP_LOG_LEVEL_INFO, "alarms_load: sizeof(Alarm)=%d", (int)sizeof(Alarm));
    for (int i = 0; i < MAX_ALARMS; i++) {
        if (persist_exists(PERSIST_KEY_ALARM(i))) {
            persist_read_data(PERSIST_KEY_ALARM(i), &s_alarms[i], sizeof(Alarm));
            APP_LOG(APP_LOG_LEVEL_INFO,
                    "alarms_load[%d]: FROM PERSIST %02d:%02d days=0x%02x enabled=%d",
                    i, s_alarms[i].hour, s_alarms[i].minute,
                    s_alarms[i].days, (int)s_alarms[i].enabled);
        } else {
            s_alarms[i] = (Alarm){
                .hour    = 0,
                .minute  = 0,
                .days    = DAYS_ONCE,
                .enabled = false,
            };
            APP_LOG(APP_LOG_LEVEL_INFO,
                    "alarms_load[%d]: DEFAULT (no persist key)", i);
        }
    }
    if (persist_exists(PERSIST_KEY_WAKEUP_ID)) {
        persist_read_data(PERSIST_KEY_WAKEUP_ID, &s_active_wakeup_id, sizeof(int32_t));
    }
}

static void alarm_save(int index) {
    persist_write_data(PERSIST_KEY_ALARM(index), &s_alarms[index], sizeof(Alarm));
}

static void wakeup_id_save(void) {
    persist_write_data(PERSIST_KEY_WAKEUP_ID, &s_active_wakeup_id, sizeof(int32_t));
}

// ─── Scheduling ──────────────────────────────────────────────────

static time_t alarm_next_fire_time(const Alarm *a) {
    time_t     now = time(NULL);
    struct tm *t   = localtime(&now);

    for (int day_offset = 0; day_offset < 7; day_offset++) {
        int wday = (t->tm_wday + day_offset) % 7;

        bool day_matches = (a->days == DAYS_ONCE) || (a->days & (1 << wday));
        if (!day_matches) continue;

        struct tm candidate = *t;
        candidate.tm_mday  += day_offset;
        candidate.tm_hour   = a->hour;
        candidate.tm_min    = a->minute;
        candidate.tm_sec    = 0;
        candidate.tm_isdst  = -1;

        time_t fire_time = mktime(&candidate);

        // 60-second buffer: alarms fire at HH:MM:00, so we need to be past
        // the entire minute to avoid a just-fired alarm rescheduling itself
        // for the same minute on the same day.
        if (fire_time > now + 60) {
            return fire_time;
        }
    }
    return 0;
}

static void schedule_next_alarm(void) {
    if (s_active_wakeup_id >= 0) {
        wakeup_cancel((WakeupId)s_active_wakeup_id);
        s_active_wakeup_id = -1;
    }

    time_t  earliest_time  = 0;
    int     earliest_index = -1;

    for (int i = 0; i < MAX_ALARMS; i++) {
        // No `configured` check — all slots exist; skip if disabled
        if (!s_alarms[i].enabled) {
            APP_LOG(APP_LOG_LEVEL_DEBUG, "schedule: slot %d disabled, skipping", i);
            continue;
        }
        time_t t = alarm_next_fire_time(&s_alarms[i]);
        APP_LOG(APP_LOG_LEVEL_DEBUG,
                "schedule: slot %d next fire = %ld", i, (long)t);
        if (t == 0) continue;
        if (earliest_time == 0 || t < earliest_time) {
            earliest_time  = t;
            earliest_index = i;
        }
    }

    if (earliest_index >= 0) {
        WakeupId wid = wakeup_schedule(
            earliest_time,
            (int32_t)earliest_index,
            false /*notify_if_missed*/
        );
        if (wid >= 0) {
            s_active_wakeup_id = (int32_t)wid;
            APP_LOG(APP_LOG_LEVEL_INFO,
                    "Scheduled alarm %d at %ld (wakeup_id %ld)",
                    earliest_index, (long)earliest_time, (long)wid);
        } else {
            APP_LOG(APP_LOG_LEVEL_ERROR,
                    "wakeup_schedule failed: error %ld", (long)wid);
        }
    } else {
        APP_LOG(APP_LOG_LEVEL_INFO, "No enabled alarms — no wakeup scheduled");
    }

    wakeup_id_save();
}

// ─── Wakeup handler ──────────────────────────────────────────────

static void wakeup_handler(WakeupId wakeup_id, int32_t cookie) {
    int index = (int)cookie;
    if (index < 0 || index >= MAX_ALARMS) return;

    s_active_wakeup_id = -1;

    Alarm *a = &s_alarms[index];
    vibes_long_pulse();

    if (a->days == DAYS_ONCE) {
        // One-shot: disarm after firing
        a->enabled = false;
        alarm_save(index);
    }

    schedule_next_alarm();

    if (s_menu_layer) {
        menu_layer_reload_data(s_menu_layer);
    }
}

// ─── AppMessage ───────────────────────────────────────────────────

// Each uint8 key-value pair costs 8 bytes in the Pebble dictionary wire format.
// 16 pairs (keys 0-15) x 8 bytes = 128 bytes minimum — use 256 for safe headroom.
#define APPMSG_INBOX_SIZE  256
#define APPMSG_OUTBOX_SIZE 256
#define APPMSG_KEY_HANDSHAKE 15
#define APPMSG_HANDSHAKE_VAL 0x42

static void appmsg_send_alarms(void) {
    DictionaryIterator *iter;
    AppMessageResult result = app_message_outbox_begin(&iter);
    if (result != APP_MSG_OK) {
        APP_LOG(APP_LOG_LEVEL_ERROR, "outbox_begin failed: %d", (int)result);
        return;
    }

    for (int slot = 0; slot < MAX_ALARMS; slot++) {
        Alarm *a = &s_alarms[slot];
        uint8_t flags = (a->days & 0x7F) | (a->enabled ? 0x80 : 0);
        dict_write_uint8(iter, slot * 3 + 0, a->hour);
        dict_write_uint8(iter, slot * 3 + 1, a->minute);
        dict_write_uint8(iter, slot * 3 + 2, flags);
    }
    dict_write_uint8(iter, APPMSG_KEY_HANDSHAKE, APPMSG_HANDSHAKE_VAL);

    result = app_message_outbox_send();
    if (result != APP_MSG_OK) {
        APP_LOG(APP_LOG_LEVEL_ERROR, "outbox_send failed: %d", (int)result);
    }
}

static void appmsg_inbox_received(DictionaryIterator *iter, void *context) {
    Tuple *handshake = dict_find(iter, APPMSG_KEY_HANDSHAKE);
    if (handshake && handshake->value->uint8 == APPMSG_HANDSHAKE_VAL) {
        APP_LOG(APP_LOG_LEVEL_INFO, "AppMsg: handshake — sending alarm state to phone");
        appmsg_send_alarms();
        return;
    }

    APP_LOG(APP_LOG_LEVEL_INFO, "AppMsg: receiving new alarm config from phone");
    APP_LOG(APP_LOG_LEVEL_DEBUG, "AppMsg: sizeof(Alarm)=%d", (int)sizeof(Alarm));
    bool any_changed = false;

    for (int slot = 0; slot < MAX_ALARMS; slot++) {
        Tuple *t_hour  = dict_find(iter, slot * 3 + 0);
        Tuple *t_min   = dict_find(iter, slot * 3 + 1);
        Tuple *t_flags = dict_find(iter, slot * 3 + 2);

        if (!t_hour || !t_min || !t_flags) continue;

        uint8_t hour    = t_hour->value->uint8;
        uint8_t min     = t_min->value->uint8;
        uint8_t flags   = t_flags->value->uint8;

        s_alarms[slot].hour    = hour;
        s_alarms[slot].minute  = min;
        s_alarms[slot].days    = flags & 0x7F;
        s_alarms[slot].enabled = (flags & 0x80) != 0;

        alarm_save(slot);
        any_changed = true;

        APP_LOG(APP_LOG_LEVEL_DEBUG,
                "Slot %d: %02d:%02d days=0x%02x enabled=%d",
                slot, hour, min, s_alarms[slot].days, (int)s_alarms[slot].enabled);
    }

    if (any_changed) {
        schedule_next_alarm();
        if (s_menu_layer) {
            menu_layer_reload_data(s_menu_layer);
        }
    }
}

static void appmsg_inbox_dropped(AppMessageResult reason, void *context) {
    APP_LOG(APP_LOG_LEVEL_ERROR, "AppMsg inbox dropped, reason: %d", (int)reason);
}

// ─── MenuLayer Callbacks ──────────────────────────────────────────

static uint16_t cb_num_rows(MenuLayer *layer, uint16_t section, void *ctx) {
    return MAX_ALARMS;
}

static int16_t cb_row_height(MenuLayer *layer, MenuIndex *cell_index, void *ctx) {
    return 52;
}

static void cb_draw_row(GContext *ctx, const Layer *cell_layer,
                         MenuIndex *cell_index, void *data) {
    Alarm *a = &s_alarms[cell_index->row];
    GRect  b = layer_get_bounds(cell_layer);
    bool   hi = menu_cell_layer_is_highlighted(cell_layer);

    // All slots are always shown — no "empty" state.
    // Disabled slots show dimmed time in grey; enabled show black + green dot.
    APP_LOG(APP_LOG_LEVEL_DEBUG,
            "draw_row[%d]: %02d:%02d days=0x%02x enabled=%d",
            (int)cell_index->row, a->hour, a->minute, a->days, (int)a->enabled);

    // ── Armed indicator ───────────────────────────────────────────
    if (a->enabled) {
        graphics_context_set_fill_color(ctx, GColorGreen);
        graphics_fill_circle(ctx, GPoint(b.size.w - 14, b.size.h / 2), 8);
    }

    // ── Text colours ──────────────────────────────────────────────
    GColor main_color = hi ? GColorWhite
                           : (a->enabled ? GColorBlack    : GColorDarkGray);
    GColor sub_color  = hi ? GColorWhite
                           : (a->enabled ? GColorDarkGray : GColorLightGray);

    // ── Time ──────────────────────────────────────────────────────
    char time_buf[8];
    snprintf(time_buf, sizeof(time_buf), "%02d:%02d", a->hour, a->minute);
    graphics_context_set_text_color(ctx, main_color);
    graphics_draw_text(ctx, time_buf,
        fonts_get_system_font(FONT_KEY_BITHAM_34_MEDIUM_NUMBERS),
        GRect(6, 2, b.size.w - 30, 38),
        GTextOverflowModeTrailingEllipsis, GTextAlignmentLeft, NULL);

    // ── Days ──────────────────────────────────────────────────────
    char days_buf[24];
    days_to_string(a->days, days_buf, sizeof(days_buf));
    graphics_context_set_text_color(ctx, sub_color);
    graphics_draw_text(ctx, days_buf,
        fonts_get_system_font(FONT_KEY_GOTHIC_14),
        GRect(6, 38, b.size.w - 30, 16),
        GTextOverflowModeTrailingEllipsis, GTextAlignmentLeft, NULL);
}

// Select toggles enabled on any slot (no configured guard needed anymore)
static void cb_select(MenuLayer *layer, MenuIndex *cell_index, void *data) {
    int    index = cell_index->row;
    Alarm *a     = &s_alarms[index];

    a->enabled = !a->enabled;
    alarm_save(index);
    schedule_next_alarm();
    menu_layer_reload_data(layer);
}

// ─── Window Lifecycle ─────────────────────────────────────────────

static void main_window_load(Window *window) {
    Layer *root   = window_get_root_layer(window);
    GRect  bounds = layer_get_bounds(root);

    s_menu_layer = menu_layer_create(bounds);
    menu_layer_set_callbacks(s_menu_layer, NULL, (MenuLayerCallbacks) {
        .get_num_rows    = cb_num_rows,
        .get_cell_height = cb_row_height,
        .draw_row        = cb_draw_row,
        .select_click    = cb_select,
    });
    menu_layer_set_click_config_onto_window(s_menu_layer, window);
    layer_add_child(root, menu_layer_get_layer(s_menu_layer));
}

static void main_window_unload(Window *window) {
    menu_layer_destroy(s_menu_layer);
    s_menu_layer = NULL;
}

// ─── App Lifecycle ────────────────────────────────────────────────

static void init(void) {
    alarms_load();

    app_message_register_inbox_received(appmsg_inbox_received);
    app_message_register_inbox_dropped(appmsg_inbox_dropped);
    app_message_open(APPMSG_INBOX_SIZE, APPMSG_OUTBOX_SIZE);

    wakeup_service_subscribe(wakeup_handler);

    if (launch_reason() == APP_LAUNCH_WAKEUP) {
        WakeupId fired_id;
        int32_t  cookie;
        if (wakeup_get_launch_event(&fired_id, &cookie)) {
            wakeup_handler(fired_id, cookie);
        }
    } else {
        schedule_next_alarm();
    }

    s_main_window = window_create();
    window_set_window_handlers(s_main_window, (WindowHandlers) {
        .load   = main_window_load,
        .unload = main_window_unload,
    });
    window_stack_push(s_main_window, true);
}

static void deinit(void) {
    window_destroy(s_main_window);
}

int main(void) {
    init();
    app_event_loop();
    deinit();
}
