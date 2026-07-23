// IMPORT IMPORTANT LIBRARY
#include <Arduino.h>
#include "LittleFS.h"
#include "BluetoothA2DPSink.h"
#include "driver/i2s.h"
// #include "dsps_biquad.h"     // ESP-DSP
// #include "dsps_biquad_gen.h" // ESP-DSP coefficient generator
#include <Preferences.h>
#include <U8g2lib.h>
#include <Wire.h>

// ── DSP FUNCTIONS (implementasi manual, tanpa library) ────────
void dsp_gen_lpf(float *c, float f, float q) {
    float w  = 2.0f * M_PI * f;
    float cs = cosf(w), sn = sinf(w);
    float alpha = sn / (2.0f * q);
    float a0    = 1.0f + alpha;
    c[0] =  ((1.0f - cs) / 2.0f) / a0; // b0
    c[1] =   (1.0f - cs)          / a0; // b1
    c[2] =  ((1.0f - cs) / 2.0f) / a0; // b2
    c[3] =  (-2.0f * cs)          / a0; // a1
    c[4] =   (1.0f - alpha)       / a0; // a2
}

void dsp_gen_hpf(float *c, float f, float q) {
    float w  = 2.0f * M_PI * f;
    float cs = cosf(w), sn = sinf(w);
    float alpha = sn / (2.0f * q);
    float a0    = 1.0f + alpha;
    c[0] =  ((1.0f + cs) / 2.0f) / a0; // b0
    c[1] = -((1.0f + cs))         / a0; // b1
    c[2] =  ((1.0f + cs) / 2.0f) / a0; // b2
    c[3] =  (-2.0f * cs)          / a0; // a1
    c[4] =   (1.0f - alpha)       / a0; // a2
}

void dsp_biquad(float *in, float *out, int len, float *c, float *w) {
    for (int i = 0; i < len; i++) {
        float d = in[i] - c[3] * w[0] - c[4] * w[1];
        out[i]  = c[0] * d  + c[1] * w[0] + c[2] * w[1];
        w[1]    = w[0];
        w[0]    = d;
    }
}

#define BTLED 2
#define ENC_CLK 32
#define ENC_DT  33
#define ENC_SW  27

BluetoothA2DPSink a2dp_sink;
Preferences prefs;

// ─── I2S PIN CONFIG ───────────────────────────────────────────
i2s_pin_config_t pin_config = {
    .mck_io_num = I2S_PIN_NO_CHANGE,
    .bck_io_num = 25,
    .ws_io_num = 26,
    .data_out_num = 22,
    .data_in_num = I2S_PIN_NO_CHANGE};

// ─── I2S DRIVER CONFIG ────────────────────────────────────────
i2s_config_t i2s_config_stereo = {
    .mode = (i2s_mode_t)(I2S_MODE_MASTER | I2S_MODE_TX),
    .sample_rate = 44100,
    .bits_per_sample = I2S_BITS_PER_SAMPLE_16BIT,
    .channel_format = I2S_CHANNEL_FMT_RIGHT_LEFT,
    .communication_format = (i2s_comm_format_t)(I2S_COMM_FORMAT_STAND_I2S),
    .intr_alloc_flags = 0,
    .dma_buf_count = 16,
    .dma_buf_len = 512,
    .use_apll = false,
    .tx_desc_auto_clear = true,
    .fixed_mclk = 0};

// ─── DSP CROSSOVER CONFIG ─────────────────────────────────────
// Crossover point & Q factor (Linkwitz-Riley = cascade 2x Butterworth Q=0.7071)
float crossover_freq = 5000.0f; // Hz, bisa diubah via Serial
const float SAMPLE_RATE = 44100.0f;
const float Q_BUTTERWORTH = 0.7071f;

// Koefisien filter [b0, b1, b2, a1, a2]
float coeffs_lpf1[5], coeffs_lpf2[5]; // LPF stage 1 & 2 → Woofer
float coeffs_hpf1[5], coeffs_hpf2[5]; // HPF stage 1 & 2 → Tweeter

// Delay line (state) tiap filter — wajib dipisah
float w_lpf1[2] = {0}, w_lpf2[2] = {0};
float w_hpf1[2] = {0}, w_hpf2[2] = {0};

// Gain woofer & tweeter (1.0 = normal)
float gain_woofer = 1.0f;
float gain_tweeter = 1.0f;

bool mono_mode = true; // true = mono mix, false = stereo passthrough (LPF ke L, HPF ke R)

// ─── UI STATE MACHINE ─────────────────────────────────────────
enum UIState  { IDLE, NAVIGATE, EDIT };
enum UIParam  { PARAM_FREQ, PARAM_GAIN_W, PARAM_GAIN_T, PARAM_WAV_VOL, PARAM_MODE, PARAM_COUNT };
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

// Fungsi hitung ulang koefisien (dipanggil saat freq berubah)
void update_crossover(float freq_hz) {
  // ESP-DSP pakai normalized frequency: f / fs, range 0.0 - 0.5
  float f_norm = freq_hz / SAMPLE_RATE;
  dsp_gen_lpf(coeffs_lpf1, f_norm, Q_BUTTERWORTH);
  dsp_gen_lpf(coeffs_lpf2, f_norm, Q_BUTTERWORTH);
  dsp_gen_hpf(coeffs_hpf1, f_norm, Q_BUTTERWORTH);
  dsp_gen_hpf(coeffs_hpf2, f_norm, Q_BUTTERWORTH);
  // Reset delay line saat koefisien berubah
  memset(w_lpf1, 0, sizeof(w_lpf1));
  memset(w_lpf2, 0, sizeof(w_lpf2));
  memset(w_hpf1, 0, sizeof(w_hpf1));
  memset(w_hpf2, 0, sizeof(w_hpf2));
  Serial.printf("Crossover updated: %.0f Hz\n", freq_hz);
}

// ─── LITTLEFS / WAV CHIME ─────────────────────────────────────
static char wav_to_play[32] = "";
float wav_volume = 0.7f;
volatile bool startup_done = false;

void wav_task(void *param)
{
  if (strlen(wav_to_play) > 0)
  {
    File file = LittleFS.open(wav_to_play, "r");
    if (file)
    {
      file.seek(44); // skip WAV header

      const size_t READ_SAMPLES = 128;
      uint8_t  raw[READ_SAMPLES * 2];       // 256 bytes
      static float   fbuf[READ_SAMPLES];
      static float   flpf[READ_SAMPLES],  flpf2[READ_SAMPLES];
      static float   fhpf[READ_SAMPLES],  fhpf2[READ_SAMPLES];
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
          // Apply crossover
          dsp_biquad(fbuf,  flpf,  num_samples, coeffs_lpf1, w_lpf1);
          dsp_biquad(flpf,  flpf2, num_samples, coeffs_lpf2, w_lpf2);
          dsp_biquad(fbuf,  fhpf,  num_samples, coeffs_hpf1, w_hpf1);
          dsp_biquad(fhpf,  fhpf2, num_samples, coeffs_hpf2, w_hpf2);

          for (size_t i = 0; i < num_samples; i++) {
            out[i*2]   = (int16_t)constrain(fhpf2[i] * gain_tweeter, -32768, 32767); // R → Tweeter
            out[i*2+1] = (int16_t)constrain(flpf2[i] * gain_woofer,  -32768, 32767); // L → Woofer
          }
        } else {
          // Stereo passthrough
          for (size_t i = 0; i < num_samples; i++) {
            out[i*2]   = (int16_t)constrain(fbuf[i], -32768, 32767);
            out[i*2+1] = (int16_t)constrain(fbuf[i], -32768, 32767);
          }
        }

        i2s_write(I2S_NUM_0, out, num_samples * 4, &bytes_written, portMAX_DELAY);
      }
      file.close();
    }
    else
    {
      Serial.printf("Gagal buka: %s\n", wav_to_play);
    }
    wav_to_play[0] = '\0';
  }
  startup_done = true;
  vTaskDelete(NULL);
}

void play_wav(const char *filename)
{
  strncpy(wav_to_play, filename, sizeof(wav_to_play));
  xTaskCreate(wav_task, "wav_task", 8192, NULL, 1, NULL);
}

// Bris program untuk LCD
// SSD1306 128x32 I2C
// Parameter: rotasi, reset pin, SCL, SDA
U8G2_SSD1306_128X32_UNIVISION_F_HW_I2C u8g2(
  U8G2_R0,        // rotasi normal
  U8X8_PIN_NONE,  // tidak pakai pin reset
  19,             // SCL
  21              // SDA
);

// Format millisecond ke MM:SS
void format_time(char *buf, uint32_t ms)
{
  uint32_t total_sec = ms / 1000;
  uint32_t min = total_sec / 60;
  uint32_t sec = total_sec % 60;
  snprintf(buf, 8, "%02d:%02d", min, sec);
}

// Scroll teks otomatis
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

// Panggil fungsi ini setiap kali ada perubahan status
// Label parameter
const char* param_labels[] = { "Freq", "GainW", "GainT", "WVol", "Mode" };

void update_display()
{
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
      snprintf(line3, sizeof(line3), "W:%.1f T:%.1f V:%d%%",
               gain_woofer, gain_tweeter, (int)(wav_volume * 100));
      u8g2.drawStr(0, 32, line3);
    }
    else
    {
      // NAVIGATE / EDIT — kode yang sudah ada
      const char *param_names[PARAM_COUNT] = {
          "Freq", "Gain W", "Gain T", "WAV Vol", "Mode"};
      char param_values[PARAM_COUNT][16];
      snprintf(param_values[PARAM_FREQ], sizeof(param_values[0]), "%dHz", (int)crossover_freq);
      snprintf(param_values[PARAM_GAIN_W], sizeof(param_values[1]), "%.1f", gain_woofer);
      snprintf(param_values[PARAM_GAIN_T], sizeof(param_values[2]), "%.1f", gain_tweeter);
      snprintf(param_values[PARAM_WAV_VOL], sizeof(param_values[3]), "%d%%", (int)(wav_volume * 100));
      snprintf(param_values[PARAM_MODE], sizeof(param_values[4]), mono_mode ? "XO" : "ST");

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
    if (!a2dp_sink.is_connected() || !track_info.has_data)
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
  prefs.end();
  Serial.println("Settings tersimpan!");
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
        crossover_freq = constrain(crossover_freq + (cw ? 100 : -100), 100, 20000);
        update_crossover(crossover_freq);
        break;
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
    play_wav("/connected.wav");
    Serial.println("Bluetooth Connected");
  }
  else if (state == ESP_A2D_CONNECTION_STATE_DISCONNECTED)
  {
    play_wav("/disconnected.wav");
    Serial.println("Bluetooth Disconnected");
  }
  update_display();
}

// ─── AUDIO CALLBACK (DSP CROSSOVER) ──────────────────────────
// Buffer float untuk ESP-DSP (proses per batch)
// Buffer DSP — global, fixed size
static float buf_mono[1024];
static float buf_lpf[1024],  buf_lpf2[1024];
static float buf_hpf[1024],  buf_hpf2[1024];

// change 8bit format to 16bit format, easier to process later
void audio_data_callback(const uint8_t *data, uint32_t len)
{
  if (mono_mode) {
    // DSP crossover
    size_t num_samples = len / 4;
    int16_t i2s_data[num_samples * 2];

    for (size_t i = 0; i < num_samples; i++)
    {
      int16_t left = (data[i * 4 + 1] << 8) | data[i * 4];
      int16_t right = (data[i * 4 + 3] << 8) | data[i * 4 + 2];

      // Mono mix
      buf_mono[i] = (float)(left + right) / 2.0f;
    }

    dsp_biquad(buf_mono, buf_lpf,  num_samples, coeffs_lpf1, w_lpf1);
    dsp_biquad(buf_lpf,  buf_lpf2, num_samples, coeffs_lpf2, w_lpf2);
    dsp_biquad(buf_mono, buf_hpf,  num_samples, coeffs_hpf1, w_hpf1);
    dsp_biquad(buf_hpf,  buf_hpf2, num_samples, coeffs_hpf2, w_hpf2);

    // ── Bagian Mono Mode (Crossover) ──
    // Hardware meminta R di index genap dan L di index ganjil
    // Masalah: Ketika mengganti file wav, kadang L dan R terbalik
    for (size_t i = 0; i < num_samples; i++) {
      i2s_data[i * 2]     = (int16_t)constrain(buf_hpf2[i] * gain_tweeter, -32768, 32767); // R -> Tweeter (Kanan)
      i2s_data[i * 2 + 1] = (int16_t)constrain(buf_lpf2[i] * gain_woofer,  -32768, 32767); // L -> Woofer (Kiri)
      // i2s_data[i * 2]     = (int16_t)constrain(buf_lpf2[i] * gain_woofer,  -32768, 32767); // L -> Woofer
      // i2s_data[i * 2 + 1] = (int16_t)constrain(buf_hpf2[i] * gain_tweeter, -32768, 32767); // R -> Tweeter
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

      // ── Bagian Stereo ──
      // Sesuaikan juga dengan hardware: R di genap, L di ganjil
      i2s_data[i * 2]     = right;
      i2s_data[i * 2 + 1] = left;
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
  prefs.end();
  Serial.printf("Settings loaded → freq:%.0f gain_w:%.2f gain_t:%.2f wav_vol:%.2f\n",
                crossover_freq, gain_woofer, gain_tweeter, wav_volume);
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
    float freq = cmd.substring(5).toFloat();
    if (freq > 100 && freq < 20000)
    {
      crossover_freq = freq;
      update_crossover(crossover_freq);
      update_display();
    }
    else
    {
      Serial.println("Freq harus antara 100 - 20000 Hz");
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

    // Update filter & simpan ke NVS
    update_crossover(crossover_freq);
    save_settings();

    Serial.println("Settings direset ke default!");
    Serial.println("freq:5000 gain_w:1.0 gain_t:1.0");
  }
  else if (cmd == "status")
  {
    Serial.println("================= Status =================");
    Serial.printf("Mono Mix    : %s\n", mono_mode ? "on" : "off");
    Serial.printf("Crossover   : %.0f Hz\n", crossover_freq);
    Serial.printf("Gain Woofer : %.2f\n", gain_woofer);
    Serial.printf("Gain Tweeter: %.2f\n", gain_tweeter);
    Serial.printf("WAV Volume  : %.2f\n", wav_volume);
    Serial.printf("BT Connected: %s\n", a2dp_sink.is_connected() ? "Ya" : "Tidak");
    Serial.println("==========================================");
  }
  else if (cmd == "help")
  {
    Serial.println("=========== Serial Controller ===========");
    Serial.println("mono:<val>    → mono mix (contoh: on/off)");
    Serial.println("freq:<Hz>     → ubah crossover (100-20000)");
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

  // Rotary encoder
  pinMode(ENC_CLK, INPUT_PULLUP);
  pinMode(ENC_DT,  INPUT_PULLUP);
  pinMode(ENC_SW,  INPUT_PULLUP);

  u8g2.begin();
  update_display();

  load_settings();
  update_crossover(crossover_freq);

  if (!LittleFS.begin(true))
  {
    Serial.println("LittleFS Mount Failed!");
    return;
  }
  Serial.println("LittleFS OK");

  // Init crossover coefficients
  Serial.println("DSP Crossover OK");

  pinMode(BTLED, OUTPUT);

  i2s_driver_install(I2S_NUM_0, &i2s_config_stereo, 0, NULL);
  i2s_set_pin(I2S_NUM_0, &pin_config);

  // --- EKSEKUSI STARTUP SOUND ---
  Serial.println("Memutar startup sound...");
  startup_done = false;
  play_wav("/jbl-startup-sound-effect.wav");
  
  while (!startup_done) {
    vTaskDelay(pdMS_TO_TICKS(10)); // Tunggu hingga wav_task selesai
  }
  Serial.println("Startup sound selesai.");
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
  a2dp_sink.start("Speaker Mahal 😁");

  Serial.println("Ready! Ketik 'help' untuk daftar perintah.");
}

// ─── LOOP ─────────────────────────────────────────────────────
void loop()
{
  digitalWrite(BTLED, a2dp_sink.is_connected() ? HIGH : LOW);
  handle_serial();
  handle_encoder();
  handle_timeout();
  handle_blink();

  // Refresh layar hanya setiap 100ms
  if (millis() - last_ui_update > UI_REFRESH_MS) {
    last_ui_update = millis();
    update_display();
  }
}