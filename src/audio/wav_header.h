#pragma once

#include <stdint.h>
#include <string.h>

// Lokasi dan format data PCM di dalam file WAV.
struct WavInfo {
  uint16_t channels = 0;
  uint32_t data_start = 0;
  uint32_t data_size = 0;
};

namespace wav_detail {
inline uint16_t u16(const uint8_t *p) { return p[0] | (p[1] << 8); }
inline uint32_t u32(const uint8_t *p) {
  return p[0] | (p[1] << 8) | (p[2] << 16) | ((uint32_t)p[3] << 24);
}
}  // namespace wav_detail

// Membaca header RIFF/WAVE dan berhenti di awal data PCM. Hanya menerima PCM
// 16-bit 44,1 kHz mono/stereo, sesuai clock I2S. Chunk lain (LIST, dll.) dilewati.
template <typename FileT>
bool read_wav_header(FileT &file, WavInfo &info) {
  using namespace wav_detail;
  uint8_t header[12];
  if (file.read(header, sizeof(header)) != sizeof(header) ||
      memcmp(header, "RIFF", 4) != 0 || memcmp(header + 8, "WAVE", 4) != 0) {
    return false;
  }

  bool format_ok = false;
  uint8_t chunk[8];
  while (file.read(chunk, sizeof(chunk)) == sizeof(chunk)) {
    const uint32_t size = u32(chunk + 4);
    const uint32_t body_start = file.position();

    if (memcmp(chunk, "fmt ", 4) == 0) {
      uint8_t fmt[16];
      if (size < sizeof(fmt) || file.read(fmt, sizeof(fmt)) != sizeof(fmt)) {
        return false;
      }
      const uint16_t audio_format = u16(fmt);  // 1 = PCM, 0xFFFE = extensible
      info.channels = u16(fmt + 2);
      format_ok = (audio_format == 1 || audio_format == 0xFFFE) &&
                  u32(fmt + 4) == 44100 && u16(fmt + 14) == 16 &&
                  (info.channels == 1 || info.channels == 2);
    } else if (memcmp(chunk, "data", 4) == 0) {
      const uint32_t available = file.size() - body_start;
      info.data_start = body_start;
      info.data_size = size < available ? size : available;
      return format_ok;
    }

    if (!file.seek(body_start + size + (size & 1))) {
      return false;
    }
  }
  return false;
}
