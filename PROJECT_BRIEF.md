# WakeMeUp — Pebble Time Alarm App — Project Brief

Last updated: 2026-06-15 21:47 CET

---

# Project Goal

Build a reliable alarm application for Pebble Time (2015, Basalt platform) using
the current RePebble ecosystem. It features 5 alarms that can be configured via a
phone-based configuration page, because direct editing on the watch is awkward.
The app occupies exactly one PebbleOS wakeup slot at all times (see Wakeup strategy).

Each alarm can fire once or repeat on selected days of the week. The initial
condition is a single alarm at 07:00, non-repeating and disabled; the user can
modify it or enable up to all 5 slots via the phone config page.

The project is developed incrementally. Each step must be fully working before
moving on to the next.

**Current status (Step 4):** The config page UI and PebbleKit JS bridge are
implemented. The known bug is that tapping Save does not close the config page
and no data reaches the watch. Root cause identified: `config.html` was
navigating to `pebblekit://close#…` (wrong scheme). The correct mechanism,
per the [RePebble static config guide](https://developer.rebble.io/guides/user-interfaces/app-configuration-static/),
is to read the `return_to` query parameter that rePebble injects when it opens
the page, and navigate to `return_to + encodeURIComponent(JSON.stringify(data))`.
The default fallback when `return_to` is absent (e.g. desktop browser testing)
is `pebblejs://close#`. This fix is the immediate next task.

**Potential future enhancements (all deferred):**
- Variable number of alarm slots (current hard limit is 5)
- Adding and deleting alarm slots
- Naming alarms (instead of "Alarm 1", "Alarm 2", etc.)
- On-watch alarm editing (time, repeat days, add/delete slots)

---

## Tech stack

| Component | Choice |
|-----------|--------|
| Watch app | Pebble C SDK, Basalt platform (Pebble Time) |
| Phone bridge | PebbleKit JS — `src/pkjs/index.js` (CloudPebble requires this exact name and location) |
| Config UI | Plain HTML + JS, hosted on GitHub Pages |
| IDE | CloudPebble — [cloudpebble.repebble.com](https://cloudpebble.repebble.com) |
| User's phone | iPhone with rePebble; implementation is phone-OS agnostic (iOS / Android) |
| Config page URL | `https://rbreen.github.io/WakeMeUp-Pebble/docs/config.html` |
| Repository | `https://github.com/rbreen/WakeMeUp-Pebble` |

**CloudPebble file placement note:** The PebbleKit JS entry point must be named
`index.js` and placed in the `src/pkjs/` directory. CloudPebble will not find
it under any other name or location.

---

## App design decisions

- **Max 5 alarms** — hard-coded limit; keeps the UI and persist layout simple
- Each alarm stored in Pebble persistent storage (Persist API), one key per slot
- **Days represented as a bitmask:** bit 0 = Sun, bit 1 = Mon … bit 6 = Sat
- `days = 0` (`DAYS_ONCE`) means fire once then disable the alarm
- Watch ↔ phone communication via AppMessage API over Bluetooth
- **Time display:** 24-hour format on both watch and config page
- **Alarm names:** deferred — would require extra AppMessage keys per slot

---

## Data model

The `Alarm` struct as it exists in `main.c`:

```c
typedef struct {
    uint8_t  hour;     // 0-23
    uint8_t  minute;   // 0-59
    uint8_t  days;     // bitmask; 0 = DAYS_ONCE (fire once, no repeat)
    bool     enabled;  // true = alarm is active and will fire
} Alarm;

#define MAX_ALARMS 5

#define DAY_SUN (1 << 0)
#define DAY_MON (1 << 1)
#define DAY_TUE (1 << 2)
#define DAY_WED (1 << 3)
#define DAY_THU (1 << 4)
#define DAY_FRI (1 << 5)
#define DAY_SAT (1 << 6)

#define DAYS_WEEKDAYS (DAY_MON|DAY_TUE|DAY_WED|DAY_THU|DAY_FRI)  // 0x3E = 62
#define DAYS_WEEKEND  (DAY_SAT|DAY_SUN)                            // 0x41 = 65
#define DAYS_ALL      (0x7F)
#define DAYS_ONCE     (0)
```

**Important notes on the struct:**
- There is no `configured` field — all 5 slots always exist in persist storage.
  A slot is considered "empty/inactive" simply when `enabled = false`.
- The field is named `enabled` throughout (C struct, JS cache, JSON envelope,
  AppMessage wire format). An earlier brief draft used `armed` — that name is
  retired.
- `wakeup_id` is **not** stored per alarm — see Wakeup strategy.

**Persist format warning:** If the `Alarm` struct layout changes (fields
added/removed/reordered), on-flash data will be misread silently. Fix by
uninstalling and reinstalling from rePebble to clear storage, or bump the
persist key range (e.g. use keys 10–15 for the next layout version).

---

## Persist key layout

| Key | Contents |
|-----|----------|
| 0–4 | `Alarm` struct for slot 0–4 (`sizeof(Alarm)` bytes each) |
| 5   | `int32_t s_active_wakeup_id` — the single live WakeupId, or -1 |

---

## Alarm editing — hybrid model

Both the watch and the phone config page can edit alarms, but never simultaneously.

**Source of truth is always the watch** (persistent storage).

**Phone config page on save:** replaces all 5 alarm slots at once. The page
receives the current watch state when it opens (via URL parameter), lets the
user edit, and writes all 5 slots back on save. No per-slot partial updates.

**Watch editing** (planned for Step 5): on-watch number pickers and day toggles.
Architectural constraints to keep this door open:
- The `Alarm` struct and persist format must be fully self-contained on the watch
- AppMessage transfers config but is not the only way to write alarms
- The watch C code must be able to create/modify/delete alarms independently

---

## Wakeup strategy — single slot

The Pebble OS Wakeup API provides **8 slots shared globally across all apps**.
There is no API to query remaining free slots; `wakeup_schedule()` returns
`E_OUT_OF_RESOURCES` if the pool is full.
Reference: [Wakeup API docs](https://developer.rebble.io/docs/c/Events_Service/Wakeup/)

WakeMeUp uses **exactly one wakeup slot** at all times:

1. After any alarm state change, call `schedule_next_alarm()`
2. That function scans all enabled alarms, computes each one's next fire time
   from *now*, picks the earliest, cancels any existing wakeup, and schedules
   exactly one new one
3. The cookie passed to `wakeup_schedule()` is the alarm index (0–4)
4. When the wakeup fires, `wakeup_handler()` handles that alarm, then calls
   `schedule_next_alarm()` again to line up the next one

---

## Missed wakeup policy

`notify_if_missed = false` is passed to `wakeup_schedule()`.

**Rationale:** if the watch battery dies overnight and the user charges it in
the morning, they are already awake — firing a stale alarm would be wrong.
With `notify_if_missed = false`, a missed wakeup is silently skipped. On next
launch, `schedule_next_alarm()` computes all fire times from the current moment
and schedules the next upcoming alarm cleanly.

---

## Scheduling logic — `alarm_next_fire_time()`

Given an alarm's `enabled`, `hour`, `minute`, and `days` bitmask:
- Skip alarms where `enabled = false`
- Walk forward day-by-day (up to 7 days) from *now*
- For each candidate day, check: is this weekday's bit set?
  (`DAYS_ONCE` / `days = 0` matches any day — fire on the next available day)
- Build a `struct tm` for that day at hour:minute:00, run through `mktime()`
- Return the first timestamp that is more than 60 seconds in the future
- The 60-second buffer prevents a just-fired alarm (which fires at HH:MM:00)
  from immediately rescheduling itself for the same minute

---

## Step 4 architecture — three components

| File | Where it runs | Role |
|------|--------------|------|
| `docs/config.html` | Phone browser, opened inside rePebble | UI: time selects, day toggles, presets, save/revert |
| `src/pkjs/index.js` | PebbleKit JS runtime inside rePebble | Bridge: opens config page, ferries data over Bluetooth |
| `src/c/main.c` | Watch | Receives AppMessage, writes to persist, reschedules |

### Config page close mechanism (the fix)

This is the mechanism specified by the
[RePebble static config guide](https://developer.rebble.io/guides/user-interfaces/app-configuration-static/):

When rePebble opens the config page via `Pebble.openURL(url)`, it appends a
`return_to` query parameter to the URL containing the correct close scheme for
the current environment (real watch vs emulator). The config page **must** read
this parameter and use it as the navigation target when saving.

**Correct implementation in `config.html`:**
```javascript
function getQueryParam(variable, defaultValue) {
  var query = location.search.substring(1);
  var vars = query.split('&');
  for (var i = 0; i < vars.length; i++) {
    var pair = vars[i].split('=');
    if (pair[0] === variable) {
      return decodeURIComponent(pair[1]);
    }
  }
  return defaultValue || false;
}

function saveAndClose() {
  var result = { alarms: alarms };
  // Read the return URL injected by rePebble; fall back for desktop testing
  var return_to = getQueryParam('return_to', 'pebblejs://close#');
  document.location = return_to + encodeURIComponent(JSON.stringify(result));
}
```

**Why the previous code failed:** `config.html` navigated to
`pebblekit://close#…` — a hardcoded, incorrect URL scheme. rePebble does not
intercept that scheme. The correct scheme (`pebblejs://close#`) also should not
be hardcoded, because the value varies by environment; it must be read from
`return_to`.

### Full data flow

```
config.html                    src/pkjs/index.js              src/c/main.c
(phone browser in rePebble)    (PebbleKit JS in rePebble)     (watch)

[user taps Save]
  → navigate to:
    return_to +
    encodeURIComponent(
      JSON.stringify({alarms:[…]})
    )
                               [webviewclosed event fires]
                               → e.response = encoded JSON
                               → parse → encodeAlarmsToMessage()
                               → Pebble.sendAppMessage(dict)
                                                              [appmsg_inbox_received]
                                                              → decode keys 0-14
                                                              → write to persist
                                                              → schedule_next_alarm()
                                                              → reload MenuLayer
```

### How alarm state reaches the config page (opening direction)

JS maintains a local `currentAlarms` cache (5 empty slots by default). On
startup (`ready` event), JS sends a handshake to the watch (`{ 15: 0x42 }`),
and the watch replies with its full alarm state (keys 0–14 plus key 15).
JS decodes this into `currentAlarms`.

When the user opens the config page (`showConfiguration` event), JS encodes
`currentAlarms` as a `?alarms=…` URL query parameter. The page reads this on
load and pre-fills all 5 alarm cards. This is a snapshot taken at page-open
time, not a live feed.

**Revert button:** the page stores a deep copy of the opening snapshot as
`originalAlarms`. Tapping Revert restores this copy in memory and re-renders
the cards. It does not re-fetch from the watch.

---

## AppMessage wire format (watch ↔ phone)

AppMessage carries integer key → integer value pairs.
Reference: [AppMessage API docs](https://developer.rebble.io/docs/c/Foundation/AppMessage/)

The full alarm state is encoded as 15 key/value pairs (3 per slot × 5 slots):

| Key            | Value  | Meaning |
|----------------|--------|---------|
| `slot * 3 + 0` | 0–23   | `hour` |
| `slot * 3 + 1` | 0–59   | `minute` |
| `slot * 3 + 2` | uint8  | packed flags (see below) |

Slot 0 → keys 0, 1, 2. Slot 4 → keys 12, 13, 14.

**Packed flags byte (`slot * 3 + 2`):**
- bits 0–6: `days` bitmask (identical to the struct field)
- bit 7: `enabled` flag (1 = alarm is active)

**Key 15 — handshake (magic value `0x42`):**
- Watch → JS: sends `{ 15: 0x42 }` plus keys 0–14 to push full state
- JS → watch: sends `{ 15: 0x42 }` alone to request state

The value `0x42` is an arbitrary magic byte chosen to distinguish a handshake
message from a normal config update.

**PebbleKit JS key delivery note:** PebbleKit JS delivers all dictionary keys
as **strings**, never numbers. Key 15 arrives as `payload["15"]`, not
`payload[15]`. The code uses `String(key)` for all lookups.

**JSON envelope used between `config.html` and `index.js`:**
```json
{
  "alarms": [
    { "hour": 7,  "minute": 0,  "days": 62, "enabled": true  },
    { "hour": 9,  "minute": 30, "days": 65, "enabled": true  },
    { "hour": 0,  "minute": 0,  "days": 0,  "enabled": false },
    { "hour": 0,  "minute": 0,  "days": 0,  "enabled": false },
    { "hour": 0,  "minute": 0,  "days": 0,  "enabled": false }
  ]
}
```
`days` is the raw bitmask integer (62 = `DAYS_WEEKDAYS`, 65 = `DAYS_WEEKEND`).

---

## config.html UI decisions

- **Time input:** two `<select>` dropdowns (hour 00–23, minute 00–59) instead
  of `<input type="time">`. The native iOS time picker has locale-dependent
  control labels (e.g. Dutch "Herstel" = Clear) and jumps to the current time
  when cleared. Selects render identically on iOS, Android, and desktop with no
  surprises.
- **Hour select:** right-aligned; minute select left-aligned. The colon acts as
  the visual centre.
- **Week starts Monday:** display order Mo Tu We Th Fr Sa Su (Sa/Su adjacent).
  Bitmask bit positions are unchanged (bit 0 = Sun … bit 6 = Sat).
- **Day presets:** four write-only shortcut buttons — Every day, Weekdays,
  Weekend, No repeat. Tapping stamps a bitmask; presets have no active/selected
  state of their own. "No repeat" sets `days = 0` (DAYS_ONCE).
- **No "fire once" checkbox:** redundant — `days = 0` already means once.
  The day row label reads "Repeat on (none = once)".
- **All fields always editable:** the enabled toggle arms/disarms only; time
  and day fields are never greyed out.
- **Revert button** sits alongside Save at the bottom of the page.
- **Debug overlay removed** once the close bug is fixed (it served its purpose).

---

## RePebble documentation references

When documentation conflicts exist, prefer current RePebble documentation over
legacy Pebble docs, old forum posts, or historic SDK discussions.

| Topic | URL |
|-------|-----|
| App Configuration (manual setup) | https://developer.rebble.io/guides/user-interfaces/app-configuration-static/ |
| AppMessage API | https://developer.rebble.io/docs/c/Foundation/AppMessage/ |
| Persistent Storage API | https://developer.rebble.io/docs/c/Foundation/Storage/ |
| Wakeup API | https://developer.rebble.io/docs/c/Events_Service/Wakeup/ |
| PebbleKit JS guide | https://developer.rebble.io/guides/communication/using-pebblekit-js/ |

---

## Build progress

| Step | Status | Summary |
|------|--------|---------|
| 1 | ✅ done | Hello World skeleton — app lifecycle, Window, TextLayer |
| 2 | ✅ done | MenuLayer alarm list — callbacks, struct, bitmask days, hardcoded data. Bug fixed: `days_to_string` custom-day loop was unreachable (early `return` removed). |
| 3 | ✅ done | Persistent storage + single-slot Wakeup API scheduling |
| 4 | 🔧 current | Phone config page. Bug fix in progress: `saveAndClose()` must use `return_to` query param, not hardcoded `pebblekit://close#`. |
| 5 | ⏳ next | On-watch alarm editor (number pickers + day toggles) |

---

## Style preferences

- Learning project — always explain new concepts before or alongside code
- Comment code clearly, especially Pebble-specific patterns
- Build incrementally, one working step at a time
- Factual explanations with references to verifiable sources, not just "do this"
- **Project brief timestamp:** every time the project brief is regenerated, the
  `Last updated` line must be set to the actual current date and time in CET
  (Europe/Amsterdam timezone), in the format `YYYY-MM-DD HH:MM CET`
