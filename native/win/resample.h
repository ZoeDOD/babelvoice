// Streaming linear resampler for one capture or playback consumer.
// One thread at a time. Keeps the fractional input position across chunks.
#pragma once

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <vector>

namespace babelvoice {

class LinearResampler {
 public:
  LinearResampler(double inputRate, double outputRate)
      : step_(inputRate / outputRate) {}

  bool needed() const { return step_ != 1.0; }

  void Process(const int16_t *input, uint32_t frames, std::vector<int16_t> &output) {
    output.clear();
    if (!input || frames == 0) return;
    if (step_ == 1.0) {
      output.assign(input, input + frames);
      prev_ = input[frames - 1];
      primed_ = true;
      return;
    }
    if (!primed_) {
      prev_ = input[0];
      primed_ = true;
    }
    // index_ is the next output's source position. Sample -1 is `prev_`;
    // samples 0..frames-1 are `input`.
    while (index_ < static_cast<double>(frames)) {
      const int i0 = static_cast<int>(std::floor(index_));
      const double frac = index_ - static_cast<double>(i0);
      const int16_t a = i0 < 0 ? prev_ : input[i0];
      const int next = i0 + 1;
      const int16_t b = next < 0 ? prev_ : (next < static_cast<int>(frames) ? input[next] : input[frames - 1]);
      const int sample = static_cast<int>(std::lround(static_cast<double>(a) + (static_cast<double>(b) - a) * frac));
      output.push_back(static_cast<int16_t>(std::clamp(sample, -32768, 32767)));
      index_ += step_;
    }
    prev_ = input[frames - 1];
    index_ -= static_cast<double>(frames);
  }

 private:
  double step_;
  double index_ = 0;
  int16_t prev_ = 0;
  bool primed_ = false;
};

}  // namespace babelvoice
