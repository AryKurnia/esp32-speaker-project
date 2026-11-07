#include "crossover.h"

#include <math.h>

#include "dsp_config.h"

namespace dsp {

// Transposed direct form II: jauh lebih presisi di float untuk filter frekuensi
// rendah seperti high-pass 50 Hz.
float Crossover::Biquad::process(float input) {
  const float output = b0 * input + s1;
  s1 = b1 * input - a1 * output + s2;
  s2 = b2 * input - a2 * output;
  return output;
}

void Crossover::Biquad::reset() {
  s1 = 0.0f;
  s2 = 0.0f;
}

bool Crossover::makeLowPass(float sample_rate, float frequency_hz,
                            float q, Biquad &filter) {
  const float w0 = 2.0f * DSP_PI_F * frequency_hz / sample_rate;
  const float cosine = cosf(w0);
  const float alpha = sinf(w0) / (2.0f * q);
  const float a0 = 1.0f + alpha;
  if (!isfinite(a0) || a0 == 0.0f) {
    return false;
  }

  filter.b0 = ((1.0f - cosine) * 0.5f) / a0;
  filter.b1 = (1.0f - cosine) / a0;
  filter.b2 = filter.b0;
  filter.a1 = (-2.0f * cosine) / a0;
  filter.a2 = (1.0f - alpha) / a0;
  filter.reset();
  return true;
}

bool Crossover::makeHighPass(float sample_rate, float frequency_hz,
                             float q, Biquad &filter) {
  const float w0 = 2.0f * DSP_PI_F * frequency_hz / sample_rate;
  const float cosine = cosf(w0);
  const float alpha = sinf(w0) / (2.0f * q);
  const float a0 = 1.0f + alpha;
  if (!isfinite(a0) || a0 == 0.0f) {
    return false;
  }

  filter.b0 = ((1.0f + cosine) * 0.5f) / a0;
  filter.b1 = -(1.0f + cosine) / a0;
  filter.b2 = filter.b0;
  filter.a1 = (-2.0f * cosine) / a0;
  filter.a2 = (1.0f - alpha) / a0;
  filter.reset();
  return true;
}

bool Crossover::makeLowShelf(float sample_rate, float frequency_hz,
                             float gain_db, float slope, Biquad &filter) {
  if (!isfinite(gain_db) || !isfinite(slope) || slope <= 0.0f) {
    return false;
  }

  const float a = powf(10.0f, gain_db / 40.0f);
  const float w0 = 2.0f * DSP_PI_F * frequency_hz / sample_rate;
  const float cosine = cosf(w0);
  const float sine = sinf(w0);
  const float alpha = (sine * 0.5f) *
      sqrtf((a + 1.0f / a) * (1.0f / slope - 1.0f) + 2.0f);
  const float beta = 2.0f * sqrtf(a) * alpha;
  const float a0 = (a + 1.0f) + (a - 1.0f) * cosine + beta;
  if (!isfinite(a0) || a0 == 0.0f) {
    return false;
  }

  filter.b0 = a * ((a + 1.0f) - (a - 1.0f) * cosine + beta) / a0;
  filter.b1 = 2.0f * a * ((a - 1.0f) - (a + 1.0f) * cosine) / a0;
  filter.b2 = a * ((a + 1.0f) - (a - 1.0f) * cosine - beta) / a0;
  filter.a1 = -2.0f * ((a - 1.0f) + (a + 1.0f) * cosine) / a0;
  filter.a2 = ((a + 1.0f) + (a - 1.0f) * cosine - beta) / a0;
  filter.reset();
  return true;
}

bool Crossover::makePeaking(float sample_rate, float frequency_hz, float q,
                            float gain_db, Biquad &filter) {
  const float a = powf(10.0f, gain_db / 40.0f);
  const float w0 = 2.0f * DSP_PI_F * frequency_hz / sample_rate;
  const float cosine = cosf(w0);
  const float alpha = sinf(w0) / (2.0f * q);
  const float a0 = 1.0f + alpha / a;
  if (!isfinite(a0) || a0 == 0.0f) {
    return false;
  }

  filter.b0 = (1.0f + alpha * a) / a0;
  filter.b1 = (-2.0f * cosine) / a0;
  filter.b2 = (1.0f - alpha * a) / a0;
  filter.a1 = filter.b1;
  filter.a2 = (1.0f - alpha / a) / a0;
  filter.reset();
  return true;
}

bool Crossover::configure(float sample_rate, float crossover_hz,
                          float bass_shelf_frequency_hz,
                          float bass_shelf_gain_db) {
  if (!isfinite(sample_rate) || !isfinite(crossover_hz) ||
      !isfinite(bass_shelf_frequency_hz) || !isfinite(bass_shelf_gain_db) ||
      sample_rate <= 0.0f || crossover_hz <= 0.0f ||
      crossover_hz >= sample_rate * 0.5f ||
      bass_shelf_frequency_hz < kMinBassShelfFrequencyHz ||
      bass_shelf_frequency_hz > kMaxBassShelfFrequencyHz ||
      bass_shelf_frequency_hz >= sample_rate * 0.5f ||
      bass_shelf_gain_db < kMinBassShelfGainDb ||
      bass_shelf_gain_db > kMaxBassShelfGainDb) {
    return false;
  }

  Biquad low_pass_1;
  Biquad low_pass_2;
  Biquad high_pass_1;
  Biquad high_pass_2;
  Biquad bass_shelf;
  if (!makeLowPass(sample_rate, crossover_hz, kCrossoverQ, low_pass_1) ||
      !makeLowPass(sample_rate, crossover_hz, kCrossoverQ, low_pass_2) ||
      !makeHighPass(sample_rate, crossover_hz, kCrossoverQ, high_pass_1) ||
      !makeHighPass(sample_rate, crossover_hz, kCrossoverQ, high_pass_2) ||
      !makeLowShelf(sample_rate, bass_shelf_frequency_hz, bass_shelf_gain_db,
                    kBassShelfSlope, bass_shelf)) {
    return false;
  }

  low_pass_1_ = low_pass_1;
  low_pass_2_ = low_pass_2;
  high_pass_1_ = high_pass_1;
  high_pass_2_ = high_pass_2;
  bass_shelf_ = bass_shelf;
  return true;
}

bool Crossover::configurePunch(float sample_rate, float high_pass_hz,
                               float punch_frequency_hz, float punch_gain_db) {
  if (!isfinite(sample_rate) || !isfinite(high_pass_hz) ||
      !isfinite(punch_frequency_hz) || !isfinite(punch_gain_db) ||
      sample_rate <= 0.0f ||
      high_pass_hz < kMinBassHighPassHz ||
      high_pass_hz > kMaxBassHighPassHz ||
      punch_frequency_hz < kMinPunchFrequencyHz ||
      punch_frequency_hz > kMaxPunchFrequencyHz ||
      punch_gain_db < kMinPunchGainDb ||
      punch_gain_db > kMaxPunchGainDb) {
    return false;
  }

  Biquad bass_high_pass;
  Biquad punch;
  if (!makeHighPass(sample_rate, high_pass_hz, kBassHighPassQ, bass_high_pass) ||
      !makePeaking(sample_rate, punch_frequency_hz, kPunchQ, punch_gain_db,
                   punch)) {
    return false;
  }

  bass_high_pass_ = bass_high_pass;
  punch_ = punch;
  limiter_release_ = expf(-1.0f / (kLimiterReleaseSeconds * sample_rate));
  return true;
}

float Crossover::limit(float sample) {
  limiter_envelope_ = fmaxf(fabsf(sample), limiter_envelope_ * limiter_release_);
  if (limiter_envelope_ <= kLimiterThreshold) {
    return sample;
  }
  return sample * (kLimiterThreshold / limiter_envelope_);
}

void Crossover::processSample(float input, float woofer_gain,
                              float tweeter_gain, float &woofer,
                              float &tweeter) {
  const float low = bass_shelf_.process(
      low_pass_2_.process(low_pass_1_.process(input)));
  const float punched = punch_.process(bass_high_pass_.process(low));
  const float high = high_pass_2_.process(high_pass_1_.process(input));
  woofer = limit(punched * woofer_gain * DSP_BASS_GAIN_BASE * DSP_BASS_GAIN_BOOST);
  tweeter = high * tweeter_gain * DSP_TREBLE_GAIN;
}

}  // namespace dsp
