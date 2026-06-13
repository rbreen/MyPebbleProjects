#include <pebble.h>

// ─── Data Model ──────────────────────────────────────────────────

#define MAX_ALARMS 5

// Days of week as a bitmask — one bit per day
#define DAY_SUN (1 << 0)   // = 1
#define DAY_MON (1 << 1)   // = 2
#define DAY_TUE (1 << 2)   // = 4
#define DAY_WED (1 << 3)   // = 8
#define DAY_THU (1 << 4)   // = 16
#define DAY_FRI (1 << 5)   // = 32
#define DAY_SAT (1 << 6)   // = 64

#define DAYS_WEEKDAYS (DAY_MON|DAY_TUE|DAY_WED|DAY_THU|DAY_FRI)
#define DAYS_WEEKEND  (DAY_SAT|DAY_SUN)
#define DAYS_ALL      (0x7F)   // all 7 bits set
#define DAYS_ONCE     (0)      // 0 = fire once then disable

// wakeup_id removed from Alarm — we now use a single global wakeup.
// NOTE: if you have persisted data from the previous struct layout (which
// included wakeup_id), clear storage once by incrementing PERSIST_KEY_ALARM
// range or doing a full uninstall/reinstall from the rePebble app.
typedef struct {
    uint8_t  hour;
    uint8_t  minute;
    uint8_t  days;
    bool     enabled;
    bool     configured;   // distinguishes "real but disarmed" from "empty slot"
} Alarm;

// ── Persist keys ─────────────────────────────────────────────────
// Keys 0–4: one Alarm struct per slot.
// Key 5:    the single active WakeupId (int32_t), so we can cancel it on
//           reschedule even after a reboot.  If not set, no wakeup is live.
#define PERSIST_KEY_ALARM(i)   (i)      // 0–4
#define PERSIST_KEY_WAKEUP_ID  (5)      // stores int32_t

static Alarm    s_alarms[MAX_ALARMS];
static int32_t  s_active_wakeup_id = -1;  // -1 = none scheduled

static Window    *s_main_window;
static MenuLayer *s_menu_layer;

// ─── Helper: days bitmask → human string ─────────────────────────
static void days_to_string(uint8_t days, char *buf, size_t buf_len) {
    if      (days == DAYS_ONCE)     { snprintf(buf, buf_len, "Once");     return; }
    else if (days == DAYS_ALL)      { snprintf(buf, buf_len, "Daily");    return; }
    else if (days == DAYS_WEEKDAYS) { snprintf(buf, buf_len, "Weekdays"); return; }
    else if (days == DAYS_WEEKEND)  { snprintf(buf, buf_len, "Weekend");  return; }

    // Custom mix — build "Su Mo Fr" style string
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
    for (int i = 0; i < MAX_ALARMS; i++) {
        if (persist_exists(PERSIST_KEY_ALARM(i))) {
            persist_read_data(PERSIST_KEY_ALARM(i), &s_alarms[i], sizeof(Alarm));
        } else {
            s_alarms[i] = (Alarm){
                .hour       = 0,
                .minute     = 0,
                .days       = DAYS_ONCE,
                .enabled    = false,
                .configured = false,
            };
        }
    }
    // Load the persisted wakeup id so we can cancel it after a reboot
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
//
// SINGLE-WAKEUP STRATEGY
// ──────────────────────
// The Pebble OS has a global pool of 8 wakeup slots shared by ALL apps.
// There is no API to query how many slots remain — you only find out a slot
// is unavailable when wakeup_schedule() returns E_OUT_OF_RESOURCES.
//
// To be a good citizen we consume exactly ONE slot at all times:
//   1. Scan all alarms, compute the next fire time for each enabled one.
//   2. Pick the alarm that fires soonest.
//   3. Cancel the currently-scheduled wakeup (if any) and schedule that one.
//   4. Store the WakeupId in flash so we can cancel it after a reboot.
//
// When the wakeup fires:
//   - Handle that alarm (vibrate, disarm-if-once, etc.)
//   - Call schedule_next_alarm() again to schedule the one after it.
//
// MISSED WAKEUPS
// ──────────────
// We pass notify_if_missed=false.  This means: if the watch was off when the
// wakeup was due, the OS discards it silently.  On next launch (user opens
// the app, or charger wakes it), schedule_next_alarm() runs, computes all
// fire times from *now*, and schedules the next future alarm.  Alarms whose
// time has already passed are naturally skipped — no stale alert fires hours
// after you've woken up.

// Returns the next UTC timestamp at which alarm `a` should fire,
// looking forward from the current moment.  Returns 0 if no future
// time can be found (should not happen for a valid enabled alarm).
static time_t alarm_next_fire_time(const Alarm *a) {
    time_t     now = time(NULL);
    struct tm *t   = localtime(&now);

    // tm_wday: 0=Sun … 6=Sat, matching our bitmask bit positions.
    for (int day_offset = 0; day_offset < 7; day_offset++) {
        int wday = (t->tm_wday + day_offset) % 7;

        // DAYS_ONCE (days==0) matches any day — fire on the next slot.
        bool day_matches = (a->days == DAYS_ONCE) || (a->days & (1 << wday));
        if (!day_matches) continue;

        struct tm candidate = *t;
        candidate.tm_mday  += day_offset;  // mktime normalises month/year overflow
        candidate.tm_hour   = a->hour;
        candidate.tm_min    = a->minute;
        candidate.tm_sec    = 0;
        candidate.tm_isdst  = -1;          // let libc resolve DST

        time_t fire_time = mktime(&candidate);

        // 1-second buffer so a just-triggered alarm doesn't immediately re-fire
        if (fire_time > now + 1) {
            return fire_time;
        }
    }
    return 0;  // no future occurrence found
}

// Cancel whatever wakeup is currently live (if any), scan all alarms,
// and schedule exactly one wakeup for the soonest upcoming alarm.
// Call this whenever alarm state changes, and after handling a fired alarm.
static void schedule_next_alarm(void) {
    // Step 1 — cancel the current wakeup
    if (s_active_wakeup_id >= 0) {
        wakeup_cancel((WakeupId)s_active_wakeup_id);
        s_active_wakeup_id = -1;
    }

    // Step 2 — find the alarm with the earliest next fire time
    time_t  earliest_time  = 0;
    int     earliest_index = -1;

    for (int i = 0; i < MAX_ALARMS; i++) {
        if (!s_alarms[i].configured || !s_alarms[i].enabled) continue;
        time_t t = alarm_next_fire_time(&s_alarms[i]);
        if (t == 0) continue;  // shouldn't happen, but skip if it does
        if (earliest_time == 0 || t < earliest_time) {
            earliest_time  = t;
            earliest_index = i;
        }
    }

    // Step 3 — schedule it (or leave s_active_wakeup_id = -1 if none)
    if (earliest_index >= 0) {
        // Cookie = alarm index, so wakeup_handler knows which alarm fired.
        // notify_if_missed=false: missed wakeups are discarded; on next
        // launch schedule_next_alarm() recomputes from 'now' automatically.
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
            // E_OUT_OF_RESOURCES means the system-wide pool is full.
            // Other apps are using all 8 slots.  Nothing we can do except log.
            APP_LOG(APP_LOG_LEVEL_ERROR,
                    "wakeup_schedule failed: error %ld", (long)wid);
        }
    }

    // Step 4 — persist the new wakeup id (or -1) so we can cancel after reboot
    wakeup_id_save();
}

// ─── Wakeup handler ──────────────────────────────────────────────
//
// Called when:
//   a) the app is already open and a wakeup fires, or
//   b) the app was launched by a wakeup (we invoke this manually from init).
//
// `cookie` holds the alarm index we passed to wakeup_schedule().

static void wakeup_handler(WakeupId wakeup_id, int32_t cookie) {
    int index = (int)cookie;
    if (index < 0 || index >= MAX_ALARMS) return;

    // The wakeup has fired and is now consumed — clear our record of it
    s_active_wakeup_id = -1;

    Alarm *a = &s_alarms[index];

    // Vibrate to alert the user.
    // vibes_long_pulse() = single ~1-second buzz.
    // Could use vibes_double_pulse() or a custom VibePattern for something
    // more insistent; keeping it simple for now.
    vibes_long_pulse();

    if (a->days == DAYS_ONCE) {
        // One-shot: disarm but keep configured so user can see + re-enable it
        a->enabled = false;
        alarm_save(index);
    }
    // Repeating alarms stay enabled; schedule_next_alarm() below will find them.

    // Schedule the next upcoming alarm across the whole list
    schedule_next_alarm();

    // Refresh the list if the app is open (s_menu_layer is NULL if not)
    if (s_menu_layer) {
        menu_layer_reload_data(s_menu_layer);
    }
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

    // ── Empty slot ────────────────────────────────────────────────
    if (!a->configured) {
        graphics_context_set_text_color(ctx, hi ? GColorWhite : GColorDarkGray);
        graphics_draw_text(ctx, "— empty —",
            fonts_get_system_font(FONT_KEY_GOTHIC_18),
            GRect(0, 12, b.size.w, 22),
            GTextOverflowModeTrailingEllipsis, GTextAlignmentCenter, NULL);
        return;
    }

    // ── Armed indicator — green dot, right edge ───────────────────
    if (a->enabled) {
        graphics_context_set_fill_color(ctx, GColorGreen);
        graphics_fill_circle(ctx,
            GPoint(b.size.w - 14, b.size.h / 2),
            8);
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

// Toggle enabled state; save and reschedule the global wakeup.
static void cb_select(MenuLayer *layer, MenuIndex *cell_index, void *data) {
    int    index = cell_index->row;
    Alarm *a     = &s_alarms[index];
    if (!a->configured) return;

    a->enabled = !a->enabled;
    alarm_save(index);
    schedule_next_alarm();         // recompute which alarm is next across all slots
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
    s_menu_layer = NULL;  // guard: wakeup_handler checks this before reloading
}

// ─── App Lifecycle ────────────────────────────────────────────────

static void init(void) {
    // Load alarms and persisted wakeup id from flash
    alarms_load();

    // Register handler for wakeups that fire while the app is already open
    wakeup_service_subscribe(wakeup_handler);

    // If the OS launched us because a wakeup fired, handle it now.
    // (The service callback above won't fire for a launch-by-wakeup event —
    //  the OS delivers it once via wakeup_get_launch_event instead.)
    //
    // Correct SDK 3 signature (confirmed by IDE / compiler):
    //   bool wakeup_get_launch_event(WakeupId *id, int32_t *cookie)
    //     — returns true and fills both out-params if launched by wakeup
    //
    // NOTE: wakeup_get_launch_wakeup_id() does not exist (wrong name used
    // in first attempt); wakeup_get_launch_cookie() also does not exist
    // (the cookie comes through the second parameter of this call instead).
    if (launch_reason() == APP_LAUNCH_WAKEUP) {
        WakeupId fired_id;
        int32_t  cookie;
        if (wakeup_get_launch_event(&fired_id, &cookie)) {
            wakeup_handler(fired_id, cookie);
        }
    } else {
        // Normal launch (user opened the app).  Reschedule defensively:
        // this handles the case where the watch was off when an alarm was due —
        // the missed wakeup was discarded, so we recompute from 'now'.
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