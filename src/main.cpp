// IMPORT IMPORTANT LIBRARY
#include <Arduino.h>
#include "LittleFS.h"
#include "BluetoothA2DPSink.h"
#include "driver/i2s.h"
#include "dsps_biquad.h"     // ESP-DSP
#include "dsps_biquad_gen.h" // ESP-DSP coefficient generator
#include <Preferences.h>
#include <U8g2lib.h>
#include <Wire.h>
#include "freertos/FreeRTOS.h"
#include "freertos/ringbuf.h"

#define BTLED 2
#define ENC_CLK 32
#define ENC_DT  33
#define ENC_SW  34

// ─── RING BUFFER CONFIG ───────────────────────────────────────
#define AUDIO_BUF_SIZE 16384 // Ukuran keranjang 16KB (cukup untuk menampung aliran data)

RingbufHandle_t ringbuf_bt;
RingbufHandle_t ringbuf_wav;
// ==============================================================

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

UIState  ui_state       = IDLE;
UIParam  selected_param = PARAM_FREQ;

unsigned long last_interaction = 0;
const unsigned long TIMEOUT_MS = 5000;

// Untuk efek kedip
bool     blink_state    = false;
unsigned long last_blink = 0;
const unsigned long BLINK_FAST = 200; // ms — mode EDIT
const unsigned long BLINK_SLOW = 600; // ms — mode NAVIGATE
// ==============================================================

// === fungsi inisialisasi untuk memesan tempat di memori ESP32 ==========
void init_ring_buffers() {
  // Membuat Ring Buffer tipe byte stream
  ringbuf_bt = xRingbufferCreate(AUDIO_BUF_SIZE, RINGBUF_TYPE_BYTEBUF);
  ringbuf_wav = xRingbufferCreate(AUDIO_BUF_SIZE, RINGBUF_TYPE_BYTEBUF);

  if (ringbuf_bt == NULL || ringbuf_wav == NULL) {
    Serial.println("Error: Gagal mengalokasikan memori untuk Ring Buffer!");
  } else {
    Serial.println("Ring Buffers untuk Mixer siap!");
  }
}
// ==============================================================

// Fungsi hitung ulang koefisien (dipanggil saat freq berubah)
void update_crossover(float freq_hz) {
    // ESP-DSP pakai normalized frequency: f / fs, range 0.0 - 0.5
    float f_norm = freq_hz / SAMPLE_RATE;
    dsps_biquad_gen_lpf_f32(coeffs_lpf1, f_norm, Q_BUTTERWORTH);
    dsps_biquad_gen_lpf_f32(coeffs_lpf2, f_norm, Q_BUTTERWORTH);
    dsps_biquad_gen_hpf_f32(coeffs_hpf1, f_norm, Q_BUTTERWORTH);
    dsps_biquad_gen_hpf_f32(coeffs_hpf2, f_norm, Q_BUTTERWORTH);
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

volatile bool is_wav_playing = false;

void wav_task(void *param)
{
  if (strlen(wav_to_play) > 0)
  {
    File file = LittleFS.open(wav_to_play, "r");
    if (file)
    {
      file.seek(44); // Lewati header WAV standar

      // KONFIGURASI AMAN UNTUK 11.025 Hz
      const int RATIO = 4; // Karena 44100 / 11025 = 4
      const size_t READ_SAMPLES = 64; // Baca 64 sampel mono (128 byte) per putaran
      
      uint8_t raw[READ_SAMPLES * 2]; 
      
      // Buffer output: 64 sampel * 4 (ratio) * 2 (stereo) = 512 elemen int16 (1024 byte)
      // Ukuran ini sangat aman untuk stack memory dan pas dengan CHUNK_SIZE
      int16_t stereo_data[512]; 
      
      int prefill_count = 0;
      
      while (file.available())
      {
        size_t bytes_read = file.read(raw, sizeof(raw));
        size_t num_samples = bytes_read / 2; 
        size_t stereo_idx = 0;
        
        for (size_t i = 0; i < num_samples; i++)
        {
          int16_t sample = (raw[i * 2 + 1] << 8) | raw[i * 2];
          int16_t sample_with_vol = (int16_t)(sample * wav_volume);
          
          // Duplikasi sampel (Stretching) 4x lipat, sekaligus jadikan Stereo Kiri-Kanan
          for(int r = 0; r < RATIO; r++) {
            stereo_data[stereo_idx++] = sample_with_vol; // Channel Kiri
            stereo_data[stereo_idx++] = sample_with_vol; // Channel Kanan
          }
        }

        // Kirim ke keranjang (jika penuh, task ini akan otomatis sabar menunggu)
        if (ringbuf_wav != NULL) {
          xRingbufferSend(ringbuf_wav, (void*)stereo_data, stereo_idx * sizeof(int16_t), portMAX_DELAY);
        }

        // TANDON AIR: Hitung seberapa banyak data yang sudah masuk keranjang
        prefill_count++;
        if (prefill_count >= 8) { 
          is_wav_playing = true; // Buka keran untuk Master Task setelah keranjang terisi aman
        }
      }
      file.close();
    }
    wav_to_play[0] = '\0';
  }
  
  // Biarkan Master Task punya waktu 500ms untuk menguras habis sisa keranjang
  vTaskDelay(pdMS_TO_TICKS(500)); 
  is_wav_playing = false; 
  vTaskDelete(NULL); 
}

void play_wav(const char *filename)
{
  // Mencegah task ganda: Jangan putar jika suara sebelumnya belum selesai!
  if (is_wav_playing) return; 

  strncpy(wav_to_play, filename, sizeof(wav_to_play));
  
  // PERBAIKAN UTAMA: Naikkan stack dari 4096 menjadi 8192 byte
  xTaskCreatePinnedToCore(wav_task, "wav_task", 8192, NULL, 4, NULL, 1);
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

// Panggil fungsi ini setiap kali ada perubahan status
// Label parameter
const char* param_labels[] = { "Freq", "GainW", "GainT", "WVol", "Mode" };

void update_display() {
  u8g2.clearBuffer();
  u8g2.setFont(u8g2_font_6x10_tr);

  if (ui_state == IDLE) {
    // ── Tampilan normal ──
    u8g2.drawStr(0, 10, a2dp_sink.is_connected() ? "BT: Connected" : "BT: Waiting...");

    char line2[32];
    snprintf(line2, sizeof(line2), "Freq:%dHz %s",
              (int)crossover_freq, mono_mode ? "XO" : "ST");
    u8g2.drawStr(0, 22, line2);

    char line3[32];
    snprintf(line3, sizeof(line3), "W:%.1f T:%.1f V:%d%%",
              gain_woofer, gain_tweeter, (int)(wav_volume * 100));
    u8g2.drawStr(0, 32, line3);

  } else {
    // ── Tampilan NAVIGATE / EDIT ──
    // Tampilkan semua parameter, highlight yang dipilih
    const char* param_names[PARAM_COUNT] = {
      "Freq", "Gain W", "Gain T", "WAV Vol", "Mode"
    };

    char param_values[PARAM_COUNT][16];
    snprintf(param_values[PARAM_FREQ],    sizeof(param_values[0]), "%dHz", (int)crossover_freq);
    snprintf(param_values[PARAM_GAIN_W],  sizeof(param_values[1]), "%.1f", gain_woofer);
    snprintf(param_values[PARAM_GAIN_T],  sizeof(param_values[2]), "%.1f", gain_tweeter);
    snprintf(param_values[PARAM_WAV_VOL], sizeof(param_values[3]), "%d%%", (int)(wav_volume * 100));
    snprintf(param_values[PARAM_MODE],    sizeof(param_values[4]), mono_mode ? "XO" : "ST");

    // Tampilkan max 3 parameter sekaligus (scroll sederhana)
    int start = (selected_param > 1) ? selected_param - 1 : 0;
    if (start + 3 > PARAM_COUNT) start = PARAM_COUNT - 3;

    for (int i = 0; i < 3; i++) {
      int idx = start + i;
      if (idx >= PARAM_COUNT) break;

      char line[32];
      snprintf(line, sizeof(line), "%s: %s", param_names[idx], param_values[idx]);

      bool is_selected = (idx == selected_param);

      if (is_selected && blink_state) {
        // Highlight dengan kotak
        u8g2.setDrawColor(1);
        u8g2.drawBox(0, (i * 11), 128, 11);
        u8g2.setDrawColor(0); // teks hitam di atas kotak putih
        u8g2.drawStr(2, (i * 11) + 9, line);
        u8g2.setDrawColor(1); // reset warna
      } else {
        u8g2.drawStr(2, (i * 11) + 9, line);
      }
    }
  }

  u8g2.sendBuffer();
  // display_needs_update = false;
}
// ============

// =================Fungsi simpan & load settings================
void save_settings()
{
  prefs.begin("speaker", false); // namespace "speaker", mode read-write
  prefs.putBool("mono", mono_mode);
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

void handle_encoder() {
    // ── Baca rotasi ──
    int clk_val = digitalRead(ENC_CLK);
    if (clk_val != last_clk && clk_val == LOW) {
        last_interaction = millis(); // reset timeout

        int dt_val = digitalRead(ENC_DT);
        bool cw = (dt_val != clk_val); // clockwise?

        if (ui_state == IDLE) {
            ui_state = NAVIGATE;

        } else if (ui_state == NAVIGATE) {
            // Pindah sorotan parameter
            if (cw) {
                selected_param = (UIParam)((selected_param + 1) % PARAM_COUNT);
            } else {
                selected_param = (UIParam)((selected_param - 1 + PARAM_COUNT) % PARAM_COUNT);
            }

        } else if (ui_state == EDIT) {
            // Ubah nilai parameter
            switch (selected_param) {
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
                default: break;
            }
        }

        update_display();
    }
    last_clk = clk_val;

    // ── Baca tombol (SW) ──
    bool sw_val = digitalRead(ENC_SW);
    if (sw_val == LOW && sw_last == HIGH && millis() - sw_debounce > 200) {
        sw_debounce      = millis();
        last_interaction = millis();

        if (ui_state == IDLE) {
            ui_state = NAVIGATE;

        } else if (ui_state == NAVIGATE) {
            ui_state = EDIT;

        } else if (ui_state == EDIT) {
            save_settings();
            ui_state = NAVIGATE;
        }

        update_display();
    }
    sw_last = sw_val;
}

void handle_timeout() {
    if (ui_state != IDLE && millis() - last_interaction > TIMEOUT_MS) {
        if (ui_state == EDIT) save_settings(); // simpan otomatis
        ui_state = IDLE;
        update_display();
    }
}

void handle_blink() {
    unsigned long interval = (ui_state == EDIT) ? BLINK_FAST : BLINK_SLOW;
    if (millis() - last_blink > interval) {
        blink_state = !blink_state;
        last_blink  = millis();
        if (ui_state != IDLE) update_display();
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
static float buf_mono[1024];
static float buf_lpf[1024];
static float buf_hpf[1024];

// change 8bit format to 16bit format, easier to process later
void audio_data_callback(const uint8_t *data, uint32_t len) {
  // Langsung lempar data mentah stereo 16-bit (yang aslinya 8-bit dari A2DP callback ini) ke Keranjang A
  // Karena format dari A2DP library ini butuh sedikit konversi endianness seperti di kodemu sebelumnya:
  
  size_t num_samples = len / 4;      
  int16_t i2s_data[num_samples * 2]; 

  for (size_t i = 0; i < num_samples; i++) {
    int16_t left = (data[i * 4 + 1] << 8) | data[i * 4];      
    int16_t right = (data[i * 4 + 3] << 8) | data[i * 4 + 2]; 

    i2s_data[i * 2] = right;
    i2s_data[i * 2 + 1] = left;
  }

  // Kirim data yang sudah jadi PCM 16-bit stereo ke Ring Buffer BT (Keranjang A)
  // portMAX_DELAY artinya jika keranjang penuh, dia akan menunggu sampai ada ruang kosong
  if (ringbuf_bt != NULL) {
    xRingbufferSend(ringbuf_bt, (void*)i2s_data, sizeof(i2s_data), portMAX_DELAY);
  }
}

// ─── FUNGSI HELPER UNTUK MENJAHIT RING BUFFER ─────────────────
size_t fetch_from_ringbuf(RingbufHandle_t rb, int16_t* dest, size_t max_bytes, TickType_t wait_ticks) {
    size_t total_read = 0;
    uint8_t* ptr = (uint8_t*)dest;
    
    while (total_read < max_bytes) {
      size_t bytes_to_read = max_bytes - total_read;
      size_t received_size = 0;
      
      // Hanya menunggu di tarikan pertama. Tarikan kedua (jika melingkar) jangan ditunggu.
      TickType_t ticks = (total_read == 0) ? wait_ticks : 0;
      
      void* data = xRingbufferReceiveUpTo(rb, &received_size, ticks, bytes_to_read);
      if (data == NULL) break; // Keranjang sudah kosong
      
      memcpy(ptr + total_read, data, received_size);
      vRingbufferReturnItem(rb, data); // Kembalikan ruang kosong
      total_read += received_size;
    }
    return total_read;
}
// ==============================================================

// ==== Fungsi utama untuk proses audio: baca dari kedua keranjang, mix, crossover, output ke I2S =====
void master_audio_task(void *param) {
  // Ambil data dalam bongkahan 1024 byte (256 frame stereo 16-bit)
  const size_t CHUNK_SIZE = 1024; 

  // Array penampung sementara agar data tidak terpotong (1024 byte = 512 int16)
  int16_t bt_buffer[512];
  int16_t wav_buffer[512];
  
  while (true) {
    // BT ditunggu maksimal 10ms
    size_t bt_bytes  = fetch_from_ringbuf(ringbuf_bt, bt_buffer, CHUNK_SIZE, 0);
    size_t wav_bytes = 0;

    if (is_wav_playing) {
      wav_bytes = fetch_from_ringbuf(ringbuf_wav, wav_buffer, CHUNK_SIZE, 0);
    }

    if (bt_bytes == 0 && wav_bytes == 0) {
      vTaskDelay(pdMS_TO_TICKS(2));
      continue;
    }

    size_t max_bytes = (bt_bytes > wav_bytes) ? bt_bytes : wav_bytes;
    size_t num_frames = max_bytes / 4;
    
    int16_t mixed_data[1024]; 
    memset(mixed_data, 0, sizeof(mixed_data));

    // ─── PROSES MIXING SOFTWARE ───
    for (size_t i = 0; i < num_frames * 2; i++) {
      // Ambil sampel secara berurutan selama index byte belum melebihi total byte
      int32_t sample_bt  = ((i * 2) < bt_bytes) ? bt_buffer[i] : 0;
      int32_t sample_wav = ((i * 2) < wav_bytes) ? wav_buffer[i] : 0;

      int32_t mixed = sample_bt + sample_wav;
      mixed_data[i] = (int16_t)constrain(mixed, -32768, 32767);
    }

    // ─── PROSES DSP CROSSOVER ───
    static int16_t final_i2s_data[1024];

    if (mono_mode) {
      for (size_t i = 0; i < num_frames; i++) {
        // Campur Kiri dan Kanan menjadi Mono untuk input DSP
        buf_mono[i] = (float)(mixed_data[i * 2] + mixed_data[i * 2 + 1]) / 2.0f;
      }

      static float buf_lpf2[1024];
      static float buf_hpf2[1024];


      // LPF ke Woofer
      dsps_biquad_f32(buf_mono, buf_lpf,  num_frames, coeffs_lpf1, w_lpf1);
      dsps_biquad_f32(buf_lpf,  buf_lpf2, num_frames, coeffs_lpf2, w_lpf2);

      // HPF ke Tweeter
      dsps_biquad_f32(buf_mono, buf_hpf,  num_frames, coeffs_hpf1, w_hpf1);
      dsps_biquad_f32(buf_hpf,  buf_hpf2, num_frames, coeffs_hpf2, w_hpf2);

      for (size_t i = 0; i < num_frames; i++) {
        final_i2s_data[i * 2]     = (int16_t)constrain(buf_hpf2[i] * gain_tweeter, -32768, 32767);// R → Tweeter
        final_i2s_data[i * 2 + 1] = (int16_t)constrain(buf_lpf2[i] * gain_woofer, -32768, 32767); // L → Woofer
      }
    } else {
      // Stereo Passthrough (Bypass DSP)
      memcpy(final_i2s_data, mixed_data, num_frames * 4);
    }

    // ─── OUTPUT KE DAC PCM5102A ───
    size_t i2s_bytes_written;
    i2s_write(I2S_NUM_0, final_i2s_data, num_frames * 4, &i2s_bytes_written, portMAX_DELAY);
  }
}
// ==============================================================

void load_settings()
{
  prefs.begin("speaker", true);                     // mode read-only
  mono_mode = prefs.getBool("mono", true);          // default true
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

  init_ring_buffers(); // Inisialisasi memori keranjang

  // Nyalakan mesin utama di core 1 dengan prioritas tinggi
  xTaskCreatePinnedToCore(
    master_audio_task, 
    "MasterAudio", 
    8192, 
    NULL, 
    configMAX_PRIORITIES - 1, 
    NULL, 
    1 // Ikat ke Core 1
  );

  a2dp_sink.set_on_connection_state_changed(bt_connection_state_changed);
  a2dp_sink.set_stream_reader(audio_data_callback, false);
  a2dp_sink.set_auto_reconnect(true);
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
  // update_display();
}