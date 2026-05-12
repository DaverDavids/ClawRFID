#pragma once

// =============================================================================
//  html.h  —  clawrfid Web UI
//  Shows the last-seen UID from each of the two RC522 readers.
//  Auto-refreshes via /data JSON every 750 ms.
// =============================================================================

const char INDEX_HTML[] PROGMEM = R"rawliteral(
<!DOCTYPE html>
<html><head>
<meta charset="UTF-8">
<meta name="viewport" content="width=device-width,initial-scale=1">
<title>ClawRFID</title>
<style>
  *{box-sizing:border-box}
  body{font-family:sans-serif;background:#111;color:#ddd;max-width:480px;margin:32px auto;padding:0 16px}
  h1{font-size:1.2em;color:#aaa;margin-bottom:16px}
  .card{background:#1e1e1e;border-radius:10px;padding:16px;margin:10px 0}
  h2{font-size:.85em;color:#666;margin:0 0 10px;text-transform:uppercase;letter-spacing:1px}
  table{width:100%;border-collapse:collapse}
  td{padding:5px 2px;font-size:.9em}
  td:last-child{text-align:right;font-family:monospace;font-weight:700;font-size:1.05em;letter-spacing:2px}
  .lbl{color:#777;font-size:.75em;margin-bottom:6px}
  .dot{display:inline-block;width:7px;height:7px;border-radius:50%;background:#333;margin-right:5px;vertical-align:middle}
  .dot.ok{background:#4caf50}
  footer{text-align:right;font-size:.7em;color:#333;margin-top:6px}
</style>
</head><body>
<h1>&#128278; ClawRFID</h1>

<div class="card">
  <h2>Reader 1 (RFID1)</h2>
  <table>
    <tr><td>Last UID</td><td id="uid1">—</td></tr>
  </table>
</div>

<div class="card">
  <h2>Reader 2 (RFID2)</h2>
  <table>
    <tr><td>Last UID</td><td id="uid2">—</td></tr>
  </table>
</div>

<footer><span class="dot" id="dot"></span><span id="ts">—</span></footer>

<script>
function update(){
  fetch('/data').then(r=>r.json()).then(d=>{
    document.getElementById('uid1').textContent = d.uid1 || '\u2014';
    document.getElementById('uid2').textContent = d.uid2 || '\u2014';
    document.getElementById('dot').className = 'dot ok';
    document.getElementById('ts').textContent = new Date().toLocaleTimeString();
  }).catch(()=>{ document.getElementById('dot').className = 'dot'; });
}
update();
setInterval(update, 750);
</script>
</body></html>
)rawliteral";

const char WIFI_HTML[] PROGMEM = R"rawliteral(
<!DOCTYPE html>
<html><head>
<meta charset="UTF-8"><meta name="viewport" content="width=device-width,initial-scale=1">
<title>WiFi Setup</title>
<style>
  body{font-family:sans-serif;background:#111;color:#ddd;max-width:400px;margin:40px auto;padding:0 16px}
  h2{color:#aaa}
  input{width:100%;padding:9px;margin:5px 0 14px;background:#222;border:1px solid #444;color:#eee;border-radius:5px;font-size:1em}
  button{width:100%;padding:11px;background:#1976d2;color:#fff;border:none;border-radius:5px;font-size:1em;cursor:pointer}
  .note{font-size:.8em;color:#555;margin-top:18px}
</style>
</head><body>
<h2>&#x1F4F6; WiFi Setup</h2>
<form method="POST" action="/setwifi">
  <label>SSID</label><input type="text" name="ssid" autocomplete="off" required>
  <label>Password</label><input type="password" name="psk">
  <button type="submit">Save &amp; Connect</button>
</form>
<p class="note">Saved to flash. Device reboots and connects.</p>
</body></html>
)rawliteral";
