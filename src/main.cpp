// IMPORT IMPORTANT LIBRARY
#include <Arduino.h>
#include "LittleFS.h"
#include "BluetoothA2DPSink.h"
#include "driver/i2s.h"
#include <Preferences.h>
#include <U8g2lib.h>
#include <Wire.h>
#include <stdlib.h>
#include <atomic>
#include "dsp/crossover.h"
#include "dsp/dsp_config.h"

// Feature switches: set to 1 to enable the peripheral.
#define FEATURE_OLED_ENABLED 0
#define FEATURE_ROTARY_ENCODER_ENABLED 0

// GPIO assignments
#define BTLED 2
#define ENC_CLK 32
#define ENC_DT  33
#define ENC_SW  27
// #define OLED_SCL_PIN 19
#define OLED_SDA_PIN 21
#define I2S_BCK_PIN 25
#define I2S_WS_PIN 26
#define I2S_DATA_OUT_PIN 19

// DAC and amplifier control pins
#define XSMT_PIN 23  // GPIO23 → XSMT PCM5102A
#define PAM_M_PIN 17 // GPIO17 -> PAM MUTE

void dac_mute() {
  digitalWrite(XSMT_PIN, LOW);
  digitalWrite(PAM_M_PIN, HIGH);
  Serial.println("[DAC] Muted");
}

void dac_unmute() {
  digitalWrite(XSMT_PIN, HIGH);
  digitalWrite(PAM_M_PIN, LOW);
  Serial.println("[DAC] Unmuted");
}

BluetoothA2DPSink a2dp_sink;
Preferences prefs;

// ─── I2S PIN CONFIG ───────────────────────────────────────────
i2s_pin_config_t pin_config = {
    .mck_io_num = I2S_PIN_NO_CHANGE,
    .bck_io_num = I2S_BCK_PIN,
    .ws_io_num = I2S_WS_PIN,
    .data_out_num = I2S_DATA_OUT_PIN,
    .data_in_num = I2S_PIN_NO_CHANGE};

static inline void set_i2s_stereo_frame(int16_t *frame, int16_t left, int16_t right)
{
  frame[0] = left;
  frame[1] = right;
}

// ─── I2S DRIVER CONFIG ────────────────────────────────────────
i2s_config_t i2s_config_stereo = {
    .mode = (i2s_mode_t)(I2S_MODE_MASTER | I2S_MODE_TX),
    .sample_rate = (int)dsp::kDefaultSampleRate,
    .bits_per_sample = I2S_BITS_PER_SAMPLE_16BIT,
    .channel_format = I2S_CHANNEL_FMT_RIGHT_LEFT,
    .communication_format = (i2s_comm_format_t)(I2S_COMM_FORMAT_STAND_I2S),
    .intr_alloc_flags = 0,
    .dma_buf_count = 16,
    .dma_buf_len = 512,
    .use_apll = false,
    .tx_desc_auto_clear = true,
    .fixed_mclk = 0};

// Independent filter state prevents WAV chimes from disturbing BT playback.
float crossover_freq = 5000.0f;
float bass_shelf_freq = dsp::kDefaultBassShelfFrequencyHz;
float bass_shelf_gain_db = dsp::kDefaultBassShelfGainDb;
float bass_high_pass_freq = dsp::kDefaultBassHighPassHz;
float punch_freq = dsp::kDefaultPunchFrequencyHz;
float punch_gain_db = dsp::kDefaultPunchGainDb;
dsp::Crossover stream_crossover;
dsp::Crossover chime_crossover;

// Gain woofer & tweeter (1.0 = normal)
float gain_woofer = 1.0f;
float gain_tweeter = 1.0f;

bool mono_mode = true; // true = mono mix, false = stereo passthrough (LPF ke L, HPF ke R)

// ─── UI STATE MACHINE ─────────────────────────────────────────
enum UIState  { IDLE, NAVIGATE, EDIT };
enum UIParam {
  PARAM_FREQ,
  PARAM_GAIN_W,
  PARAM_GAIN_T,
  PARAM_WAV_VOL,
  PARAM_MODE,
  PARAM_BASS_FREQ,
  PARAM_BASS_DB,
  PARAM_COUNT
};
enum UIPage {
  PAGE_NOWPLAY,
  PAGE_CONFIG,
  PAGE_CONTROL
};

enum ControlAction {
  CTRL_PREV,
  CTRL_PAUSE,
  CTRL_PLAY,
  CTRL_NEXT,
  CTRL_COUNT
};

UIState  ui_state       = IDLE;
UIParam  selected_param = PARAM_FREQ;
UIPage current_page = PAGE_NOWPLAY;
ControlAction selected_action = CTRL_PLAY;

unsigned long last_interaction = 0;
const unsigned long TIMEOUT_MS = 3000;

// Untuk efek kedip
bool     blink_state    = false;
unsigned long last_blink = 0;
const unsigned long BLINK_FAST = 200; // ms — mode EDIT
const unsigned long BLINK_SLOW = 600; // ms — mode NAVIGATE

// Timeout PAGE_CONTROL
const unsigned long CONTROL_TIMEOUT_MS = 5000;
unsigned long last_control_interaction = 0;
// ==============================================================

// ─── TRACK INFO ───────────────────────────────────────────────
struct TrackInfo {
    char title[64];
    char artist[64];
    uint32_t duration_ms;
    uint32_t position_ms;
    bool has_data;
};

TrackInfo track_info = {"", "", 0, 0, false};

// ─── SCROLL CONFIG ────────────────────────────────────────────
int scroll_x_title  = 0;
int scroll_x_artist = 0;
unsigned long last_scroll = 0;
const int SCROLL_SPEED_MS = 80;
bool scroll_pausing = false;
unsigned long scroll_pause_start = 0;
const int SCROLL_PAUSE_MS = 2000;
// ==============================================================

// Update both independent filter paths with the same settings.
bool update_crossover(float freq_hz, float shelf_freq_hz, float shelf_gain_db) {
  if (!stream_crossover.configure(dsp::kDefaultSampleRate, freq_hz,
                                  shelf_freq_hz, shelf_gain_db) ||
      !chime_crossover.configure(dsp::kDefaultSampleRate, freq_hz,
                                 shelf_freq_hz, shelf_gain_db)) {
    return false;
  }
  return true;
}

bool update_punch(float high_pass_hz, float frequency_hz, float gain_db) {
  return stream_crossover.configurePunch(dsp::kDefaultSampleRate, high_pass_hz,
                                         frequency_hz, gain_db) &&
         chime_crossover.configurePunch(dsp::kDefaultSampleRate, high_pass_hz,
                                        frequency_hz, gain_db);
}

// Preset bass: mengisi semua parameter sekaligus.
struct BassPreset {
  const char *name;
  float high_pass_hz;
  float punch_db;
  float shelf_db;
};
static const BassPreset BASS_PRESETS[] = {
    {"punch", dsp::kDefaultBassHighPassHz, dsp::kDefaultPunchGainDb, dsp::kDefaultBassShelfGainDb},
    {"soft", 45.0f, 2.0f, 1.0f},
    {"flat", dsp::kMinBassHighPassHz, 0.0f, 0.0f},
};

bool apply_bass_preset(const String &name) {
  for (const BassPreset &preset : BASS_PRESETS) {
    if (name != preset.name) {
      continue;
    }
    bass_high_pass_freq = preset.high_pass_hz;
    punch_freq = dsp::kDefaultPunchFrequencyHz;
    punch_gain_db = preset.punch_db;
    bass_shelf_freq = dsp::kDefaultBassShelfFrequencyHz;
    bass_shelf_gain_db = preset.shelf_db;
    return update_crossover(crossover_freq, bass_shelf_freq, bass_shelf_gain_db) &&
           update_punch(bass_high_pass_freq, punch_freq, punch_gain_db);
  }
  return false;
}

// ─── LITTLEFS / WAV CHIME ─────────────────────────────────────
// Chime dan stream BT memakai I2S_NUM_0 yang sama. Selama wav_pending > 0 audio
// BT dibuang, jadi hanya satu sumber yang menulis ke I2S. Semua chime diputar
// bergantian oleh satu task lewat wav_queue.
struct WavRequest {
  char path[32];
};
static QueueHandle_t wav_queue = nullptr;
static std::atomic<int> wav_pending{0};
float wav_volume = 0.7f;
bool was_connected = false;
bool reconnect_window_open = true;

bool is_startup = true;  // true sampai window reconnect selesai
bool ever_connected = false; // true kalau pernah konek sejak startup

static void play_wav_file(const char *path)
{
  File file = LittleFS.open(path, "r");
  if (file)
  {
    file.seek(44); // skip WAV header

    const size_t READ_SAMPLES = 128;
    uint8_t  raw[READ_SAMPLES * 2];       // 256 bytes
    static float   fbuf[READ_SAMPLES];
    static int16_t out[READ_SAMPLES * 2]; // stereo output

    size_t bytes_written;

    while (file.available())
    {
      size_t bytes_read  = file.read(raw, sizeof(raw));
        // Dibagi 4 karena 1 sampel stereo = 4 byte (16-bit L + 16-bit R)
        size_t num_samples = bytes_read / 4; 

        // Convert ke float + apply volume (Mix Stereo ke Mono)
        for (size_t i = 0; i < num_samples; i++) {
          int16_t left  = (raw[i*4+1] << 8) | raw[i*4];
          int16_t right = (raw[i*4+3] << 8) | raw[i*4+2];
          
          // Gabungkan kiri dan kanan lalu bagi 2 untuk dijadikan mono
          fbuf[i] = (float)((left + right) / 2.0f) * wav_volume; 
        }

      if (mono_mode) {
        for (size_t i = 0; i < num_samples; i++) {
          float woofer, tweeter;
          chime_crossover.processSample(fbuf[i], gain_woofer, gain_tweeter,
                                        woofer, tweeter);
          const int16_t left = (int16_t)constrain(woofer, -32768.0f, 32767.0f);
          const int16_t right = (int16_t)constrain(tweeter, -32768.0f, 32767.0f);
          set_i2s_stereo_frame(&out[i * 2], left, right);
        }
      } else {
        // Stereo passthrough
        for (size_t i = 0; i < num_samples; i++) {
          const int16_t sample = (int16_t)constrain(fbuf[i], -32768, 32767);
          set_i2s_stereo_frame(&out[i * 2], sample, sample);
        }
      }

      i2s_write(I2S_NUM_0, out, num_samples * 4, &bytes_written, portMAX_DELAY);
    }
    file.close();
  }
  else
  {
    Serial.printf("Gagal buka: %s\n", path);
  }
}

void wav_task(void *param)
{
  WavRequest request;
  for (;;)
  {
    xQueueReceive(wav_queue, &request, portMAX_DELAY);
    dac_unmute(); // ← unmute sebelum play
    play_wav_file(request.path);

    // Mute kembali setelah antrean WAV habis
    // Hanya mute kalau BT tidak sedang tersambung
    if (wav_pending.fetch_sub(1) == 1 && !a2dp_sink.is_connected()) {
      dac_mute();
    }
  }
}

void play_wav(const char *filename)
{
  WavRequest request;
  strlcpy(request.path, filename, sizeof(request.path));

  // Naikkan sebelum masuk antrean supaya audio BT langsung berhenti.
  wav_pending++;
  if (xQueueSend(wav_queue, &request, 0) != pdTRUE) {
    wav_pending--;
    Serial.printf("Antrean WAV penuh: %s\n", filename);
  }
}

static void wait_for_wav()
{
  while (wav_pending.load() > 0) {
    vTaskDelay(pdMS_TO_TICKS(10));
  }
}

// Bris program untuk LCD
// SSD1306 128x32 I2C
// Parameter: rotasi, reset pin, SCL, SDA
#if FEATURE_OLED_ENABLED
U8G2_SSD1306_128X32_UNIVISION_F_HW_I2C u8g2(
  U8G2_R2,        // [U8G2_R2](rotasi 180°)||[U8G2_R0](rotasi normal)
  U8X8_PIN_NONE,  // tidak pakai pin reset
  OLED_SCL_PIN,
  OLED_SDA_PIN
);
#endif

// Format millisecond ke MM:SS
void format_time(char *buf, uint32_t ms)
{
  uint32_t total_sec = ms / 1000;
  uint32_t min = total_sec / 60;
  uint32_t sec = total_sec % 60;
  snprintf(buf, 8, "%02d:%02d", min, sec);
}

// Scroll teks otomatis
#if FEATURE_OLED_ENABLED
int get_scroll_offset(const char *text, int max_width, int &scroll_x)
{
  int text_width = u8g2.getStrWidth(text);
  if (text_width <= max_width)
    return 0; // tidak perlu scroll

  unsigned long now = millis();

  if (scroll_pausing)
  {
    if (now - scroll_pause_start > SCROLL_PAUSE_MS)
    {
      scroll_pausing = false;
    }
    return scroll_x;
  }

  if (now - last_scroll > SCROLL_SPEED_MS)
  {
    last_scroll = now;
    scroll_x++;
    if (scroll_x > text_width - max_width + 10)
    {
      scroll_x = 0;
      scroll_pausing = true;
      scroll_pause_start = now;
    }
  }
  return scroll_x;
}
#endif

// Panggil fungsi ini setiap kali ada perubahan status
// Label parameter
const char* param_labels[] = {
    "Freq", "GainW", "GainT", "WVol", "Mode", "BassHz", "BassDB"};

void update_display()
{
#if FEATURE_OLED_ENABLED
  u8g2.clearBuffer();
  u8g2.setFont(u8g2_font_6x10_tr);

  if (current_page == PAGE_CONFIG)
  {
    // ── Kode PAGE_CONFIG yang sudah ada ──
    if (ui_state == IDLE)
    {
      u8g2.drawStr(0, 10, a2dp_sink.is_connected() ? "BT: Connected" : "BT: Waiting...");
      char line2[32];
      snprintf(line2, sizeof(line2), "Freq:%dHz %s",
               (int)crossover_freq, mono_mode ? "XO" : "ST");
      u8g2.drawStr(0, 22, line2);
      char line3[32];
      snprintf(line3, sizeof(line3), "Bass:%+.1fdB@%.0fHz",
               bass_shelf_gain_db, bass_shelf_freq);
      u8g2.drawStr(0, 32, line3);
    }
    else
    {
      // NAVIGATE / EDIT — kode yang sudah ada
      const char *param_names[PARAM_COUNT] = {
          "Freq", "Gain W", "Gain T", "WAV Vol", "Mode", "Bass Hz", "Bass dB"};
      char param_values[PARAM_COUNT][16];
      snprintf(param_values[PARAM_FREQ], sizeof(param_values[0]), "%dHz", (int)crossover_freq);
      snprintf(param_values[PARAM_GAIN_W], sizeof(param_values[1]), "%.1f", gain_woofer);
      snprintf(param_values[PARAM_GAIN_T], sizeof(param_values[2]), "%.1f", gain_tweeter);
      snprintf(param_values[PARAM_WAV_VOL], sizeof(param_values[3]), "%d%%", (int)(wav_volume * 100));
      snprintf(param_values[PARAM_MODE], sizeof(param_values[4]), mono_mode ? "XO" : "ST");
      snprintf(param_values[PARAM_BASS_FREQ], sizeof(param_values[PARAM_BASS_FREQ]),
               "%.0fHz", bass_shelf_freq);
      snprintf(param_values[PARAM_BASS_DB], sizeof(param_values[PARAM_BASS_DB]),
               "%.1fdB", bass_shelf_gain_db);

      int start = (selected_param > 1) ? selected_param - 1 : 0;
      if (start + 3 > PARAM_COUNT)
        start = PARAM_COUNT - 3;

      for (int i = 0; i < 3; i++)
      {
        int idx = start + i;
        if (idx >= PARAM_COUNT)
          break;
        char line[32];
        snprintf(line, sizeof(line), "%s: %s", param_names[idx], param_values[idx]);
        bool is_selected = (idx == selected_param);
        if (is_selected && blink_state)
        {
          u8g2.setDrawColor(1);
          u8g2.drawBox(0, (i * 11), 128, 11);
          u8g2.setDrawColor(0);
          u8g2.drawStr(2, (i * 11) + 9, line);
          u8g2.setDrawColor(1);
        }
        else
        {
          u8g2.drawStr(2, (i * 11) + 9, line);
        }
      }
    }
  }
  else if (current_page == PAGE_NOWPLAY)
  {
    if (is_startup) {
      // Start Up
      u8g2.drawStr(30, 18, "Start Up");
    }
    else if (!a2dp_sink.is_connected()){
      if (!ever_connected)
      {
        u8g2.drawStr(30, 18, "Reconnect...");
      }
      else if (ever_connected)
      {
        u8g2.drawStr(30, 18, "Waiting...");
      }
    }
    else if (!a2dp_sink.is_connected() || !track_info.has_data)
    {
      // Tidak ada musik
      u8g2.drawStr(30, 18, "No Music");
    }
    else
    {
      // Baris 1 — Judul lagu (scroll)
      u8g2.setClipWindow(0, 0, 127, 12);
      int ox = get_scroll_offset(track_info.title, 128, scroll_x_title);
      u8g2.drawStr(-ox, 10, track_info.title);
      u8g2.setMaxClipWindow();

      // Baris 2 — Artist
      u8g2.setClipWindow(0, 12, 127, 22);
      int oa = get_scroll_offset(track_info.artist, 128, scroll_x_artist);
      u8g2.drawStr(-oa, 22, track_info.artist);
      u8g2.setMaxClipWindow();

      // Baris 3 — Progress bar
      char time_cur[8], time_dur[8];
      format_time(time_cur, track_info.position_ms);
      format_time(time_dur, track_info.duration_ms);

      u8g2.drawStr(0, 32, time_cur);
      u8g2.drawStr(100, 32, time_dur);

      // Progress bar
      int bar_x = 30, bar_y = 25;
      int bar_w = 68, bar_h = 6;
      u8g2.drawFrame(bar_x, bar_y, bar_w, bar_h);

      if (track_info.duration_ms > 0)
      {
        int fill = (int)((float)track_info.position_ms / track_info.duration_ms * (bar_w - 2));
        if (fill > 0)
          u8g2.drawBox(bar_x + 1, bar_y + 1, fill, bar_h - 2);
      }
    }
  }
  else if (current_page == PAGE_CONTROL)
  {
    // Icon kontrol
    const char *icons[CTRL_COUNT] = {"<<", "||", ">", ">>"};
    
    // PERBAIKAN: Kita ubah array ini menjadi titik X awal MULAINYA KOTAK, bukan teks.
    // Jaraknya kita buat merata: 4, 36, 68, 100 (selisih 32 pixel tiap kotak)
    const int box_x[CTRL_COUNT] = {4, 36, 68, 100}; 
    
    const int box_y = 8;
    const int box_w = 24;
    const int box_h = 16;

    for (int i = 0; i < CTRL_COUNT; i++)
    {
      // 1. Ambil lebar teks secara dinamis (berapapun jumlah karakternya)
      int text_w = u8g2.getStrWidth(icons[i]);
      
      // 2. Rumus X Tengah = Titik_X_Kotak + (Setengah_Lebar_Kotak) - (Setengah_Lebar_Teks)
      int text_x = box_x[i] + (box_w / 2) - (text_w / 2);
      
      // 3. Rumus Y Tengah (Baseline) = Titik_Y_Kotak + (Setengah_Tinggi_Kotak) + Penyesuaian
      // Angka 3 di bawah adalah offset manual agar baseline font 6x10 pas di tengah vertikal.
      int text_y = box_y + (box_h / 2) + 3; 

      if (i == selected_action)
      {
        // Highlight
        u8g2.drawBox(box_x[i], box_y, box_w, box_h);
        
        u8g2.setDrawColor(0); // Ubah teks jadi warna hitam (kebalikan)
        u8g2.drawStr(text_x, text_y, icons[i]);
        u8g2.setDrawColor(1); // Kembalikan ke warna normal
      }
      else
      {
        u8g2.drawStr(text_x, text_y, icons[i]);
      }
    }
  }

  u8g2.sendBuffer();
#endif
}
// ============

// =================Fungsi simpan & load settings================
void save_settings()
{
  prefs.begin("speaker", false); // namespace "speaker", mode read-write
  prefs.putFloat("mono", mono_mode);
  prefs.putFloat("freq", crossover_freq);
  prefs.putFloat("gain_w", gain_woofer);
  prefs.putFloat("gain_t", gain_tweeter);
  prefs.putFloat("wav_vol", wav_volume);
  prefs.putFloat("bass_hz", bass_shelf_freq);
  prefs.putFloat("bass_db", bass_shelf_gain_db);
  prefs.putFloat("hpf_hz", bass_high_pass_freq);
  prefs.putFloat("punch_hz", punch_freq);
  prefs.putFloat("punch_db", punch_gain_db);
  prefs.end();
  Serial.println("Settings tersimpan!");
}

bool parse_float_value(String text, float &value)
{
  text.trim();
  if (text.length() == 0)
    return false;

  const char *input = text.c_str();
  char *end = nullptr;
  const float parsed = strtof(input, &end);
  if (end == input || *end != '\0' || !isfinite(parsed))
    return false;

  value = parsed;
  return true;
}
// ============================================================

// ─── ROTARY ENCODER ───────────────────────────────────────────
int  last_clk        = HIGH;
bool sw_last         = HIGH;
unsigned long sw_debounce = 0;

void handle_encoder()
{
  int clk_val = digitalRead(ENC_CLK);
  if (clk_val != last_clk && clk_val == LOW)
  {
    last_interaction = millis();
    last_control_interaction = millis();
    int dt_val = digitalRead(ENC_DT);
    bool cw = (dt_val != clk_val);

    if (current_page == PAGE_CONTROL)
    {
      // Pindah pilihan kontrol
      if (cw)
      {
        selected_action = (ControlAction)((selected_action + 1) % CTRL_COUNT);
      }
      else
      {
        selected_action = (ControlAction)((selected_action - 1 + CTRL_COUNT) % CTRL_COUNT);
      }
    }
    else if (ui_state == IDLE)
    {
      // Pindah halaman
      if (cw)
      {
        current_page = (current_page == PAGE_CONFIG) ? PAGE_NOWPLAY : PAGE_CONFIG;
      }
      else
      {
        current_page = (current_page == PAGE_NOWPLAY) ? PAGE_CONFIG : PAGE_NOWPLAY;
      }
    }
    else if (ui_state == NAVIGATE)
    {
      if (cw)
      {
        selected_param = (UIParam)((selected_param + 1) % PARAM_COUNT);
      }
      else
      {
        selected_param = (UIParam)((selected_param - 1 + PARAM_COUNT) % PARAM_COUNT);
      }
    }
    else if (ui_state == EDIT)
    {
      switch (selected_param)
      {
      case PARAM_FREQ:
      {
        const float candidate = constrain(crossover_freq + (cw ? 100 : -100), 100, 20000);
        if (update_crossover(candidate, bass_shelf_freq, bass_shelf_gain_db)) {
          crossover_freq = candidate;
        }
        break;
      }
      case PARAM_GAIN_W:
        gain_woofer = constrain(gain_woofer + (cw ? 0.1f : -0.1f), 0.0f, 2.0f);
        break;
      case PARAM_GAIN_T:
        gain_tweeter = constrain(gain_tweeter + (cw ? 0.1f : -0.1f), 0.0f, 2.0f);
        break;
      case PARAM_WAV_VOL:
        wav_volume = constrain(wav_volume + (cw ? 0.1f : -0.1f), 0.0f, 1.0f);
        break;
      case PARAM_MODE:
        mono_mode = !mono_mode;
        break;
      case PARAM_BASS_FREQ:
      {
        const float candidate = constrain(
            bass_shelf_freq + (cw ? 5.0f : -5.0f),
            dsp::kMinBassShelfFrequencyHz, dsp::kMaxBassShelfFrequencyHz);
        if (update_crossover(crossover_freq, candidate, bass_shelf_gain_db)) {
          bass_shelf_freq = candidate;
        }
        break;
      }
      case PARAM_BASS_DB:
      {
        const float candidate = constrain(
            bass_shelf_gain_db + (cw ? 0.5f : -0.5f),
            dsp::kMinBassShelfGainDb, dsp::kMaxBassShelfGainDb);
        if (update_crossover(crossover_freq, bass_shelf_freq, candidate)) {
          bass_shelf_gain_db = candidate;
        }
        break;
      }
      default:
        break;
      }
    }
    update_display();
  }
  last_clk = clk_val;

  // ── Tombol SW ──
  bool sw_val = digitalRead(ENC_SW);
  if (sw_val == LOW && sw_last == HIGH && millis() - sw_debounce > 200)
  {
    sw_debounce = millis();
    last_interaction = millis();
    last_control_interaction = millis();

    if (current_page == PAGE_CONTROL)
    {
      // Eksekusi kontrol media via AVRCP
      switch (selected_action)
      {
      case CTRL_PREV:
        a2dp_sink.previous();
        break;
      case CTRL_PAUSE:
        a2dp_sink.pause();
        break;
      case CTRL_PLAY:
        a2dp_sink.play();
        break;
      case CTRL_NEXT:
        a2dp_sink.next();
        break;
      }
      // Kembali ke NOW PLAYING
      current_page = PAGE_NOWPLAY;
    }
    else if (current_page == PAGE_NOWPLAY)
    {
      // Masuk PAGE_CONTROL
      current_page = PAGE_CONTROL;
      selected_action = CTRL_PLAY; // default ke Play
    }
    else if (current_page == PAGE_CONFIG)
    {
      if (ui_state == IDLE)
      {
        ui_state = NAVIGATE;
      }
      else if (ui_state == NAVIGATE)
      {
        ui_state = EDIT;
      }
      else if (ui_state == EDIT)
      {
        save_settings();
        ui_state = NAVIGATE;
      }
    }
    update_display();
  }
  sw_last = sw_val;
}

void initialize_gpio()
{
  pinMode(BTLED, OUTPUT);
  pinMode(XSMT_PIN, OUTPUT);
  pinMode(PAM_M_PIN, OUTPUT);

  digitalWrite(XSMT_PIN, LOW);
  digitalWrite(PAM_M_PIN, HIGH);

#if FEATURE_ROTARY_ENCODER_ENABLED
  pinMode(ENC_CLK, INPUT_PULLUP);
  pinMode(ENC_DT, INPUT_PULLUP);
  pinMode(ENC_SW, INPUT_PULLUP);
#endif
}

void handle_timeout()
{
  // Timeout CONFIG
  if (current_page == PAGE_CONFIG && ui_state != IDLE &&
      millis() - last_interaction > TIMEOUT_MS)
  {
    if (ui_state == EDIT)
      save_settings();
    ui_state = IDLE;
    update_display();
  }

  // Timeout PAGE_CONTROL → kembali ke NOW PLAYING
  if (current_page == PAGE_CONTROL &&
      millis() - last_control_interaction > CONTROL_TIMEOUT_MS)
  {
    current_page = PAGE_NOWPLAY;
    update_display();
  }
}

void handle_blink()
{
  unsigned long interval = (ui_state == EDIT) ? BLINK_FAST : BLINK_SLOW;
  if (millis() - last_blink > interval)
  {
    blink_state = !blink_state;
    last_blink = millis();
    if (ui_state != IDLE)
      update_display();
  }
}
// ==============================================================

// ─── BLUETOOTH CALLBACK ───────────────────────────────────────
void bt_connection_state_changed(esp_a2d_connection_state_t state, void *ptr)
{
  if (state == ESP_A2D_CONNECTION_STATE_CONNECTED)
  {
    was_connected = true;
    ever_connected = true;
    play_wav("/connected.wav");
    Serial.println("[BT] Connected");
  }
  else if (state == ESP_A2D_CONNECTION_STATE_DISCONNECTED)
  {
    if (was_connected)
    {
      // Hanya play disconnected.wav kalau sebelumnya memang konek
      play_wav("/disconnected.wav");
      Serial.println("[BT] Disconnected");
    }
    else
    {
      Serial.println("[BT] Ready (waiting...)");
    }
    was_connected = false;
  }
  update_display();
}

// ─── AUDIO CALLBACK (DSP CROSSOVER) ──────────────────────────
// Fade-in 20 ms saat musik kembali setelah chime, supaya tidak ada bunyi klik.
static float stream_gain = 1.0f;
static constexpr float STREAM_FADE_STEP = 1.0f / (0.02f * dsp::kDefaultSampleRate);

static inline float next_stream_gain()
{
  stream_gain = min(stream_gain + STREAM_FADE_STEP, 1.0f);
  return stream_gain;
}

// change 8bit format to 16bit format, easier to process later
void audio_data_callback(const uint8_t *data, uint32_t len)
{
  // Chime sedang diputar: buang audio BT agar tidak tercampur di I2S.
  if (wav_pending.load() > 0) {
    stream_gain = 0.0f;
    return;
  }

  // Unmute DAC saat ada audio
  static bool is_unmuted = false;
  if (!is_unmuted) {
    dac_unmute();
    is_unmuted = true;
  }

  if (mono_mode) {
    size_t num_samples = len / 4;
    int16_t i2s_data[num_samples * 2];

    for (size_t i = 0; i < num_samples; i++)
    {
      int16_t left = (data[i * 4 + 1] << 8) | data[i * 4];
      int16_t right = (data[i * 4 + 3] << 8) | data[i * 4 + 2];

      const float mono = ((float)left + (float)right) * 0.5f * next_stream_gain();
      float woofer, tweeter;
      stream_crossover.processSample(mono, gain_woofer, gain_tweeter,
                                     woofer, tweeter);

      const int16_t left_out = (int16_t)constrain(woofer, -32768.0f, 32767.0f);
      const int16_t right_out = (int16_t)constrain(tweeter, -32768.0f, 32767.0f);
      set_i2s_stereo_frame(&i2s_data[i * 2], left_out, right_out);
    }

    size_t i2s_bytes_written;
    i2s_write(I2S_NUM_0, i2s_data, sizeof(int16_t) * num_samples * 2, &i2s_bytes_written, portMAX_DELAY);
  } else {
    // Stereo normal
    size_t num_samples = len / 4;      
    int16_t i2s_data[num_samples * 2]; 

    for (size_t i = 0; i < num_samples; i++)
    {
      int16_t left = (data[i * 4 + 1] << 8) | data[i * 4];      
      int16_t right = (data[i * 4 + 3] << 8) | data[i * 4 + 2]; 

      const float gain = next_stream_gain();
      set_i2s_stereo_frame(&i2s_data[i * 2], (int16_t)(left * gain), (int16_t)(right * gain));
    }
    size_t i2s_bytes_written;
    i2s_write(I2S_NUM_0, i2s_data, sizeof(i2s_data), &i2s_bytes_written, portMAX_DELAY); 
  }
}

void load_settings()
{
  prefs.begin("speaker", true);                     // mode read-only
  mono_mode = prefs.getFloat("mono", true);         // default true
  crossover_freq = prefs.getFloat("freq", 5000.0f); // default 5000 Hz
  gain_woofer = prefs.getFloat("gain_w", 1.0f);     // default 1.0
  gain_tweeter = prefs.getFloat("gain_t", 1.0f);    // default 1.0
  wav_volume = prefs.getFloat("wav_vol", 0.7f);     // default 0.7
  bass_shelf_freq = prefs.getFloat(
      "bass_hz", dsp::kDefaultBassShelfFrequencyHz);
  bass_shelf_gain_db = prefs.getFloat(
      "bass_db", dsp::kDefaultBassShelfGainDb);
  bass_high_pass_freq = prefs.getFloat("hpf_hz", dsp::kDefaultBassHighPassHz);
  punch_freq = prefs.getFloat("punch_hz", dsp::kDefaultPunchFrequencyHz);
  punch_gain_db = prefs.getFloat("punch_db", dsp::kDefaultPunchGainDb);
  prefs.end();

  if (!isfinite(bass_shelf_freq) ||
      bass_shelf_freq < dsp::kMinBassShelfFrequencyHz ||
      bass_shelf_freq > dsp::kMaxBassShelfFrequencyHz) {
    bass_shelf_freq = dsp::kDefaultBassShelfFrequencyHz;
  }
  if (!isfinite(bass_shelf_gain_db) ||
      bass_shelf_gain_db < dsp::kMinBassShelfGainDb ||
      bass_shelf_gain_db > dsp::kMaxBassShelfGainDb) {
    bass_shelf_gain_db = dsp::kDefaultBassShelfGainDb;
  }
  if (!update_punch(bass_high_pass_freq, punch_freq, punch_gain_db)) {
    bass_high_pass_freq = dsp::kDefaultBassHighPassHz;
    punch_freq = dsp::kDefaultPunchFrequencyHz;
    punch_gain_db = dsp::kDefaultPunchGainDb;
    update_punch(bass_high_pass_freq, punch_freq, punch_gain_db);
  }

  Serial.printf("Settings loaded -> freq:%.0f gain_w:%.2f gain_t:%.2f wav_vol:%.2f bass:%.0fHz/%+.1fdB\n",
                crossover_freq, gain_woofer, gain_tweeter, wav_volume,
                bass_shelf_freq, bass_shelf_gain_db);
  update_display();
}

// ─── SERIAL CONTROLLER ────────────────────────────────────────
void handle_serial()
{
  if (!Serial.available())
    return;

  String cmd = Serial.readStringUntil('\n');
  cmd.trim();

  if (cmd.startsWith("mono:"))
  {
    String val = cmd.substring(5);
    if (val == "on")
    {
      mono_mode = true;
      Serial.println("Mono mix diaktifkan");
      update_display();
    }
    else if (val == "off")
    {
      mono_mode = false;
      Serial.println("Mono mix dinonaktifkan (stereo passthrough)");
      update_display();
    }
    else
    {
      Serial.println("Nilai tidak valid. Gunakan 'mono:on' atau 'mono:off'");
    }
  }
  else if (cmd.startsWith("freq:"))
  {
    float freq;
    if (parse_float_value(cmd.substring(5), freq) &&
        freq > 100 && freq < 20000 &&
        update_crossover(freq, bass_shelf_freq, bass_shelf_gain_db))
    {
      crossover_freq = freq;
      Serial.printf("Crossover updated: %.0f Hz\n", crossover_freq);
      update_display();
    }
    else
    {
      Serial.println("Freq harus antara 100 - 20000 Hz");
    }
  }
  else if (cmd.startsWith("bass_hz:"))
  {
    float freq;
    if (parse_float_value(cmd.substring(8), freq) &&
        freq >= dsp::kMinBassShelfFrequencyHz &&
        freq <= dsp::kMaxBassShelfFrequencyHz &&
        update_crossover(crossover_freq, freq, bass_shelf_gain_db))
    {
      bass_shelf_freq = freq;
      Serial.printf("Bass shelf frequency: %.1f Hz\n", bass_shelf_freq);
      update_display();
    }
    else
    {
      Serial.printf("Bass shelf frequency must be %.0f-%.0f Hz\n",
                    dsp::kMinBassShelfFrequencyHz,
                    dsp::kMaxBassShelfFrequencyHz);
    }
  }
  else if (cmd.startsWith("bass_db:"))
  {
    float gain_db;
    if (parse_float_value(cmd.substring(8), gain_db) &&
        gain_db >= dsp::kMinBassShelfGainDb &&
        gain_db <= dsp::kMaxBassShelfGainDb &&
        update_crossover(crossover_freq, bass_shelf_freq, gain_db))
    {
      bass_shelf_gain_db = gain_db;
      Serial.printf("Bass shelf gain: %+.1f dB\n", bass_shelf_gain_db);
      update_display();
    }
    else
    {
      Serial.printf("Bass shelf gain must be %.0f to %+.0f dB\n",
                    dsp::kMinBassShelfGainDb, dsp::kMaxBassShelfGainDb);
    }
  }
  else if (cmd.startsWith("bass:"))
  {
    if (apply_bass_preset(cmd.substring(5)))
    {
      Serial.printf("Preset bass: %s\n", cmd.substring(5).c_str());
      update_display();
    }
    else
    {
      Serial.println("Preset bass: punch, soft, atau flat");
    }
  }
  else if (cmd.startsWith("hpf_hz:"))
  {
    float freq;
    if (parse_float_value(cmd.substring(7), freq) &&
        update_punch(freq, punch_freq, punch_gain_db))
    {
      bass_high_pass_freq = freq;
      Serial.printf("Bass high-pass: %.1f Hz\n", bass_high_pass_freq);
    }
    else
    {
      Serial.printf("High-pass harus %.0f-%.0f Hz\n",
                    dsp::kMinBassHighPassHz, dsp::kMaxBassHighPassHz);
    }
  }
  else if (cmd.startsWith("punch_hz:"))
  {
    float freq;
    if (parse_float_value(cmd.substring(9), freq) &&
        update_punch(bass_high_pass_freq, freq, punch_gain_db))
    {
      punch_freq = freq;
      Serial.printf("Punch frequency: %.1f Hz\n", punch_freq);
    }
    else
    {
      Serial.printf("Punch frequency harus %.0f-%.0f Hz\n",
                    dsp::kMinPunchFrequencyHz, dsp::kMaxPunchFrequencyHz);
    }
  }
  else if (cmd.startsWith("punch_db:"))
  {
    float gain_db;
    if (parse_float_value(cmd.substring(9), gain_db) &&
        update_punch(bass_high_pass_freq, punch_freq, gain_db))
    {
      punch_gain_db = gain_db;
      Serial.printf("Punch gain: %+.1f dB\n", punch_gain_db);
    }
    else
    {
      Serial.printf("Punch gain harus %.0f sampai %+.0f dB\n",
                    dsp::kMinPunchGainDb, dsp::kMaxPunchGainDb);
    }
  }
  else if (cmd.startsWith("gain_w:"))
  {
    gain_woofer = cmd.substring(7).toFloat();
    Serial.printf("Gain woofer: %.2f\n", gain_woofer);
    update_display();
  }
  else if (cmd.startsWith("gain_t:"))
  {
    gain_tweeter = cmd.substring(7).toFloat();
    Serial.printf("Gain tweeter: %.2f\n", gain_tweeter);
    update_display();
  }
  else if (cmd.startsWith("wav_vol:"))
  {
    float vol = cmd.substring(8).toFloat();
    if (vol >= 0.0f && vol <= 1.0f)
    {
      wav_volume = vol;
      Serial.printf("WAV volume: %.0f%%\n", vol * 100);
      update_display();
    }
    else
    {
      Serial.println("Nilai harus antara 0.0 - 1.0");
    }
  }
  else if (cmd == "save")
  {
    save_settings();
  }
  else if (cmd == "reset")
  {
    // Reset ke default
    mono_mode = true;
    crossover_freq = 5000.0f;
    gain_woofer = 1.0f;
    gain_tweeter = 1.0f;
    wav_volume = 0.7f;
    bass_shelf_freq = dsp::kDefaultBassShelfFrequencyHz;
    bass_shelf_gain_db = dsp::kDefaultBassShelfGainDb;
    bass_high_pass_freq = dsp::kDefaultBassHighPassHz;
    punch_freq = dsp::kDefaultPunchFrequencyHz;
    punch_gain_db = dsp::kDefaultPunchGainDb;

    // Update filter & simpan ke NVS
    update_crossover(crossover_freq, bass_shelf_freq, bass_shelf_gain_db);
    update_punch(bass_high_pass_freq, punch_freq, punch_gain_db);
    save_settings();
    update_display();

    Serial.println("Settings direset ke default!");
    Serial.printf("freq:5000 gain_w:1.0 gain_t:1.0 bass_hz:%.0f bass_db:%+.1f\n",
                  bass_shelf_freq, bass_shelf_gain_db);
  }
  else if (cmd == "status")
  {
    Serial.println("================= Status =================");
    Serial.printf("Mono Mix    : %s\n", mono_mode ? "on" : "off");
    Serial.printf("Crossover   : %.0f Hz\n", crossover_freq);
    Serial.printf("Gain Woofer : %.2f\n", gain_woofer);
    Serial.printf("Gain Tweeter: %.2f\n", gain_tweeter);
    Serial.printf("Bass Shelf  : %+.1f dB @ %.1f Hz\n",
                  bass_shelf_gain_db, bass_shelf_freq);
    Serial.printf("Bass HPF    : %.1f Hz\n", bass_high_pass_freq);
    Serial.printf("Punch       : %+.1f dB @ %.1f Hz\n",
                  punch_gain_db, punch_freq);
    Serial.printf("WAV Volume  : %.2f\n", wav_volume);
    Serial.printf("BT Connected: %s\n", a2dp_sink.is_connected() ? "Ya" : "Tidak");
    Serial.println("==========================================");
  }
  else if (cmd == "help")
  {
    Serial.println("=========== Serial Controller ===========");
    Serial.println("mono:<val>    → mono mix (contoh: on/off)");
    Serial.println("freq:<Hz>     → ubah crossover (100-20000)");
    Serial.printf("bass_hz:<Hz>  -> frekuensi shelf bass (%.0f-%.0f Hz)\n",
                  dsp::kMinBassShelfFrequencyHz,
                  dsp::kMaxBassShelfFrequencyHz);
    Serial.printf("bass_db:<dB>  -> gain shelf bass (%+.0f to %+.0f dB)\n",
                  dsp::kMinBassShelfGainDb, dsp::kMaxBassShelfGainDb);
    Serial.println("bass:<nama>   → preset bass (punch/soft/flat)");
    Serial.printf("hpf_hz:<Hz>   -> high-pass woofer (%.0f-%.0f Hz)\n",
                  dsp::kMinBassHighPassHz, dsp::kMaxBassHighPassHz);
    Serial.printf("punch_hz:<Hz> -> frekuensi punch (%.0f-%.0f Hz)\n",
                  dsp::kMinPunchFrequencyHz, dsp::kMaxPunchFrequencyHz);
    Serial.printf("punch_db:<dB> -> gain punch (%.0f to %+.0f dB)\n",
                  dsp::kMinPunchGainDb, dsp::kMaxPunchGainDb);
    Serial.println("gain_w:<val>  → gain woofer  (contoh: 1.2)");
    Serial.println("gain_t:<val>  → gain tweeter (contoh: 0.8)");
    Serial.println("wav_vol:<val> → volume nada wav, antara 0.0 - 1.0");
    Serial.println("status        → lihat setting saat ini");
    Serial.println("save          → simpan setting saat ini");
    Serial.println("reset         → kembali ke setting default");
    Serial.println("help          → tampilkan perintah ini");
    Serial.println("==========================================");
  }
  else
  {
    Serial.printf("Perintah tidak dikenal: %s\n", cmd.c_str());
    Serial.println("Ketik 'help' untuk daftar perintah");
  }
}

// ─── AVRCP METADATA CALLBACK ──────────────────────────────────
void avrc_metadata_callback(uint8_t id, const uint8_t *text) {
  switch (id) {
    case ESP_AVRC_MD_ATTR_TITLE:
      strncpy(track_info.title, (const char*)text, sizeof(track_info.title) - 1);
      track_info.has_data = true;
      scroll_x_title = 0; // reset scroll
      break;
    case ESP_AVRC_MD_ATTR_ARTIST:
      strncpy(track_info.artist, (const char*)text, sizeof(track_info.artist) - 1);
      scroll_x_artist = 0; // reset scroll
      break;
    case ESP_AVRC_MD_ATTR_PLAYING_TIME:
      track_info.duration_ms = atoi((const char*)text);
      break;
  }
  update_display();
}

// Callback posisi lagu (progress)
void avrc_rn_play_pos_callback(uint32_t play_pos) {
    track_info.position_ms = play_pos;
    // update_display();
}
// =================================================================

// ─── UI REFRESH CONFIG ─────────────────────────────────────────
unsigned long last_ui_update = 0;
const unsigned long UI_REFRESH_MS = 100;

// ─── SETUP ────────────────────────────────────────────────────
void setup()
{
  Serial.begin(115200);
  initialize_gpio();

#if FEATURE_OLED_ENABLED
  u8g2.begin();
  update_display();
#endif

  load_settings();
  if (!update_crossover(crossover_freq, bass_shelf_freq, bass_shelf_gain_db)) {
    Serial.println("DSP configuration invalid; check stored settings.");
  }

  if (!LittleFS.begin(true))
  {
    Serial.println("LittleFS Mount Failed!");
    return;
  }
  Serial.println("LittleFS OK");

  // Init crossover coefficients
  Serial.println("DSP Crossover OK");

  i2s_driver_install(I2S_NUM_0, &i2s_config_stereo, 0, NULL);
  i2s_set_pin(I2S_NUM_0, &pin_config);

  wav_queue = xQueueCreate(4, sizeof(WavRequest));
  xTaskCreate(wav_task, "wav_task", 8192, NULL, 1, NULL);

  // --- EKSEKUSI STARTUP SOUND ---
  Serial.println("Memutar startup sound...");
  play_wav("/startup-sound-effect.wav");
  wait_for_wav(); // Tunggu hingga wav_task selesai
  Serial.println("Startup sound selesai.");
  is_startup = false;
  update_display();
  // ------------------------------

  a2dp_sink.set_on_connection_state_changed(bt_connection_state_changed);
  a2dp_sink.set_stream_reader(audio_data_callback, false);
  a2dp_sink.set_auto_reconnect(true);
  a2dp_sink.set_avrc_metadata_attribute_mask(
    ESP_AVRC_MD_ATTR_TITLE |
    ESP_AVRC_MD_ATTR_ARTIST |
    ESP_AVRC_MD_ATTR_PLAYING_TIME
  );
  a2dp_sink.set_avrc_metadata_callback(avrc_metadata_callback);
  a2dp_sink.set_avrc_rn_play_pos_callback(avrc_rn_play_pos_callback, 1);
  a2dp_sink.start("Oke Gass ✌️");

  Serial.println("Ready! Ketik 'help' untuk daftar perintah.");

  // ── Window Tunggu Auto-reconnect (6 detik) ──
  const unsigned long RECONNECT_WINDOW = 6000;
  unsigned long wait_start = millis();

  while (millis() - wait_start < RECONNECT_WINDOW)
  {
    if (was_connected)
    {
      Serial.println("[BT] Auto-reconnect berhasil!");
      break;
    }
    vTaskDelay(pdMS_TO_TICKS(100));
  }

  if (!was_connected)
  {
    Serial.println("[BT] Tidak ada reconnect → play waiting.wav");
    play_wav("/waiting.wav");
    wait_for_wav();
    ever_connected = true;
    update_display();
  }
}

// ─── LOOP ─────────────────────────────────────────────────────
void loop()
{
  digitalWrite(BTLED, a2dp_sink.is_connected() ? HIGH : LOW);
  handle_serial();
#if FEATURE_ROTARY_ENCODER_ENABLED
  handle_encoder();
#endif
  handle_timeout();
  handle_blink();

  // Refresh layar hanya setiap 100ms
  if (millis() - last_ui_update > UI_REFRESH_MS) {
    last_ui_update = millis();
    update_display();
  }
}