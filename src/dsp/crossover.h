#pragma once

namespace dsp {

class Crossover {
public:
  bool configure(float sample_rate, float crossover_hz,
                 float bass_shelf_frequency_hz, float bass_shelf_gain_db);
  bool configurePunch(float sample_rate, float high_pass_hz,
                      float punch_frequency_hz, float punch_gain_db);
  void processSample(float input, float woofer_gain, float tweeter_gain,
                     float &woofer, float &tweeter);

private:
  struct Biquad {
    float b0 = 0.0f;
    float b1 = 0.0f;
    float b2 = 0.0f;
    float a1 = 0.0f;
    float a2 = 0.0f;
    float s1 = 0.0f;
    float s2 = 0.0f;

    float process(float input);
    void reset();
  };

  static bool makeLowPass(float sample_rate, float frequency_hz,
                          float q, Biquad &filter);
  static bool makeHighPass(float sample_rate, float frequency_hz,
                           float q, Biquad &filter);
  static bool makeLowShelf(float sample_rate, float frequency_hz,
                           float gain_db, float slope, Biquad &filter);
  static bool makePeaking(float sample_rate, float frequency_hz, float q,
                          float gain_db, Biquad &filter);

  float limit(float sample);

  Biquad low_pass_1_;
  Biquad low_pass_2_;
  Biquad high_pass_1_;
  Biquad high_pass_2_;
  Biquad bass_shelf_;
  Biquad bass_high_pass_;
  Biquad punch_;
  float limiter_envelope_ = 0.0f;
  float limiter_release_ = 0.0f;
};

}  // namespace dsp
