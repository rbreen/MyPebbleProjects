# WakeMeUp — Pebble Time Alarm App — Project Brief

Last updated: 2026-06-15 22:59 CET

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

**Current status: Step 4 complete.** The full phone→watch configuration pipeline
is working end-to-end: the config page opens pre-filled with the current watch
state, the user edits alarms, tapping Save closes the page and passes data back
to PebbleKit JS, which sends it to the watch over Bluetooth.

**Next: Step 5** — on-watch alarm editor and onboarding screen (see Build progress).

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
| 6   | `uint8_t s_vibe_pattern` — global vibration pattern index (0 = short, 1 = double, 2 = long, 3 = persistent); default 1 |

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

**Confirmed working implementation in `config.html` (v17):**
```javascript
function getQueryParam(name, defaultValue) {
  var query = location.search.substring(1);
  var vars = query.split('&');
  for (var i = 0; i < vars.length; i++) {
    var pair = vars[i].split('=');
    if (decodeURIComponent(pair[0]) === name) {
      return decodeURIComponent(pair[1]);
    }
  }
  return defaultValue;
}

function saveAndClose() {
  var result = { alarms: alarms };
  // Read the return URL injected by rePebble; fall back for desktop testing.
  // Use document.location.href (not document.location) — more reliable in
  // rePebble's embedded WebView on iOS.
  var return_to = getQueryParam('return_to', 'pebblejs://close#');
  document.location.href = return_to + encodeURIComponent(JSON.stringify(result));
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
- **Debug overlay removed** in v17 — it served its purpose diagnosing the close bug.

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

## Step 5 design — on-watch interaction model

### Main alarm list — layout

Row 0 is a **header row**, not an alarm slot. It displays the app title and a
one-line reminder that configuration is done via the phone, e.g.:
*"Configure alarms via the Pebble app"*. It scrolls out of view naturally as
the user scrolls down through the alarm slots. Long-pressing Select on this row
opens a help/about screen rather than an edit screen.

Rows 1–5 correspond to alarm slots 0–4.

### Main alarm list — button assignment

| Button | Press type | Action |
|--------|-----------|--------|
| Up | Short | Scroll selection up |
| Down | Short | Scroll selection down |
| Select | Short | Toggle enabled/disabled on selected alarm (immediate: saves to persist + reschedules) |
| Select | Long | Open edit screen for selected alarm (or help screen if header row selected) |
| Back | Short | Exit app (reserved by OS) |
| Back | Long | Power menu (reserved by OS) |

**Short Select on an alarm row** is an atomic action — it writes to persist and
calls `schedule_next_alarm()` immediately, since there is no edit session
involved. This lets the user quickly arm/disarm an alarm with one press without
entering the editor.

**Short Select on the header row** does nothing (or could open help — TBD).

**MenuLayer + custom long-press note:** MenuLayer installs its own click config
via `menu_layer_set_click_config_onto_window()`. The Select long-press handler
must be registered after that call so it supplements rather than replaces
MenuLayer's built-in navigation. The exact pattern will be documented when
Step 5 is implemented.

### Edit screen — fields and layout

Pushing the edit screen replaces the window stack with a simple scrollable list
of fields for the selected alarm, in this order:

1. **Enabled** — checkbox, toggled directly by Select press
2. **Hour** — number field (0–23)
3. **Minute** — number field (0–59)
4. **Repeat days** — seven checkboxes: Mo Tu We Th Fr Sa Su
   (none selected = `DAYS_ONCE`, fire once)

If all fields don't fit on screen, the list scrolls naturally with Up/Down.

### Edit screen — button behaviour

Two modes exist within the edit screen: **navigate mode** and **edit mode**.

**Navigate mode (default):**

| Button | Action |
|--------|--------|
| Up | Move highlight to previous field |
| Down | Move highlight to next field |
| Select | On a checkbox field: toggle immediately. On a number field: enter edit mode. |
| Back | Accept all changes, save to persist, call `schedule_next_alarm()`, return to alarm list |

**Edit mode (active on a number field — hour or minute):**

| Button | Action |
|--------|--------|
| Up | Increment value (wraps: 23→0 for hour, 59→0 for minute) |
| Down | Decrement value (wraps: 0→23 for hour, 0→59 for minute) |
| Select | Accept value, exit edit mode, return to navigate mode |
| Back | Accept value, exit edit mode, save to persist, call `schedule_next_alarm()`, return to alarm list |

The visual distinction between navigate mode and edit mode (e.g. highlight colour
or an indicator) will be decided during implementation.

### Save / persist strategy

- **In-memory (`s_alarms[]`):** updated immediately as the user changes each
  field in the editor. The alarm list reflects changes the moment the user
  returns to it.
- **Persist write + `schedule_next_alarm()`:** called once when the user presses
  Back from the edit screen to the alarm list. This avoids hammering flash
  storage on every Up/Down press (Pebble's persist API has finite write
  endurance) and avoids rescheduling the wakeup mid-edit.
- **Rationale for persist-on-return (not persist-on-app-exit):** the user may
  want to set an alarm 1 minute from now for testing and remain in the app to
  watch it fire. Persisting and rescheduling on return to the list means the
  wakeup is live immediately after leaving the editor.

### Header row — help screen and global settings

Long-pressing Select on the header row opens a settings/help window. This
serves two purposes:

1. **Help text** — brief instructions: how to configure via the phone, the
   config page URL, and a summary of watch controls.
2. **Global settings** — app-wide preferences that apply to all alarms.
   Currently the only candidate is the vibration pattern (see below).

Short-pressing Select on the header row does nothing for now, reserving the
possibility of an easter egg or future use.

### Vibration pattern — global setting

**What the Pebble Vibes API provides:**
The vibration motor is on/off only — there is no intensity/strength control.
What can be configured is the *pattern*: the sequence and duration of on/off
pulses. Reference: [Vibes API](https://developer.rebble.io/docs/c/User_Interface/Vibes/)

The API offers four built-in calls:
- `vibes_short_pulse()` — one brief pulse
- `vibes_long_pulse()` — one long pulse
- `vibes_double_pulse()` — two brief pulses
- `vibes_enqueue_custom_pattern(VibePattern)` — arbitrary on/off sequence,
  each segment up to 10,000ms, e.g. `{ 200, 100, 400 }` = on 200ms, off 100ms, on 400ms

There is no OS-level vibration setting — it is entirely per-app. The Pebble
SDK guidelines recommend custom patterns specifically for user-configurable
haptic feedback within an app.

**Design decision: one global vibration pattern for all alarms** (not per-alarm).
Per-alarm vibration is deferred; it would require an extra field in the `Alarm`
struct and additional AppMessage keys.

**Proposed selectable patterns** (exact list TBD during Step 5 implementation):

| Name | Pattern | Use case |
|------|---------|----------|
| Short | single short pulse | Light sleeper / quiet wake |
| Double | two short pulses | Default |
| Long | single long pulse | Heavier sleeper |
| Persistent | repeating pattern, e.g. `{ 500, 200, 500, 200, 500 }` | Heavy sleeper |

**Storage:** the selected pattern index is stored in a new persist key (key 6).
It is a global setting, not part of the `Alarm` struct.

**Current code** in `wakeup_handler()` calls `vibes_long_pulse()` unconditionally.
This will be replaced with a pattern lookup from the global setting in Step 5.

---

## Future: user documentation

Once the app is stable, user-facing documentation will be generated from this
project brief. Planned deliverables:

- **In-app About/Help screen** — as described above
- **App store description** — for the Rebble app store listing
- **README / user guide** — GitHub Pages or repository README, covering
  installation, first-time setup, phone configuration, and on-watch controls

These will be authored from the project brief so documentation stays consistent
with the implementation. No work planned until Step 5 is complete.

---



| Step | Status | Summary |
|------|--------|---------|
| 1 | ✅ done | Hello World skeleton — app lifecycle, Window, TextLayer |
| 2 | ✅ done | MenuLayer alarm list — callbacks, struct, bitmask days, hardcoded data. Bug fixed: `days_to_string` custom-day loop was unreachable (early `return` removed). |
| 3 | ✅ done | Persistent storage + single-slot Wakeup API scheduling |
| 4 | ✅ done | Phone config page working end-to-end. Root cause of close bug: `pebblekit://close#` is the wrong scheme. Fix: read `return_to` param injected by rePebble, navigate via `document.location.href`. Also fixed: `CONFIG_PAGE_URL` in `index.js` updated to renamed repo. |
| 5 | 🔧 current | On-watch alarm editor (Select long-press → edit window, number pickers for hour/minute, day toggles) + onboarding/instructions screen |

---

## Style preferences

- Learning project — always explain new concepts before or alongside code
- Comment code clearly, especially Pebble-specific patterns
- Build incrementally, one working step at a time
- Factual explanations with references to verifiable sources, not just "do this"
- **Project brief timestamp:** every time the project brief is regenerated, the
  `Last updated` line must be set to the actual current date and time in CET
  (Europe/Amsterdam timezone), in the format `YYYY-MM-DD HH:MM CET`
