# WakeMeUp — Pebble Time Alarm App — Project Brief

Last updated: 2026-06-14 11:18 CET

---

# Project Goal

Build a reliable alarm application for Pebble Time using the current RePebble ecosystem. It features multiple alarms that can be configured using a configuration page on the users phone because direct editing on the watch is awkward. The app will only occupy a single PebbleOS wakeup_schedule slot, see wakeup strategy, it is currently setup for 5 alarms that can fire once or repeat on certain days in the week, see data model, but a future idea is to make that flexible, the inital condition is a single alarm at 07:00 non-repeating and unarmed but you can modify that singele alarm or add additional alarms (up to 10) but that should also allow deleteing alarms.  

The project is developed incrementally. Each step should be fully working before moving on to the next. Currently we are stuck at communicating the 5 alarms back to the watch, the observed behavior is that the configuration page is not closed in the RePebble app and nothing is communicated, this is probably due to the used closing and encoding of the data that is not exactly according to RePebble documentation and examples.

Current focus:

- Reliable alarm scheduling
- Persistent alarm storage
- Phone-based configuration
- Support for on-watch enabling/disabling of configured alarms

Potential future enhancements:

- Variable number of alarms
- Allowing to add alarm slots
- But also Alarm deletion
- Naming the alarms, instead of Alarm 1, Alarm 2, etc.
- Support the same editing (change the time, repeating days, adding and removing alarm slots) on the watch itself.

These items are intentionally deferred until configuration synchronization is fully operational.

---

## Tech stack
- **Watch app:** Pebble C SDK (C language, Basalt platform)
- **Phone config UI:** PebbleKit JS + HTML config page hosted on GitHub Pages
- **IDE:** CloudPebble — browser-based, no local install
- **User's phone:** iPhone with rePebble app installed but the implementation should be phone OS agnostic (IOS / Android / ....?)
- **Config page URL:** `https://rbreen.github.io/MyPebbleProjects/config.html`
- **Project repository:** 'https://github.com/rbreen/MyPebbleProjects/tree/master'

## App design decisions
- **Max 5 alarms** — user-visible limit; keeps the list manageable
- Each alarm stored in Pebble persistent storage (Persist API), one key per slot
- **Days represented as a bitmask:** bit0=Sun, bit1=Mon … bit6=Sat
- `days=0` (`DAYS_ONCE`) means fire once then disarm (keep configured, set `enabled=false`)
- Communication watch↔phone via AppMessage API over Bluetooth
- **Time display:** 24-hour format on both watch and phone config page
- **Alarm names:** not implemented — storage has room (~16 chars × 5 = 80 bytes) but
  AppMessage wire format would need extra keys per slot; deferred to a later step

## Alarm editing — hybrid model

Both the watch and the phone config page can create, edit, and delete alarms.
They are never used simultaneously (user switches between one mode and the other).

**Source of truth is always the watch** (persistent storage).

**Phone config page behaviour on save:** replaces all 5 alarm slots at once.
It reads the current state from the watch when it opens, lets the user edit,
and writes all 5 slots back on save. No per-slot partial update; the full
configuration is replaced.

**Watch editing** (planned for Step 5): on-watch number pickers and day
toggles. Architectural decisions must not close this door — in particular:
- The `Alarm` struct and persist format must be fully self-contained on the watch
- AppMessage is used to transfer config, not as the only way to write alarms
- The watch C code must be able to create/modify/delete alarms independently

## Data model

```c
typedef struct {
    uint8_t  hour;       // 0-23
    uint8_t  minute;     // 0-59
    uint8_t  days;       // bitmask; 0 = once
    bool     armed;
    bool     configured; // false = empty slot (not just disarmed)
} Alarm;

#define MAX_ALARMS    5
#define DAY_SUN (1<<0), DAY_MON (1<<1), DAY_TUE (1<<2)
#define DAY_WED (1<<3), DAY_THU (1<<4), DAY_FRI (1<<5), DAY_SAT (1<<6)
#define DAYS_WEEKDAYS (0x3E), DAYS_WEEKEND (0x41), DAYS_ALL (0x7F), DAYS_ONCE (0)
```

Note: `wakeup_id` is **not** stored per alarm — see Wakeup strategy below.

## Persist key layout

| Key | Contents |
|-----|----------|
| 0-4 | `Alarm` struct for slot 0-4 (`sizeof(Alarm)` bytes each) |
| 5   | `int32_t s_active_wakeup_id` — the single live WakeupId, or -1 |

## Wakeup strategy — single slot

The Pebble OS Wakeup API provides **8 slots shared globally across all apps**.
There is no API to query remaining slots; `wakeup_schedule()` returns
`E_OUT_OF_RESOURCES` if the pool is full.

To be a good citizen, WakeMeUp uses **exactly one wakeup slot** at all times:

1. After any alarm state change, call `schedule_next_alarm()`
2. That function scans all enabled+configured alarms, computes each one's next
   fire time from *now*, picks the earliest, cancels any existing wakeup, and
   schedules exactly one new one
3. The cookie passed to `wakeup_schedule()` is the alarm index (0-4)
4. When the wakeup fires, `wakeup_handler()` handles that alarm, then calls
   `schedule_next_alarm()` again to line up the one after it

## Missed wakeup policy

`notify_if_missed=true` is passed to `wakeup_schedule()`.

**Rationale:** if the watch battery dies overnight and the user charges it in
the morning, they are already awake — firing a stale alarm is wrong. However that
trigger should run the `schedule_next_alarm()`, which computes
all fire times from the current moment, and schedules the next upcoming alarm
cleanly.

## Scheduling logic — `alarm_next_fire_time()`

Given an alarm's 'enabled', `hour`, `minute`, and `days` bitmask:
- Only consider alarms that are enabled or 'armed'
- Walk forward day-by-day (up to 7 days) from *now*
- For each candidate day check: is this weekday's bit set? (DAYS_ONCE matches any day)
- Build a `struct tm` for that day at hour:minute:00, run through `mktime()`
- Return the first timestamp that is more than 1 second in the future
- The 1-second buffer prevents a just-fired alarm from immediately rescheduling itself

## Step 4 architecture — three components

| File | Where it runs | Role |
|------|--------------|------|
| `docs/config.html` | iPhone browser (inside rePebble) | UI: time selects, day toggles, presets, save/revert |
| `src/pkjs/index.js` | PebbleKit JS (inside rePebble app) | Bridge: opens config page, ferries data over Bluetooth |
| `src/c/main.c` additions | Watch | Receives AppMessage, writes alarms to persist, reschedules |

### Data flow

```
config.html  -->  closes with result URL  -->  pebble-js-app.js  -->  AppMessage  -->  main.c
  (phone UI)            (phone)                 (phone bridge)       (Bluetooth)       (watch)
```

The config page communicates its result by closing itself with a special URL:
`pebblekit://close#<JSON>`. PebbleKit JS intercepts that URL, parses
the JSON, and sends the data to the watch as AppMessage key-value pairs.
The watch never talks to the internet directly.
However this is the part we are currently struggling with.

# RePebble Documentation Policy

When documentation conflicts exist, prefer current RePebble documentation over:

- Legacy Pebble documentation
- Old forum posts
- Historic SDK discussions

Primary references:

- https://developer.repebble.com/guides/
- https://developer.repebble.com/examples/

Relevant topics:

- App Configuration
- AppMessage
- Clay
- Persistent Storage
- Wakeup API

### How current alarm state reaches the config page

JS maintains a local `currentAlarms` cache (5 empty slots by default). The
watch sends its full alarm state to JS whenever it has something to report
(via key 15 = 0x42 handshake). When the user opens the config page, JS
encodes `currentAlarms` as a URL query parameter and passes it to the page.
The page reads it on load and pre-fills the form. This is a snapshot — it
reflects the watch state at the moment the page opens, not a live feed.

**Revert button:** the page keeps a deep copy of the opening snapshot
(`originalAlarms`). Tapping Revert restores this copy, discarding any edits.
This is a local restore, not a re-fetch from the watch — see note below.

### config.html UI decisions

- **Time input:** two `<select>` dropdowns (hour 00-23, minute 00-59) instead
  of `<input type="time">`. The native time picker on iOS has locale-dependent
  buttons (e.g. Dutch "Herstel" = Clear) and jumps to current time when
  cleared. Selects render identically on iOS, Android, and desktop.
- **Hour select:** right-aligned; minute select left-aligned. The colon
  separator acts as the visual centre of the time display.
- **Week starts Monday:** display order Mo Tu We Th Fr Sa Su — Sa/Su adjacent.
  Bit positions in the bitmask are unchanged (bit0=Sun … bit6=Sat).
- **Day presets:** four write-only shortcut buttons below the day toggles —
  Every day, Weekdays, Weekend, No repeat. Tapping one stamps a bitmask onto
  the day buttons; no "active" state on the presets themselves. No repeat sets
  `days=0` (DAYS_ONCE).
- **"Fire once" checkbox removed:** redundant — `days=0` already means once.
  The day row label reads "Repeat on (none = once)" to communicate this.
- **All fields always editable:** the enabled toggle arms/disarms only; time
  and day fields are never greyed out.
- **Revert button** sits alongside Save at the bottom of the page.

## AppMessage wire format (watch <-> phone)

AppMessage passes integer key to integer value pairs. The full alarm state is
encoded as 15 keys (3 per slot x 5 slots):

| Key            | Value  | Meaning |
|----------------|--------|---------|
| `slot * 3 + 0` | 0-23   | `hour` |
| `slot * 3 + 1` | 0-59   | `minute` |
| `slot * 3 + 2` | uint8  | packed flags — see below |

where `slot` is 0-4, giving keys 0-14.

**Packed flags byte (`slot * 3 + 2`):**
- bits 0-6: days bitmask (`DAY_SUN` ... `DAY_SAT`), identical to the struct field
- bit 7: `enabled` flag (1 = armed)

**`configured` is not transmitted explicitly.** The rule: if a slot arrives
with hour=0, minute=0, flags=0 (days=0, enabled=false), the watch sets
`configured=false` (empty slot). Any other combination sets `configured=true`.
This lets the phone clear a slot by zeroing it out.

**Key 15 = handshake:** watch sends `{ 15: 0x42 }` plus keys 0-14 to push its
current state to JS. JS sends `{ 15: 0x42 }` alone to request the watch state.

**JSON envelope used by `config.html` and `pebble-js-app.js`:**

```json
{
  "alarms": [
    { "hour": 7, "minute": 0,  "days": 62, "enabled": true  },
    { "hour": 9, "minute": 30, "days": 65, "enabled": true  },
    { "hour": 0, "minute": 0,  "days": 0,  "enabled": false },
    { "hour": 0, "minute": 0,  "days": 0,  "enabled": false },
    { "hour": 0, "minute": 0,  "days": 0,  "enabled": false }
  ]
}
```

`days` is the raw bitmask integer (62 = DAYS_WEEKDAYS = Mo-Fr, 65 = DAYS_WEEKEND = Sa+Su).

## Persist format note

If you change the `Alarm` struct layout (add/remove/reorder fields), the
on-flash data will be misread. Options:
- Uninstall + reinstall from rePebble (clears storage)
- Or bump the persist key range (e.g. use keys 10-15 for the next layout version)


## Build progress

| Step | Status | Summary |
|------|--------|---------|
| 1 | done | Hello World skeleton — app lifecycle, Window, TextLayer |
| 2 | done | MenuLayer alarm list — callbacks, struct, bitmask days, hardcoded data. Bug fixed: `days_to_string` custom-day loop was unreachable (early `return` removed). |
| 3 | done | Persistent storage + single-slot Wakeup API scheduling |
| 4 | current | Phone config page: `config.html` + `pebble-js-app.js` + AppMessage handler in `main.c` |
| 5 | next | On-watch alarm editor (number pickers + day toggles) |

## Style preferences
- Learning project — always explain new concepts before or alongside code
- Comment code clearly, especially Pebble-specific patterns
- Build incrementally, one working step at a time
- Factual explanations with reasoning, not just "do this"
