// index.js — PebbleKit JS bridge, runs inside rePebble on the phone.
//
// KEY SCHEME (matches main.c):
//   slot*3+0  → hour   (0-23)
//   slot*3+1  → minute (0-59)
//   slot*3+2  → packed flags: bits 0-6 = days bitmask, bit 7 = enabled
//   key 15    → handshake value 0x42

var CONFIG_PAGE_URL = 'https://rbreen.github.io/MyPebbleProjects/config.html';

// ── Alarm state cache ─────────────────────────────────────────────
var currentAlarms = [
  { hour: 0, minute: 0, days: 0, enabled: false },
  { hour: 0, minute: 0, days: 0, enabled: false },
  { hour: 0, minute: 0, days: 0, enabled: false },
  { hour: 0, minute: 0, days: 0, enabled: false },
  { hour: 0, minute: 0, days: 0, enabled: false }
];

// ── Decode AppMessage payload → currentAlarms[] ───────────────────
function decodeAlarmsFromMessage(payload) {
  console.log('[DEBUG] decodeAlarmsFromMessage — raw payload: ' + JSON.stringify(payload));
  for (var slot = 0; slot < 5; slot++) {
    var keyHour  = slot * 3 + 0;
    var keyMin   = slot * 3 + 1;
    var keyFlags = slot * 3 + 2;
    // PebbleKit JS delivers ALL keys as strings — always use String() to look them up
    var hour  = payload[String(keyHour)];
    var min   = payload[String(keyMin)];
    var flags = payload[String(keyFlags)];
    if (hour !== undefined) {
      flags = flags || 0;
      currentAlarms[slot] = {
        hour:    hour,
        minute:  min,
        days:    flags & 0x7F,
        enabled: (flags & 0x80) !== 0
      };
      console.log('[DEBUG] slot ' + slot + ': hour=' + hour + ' min=' + min +
                  ' flags=0x' + flags.toString(16) +
                  ' days=0x' + (flags & 0x7F).toString(16) +
                  ' enabled=' + ((flags & 0x80) !== 0));
    } else {
      console.log('[DEBUG] slot ' + slot + ': keys not found in payload');
    }
  }
}

// ── Encode currentAlarms[] → AppMessage dict ──────────────────────
function encodeAlarmsToMessage(alarms) {
  var dict = {};
  for (var slot = 0; slot < 5; slot++) {
    var a = alarms[slot];
    var flags = (a.days & 0x7F) | (a.enabled ? 0x80 : 0);
    dict[slot * 3 + 0] = a.hour;
    dict[slot * 3 + 1] = a.minute;
    dict[slot * 3 + 2] = flags;
    console.log('[DEBUG] encode slot ' + slot + ': hour=' + a.hour +
                ' min=' + a.minute + ' flags=0x' + flags.toString(16));
  }
  return dict;
}

// ── Receive messages from the watch ──────────────────────────────
Pebble.addEventListener('appmessage', function(e) {
  console.log('[DEBUG] appmessage received — raw: ' + JSON.stringify(e.payload));
  var payload = e.payload;

  // Key 15 = handshake — watch is pushing its current alarm state
  // PebbleKit JS delivers ALL dictionary keys as strings, never numbers.
  // So key 15 arrives as payload["15"], not payload[15].
  var handshakeVal = payload['15'];
  console.log('[DEBUG] handshake key "15" value: ' + handshakeVal + ' (expect 66 = 0x42)');

  // Value also arrives as a number, so compare numerically
  if (handshakeVal !== undefined && Number(handshakeVal) === 0x42) {
    console.log('[DEBUG] handshake matched — decoding alarms from watch');
    decodeAlarmsFromMessage(payload);
    console.log('[DEBUG] currentAlarms after decode: ' + JSON.stringify(currentAlarms));
  } else {
    console.log('[DEBUG] no handshake key in message — ignoring (val=' + handshakeVal + ')');
  }
});

// ── Open config page ──────────────────────────────────────────────
Pebble.addEventListener('showConfiguration', function() {
  console.log('[DEBUG] showConfiguration fired');
  console.log('[DEBUG] currentAlarms at open time: ' + JSON.stringify(currentAlarms));

  var alarmParam = encodeURIComponent(JSON.stringify(currentAlarms));
  // Cache-buster: append current timestamp so rePebble's UIWebView always
  // fetches a fresh copy of config.html rather than serving a cached version.
  var cacheBust = '&_=' + Date.now();
  var url = CONFIG_PAGE_URL + '?alarms=' + alarmParam + cacheBust;
  console.log('[DEBUG] opening URL: ' + url.substring(0, 120) + '...');
  Pebble.openURL(url);
});

// ── Receive result when config page closes ────────────────────────
Pebble.addEventListener('webviewclosed', function(e) {
  console.log('[DEBUG] webviewclosed fired');
  console.log('[DEBUG] e.response present: ' + (!!e.response));

  if (!e.response || e.response === 'CANCELLED') {
    console.log('[DEBUG] webviewclosed — no result or cancelled, doing nothing');
    return;
  }

  console.log('[DEBUG] raw response (first 200 chars): ' + e.response.substring(0, 200));

  var result;
  try {
    result = JSON.parse(decodeURIComponent(e.response));
    console.log('[DEBUG] parsed result OK — alarm count: ' +
                (result.alarms ? result.alarms.length : 'NO ALARMS KEY'));
  } catch (err) {
    console.log('[DEBUG] ERROR parsing result: ' + err);
    return;
  }

  if (!result.alarms || result.alarms.length !== 5) {
    console.log('[DEBUG] ERROR unexpected result format: ' + JSON.stringify(result));
    return;
  }

  currentAlarms = result.alarms;
  console.log('[DEBUG] currentAlarms updated from config page: ' + JSON.stringify(currentAlarms));

  var dict = encodeAlarmsToMessage(currentAlarms);
  console.log('[DEBUG] sending AppMessage to watch: ' + JSON.stringify(dict));

  Pebble.sendAppMessage(
    dict,
    function() { console.log('[DEBUG] AppMessage ACK — watch received the config'); },
    function(e) { console.log('[DEBUG] AppMessage NACK — delivery failed: ' + JSON.stringify(e)); }
  );
});

// ── App ready ─────────────────────────────────────────────────────
// Fires when the JS runtime starts and the watch is connected.
// We send the handshake immediately so the watch pushes its current
// alarm state to us — this primes currentAlarms before the user
// opens the config page.
Pebble.addEventListener('ready', function() {
  console.log('[DEBUG] ready — watch: ' + Pebble.getActiveWatchInfo().model);
  console.log('[DEBUG] ready — sending handshake to request alarm state from watch');
  Pebble.sendAppMessage(
    { 15: 0x42 },
    function() { console.log('[DEBUG] ready handshake ACK'); },
    function(e) { console.log('[DEBUG] ready handshake NACK: ' + JSON.stringify(e)); }
  );
});
