#pragma once

// =============================================================================
//  html.h  —  clawrfid Web UI
//  INDEX_HTML  : main page — shows last UID, signal strength bar, error rate
//                bar, and seconds since last read for each RC522 reader.
//  PORTAL_HTML : captive portal WiFi setup page (AP mode)
//  Auto-refreshes via /data JSON every 750 ms.
//
//  /data JSON fields:
//    uid1/uid2   — last UID string
//    gain1/gain2 — RFCfgReg gain nibble 0-7 (antenna gain level)
//    hits1/hits2 — successful detections out of last 16 polls (0-16)
//    age1/age2   — ms since last successful read (0xFFFFFFFF = never)
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
  td{padding:5px 2px;font-size:.9em;vertical-align:middle}
  td:last-child{text-align:right;font-family:monospace;font-weight:700;font-size:1.05em;letter-spacing:2px}
  .dot{display:inline-block;width:7px;height:7px;border-radius:50%;background:#333;margin-right:5px;vertical-align:middle}
  .dot.ok{background:#4caf50}
  footer{text-align:right;font-size:.7em;color:#333;margin-top:6px}

  /* ── Vertical bar graph ───────────────────────────────────────────── */
  .bars{
    display:inline-flex;
    align-items:flex-end;
    gap:2px;
    height:18px;
    vertical-align:middle;
  }
  .bar{
    width:5px;
    border-radius:1px 1px 0 0;
    background:#333;
    transition:height .3s,background .3s;
  }
  .bar.lit-sig { background:#4caf50; }
  .bar.lit-err { background:#f44336; }
  .age-val{
    font-family:monospace;
    font-size:.95em;
    font-weight:700;
    color:#64b5f6;
  }
  .age-val.stale{ color:#f44336; }
  .age-label{ font-size:.8em; color:#666; margin-left:2px; }
</style>
</head><body>
<h1>&#128278; ClawRFID</h1>

<div class="card">
  <h2>Reader 1 (RFID1)</h2>
  <table>
    <tr><td>Last UID</td><td id="uid1">&mdash;</td></tr>
    <tr>
      <td>Signal</td>
      <td><span class="bars" id="sig1"></span></td>
    </tr>
    <tr>
      <td>Error rate</td>
      <td><span class="bars" id="err1"></span></td>
    </tr>
    <tr>
      <td>Last read</td>
      <td><span class="age-val" id="age1">&mdash;</span><span class="age-label">s</span></td>
    </tr>
  </table>
</div>

<div class="card">
  <h2>Reader 2 (RFID2)</h2>
  <table>
    <tr><td>Last UID</td><td id="uid2">&mdash;</td></tr>
    <tr>
      <td>Signal</td>
      <td><span class="bars" id="sig2"></span></td>
    </tr>
    <tr>
      <td>Error rate</td>
      <td><span class="bars" id="err2"></span></td>
    </tr>
    <tr>
      <td>Last read</td>
      <td><span class="age-val" id="age2">&mdash;</span><span class="age-label">s</span></td>
    </tr>
  </table>
</div>

<footer><span class="dot" id="dot"></span><span id="ts">&mdash;</span></footer>

<script>
// Build a vertical bar graph inside container element.
// n     : number of bars total
// lit   : how many bars are filled (from left)
// cls   : CSS class to apply to lit bars ('lit-sig' or 'lit-err')
// hMin/hMax : min/max bar height in px
function buildBars(el, n, lit, cls, hMin, hMax) {
  // Re-use existing bar divs if count matches, else rebuild once.
  if (el.children.length !== n) {
    el.innerHTML = '';
    for (var i = 0; i < n; i++) {
      var b = document.createElement('div');
      b.className = 'bar';
      el.appendChild(b);
    }
  }
  var bars = el.children;
  for (var i = 0; i < n; i++) {
    var frac = (i + 1) / n;
    var h = Math.round(hMin + frac * (hMax - hMin));
    bars[i].style.height = h + 'px';
    bars[i].className = 'bar' + (i < lit ? ' ' + cls : '');
  }
}

function update() {
  fetch('/data').then(function(r){ return r.json(); }).then(function(d) {
    // UIDs
    document.getElementById('uid1').textContent = d.uid1 || '\u2014';
    document.getElementById('uid2').textContent = d.uid2 || '\u2014';

    // Signal strength: gain nibble 0-7, show as 8-bar graph
    buildBars(document.getElementById('sig1'), 8, d.gain1 + 1, 'lit-sig', 4, 18);
    buildBars(document.getElementById('sig2'), 8, d.gain2 + 1, 'lit-sig', 4, 18);

    // Error rate: hits1/hits2 out of 16 polls.
    // More hits = fewer errors. Show error bars = 16 - hits (red = bad).
    // 8-bar display scaled over 0-16 range.
    var errBars1 = Math.round((16 - d.hits1) / 16 * 8);
    var errBars2 = Math.round((16 - d.hits2) / 16 * 8);
    buildBars(document.getElementById('err1'), 8, errBars1, 'lit-err', 4, 18);
    buildBars(document.getElementById('err2'), 8, errBars2, 'lit-err', 4, 18);

    // Seconds since last read
    var NEVER = 4294967295; // 0xFFFFFFFF sentinel
    var a1 = document.getElementById('age1');
    var a2 = document.getElementById('age2');
    if (d.age1 === NEVER || d.age1 === 0) {
      a1.textContent = '\u2014'; a1.className = 'age-val';
    } else {
      var s1 = (d.age1 / 1000).toFixed(1);
      a1.textContent = s1;
      a1.className = 'age-val' + (d.age1 > 30000 ? ' stale' : '');
    }
    if (d.age2 === NEVER || d.age2 === 0) {
      a2.textContent = '\u2014'; a2.className = 'age-val';
    } else {
      var s2 = (d.age2 / 1000).toFixed(1);
      a2.textContent = s2;
      a2.className = 'age-val' + (d.age2 > 30000 ? ' stale' : '');
    }

    document.getElementById('dot').className = 'dot ok';
    document.getElementById('ts').textContent = new Date().toLocaleTimeString();
  }).catch(function(){
    document.getElementById('dot').className = 'dot';
  });
}
update();
setInterval(update, 750);
</script>
</body></html>
)rawliteral";

const char PORTAL_HTML[] PROGMEM = R"rawliteral(
<!DOCTYPE html>
<html><head>
<meta charset="UTF-8"><meta name="viewport" content="width=device-width,initial-scale=1">
<title>ClawRFID — WiFi Setup</title>
<style>
  body{font-family:sans-serif;background:#111;color:#ddd;max-width:400px;margin:40px auto;padding:0 16px}
  h2{color:#aaa}
  input{width:100%;padding:9px;margin:5px 0 14px;background:#222;border:1px solid #444;color:#eee;border-radius:5px;font-size:1em}
  button{width:100%;padding:11px;background:#1976d2;color:#fff;border:none;border-radius:5px;font-size:1em;cursor:pointer}
  button:hover{background:#1565c0}
  .note{font-size:.8em;color:#555;margin-top:18px}
</style>
</head><body>
<h2>&#x1F4F6; WiFi Setup</h2>
<form method="POST" action="/savewifi">
  <label>SSID</label><input type="text" name="ssid" autocomplete="off" required>
  <label>Password</label><input type="password" name="psk">
  <button type="submit">Save &amp; Connect</button>
</form>
<p class="note">Credentials saved to flash. Device reboots and connects.</p>
</body></html>
)rawliteral";
