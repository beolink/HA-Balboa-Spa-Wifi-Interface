#include "web.h"

#include <Update.h>
#include <WebServer.h>
#include <WiFi.h>

#include "bus_task.h"
#include "ota_guard.h"
#include "version.h"

namespace {

WebServer server(80);
Settings* settings = nullptr;
bwa::Hub* hub = nullptr;
bwa::NetServer* net = nullptr;

constexpr int kLogSize = 48;
BusLogEntry log_entries[kLogSize];
int log_head = 0;
int log_count = 0;

void pull_log() {
  BusLogEntry e;
  while (bus_log_pop(e)) {
    log_entries[log_head] = e;
    log_head = (log_head + 1) % kLogSize;
    if (log_count < kLogSize) ++log_count;
  }
}

bool authorized() {
  if (settings->admin_pass.isEmpty()) return true;
  if (server.authenticate("admin", settings->admin_pass.c_str())) return true;
  server.requestAuthentication(BASIC_AUTH, "spa-bridge");
  return false;
}

String hex_of(const bwa::Frame& f) {
  String s;
  s.reserve(f.size() * 3);
  char b[4];
  for (size_t i = 1; i + 1 < f.size(); ++i) {  // without the delimiters
    snprintf(b, sizeof(b), i > 1 ? " %02x" : "%02x", f.bytes[i]);
    s += b;
  }
  return s;
}

String json_escape(const String& in) {
  String out;
  for (size_t i = 0; i < in.length(); ++i) {
    const char c = in[i];
    if (c == '"' || c == '\\') {
      out += '\\';
      out += c;
    } else if (static_cast<uint8_t>(c) >= 0x20) {
      out += c;
    }
  }
  return out;
}

String fmt_temp(float v) {
  if (v <= -999) return "null";
  char b[16];
  snprintf(b, sizeof(b), "%.1f", v);
  return b;
}

void handle_status() {
  const BusReport b = bus_report();
  const bwa::BusStats& bs = bus_stats();
  const bwa::HubStats& hs = hub->stats();
  const bwa::Hub::Snapshot snap = hub->snapshot();
  String j;
  j.reserve(1600);
  j += "{\"firmware\":\"" FIRMWARE_VERSION "\",\"board\":\"" BOARD_NAME "\"";
  j += ",\"uptime_s\":" + String(millis() / 1000);
  const OtaReport ota = ota_guard_report();
  j += ",\"update\":{\"on_trial\":" + String(ota.on_trial ? "true" : "false");
  j += ",\"trial_left_s\":" + String(ota.trial_left_s);
  j += ",\"rolled_back\":" + String(ota.rolled_back ? "true" : "false");
  j += ",\"confirmed_by\":\"" + json_escape(ota.confirmed_by) + "\"}";
  j += ",\"wifi\":{\"connected\":" + String(WiFi.isConnected() ? "true" : "false");
  j += ",\"ssid\":\"" + json_escape(WiFi.SSID()) + "\",\"ip\":\"" + WiFi.localIP().toString() + "\"";
  j += ",\"rssi\":" + String(WiFi.RSSI()) + ",\"hostname\":\"" + json_escape(settings->hostname) + "\"}";
  j += ",\"bus\":{\"mode\":\"" + String(bwa::mode_name(b.mode)) + "\"";
  j += ",\"state\":\"" + String(bwa::state_name(b.state)) + "\"";
  j += ",\"channel\":" + String(b.channel);
  j += ",\"rx_inverted\":" + String(b.rx_inverted ? "true" : "false");
  j += ",\"bytes\":" + String(b.bytes) + ",\"frames\":" + String(b.frames);
  j += ",\"skipped_bytes\":" + String(b.skipped) + ",\"overflows\":" + String(b.overflows);
  j += ",\"last_frame_age_ms\":" + (b.last_frame_age_ms == UINT32_MAX ? String("null") : String(b.last_frame_age_ms));
  j += ",\"cts_seen\":" + String(bs.cts_seen.load()) + ",\"cts_ours\":" + String(bs.cts_ours.load());
  j += ",\"sent\":" + String(bs.sent.load()) + ",\"echoes\":" + String(bs.echoes.load());
  j += ",\"dropped_listen\":" + String(bs.dropped_listen.load());
  j += ",\"channel_requests\":" + String(bs.channel_requests.load());
  j += ",\"channel_losses\":" + String(bs.channel_losses.load());
  j += ",\"wifi_channel_cts\":" + String(bs.wifi_channel_cts.load());
  j += ",\"wifi_channel_answers\":" + String(bs.wifi_channel_answers.load());
  j += ",\"answer_us_last\":" + String(b.answer_us_last) + ",\"answer_us_max\":" + String(b.answer_us_max);
  j += ",\"up_dropped\":" + String(b.up_dropped) + "}";
  j += ",\"tcp\":{\"clients\":" + String(net->clients()) + ",\"refused\":" + String(net->refused());
  j += ",\"slow_drops\":" + String(net->slow_drops()) + ",\"discoveries\":" + String(net->discoveries());
  j += ",\"client_frames\":" + String(hs.client_frames.load()) + ",\"to_bus\":" + String(hs.to_bus.load());
  j += ",\"bus_queue_full\":" + String(hs.bus_queue_full.load());
  j += ",\"cache_answers\":" + String(hs.cache_answers.load()) + ",\"cache_misses\":" + String(hs.cache_misses.load());
  j += ",\"dropped_listen\":" + String(hs.dropped_listen.load()) + ",\"dropped_type\":" + String(hs.dropped_type.load()) + "}";
  j += ",\"spa\":{\"mac\":\"" + format_mac(settings->mac) + "\"";
  j += ",\"model\":\"" + json_escape(snap.info.model) + "\",\"software\":\"" + json_escape(snap.info.software) + "\"";
  j += ",\"status_age_ms\":" + (snap.status_ms ? String(millis() - snap.status_ms) : String("null"));
  if (snap.status.valid) {
    const bwa::SpaStatus& s = snap.status;
    j += ",\"unit\":\"" + String(s.celsius ? "C" : "F") + "\"";
    j += ",\"water\":" + fmt_temp(s.water) + ",\"target\":" + fmt_temp(s.target);
    char t[8];
    snprintf(t, sizeof(t), "%02u:%02u", s.hour, s.minute);
    j += ",\"clock\":\"" + String(t) + "\"";
    j += ",\"heating\":" + String(s.heating) + ",\"heat_mode\":" + String(s.heat_mode);
    j += ",\"high_range\":" + String(s.high_range ? "true" : "false");
    j += ",\"pumps\":[" + String(s.pumps[0]) + "," + String(s.pumps[1]) + "," + String(s.pumps[2]) + "]";
    j += ",\"blower\":" + String(s.blower) + ",\"light\":" + String(s.light ? "true" : "false");
    j += ",\"priming\":" + String(s.priming ? "true" : "false");
  }
  j += "}}";
  server.send(200, "application/json", j);
}

void handle_log() {
  pull_log();
  String j = "[";
  for (int i = 0; i < log_count; ++i) {
    const BusLogEntry& e = log_entries[(log_head - log_count + i + kLogSize) % kLogSize];
    if (i) j += ",";
    j += "{\"ms\":" + String(e.ms) + ",\"tx\":" + String(e.tx ? "true" : "false") + ",\"hex\":\"" +
         hex_of(e.frame) + "\"}";
  }
  j += "]";
  server.send(200, "application/json", j);
}

BusSettings bus_settings_from(const Settings& s) {
  BusSettings b;
  b.mode = s.mode;
  b.rx_inverted = s.rx_inverted;
  b.client_hash = static_cast<uint16_t>((s.mac[4] << 8) | s.mac[5]);
  return b;
}

void apply_hub_settings() {
  bwa::HubConfig c = hub->config();
  memcpy(c.mac, settings->mac, 6);
  c.status_interval_ms = settings->status_interval_ms;
  hub->configure(c);
}

void handle_mode() {
  if (!authorized()) return;
  bwa::BusMode m;
  if (!bwa::parse_mode(server.arg("mode").c_str(), m)) {
    server.send(400, "text/plain", "unknown mode");
    return;
  }
  settings->mode = m;
  settings->save();
  bus_task_reconfigure(bus_settings_from(*settings));
  server.sendHeader("Location", "/");
  server.send(303);
}

void handle_settings() {
  if (!authorized()) return;
  bool restart = false;
  if (server.hasArg("ssid") && server.arg("ssid") != settings->wifi_ssid) {
    settings->wifi_ssid = server.arg("ssid");
    restart = true;
  }
  if (server.hasArg("pass") && !server.arg("pass").isEmpty()) {
    settings->wifi_pass = server.arg("pass");
    restart = true;
  }
  if (server.hasArg("hostname") && !server.arg("hostname").isEmpty() &&
      server.arg("hostname") != settings->hostname) {
    settings->hostname = server.arg("hostname");
    restart = true;
  }
  if (server.hasArg("admin") && !server.arg("admin").isEmpty()) settings->admin_pass = server.arg("admin");
  if (server.hasArg("mac") && !server.arg("mac").isEmpty()) {
    uint8_t mac[6];
    if (!parse_mac(server.arg("mac"), mac)) {
      server.send(400, "text/plain", "MAC not understood (aa:bb:cc:dd:ee:ff)");
      return;
    }
    memcpy(settings->mac, mac, 6);
  }
  if (server.hasArg("status_ms")) settings->status_interval_ms = server.arg("status_ms").toInt();
  if (server.hasArg("polarity")) {
    const String p = server.arg("polarity");
    settings->rx_inverted = p == "normal" ? 0 : p == "inverted" ? 1 : -1;
    bus_task_reconfigure(bus_settings_from(*settings));
  }
  settings->save();
  apply_hub_settings();
  if (restart) {
    ota_guard_confirm("settings saved on the web page");
    server.send(200, "text/plain", "Saved. Restarting to join the network...");
    delay(500);
    ESP.restart();
  }
  server.sendHeader("Location", "/");
  server.send(303);
}

void handle_reboot() {
  if (!authorized()) return;
  ota_guard_confirm("restart from the web page");
  server.send(200, "text/plain", "Restarting...");
  delay(300);
  ESP.restart();
}

bool upload_ok = false;

void handle_update_done() {
  if (!authorized()) return;
  const bool ok = upload_ok && !Update.hasError();
  server.send(ok ? 200 : 500, "text/plain", ok ? "Updated. Restarting..." : "Update failed");
  if (ok) {
    delay(500);
    ESP.restart();
  }
}

void handle_update_upload() {
  HTTPUpload& up = server.upload();
  if (up.status == UPLOAD_FILE_START) {
    upload_ok = settings->admin_pass.isEmpty() ||
                server.authenticate("admin", settings->admin_pass.c_str());
    if (upload_ok) {
      ota_guard_confirm("new update starting");
      upload_ok = Update.begin(UPDATE_SIZE_UNKNOWN);
    }
  } else if (up.status == UPLOAD_FILE_WRITE) {
    if (upload_ok && Update.write(up.buf, up.currentSize) != up.currentSize) upload_ok = false;
  } else if (up.status == UPLOAD_FILE_END) {
    if (upload_ok) upload_ok = Update.end(true);
  } else if (up.status == UPLOAD_FILE_ABORTED) {
    Update.abort();
    upload_ok = false;
  }
}

const char kPage[] PROGMEM = R"HTML(<!doctype html>
<html><head><meta charset="utf-8"><meta name="viewport" content="width=device-width,initial-scale=1">
<title>Spa bridge</title>
<style>
body{font:15px/1.4 system-ui,sans-serif;margin:0;padding:16px;max-width:860px;color:#1d2a33;background:#f6f8f9}
h1{font-size:20px;margin:0 0 4px}h2{font-size:15px;margin:22px 0 8px;text-transform:uppercase;letter-spacing:.04em;color:#52646f}
.card{background:#fff;border:1px solid #dde4e8;border-radius:10px;padding:12px 14px;margin:8px 0}
table{border-collapse:collapse;width:100%}td{padding:3px 6px;vertical-align:top}td:first-child{color:#52646f;width:44%}
.ok{color:#18794e}.warn{color:#a35200}.bad{color:#b42318}
label{display:block;margin:6px 0}input[type=text],input[type=password]{width:100%;padding:6px;box-sizing:border-box}
button{padding:7px 14px;margin-top:8px}code,pre{font:12px ui-monospace,monospace}pre{white-space:pre-wrap;margin:0}
small{color:#52646f}
</style></head><body>
<h1>Spa bridge</h1><small id="sub"></small>
<h2>Bus</h2><div class="card"><table id="bus"></table></div>
<h2>Spa</h2><div class="card"><table id="spa"></table></div>
<h2>Network</h2><div class="card"><table id="net"></table></div>
<h2>Mode</h2><div class="card"><form method="post" action="/api/mode">
<label><input type="radio" name="mode" value="listen_only"> <b>Listen only</b> &mdash; never writes to the bus. Reads status, and answers settings requests from what it has heard.</label>
<label><input type="radio" name="mode" value="assigned"> <b>Own channel</b> &mdash; asks the main board for a channel like a top-side panel and writes only when the board gives it the turn. Works beside the stock Wi-Fi module.</label>
<label><input type="radio" name="mode" value="fixed_wifi"> <b>Wi-Fi module channel 0x0A</b> &mdash; takes the stock module's channel. Only with the stock module unplugged; backs off if anything else answers on 0x0A.</label>
<button>Set mode</button></form></div>
<h2>Bus log</h2><div class="card"><small>Without CTS, Nothing to Send and status. &rarr; marks frames the bridge wrote.</small><pre id="log"></pre></div>
<h2>Settings</h2><div class="card"><form method="post" action="/api/settings">
<label>Wi-Fi network <input type="text" name="ssid" id="ssid"></label>
<label>Wi-Fi password (blank: unchanged) <input type="password" name="pass"></label>
<label>Host name <input type="text" name="hostname" id="hostname"></label>
<label>MAC reported to clients (use the stock module's to keep a Home Assistant entry) <input type="text" name="mac" id="mac"></label>
<label>Unchanged status passed on at most every (ms, 0 = all) <input type="text" name="status_ms" id="status_ms"></label>
<label>Receiver polarity <select name="polarity"><option value="auto">auto</option><option value="normal">normal</option><option value="inverted">inverted (A/B swapped: listen only)</option></select></label>
<label>Admin password for this page and updates (blank: unchanged) <input type="password" name="admin"></label>
<button>Save</button></form></div>
<h2>Firmware</h2><div class="card"><form method="post" action="/update" enctype="multipart/form-data">
<input type="file" name="firmware" accept=".bin"> <button>Upload</button></form>
<form method="post" action="/api/reboot"><button>Restart</button></form></div>
<script>
const $=id=>document.getElementById(id);
const row=(k,v,c)=>`<tr><td>${k}</td><td${c?` class="${c}"`:''}>${v}</td></tr>`;
const age=ms=>ms==null?'never':ms<2000?'now':Math.round(ms/1000)+' s ago';
let filled=false;
async function tick(){
 try{
  const s=await (await fetch('/api/status')).json(),b=s.bus,p=s.spa,t=s.tcp,w=s.wifi;
  $('sub').textContent=`${s.board} · firmware ${s.firmware} · up ${Math.round(s.uptime_s/60)} min`;
  const st={online:'ok',listening:'ok',observing:'warn',requesting:'warn',awaiting_channel:'warn',gave_up:'bad',silent:'bad'}[b.state];
  $('bus').innerHTML=row('Mode',b.mode)+row('State',b.state+(b.channel?` on channel 0x${b.channel.toString(16)}`:''),st)
   +row('Last valid frame',age(b.last_frame_age_ms),b.last_frame_age_ms==null||b.last_frame_age_ms>5000?'bad':'')
   +row('Frames / bytes',`${b.frames} / ${b.bytes} (${b.skipped_bytes} outside frames, ${b.overflows} overruns)`)
   +row('Receiver',b.rx_inverted?'inverted: A and B are swapped, writing blocked':'normal',b.rx_inverted?'bad':'')
   +row('CTS answered',`${b.cts_ours} of ${b.cts_seen} seen; ${b.sent} frames sent; answer ${b.answer_us_last} µs (max ${b.answer_us_max})`)
   +row('Channel requests',`${b.channel_requests} (${b.channel_losses} lost)`)
   +row('Stock module on 0x0A',b.wifi_channel_answers?`answers (${b.wifi_channel_answers})`:(b.wifi_channel_cts?'polled, silent':'not polled'))
   +row('Writes held back (listen only)',b.dropped_listen+t.dropped_listen);
  $('spa').innerHTML=row('Model',p.model?`${p.model} (${p.software})`:'not heard yet')
   +row('Status',age(p.status_age_ms))
   +(p.water!==undefined?row('Water / target',`${p.water??'?'} / ${p.target} °${p.unit}`)+row('Clock',p.clock)
   +row('Pumps',p.pumps.join(', '))+row('Light / blower',`${p.light?'on':'off'} / ${p.blower}`)
   +row('Heating',['off','heating','waiting'][p.heating]+(p.high_range?', high range':', low range')):'')
   +row('MAC reported',p.mac);
  const u=s.update;
  $('net').innerHTML=row('Firmware',u.on_trial?`new update on trial: kept once online for a minute, otherwise back to the previous one in ${u.trial_left_s} s`:(u.rolled_back?'the last update did not come online and was rolled back; this is the one before it':(u.confirmed_by?`update kept (${u.confirmed_by})`:'running')),u.on_trial||u.rolled_back?'warn':'')
   +row('Wi-Fi',w.connected?`${w.ssid} (${w.rssi} dBm) ${w.ip}`:'not connected',w.connected?'':'bad')
   +row('Clients on 4257',`${t.clients} (${t.refused} refused, ${t.slow_drops} too slow)`)
   +row('From clients',`${t.client_frames} frames, ${t.to_bus} to the bus, ${t.cache_answers} answered from what was heard (${t.cache_misses} not), ${t.dropped_type} not passed on`)
   +row('Discovery answers',t.discoveries);
  if(!filled){filled=true;$('ssid').value=w.ssid;$('hostname').value=w.hostname;$('mac').value=p.mac;
   document.querySelectorAll('input[name=mode]').forEach(r=>r.checked=r.value==b.mode);}
  const l=await (await fetch('/api/log')).json();
  $('log').textContent=l.map(e=>`${(e.ms/1000).toFixed(3).padStart(10)} ${e.tx?'→':' '} ${e.hex}`).join('\n');
 }catch(e){}
}
tick();setInterval(tick,2000);
</script></body></html>)HTML";

}  // namespace

void web_start(Settings& s, bwa::Hub& h, bwa::NetServer& n) {
  settings = &s;
  hub = &h;
  net = &n;
  server.on("/", HTTP_GET, [] { server.send_P(200, "text/html", kPage); });
  server.on("/api/status", HTTP_GET, handle_status);
  server.on("/api/log", HTTP_GET, handle_log);
  server.on("/api/mode", HTTP_POST, handle_mode);
  server.on("/api/settings", HTTP_POST, handle_settings);
  server.on("/api/reboot", HTTP_POST, handle_reboot);
  server.on("/update", HTTP_POST, handle_update_done, handle_update_upload);
  server.onNotFound([] { server.send(404, "text/plain", "not found"); });
  server.begin();
}

void web_loop() {
  server.handleClient();
  pull_log();
}
