#include "wifi_setup_portal.h"

#include <WiFi.h>

#include <string>

#include "../config/runtime_config.h"
#include "../health/structured_log.h"
#include "../protocol/protocol_codec.h"  // reuse json_escape for the scan-results list

namespace kiosk::network {

namespace {
const char* state_name(WifiRecoveryState s) {
  switch (s) {
    case WifiRecoveryState::INACTIVE: return "INACTIVE";
    case WifiRecoveryState::AP_ACTIVE: return "AP_ACTIVE";
    case WifiRecoveryState::TESTING: return "TESTING";
    case WifiRecoveryState::FAILED: return "FAILED";
  }
  return "UNKNOWN";
}

String setup_page(const String& ssid) {
  String page;
  page.reserve(3000);
  page += F("<!doctype html><html><head><meta charset='utf-8'>"
             "<meta name='viewport' content='width=device-width,initial-scale=1'>"
             "<title>MESFlow Kiosk - Cai dat Wi-Fi</title><style>"
             "body{font-family:Arial,sans-serif;background:#101828;color:#fff;margin:0;padding:18px}"
             ".card{max-width:480px;margin:auto;background:#1d2939;border-radius:14px;padding:20px}"
             "h1{font-size:20px;margin:0 0 4px}.sub{color:#98a2b3;margin-bottom:16px;font-size:13px}"
             "label{display:block;font-weight:700;margin-top:14px;font-size:13px}"
             "input,select,button{width:100%;box-sizing:border-box;padding:11px;margin-top:6px;"
             "border-radius:8px;border:1px solid #344054;font-size:15px}"
             "button{background:#1570ef;color:#fff;border:0;font-weight:700;margin-top:14px}"
             ".cancel{background:#475467}.status{white-space:pre-wrap;background:#0d1420;"
             "padding:10px;border-radius:8px;margin-top:12px;font-size:13px;min-height:20px}"
             "</style></head><body><div class='card'>"
             "<h1>Cai dat Wi-Fi cho Kiosk</h1><div class='sub'>AP: ");
  page += ssid;
  page += F(" &middot; 192.168.4.1</div>"
            "<button type='button' onclick='doScan()'>Quet Wi-Fi gan day</button>"
            "<label>Ten Wi-Fi (SSID)</label><select id='s'><option value=''>-- chon hoac go tay --</option></select>"
            "<input id='sm' placeholder='hoac nhap SSID thu cong' style='margin-top:6px'>"
            "<label>Mat khau Wi-Fi</label><input id='p' type='password' autocomplete='new-password'>"
            "<button onclick='doSave()'>Luu va kiem tra ket noi</button>"
            "<button class='cancel' onclick='doCancel()'>Huy, thoat cai dat</button>"
            "<div id='st' class='status'>San sang.</div></div><script>"
            "async function doScan(){const st=document.getElementById('st');st.textContent='Dang quet...';"
            "try{const r=await fetch('/scan');const j=await r.json();const e=document.getElementById('s');"
            "e.innerHTML='<option value=\"\">-- chon --</option>';j.forEach(n=>{const o=document.createElement('option');"
            "o.value=n.ssid;o.textContent=n.ssid+' ('+n.rssi+' dBm)'+(n.secure?' [khoa]':'');e.appendChild(o)});"
            "st.textContent='Tim thay '+j.length+' mang.'}catch(e){st.textContent='Loi quet: '+e}}"
            "async function doSave(){const st=document.getElementById('st');"
            "const ssid=document.getElementById('sm').value||document.getElementById('s').value;"
            "const pass=document.getElementById('p').value;"
            "if(!ssid){st.textContent='Chon hoac nhap ten Wi-Fi.';return}"
            "st.textContent='Dang luu va kiem tra ket noi (co the mat toi 15 giay)...';"
            "try{const r=await fetch('/save',{method:'POST',headers:{'Content-Type':'application/x-www-form-urlencoded'},"
            "body:'ssid='+encodeURIComponent(ssid)+'&password='+encodeURIComponent(pass)});"
            "const t=await r.text();st.textContent=t;}catch(e){st.textContent='Loi luu: '+e}}"
            "async function doCancel(){await fetch('/cancel',{method:'POST'});"
            "document.getElementById('st').textContent='Da huy. Dang thoat che do cai dat...';}"
            "</script></body></html>");
  return page;
}
}  // namespace

void WifiSetupPortal::compute_ap_credentials() {
  // AP password derived per-device (not one fixed password shared across
  // the whole fleet -- see docs/WIFI_RECOVERY.md for why). Named after
  // device_id if provisioned, else hardware_id -- an UNPROVISIONED unit
  // (no device_id yet) must still be recoverable (§40).
  String device_id = identity_.device_id();
  String name = device_id.length() > 0 ? device_id : identity_.hardware_id();
  ap_ssid_ = "MESFlow-Setup-" + name;
  ap_password_ = "mesflow-" + name.substring(name.length() > 6 ? name.length() - 6 : 0);
  if (ap_password_.length() < 8) ap_password_ = ap_password_ + "-recovery";  // WPA2 minimum length
}

void WifiSetupPortal::start(const char* reason) {
  if (active()) return;

  compute_ap_credentials();

  WiFi.mode(WIFI_AP_STA);
  WiFi.softAP(ap_ssid_.c_str(), ap_password_.c_str());
  dns_.start(53, "*", WiFi.softAPIP());

  web_.on("/", HTTP_GET, [this]() { handle_root(); });
  web_.on("/scan", HTTP_GET, [this]() { handle_scan(); });
  web_.on("/save", HTTP_POST, [this]() { handle_save(); });
  web_.on("/cancel", HTTP_POST, [this]() { handle_cancel(); });
  web_.on("/generate_204", HTTP_ANY, [this]() { handle_captive_redirect(); });
  web_.on("/hotspot-detect.html", HTTP_ANY, [this]() { handle_captive_redirect(); });
  web_.on("/connecttest.txt", HTTP_ANY, [this]() { handle_captive_redirect(); });
  web_.onNotFound([this]() { handle_captive_redirect(); });
  web_.begin();

  touch_activity();
  set_state(WifiRecoveryState::AP_ACTIVE);

  kiosk::health::log_structured("INFO", "NET_WIFI_RECOVERY_START", "wifi_setup_portal", reason);
}

void WifiSetupPortal::stop(const char* reason) {
  if (!active()) return;
  web_.stop();
  dns_.stop();
  WiFi.softAPdisconnect(true);
  WiFi.mode(WIFI_STA);
  set_state(WifiRecoveryState::INACTIVE);
  kiosk::health::log_structured("INFO", "NET_WIFI_RECOVERY_STOP", "wifi_setup_portal", reason);
}

void WifiSetupPortal::poll() {
  if (!active()) return;

  dns_.processNextRequest();
  web_.handleClient();

  if (state_ == WifiRecoveryState::TESTING) {
    if (WiFi.status() == WL_CONNECTED) {
      // Success: candidate network works. Commit and reboot into it.
      config_.set_wifi_credentials(pending_ssid_, pending_password_);
      kiosk::health::log_structured("INFO", "NET_WIFI_RECOVERY_SUCCESS", "wifi_setup_portal",
                                     pending_ssid_.c_str());
      delay(300);
      ESP.restart();
      return;
    }
    if (millis() - testing_started_ms_ > WIFI_RECOVERY_TEST_TIMEOUT_MS) {
      last_error_ = "Khong ket noi duoc toi mang da chon. Vui long kiem tra lai mat khau.";
      kiosk::health::log_structured("WARN", "NET_WIFI_RECOVERY_TEST_FAILED", "wifi_setup_portal",
                                     pending_ssid_.c_str());
      // Roll back: previously stored credentials were never overwritten,
      // so just drop the failed STA attempt and stay on the AP.
      WiFi.disconnect();
      set_state(WifiRecoveryState::FAILED);
    }
    return;
  }

  if (millis() - last_activity_ms_ > WIFI_RECOVERY_PORTAL_TIMEOUT_MS) {
    stop("timeout");
  }
}

void WifiSetupPortal::handle_root() {
  touch_activity();
  web_.send(200, "text/html; charset=utf-8", setup_page(ap_ssid_));
}

void WifiSetupPortal::handle_scan() {
  touch_activity();
  int count = WiFi.scanNetworks(false, true);
  std::string body = "[";
  int emitted = 0;
  for (int i = 0; i < count && i < 30; ++i) {
    if (WiFi.SSID(i).length() == 0) continue;
    if (emitted > 0) body += ",";
    body += "{\"ssid\":\"" + kiosk::protocol::json_escape(WiFi.SSID(i).c_str()) + "\",";
    body += "\"rssi\":" + std::to_string(WiFi.RSSI(i)) + ",";
    body += std::string("\"secure\":") + (WiFi.encryptionType(i) != WIFI_AUTH_OPEN ? "true" : "false") + "}";
    ++emitted;
  }
  body += "]";
  WiFi.scanDelete();
  web_.send(200, "application/json", body.c_str());
}

void WifiSetupPortal::handle_save() {
  touch_activity();
  String ssid = web_.arg("ssid");
  String password = web_.arg("password");
  ssid.trim();

  if (ssid.length() == 0) {
    web_.send(400, "text/plain; charset=utf-8", "Loi: chua nhap ten Wi-Fi.");
    return;
  }

  pending_ssid_ = ssid;
  pending_password_ = password;

  // §24: do NOT touch stored credentials yet. Attempt the candidate network
  // while keeping the AP up (WIFI_AP_STA); only ConfigStore is written, and
  // only after WL_CONNECTED is observed, in poll().
  WiFi.begin(pending_ssid_.c_str(), pending_password_.c_str());
  testing_started_ms_ = millis();
  set_state(WifiRecoveryState::TESTING);

  kiosk::health::log_structured("INFO", "NET_WIFI_RECOVERY_TESTING", "wifi_setup_portal",
                                 pending_ssid_.c_str());
  web_.send(200, "text/plain; charset=utf-8",
            "Dang kiem tra ket noi toi '" + pending_ssid_ +
            "'... Thiet bi se tu khoi dong lai neu thanh cong. Neu that bai, ban se thay loi tai day sau khoang 15 giay.");
}

void WifiSetupPortal::handle_cancel() {
  touch_activity();
  web_.send(200, "text/plain; charset=utf-8", "Da huy. Dang thoat...");
  delay(200);
  stop("operator_cancel");
}

void WifiSetupPortal::handle_captive_redirect() {
  touch_activity();
  web_.sendHeader("Location", "http://192.168.4.1/", true);
  web_.send(302, "text/plain", "");
}

void WifiSetupPortal::set_state(WifiRecoveryState new_state) {
  if (new_state == state_) return;
  state_ = new_state;
  kiosk::runtime::LocalEvent event;
  event.kind = kiosk::runtime::LocalEventKind::WIFI_RECOVERY_STATE;
  event.text = state_name(new_state);
  event.timestamp_ms = millis();
  bus_.publish(event);
}

}  // namespace kiosk::network
