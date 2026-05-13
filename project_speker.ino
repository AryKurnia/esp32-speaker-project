// IMPORT IMPORTANT LIBRARY
#include "LittleFS.h"
#include "BluetoothA2DPSink.h"
#include "driver/i2s.h"

#define BTLED 2
BluetoothA2DPSink a2dp_sink; // Initialize Library

// config your I2S PIN
i2s_pin_config_t pin_config = {
    // .mck_io_num = 3,                 // MCLK pin, ONLY USE gpio 0,1,or 3 otherwise not work
    .mck_io_num   = I2S_PIN_NO_CHANGE,
    .bck_io_num   = 25,                // GPIO25 → BCK  (Pin 9 kiri)
    .ws_io_num    = 26,                // GPIO26 → LCK  (Pin 10 kiri)
    .data_out_num = 22,                // GPIO22 → DIN  (Pin 36 kanan)
    .data_in_num = I2S_PIN_NO_CHANGE // Not used
};

// Custom I2S configuration
i2s_config_t i2s_config_stereo = {
    .mode = (i2s_mode_t)(I2S_MODE_MASTER | I2S_MODE_TX),                    // TX only
    .sample_rate = 44100,                                                   // Sample rate
    .bits_per_sample = I2S_BITS_PER_SAMPLE_16BIT,                           // Bits per sample
    .channel_format = I2S_CHANNEL_FMT_RIGHT_LEFT,                           // Channel format
    .communication_format = (i2s_comm_format_t)(I2S_COMM_FORMAT_STAND_I2S), // Communication format
    .intr_alloc_flags = 0,                                                  // Interrupt allocation
    .dma_buf_count = 8,                                                     // DMA buffer count
    .dma_buf_len = 1024,                                                    // DMA buffer length
    .use_apll = false,                                                       // Use APLL
    .tx_desc_auto_clear = true,                                             // Auto clear tx descriptor on underflow
    .fixed_mclk = 0                                                         // Konfigurasi MCLK ke 11.2896 MHz
};

// ← Simpan nama file yang akan diputar
static char wav_to_play[32] = "";

// Task terpisah untuk putar WAV
void wav_task(void *param) {
  if (strlen(wav_to_play) > 0) {
    File file = LittleFS.open(wav_to_play, "r");
    if (file) {
      file.seek(44); // skip WAV header
      uint8_t buffer[1024];
      size_t bytes_written;
      while (file.available()) {
        size_t bytes_read = file.read(buffer, sizeof(buffer));
        i2s_write(I2S_NUM_0, buffer, bytes_read, &bytes_written, portMAX_DELAY);
      }
      file.close();
    } else {
      Serial.printf("Gagal buka: %s\n", wav_to_play);
    }
    wav_to_play[0] = '\0'; // reset
  }
  vTaskDelete(NULL); // hapus task setelah selesai
}

// Panggil ini untuk putar WAV di task baru
void play_wav(const char* filename) {
  strncpy(wav_to_play, filename, sizeof(wav_to_play));
  xTaskCreate(wav_task, "wav_task", 8192, NULL, 1, NULL);
}

// Callback saat status BT berubah
void bt_connection_state_changed(esp_a2d_connection_state_t state, void *ptr) {
  if (state == ESP_A2D_CONNECTION_STATE_CONNECTED) {
    // Suara konek: 2 nada naik
    play_wav("/connected.wav");
    Serial.println("Bluetooth Connected");

  } else if (state == ESP_A2D_CONNECTION_STATE_DISCONNECTED) {
    // Suara putus: 2 nada turun
    play_wav("/disconnected.wav");
    Serial.println("Bluetooth Disconnected");
  }
}

// change 8bit format to 16bit format, easier to process later
void audio_data_callback(const uint8_t *data, uint32_t len) // BT data on 8bit format
{
  // Serial.printf("Audio diterima: %d bytes\n", len);

  size_t num_samples = len / 4;      // LLSB, LMSB, RLSB, RMSB, so divide by 4 per point
  int16_t i2s_data[num_samples * 2]; // Create a temporary buffer for 16-bit stereo samples

  for (size_t i = 0; i < num_samples; i++)
  {
    // Convert each stereo sample from uint8_t to int16_t
    int16_t left = (data[i * 4 + 1] << 8) | data[i * 4];      // Left channel
    int16_t right = (data[i * 4 + 3] << 8) | data[i * 4 + 2]; // Right channel

    // Store the converted data in the i2s_data buffer
    i2s_data[i * 2] = right; 
    i2s_data[i * 2 + 1] = left;
  }
  size_t i2s_bytes_written;
  i2s_write(I2S_NUM_0, i2s_data, sizeof(i2s_data), &i2s_bytes_written, portMAX_DELAY); // sent to DAC
}

void setup()
{ 
  Serial.begin(115200); // disable if not used please, since pin RX/gpio 3 is used for MCLK

  // Init LittleFS
  if (!LittleFS.begin(true)) {
    Serial.println("LittleFS Mount Failed!");
    return;
  }
  Serial.println("LittleFS OK");

  pinMode(BTLED, OUTPUT); // Set BTLED as output

  i2s_driver_install(I2S_NUM_0, &i2s_config_stereo, 0, NULL); // install i2s config
  i2s_set_pin(I2S_NUM_0, &pin_config);                        // configure pin

  a2dp_sink.set_on_connection_state_changed(bt_connection_state_changed); 
  a2dp_sink.set_stream_reader(audio_data_callback, false); // if audio received, run this program
  a2dp_sink.set_auto_reconnect(true);                      // remember last device and try to connect
  a2dp_sink.start("Speaker Mahal 😁");                  // Speaker name on BT
}

void loop()
{
  if (a2dp_sink.is_connected())
  {
    digitalWrite(BTLED, HIGH); // Turn on LED when connected
  }
  else
  {
    digitalWrite(BTLED, LOW); // Turn off LED when disconnected
  }
}