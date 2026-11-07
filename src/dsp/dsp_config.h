#pragma once

// Fixed DSP gains adapted from the ESP32-EDF reference.
#define DSP_BASS_GAIN_BASE  1.0f
#define DSP_BASS_GAIN_BOOST 1.0f
#define DSP_TREBLE_GAIN     1.0f
#define DSP_PI_F            3.14159265f

namespace dsp {
constexpr float kCrossoverQ = 0.7071f;
constexpr float kDefaultSampleRate = 44100.0f;
constexpr float kDefaultBassShelfFrequencyHz = 150.0f;
constexpr float kDefaultBassShelfGainDb = 2.0f;
constexpr float kMinBassShelfFrequencyHz = 40.0f;
constexpr float kMaxBassShelfFrequencyHz = 300.0f;
constexpr float kMinBassShelfGainDb = -6.0f;
constexpr float kMaxBassShelfGainDb = 6.0f;
constexpr float kBassShelfSlope = 1.0f;

// Default bass punch untuk woofer 4-5 inci.
constexpr float kBassHighPassQ = 0.7071f;
constexpr float kDefaultBassHighPassHz = 50.0f;
constexpr float kMinBassHighPassHz = 30.0f;
constexpr float kMaxBassHighPassHz = 100.0f;
constexpr float kPunchQ = 1.0f;
constexpr float kDefaultPunchFrequencyHz = 80.0f;
constexpr float kDefaultPunchGainDb = 4.0f;
constexpr float kMinPunchFrequencyHz = 50.0f;
constexpr float kMaxPunchFrequencyHz = 150.0f;
constexpr float kMinPunchGainDb = 0.0f;
constexpr float kMaxPunchGainDb = 8.0f;

// Limiter woofer: puncak ditahan di -1 dBFS agar boost bass tidak clipping.
constexpr float kLimiterThreshold = 29204.0f;
constexpr float kLimiterReleaseSeconds = 0.15f;
}
