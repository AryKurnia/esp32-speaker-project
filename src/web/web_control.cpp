#include "web_control.h"

#include <DNSServer.h>
#include <WebServer.h>
#include <WiFi.h>
#include <esp_attr.h>
#include <esp_system.h>

#include "web_page.h"

namespace web_control {
namespace {

constexpr uint8_t kButtonPin = 0;  // Tombol BOOT
constexpr uint32_t kButtonDebounceMs = 30;
constexpr uint32_t kButtonHoldMs = 2000;
constexpr uint32_t kExitCountdownMs = 7000;
constexpr uint32_t kIdleTimeoutMs = 5UL * 60UL * 1000UL;
constexpr uint32_t kConfigMagic = 0xC0F16A55;
constexpr char kSsid[] = "OkeGass-Setup";

// Bertahan saat restart software, hilang saat listrik dicabut. Jadi cabut-pasang
// listrik selalu kembali ke mode Bluetooth.
RTC_NOINIT_ATTR uint32_t boot_request;

WebServer server(80);
DNSServer dns;
CommandHandler command_handler = nullptr;
StatusProvider status_provider = nullptr;
RestartHook restart_hook = nullptr;
bool is_config = false;
bool counting_down = false;
uint32_t countdown_start = 0;
uint32_t last_activity = 0;

bool button_raw = false;
bool button_down = false;
bool button_handled = false;
uint32_t button_changed_at = 0;
uint32_t button_down_since = 0;

void restart(bool into_config) {
  Serial.printf("[Mode] Restart ke mode %s\n", into_config ? "setting" : "Bluetooth");
  if (restart_hook != nullptr) {
    restart_hook();
  }
  boot_request = into_config ? kConfigMagic : 0;
  delay(100);  // Beri waktu Serial dan respons HTTP terakhir terkirim
  ESP.restart();
}

void sendStatus() {
  // Polling halaman tidak dihitung sebagai aktivitas pengguna.
  if (!server.hasArg("poll")) {
    last_activity = millis();
  }
  server.send(200, "application/json", status_provider());
}

// Captive portal: semua nama domain dijawab dengan IP speaker, dan setiap
// alamat yang bukan milik halaman setting dialihkan ke sana. Pengecekan internet
// dari Android, iPhone, dan Windows ikut dialihkan, sehingga HP membuka halaman
// setting sendiri. Pengalihan tidak dihitung sebagai aktivitas pengguna.
String portalUrl() {
  return "http://" + WiFi.softAPIP().toString() + "/";
}

void redirectToPortal() {
  server.sendHeader("Location", portalUrl(), true);
  server.sendHeader("Cache-Control", "no-store");
  server.send(302, "text/plain", "");
}

void handlePage() {
  if (server.hostHeader() != WiFi.softAPIP().toString()) {
    redirectToPortal();
    return;
  }
  last_activity = millis();
  server.sendHeader("Cache-Control", "no-store");
  server.send_P(200, "text/html", kWebPage);
}

void handleCommand() {
  if (!server.hasArg("c")) {
    server.send(400, "text/plain", "missing command");
    return;
  }
  command_handler(server.arg("c"));
  sendStatus();
}

void startAccessPoint() {
  WiFi.persistent(false);
  WiFi.mode(WIFI_AP);
  if (!WiFi.softAP(kSsid)) {
    Serial.println("[WiFi] Gagal menyalakan AP, kembali ke Bluetooth");
    restart(false);
  }
  dns.setErrorReplyCode(DNSReplyCode::NoError);
  dns.start(53, "*", WiFi.softAPIP());
  server.begin();
  last_activity = millis();
  Serial.printf("[WiFi] AP %s aktif: http://%s, free heap: %u\n", kSsid,
                WiFi.softAPIP().toString().c_str(), ESP.getFreeHeap());
}

void onButtonPressed() {
  // Tekan sekali saat hitung mundur = batal.
  if (counting_down) {
    cancel_exit_countdown();
    button_handled = true;
  }
}

void onButtonHeld() {
  if (is_config) {
    start_exit_countdown();
  } else {
    request_config_mode();
  }
}

void pollButton() {
  const bool raw = digitalRead(kButtonPin) == LOW;
  const uint32_t now = millis();
  if (raw != button_raw) {
    button_raw = raw;
    button_changed_at = now;
  }
  if (now - button_changed_at < kButtonDebounceMs) {
    return;
  }

  if (!raw) {
    button_down = false;
    button_handled = false;
    return;
  }
  if (!button_down) {
    button_down = true;
    button_down_since = now;
    onButtonPressed();
  }
  if (!button_handled && now - button_down_since >= kButtonHoldMs) {
    button_handled = true;
    onButtonHeld();
  }
}

}  // namespace

bool boot_into_config_mode() {
  const bool requested =
      boot_request == kConfigMagic && esp_reset_reason() == ESP_RST_SW;
  boot_request = 0;  // Crash atau restart berikutnya kembali ke mode Bluetooth
  return requested;
}

void begin(bool config_mode, CommandHandler on_command,
           StatusProvider status_json, RestartHook before_restart) {
  is_config = config_mode;
  command_handler = on_command;
  status_provider = status_json;
  restart_hook = before_restart;
  pinMode(kButtonPin, INPUT_PULLUP);
  if (!is_config) {
    return;
  }

  server.on("/", HTTP_GET, handlePage);
  server.on("/status", HTTP_GET, sendStatus);
  server.on("/cmd", HTTP_POST, handleCommand);
  server.onNotFound(redirectToPortal);
  startAccessPoint();
}

void loop() {
  pollButton();
  if (!is_config) {
    return;
  }

  dns.processNextRequest();
  server.handleClient();
  if (counting_down) {
    if (exit_countdown_ms() == 0) {
      restart(false);
    }
  } else if (millis() - last_activity >= kIdleTimeoutMs) {
    Serial.println("[Mode] 5 menit tanpa aktivitas");
    restart(false);
  }
}

bool config_mode() {
  return is_config;
}

void request_config_mode() {
  if (!is_config) {
    restart(true);
  }
}

void start_exit_countdown() {
  if (!is_config || counting_down) {
    return;
  }
  counting_down = true;
  countdown_start = millis();
  Serial.printf("[Mode] Kembali ke Bluetooth dalam %u detik\n",
                (unsigned)(kExitCountdownMs / 1000));
}

void cancel_exit_countdown() {
  if (!counting_down) {
    return;
  }
  counting_down = false;
  last_activity = millis();
  Serial.println("[Mode] Hitung mundur dibatalkan");
}

int32_t exit_countdown_ms() {
  if (!counting_down) {
    return -1;
  }
  const uint32_t elapsed = millis() - countdown_start;
  return elapsed >= kExitCountdownMs ? 0 : (int32_t)(kExitCountdownMs - elapsed);
}

}  // namespace web_control
