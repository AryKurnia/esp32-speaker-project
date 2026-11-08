#pragma once

#include <Arduino.h>

// Mode setting: ESP32 restart tanpa Bluetooth, menyalakan WiFi Access Point, dan
// melayani halaman setting. Keluar lewat hitung mundur yang bisa dibatalkan.
namespace web_control {

using CommandHandler = void (*)(const String &command);
using StatusProvider = String (*)();
using RestartHook = void (*)();

// Panggil paling awal di setup(): true kalau ESP32 baru restart ke mode setting.
bool boot_into_config_mode();

void begin(bool config_mode, CommandHandler on_command,
           StatusProvider status_json, RestartHook before_restart);
void loop();
bool config_mode();

void request_config_mode();     // Restart ke mode setting
void start_exit_countdown();    // Mulai hitung mundur kembali ke Bluetooth
void cancel_exit_countdown();
int32_t exit_countdown_ms();    // -1 kalau tidak sedang hitung mundur

}  // namespace web_control
