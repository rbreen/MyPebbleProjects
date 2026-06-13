// pebble-js-app.js — runs inside rePebble on the phone.
//
// Responsibilities:
//   1. When the user taps the gear icon, read current alarms from the watch,
//      then open config.html with that data pre-loaded as a URL parameter.
//   2. When config.html closes (user tapped Save), receive the JSON result
//      and forward all 5 alarm slots to the watch via AppMessage.
//
// AppMessage key scheme (matches main.c and config.html):
//   slot*3+0  → hour   (0-23)
//   slot*3+1  → minute (0-59)
//   slot*3+2  → packed flags: bits 0-6 = days bitmask, bit 7 = enabled
//   key 15    → handshake: watch sends 0x42 to say "I'm ready, here is my state"
//
// The config page URL is a data: URI so no external hosting is needed.
// In production you'd host config.html on GitHub Pages and use that URL instead.

// ── Alarm state cache ────────────────────────────────────────────
// We hold a local copy of the alarm state so we can pass it to the
// config page immediately when the gear icon is tapped, rather than
// waiting for an async round-trip to the watch.
// Initialise to 5 empty slots.
var currentAlarms = [
  { hour: 0, minute: 0, days: 0, enabled: false },
  { hour: 0, minute: 0, days: 0, enabled: false },
  { hour: 0, minute: 0, days: 0, enabled: false },
  { hour: 0, minute: 0, days: 0, enabled: false },
  { hour: 0, minute: 0, days: 0, enabled: false }
];

// ── Decode an AppMessage dictionary into currentAlarms[] ─────────
// Called when the watch sends its state to us.
function decodeAlarmsFromMessage(payload) {
  for (var slot = 0; slot < 5; slot++) {
    var keyHour  = slot * 3 + 0;
    var keyMin   = slot * 3 + 1;
    var keyFlags = slot * 3 + 2;

    // payload keys arrive as strings in PebbleKit JS
    if (payload[keyHour] !== undefined) {
      var flags = payload[keyFlags] || 0;
      currentAlarms[slot] = {
        hour:    payload[keyHour],
        minute:  payload[keyMin],
        days:    flags & 0x7F,          // bits 0-6
        enabled: (flags & 0x80) !== 0   // bit 7
      };
    }
  }
}

// ── Encode currentAlarms[] into an AppMessage dictionary ─────────
// Called when we need to send the config page's result back to the watch.
function encodeAlarmsToMessage(alarms) {
  var dict = {};
  for (var slot = 0; slot < 5; slot++) {
    var a = alarms[slot];
    var flags = (a.days & 0x7F) | (a.enabled ? 0x80 : 0);
    dict[slot * 3 + 0] = a.hour;
    dict[slot * 3 + 1] = a.minute;
    dict[slot * 3 + 2] = flags;
  }
  return dict;
}

// ── Receive messages from the watch ─────────────────────────────
// The watch sends key 15 = 0x42 as a "here is my state" signal,
// accompanied by the current alarm slots in keys 0-14.
Pebble.addEventListener('appmessage', function(e) {
  var payload = e.payload;
  console.log('JS received appmessage: ' + JSON.stringify(payload));

  // Key 15 = handshake — the watch is sending us its current alarm state
  if (payload[15] === 0x42) {
    decodeAlarmsFromMessage(payload);
    console.log('JS updated alarm cache from watch: ' + JSON.stringify(currentAlarms));
  }
});

// ── Build and open the config page ──────────────────────────────
// Fires when the user taps the gear (⚙) icon next to the app in rePebble.
Pebble.addEventListener('showConfiguration', function() {
  console.log('JS showConfiguration — opening config page');

  // Pass the current alarm state to the page as a URL-encoded JSON query param.
  // The page reads this on load to pre-fill its form fields.
  var alarmParam = encodeURIComponent(JSON.stringify(currentAlarms));

  // CONFIG_PAGE_URL is set at the bottom of this file.
  // In development this is a data: URI; swap for a GitHub Pages URL in production.
  var url = CONFIG_PAGE_URL + '?alarms=' + alarmParam;
  Pebble.openURL(url);
});

// ── Receive the result when the config page closes ───────────────
// Fires when config.html navigates to pebblekit://close?result=<JSON>.
// e.response contains the URL-decoded result string.
Pebble.addEventListener('webviewclosed', function(e) {
  if (!e.response) {
    console.log('JS webviewclosed — no result (user cancelled)');
    return;
  }

  console.log('JS webviewclosed — result: ' + e.response);

  var result;
  try {
    result = JSON.parse(decodeURIComponent(e.response));
  } catch (err) {
    console.log('JS failed to parse result: ' + err);
    return;
  }

  if (!result.alarms || result.alarms.length !== 5) {
    console.log('JS result has unexpected format');
    return;
  }

  // Update local cache
  currentAlarms = result.alarms;

  // Send all 5 slots to the watch via AppMessage
  var dict = encodeAlarmsToMessage(currentAlarms);
  console.log('JS sending to watch: ' + JSON.stringify(dict));

  Pebble.sendAppMessage(
    dict,
    function() { console.log('JS AppMessage send ACK'); },
    function() { console.log('JS AppMessage send NACK — delivery failed'); }
  );
});

// ── Config page URL ──────────────────────────────────────────────
// During development: set this to the data: URI generated from config.html.
// The CloudPebble simulator will show the page URL in the JS console when
// you run it — or paste the config.html content into a base64 encoder.
//
// For production: host config.html on GitHub Pages and use:
//   var CONFIG_PAGE_URL = 'https://yourusername.github.io/wakemeup/config.html';
//
// For now, point at a local file path. Replace with your actual URL.
// If using CloudPebble's built-in hosting, this will be provided automatically.
var CONFIG_PAGE_URL = 'REPLACE_WITH_YOUR_CONFIG_URL';
