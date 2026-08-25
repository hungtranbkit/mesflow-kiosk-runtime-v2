#include "wifi_setup_portal.h"

#include <WiFi.h>

#include <string>

#include "../config/runtime_config.h"
#include "../health/structured_log.h"
#include "../protocol/ap_ssid.h"
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
             "<title>MESFlow Kiosk - Cài đặt Wi-Fi</title><style>"
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
             "<h1>Cài đặt Wi-Fi cho Kiosk</h1><div class='sub'>AP: ");
  page += ssid;
  page += F(" (không mật khẩu) &middot; 192.168.4.1</div>"
            "<button type='button' onclick='doScan()'>Quét Wi-Fi gần đây</button>"
            "<div class='sub' style='margin:6px 0 0'>Lưu ý: quá trình quét có thể làm mất kết nối "
            "vài giây (giới hạn phần cứng một ăng-ten của ESP32) -- nếu bị rớt, đợi vài giây rồi nối "
            "lại mạng này và thử lại.</div>"
            "<label>Tên Wi-Fi (SSID)</label><select id='s'><option value=''>-- chọn hoặc gõ tay --</option></select>"
            "<input id='sm' placeholder='hoặc nhập SSID thủ công' style='margin-top:6px'>"
            "<label>Mật khẩu Wi-Fi</label><input id='p' type='password' autocomplete='new-password'>"
            "<button onclick='doSave()'>Lưu và kiểm tra kết nối</button>"
            "<button class='cancel' onclick='doCancel()'>Hủy, thoát cài đặt</button>"
            "<div id='st' class='status'>Sẵn sàng.</div></div><script>"
            "async function doScan(){const st=document.getElementById('st');st.textContent='Đang quét...';"
            "try{const r=await fetch('/scan');const j=await r.json();const e=document.getElementById('s');"
            "e.innerHTML='<option value=\"\">-- chon --</option>';j.forEach(n=>{const o=document.createElement('option');"
            "o.value=n.ssid;o.textContent=n.ssid+' ('+n.rssi+' dBm)'+(n.secure?' [khóa]':'');e.appendChild(o)});"
            "st.textContent='Tìm thấy '+j.length+' mạng.'}catch(e){st.textContent='Lỗi quét: '+e}}"
            "async function doSave(){const st=document.getElementById('st');"
            "const ssid=document.getElementById('sm').value||document.getElementById('s').value;"
            "const pass=document.getElementById('p').value;"
            "if(!ssid){st.textContent='Chọn hoặc nhập tên Wi-Fi.';return}"
            "st.textContent='Đang lưu và kiểm tra kết nối (có thể mất tới 15 giây)...';"
            "try{const r=await fetch('/save',{method:'POST',headers:{'Content-Type':'application/x-www-form-urlencoded'},"
            "body:'ssid='+encodeURIComponent(ssid)+'&password='+encodeURIComponent(pass)});"
            "const t=await r.text();st.textContent=t;}catch(e){st.textContent='Lỗi lưu: '+e}}"
            "async function doCancel(){await fetch('/cancel',{method:'POST'});"
            "document.getElementById('st').textContent='Đã hủy. Đang thoát chế độ cài đặt...';}"
            "</script></body></html>");
  return page;
}
}  // namespace

void WifiSetupPortal::compute_ap_credentials() {
  // AP SSID derived per-device (a stable "MesflowKiosk-XXXX" suffix, see
  // src/protocol/ap_ssid.h -- pure/host-tested). Named after device_id if
  // provisioned, else hardware_id -- an UNPROVISIONED unit (no device_id
  // yet) must still be recoverable (§40).
  //
  // 2026-08-24 open-AP rework: this AP is now deliberately OPEN (no
  // password) -- see docs/WIFI_RECOVERY.md's security note for the full
  // reasoning. A per-device WPA2 password used to be generated here; it
  // made initial setup unnecessarily hard (an operator had to already know
  // a secret to open the very portal whose job is fixing connectivity) for
  // a trade-off that only mattered for the short, operator-supervised
  // window this AP is actually up (no stored credentials / a deliberate
  // 10s '*' hold / explicit recovery mode -- never permanently on). Kept as
  // an empty String (rather than removing the field) so `ap_password()`
  // still has a well-defined, honest answer ("") for any caller that asks.
  String device_id = identity_.device_id();
  String name = device_id.length() > 0 ? device_id : identity_.hardware_id();
  ap_ssid_ = kiosk::protocol::compute_setup_ap_ssid(name.c_str()).c_str();
  ap_password_ = "";
}

void WifiSetupPortal::start(const char* reason) {
  if (active()) return;

  compute_ap_credentials();

  WiFi.mode(WIFI_AP_STA);
  // No password argument -- WiFi.softAP(ssid) with no passphrase (or an
  // explicit nullptr/"") starts an OPEN AP. See compute_ap_credentials()'s
  // comment and docs/WIFI_RECOVERY.md for why that's intentional here.
  WiFi.softAP(ap_ssid_.c_str());
  dns_.start(53, "*", WiFi.softAPIP());

  web_.on("/", HTTP_GET, [this]() { handle_root(); });
  web_.on("/scan", HTTP_GET, [this]() { handle_scan(); });
  web_.on("/save", HTTP_POST, [this]() { handle_save(); });
  web_.on("/cancel", HTTP_POST, [this]() { handle_cancel(); });
  // Standard captive-portal probe paths across the major client OSes, so
  // joining the (now visibly password-less) AP pops the setup page
  // automatically on most phones instead of requiring the operator to know
  // to open a browser to 192.168.4.1 themselves (§7 of the 2026-08-24
  // open-AP rework: "keep/improve the captive portal behavior").
  web_.on("/generate_204", HTTP_ANY, [this]() { handle_captive_redirect(); });        // Android
  web_.on("/gen_204", HTTP_ANY, [this]() { handle_captive_redirect(); });             // Android (older)
  web_.on("/hotspot-detect.html", HTTP_ANY, [this]() { handle_captive_redirect(); }); // Apple
  web_.on("/library/test/success.html", HTTP_ANY, [this]() { handle_captive_redirect(); });  // Apple (older iOS)
  web_.on("/connecttest.txt", HTTP_ANY, [this]() { handle_captive_redirect(); });     // Windows NCSI
  web_.on("/ncsi.txt", HTTP_ANY, [this]() { handle_captive_redirect(); });            // Windows NCSI (legacy)
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
      last_error_ = "Không kết nối được tới mạng đã chọn. Vui lòng kiểm tra lại mật khẩu.";
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

  // Real bug found + reproduced live (2026-08-24): ESP32 has a single
  // 2.4GHz radio shared between the AP and STA (scan) roles. The old call
  // -- WiFi.scanNetworks(false, true), i.e. synchronous with the library's
  // 300ms-per-channel default -- makes the radio leave the AP's channel for
  // up to ~300ms at a time, once per channel (up to ~13), a multi-second
  // stretch during which the AP genuinely cannot send beacons at all. A
  // client that's already joined (confirmed with a laptop: found the AP at
  // 100% signal, then failed to associate moments later, reproducibly,
  // right after a scan had run) reads that as the AP disappearing. This
  // can't be eliminated -- it's a real single-radio hardware limit, not a
  // logic bug -- but it CAN be shrunk a lot:
  //   - max_ms_per_chan 120 instead of the 300 default cuts the total scan
  //     window by more than half.
  //   - async (true) + polling scanComplete() here (rather than the
  //     library's own fully-blocking wait) means dns_.processNextRequest()
  //     keeps running during the wait, so captive-portal probes from OTHER
  //     devices (and this AP's own DNS responsiveness generally) aren't
  //     ALSO frozen for the whole scan on top of the radio-level gap.
  int status = WiFi.scanNetworks(true, true, false, 120);
  int16_t count = static_cast<int16_t>(status);
  if (status == WIFI_SCAN_RUNNING || status >= 0) {
    unsigned long deadline = millis() + 6000;  // generous cap even at the reduced dwell time
    while ((count = WiFi.scanComplete()) == WIFI_SCAN_RUNNING && millis() < deadline) {
      dns_.processNextRequest();
      delay(20);
    }
  }

  if (count < 0) {
    // WIFI_SCAN_FAILED (-2) or our own deadline above -- log it as a real
    // failure, not silently reported as "found 0 networks" (the previous
    // code's `for (int i = 0; i < count...)` loop just happened to also
    // produce an empty array for a negative count, but for the wrong
    // reason -- indistinguishable from a genuine empty scan, which made
    // this exact bug harder to diagnose).
    kiosk::health::log_structured("WARN", "NET_WIFI_SCAN_FAILED", "wifi_setup_portal",
                                   (std::string("scanComplete()=") + std::to_string(count)).c_str());
    count = 0;
  }

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
    web_.send(400, "text/plain; charset=utf-8", "Lỗi: chưa nhập tên Wi-Fi.");
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
            "Đang kiểm tra kết nối tới '" + pending_ssid_ +
            "'... Thiết bị sẽ tự khởi động lại nếu thành công. Nếu thất bại, bạn sẽ thấy lỗi tại đây sau khoảng 15 giây.");
}

void WifiSetupPortal::handle_cancel() {
  touch_activity();
  web_.send(200, "text/plain; charset=utf-8", "Đã hủy. Đang thoát...");
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
