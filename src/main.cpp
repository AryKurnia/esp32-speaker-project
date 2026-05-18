// IMPORT IMPORTANT LIBRARY
#include <Arduino.h>
#include "LittleFS.h"
#include "BluetoothA2DPSink.h"
#include "driver/i2s.h"
#include "dsps_biquad.h"     // ESP-DSP
#include "dsps_biquad_gen.h" // ESP-DSP coefficient generator
#include <Preferences.h>

#define BTLED 2
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

void wav_task(void *param)
{
  if (strlen(wav_to_play) > 0)
  {
    File file = LittleFS.open(wav_to_play, "r");
    if (file)
    {
      file.seek(44);
      uint8_t raw[512];
      size_t bytes_written;
      while (file.available())
      {
        size_t bytes_read = file.read(raw, sizeof(raw));
        size_t num_samples = bytes_read / 2;

        // Apply volume
        int16_t out[256];
        for (size_t i = 0; i < num_samples; i++)
        {
          int16_t sample = (raw[i * 2 + 1] << 8) | raw[i * 2];
          out[i] = (int16_t)(sample * wav_volume);
        }
        i2s_write(I2S_NUM_0, out, num_samples * 2, &bytes_written, portMAX_DELAY);
      }
      file.close();
    }
    else
    {
      Serial.printf("Gagal buka: %s\n", wav_to_play);
    }
    wav_to_play[0] = '\0';
  }
  vTaskDelete(NULL);
}

void play_wav(const char *filename)
{
  strncpy(wav_to_play, filename, sizeof(wav_to_play));
  xTaskCreate(wav_task, "wav_task", 8192, NULL, 1, NULL);
}

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
}

// ─── AUDIO CALLBACK (DSP CROSSOVER) ──────────────────────────
// Buffer float untuk ESP-DSP (proses per batch)
static float buf_mono[1024];
static float buf_lpf[1024];
static float buf_hpf[1024];

void audio_data_callback(const uint8_t *data, uint32_t len)
{
  size_t num_samples = len / 4;
  int16_t i2s_data[num_samples * 2];

  for (size_t i = 0; i < num_samples; i++)
  {
    int16_t left = (data[i * 4 + 1] << 8) | data[i * 4];
    int16_t right = (data[i * 4 + 3] << 8) | data[i * 4 + 2];

    // Mono mix
    buf_mono[i] = (float)(left + right) / 2.0f;
  }

  // LPF cascade 2x → Woofer (Linkwitz-Riley 24dB/oct)
  dsps_biquad_f32(buf_mono, buf_lpf, num_samples, coeffs_lpf1, w_lpf1);
  dsps_biquad_f32(buf_lpf,  buf_lpf, num_samples, coeffs_lpf2, w_lpf2);

  // HPF cascade 2x → Tweeter (Linkwitz-Riley 24dB/oct)
  dsps_biquad_f32(buf_mono, buf_hpf, num_samples, coeffs_hpf1, w_hpf1);
  dsps_biquad_f32(buf_hpf,  buf_hpf, num_samples, coeffs_hpf2, w_hpf2);

  // Convert float → int16 dengan gain
  for (size_t i = 0; i < num_samples; i++)
  {
    i2s_data[i * 2] = (int16_t)constrain(buf_hpf[i] * gain_tweeter, -32768, 32767);    // R → Tweeter
    i2s_data[i * 2 + 1] = (int16_t)constrain(buf_lpf[i] * gain_woofer, -32768, 32767); // L → Woofer
  }

  size_t i2s_bytes_written;
  i2s_write(I2S_NUM_0, i2s_data, sizeof(int16_t) * num_samples * 2, &i2s_bytes_written, portMAX_DELAY);
}

// Fungsi simpan & load settings
void save_settings()
{
  prefs.begin("speaker", false); // namespace "speaker", mode read-write
  prefs.putFloat("freq", crossover_freq);
  prefs.putFloat("gain_w", gain_woofer);
  prefs.putFloat("gain_t", gain_tweeter);
  prefs.putFloat("wav_vol", wav_volume);
  prefs.end();
  Serial.println("Settings tersimpan!");
}

void load_settings()
{
  prefs.begin("speaker", true);                     // mode read-only
  crossover_freq = prefs.getFloat("freq", 5000.0f); // default 5000 Hz
  gain_woofer = prefs.getFloat("gain_w", 1.0f);     // default 1.0
  gain_tweeter = prefs.getFloat("gain_t", 1.0f);    // default 1.0
  wav_volume = prefs.getFloat("wav_vol", 0.7f);     // default 0.7
  prefs.end();
  Serial.printf("Settings loaded → freq:%.0f gain_w:%.2f gain_t:%.2f wav_vol:%.2f\n",
                crossover_freq, gain_woofer, gain_tweeter, wav_volume);
}

// ─── SERIAL CONTROLLER ────────────────────────────────────────
void handle_serial()
{
  if (!Serial.available())
    return;

  String cmd = Serial.readStringUntil('\n');
  cmd.trim();

  if (cmd.startsWith("freq:"))
  {
    float freq = cmd.substring(5).toFloat();
    if (freq > 100 && freq < 20000)
    {
      crossover_freq = freq;
      update_crossover(crossover_freq);
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
  }
  else if (cmd.startsWith("gain_t:"))
  {
    gain_tweeter = cmd.substring(7).toFloat();
    Serial.printf("Gain tweeter: %.2f\n", gain_tweeter);
  }
  else if (cmd.startsWith("wav_vol:"))
  {
    float vol = cmd.substring(8).toFloat();
    if (vol >= 0.0f && vol <= 1.0f)
    {
      wav_volume = vol;
      Serial.printf("WAV volume: %.0f%%\n", vol * 100);
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
    Serial.printf("Crossover   : %.0f Hz\n", crossover_freq);
    Serial.printf("Gain Woofer : %.2f\n", gain_woofer);
    Serial.printf("Gain Tweeter: %.2f\n", gain_tweeter);
    Serial.printf("WAV Volume  : %.2f\n", wav_volume);
    Serial.printf("BT Connected: %s\n", a2dp_sink.is_connected() ? "Ya" : "Tidak");
  }
  else if (cmd == "help")
  {
    Serial.println("=== Serial Controller ===");
    Serial.println("freq:<Hz>     → ubah crossover (100-20000)");
    Serial.println("gain_w:<val>  → gain woofer  (contoh: 1.2)");
    Serial.println("gain_t:<val>  → gain tweeter (contoh: 0.8)");
    Serial.println("wav_vol:<val> → volume nada wav, antara 0.0 - 1.0");
    Serial.println("status        → lihat setting saat ini");
    Serial.println("save          → simpan setting saat ini");
    Serial.println("reset         → kembali ke setting default");
    Serial.println("help          → tampilkan perintah ini");
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
}