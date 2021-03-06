/*
 *  Copyright (c) 2017 The WebRTC project authors. All Rights Reserved.
 *
 *  Use of this source code is governed by a BSD-style license
 *  that can be found in the LICENSE file in the root of the source
 *  tree. An additional intellectual property rights grant can be found
 *  in the file PATENTS.  All contributing project authors may
 *  be found in the AUTHORS file in the root of the source tree.
 */

#include "modules/audio_processing/aec3/subtractor.h"

#include <algorithm>
#include <numeric>

#include "api/array_view.h"
#include "modules/audio_processing/logging/apm_data_dumper.h"
#include "rtc_base/checks.h"
#include "rtc_base/numerics/safe_minmax.h"

namespace webrtc {

namespace {

void PredictionError(const Aec3Fft& fft,
                     const FftData& S,
                     rtc::ArrayView<const float> y,
                     std::array<float, kBlockSize>* e,
                     std::array<float, kBlockSize>* s) {
  std::array<float, kFftLength> tmp;
  fft.Ifft(S, &tmp);
  constexpr float kScale = 1.0f / kFftLengthBy2;
  std::transform(y.begin(), y.end(), tmp.begin() + kFftLengthBy2, e->begin(),
                 [&](float a, float b) { return a - b * kScale; });

  if (s) {
    for (size_t k = 0; k < s->size(); ++k) {
      (*s)[k] = kScale * tmp[k + kFftLengthBy2];
    }
  }

  std::for_each(e->begin(), e->end(),
                [](float& a) { a = rtc::SafeClamp(a, -32768.f, 32767.f); });
}

}  // namespace

Subtractor::Subtractor(const EchoCanceller3Config& config,
                       ApmDataDumper* data_dumper,
                       Aec3Optimization optimization)
    : fft_(),
      data_dumper_(data_dumper),
      optimization_(optimization),
      main_filter_(config.filter.length_blocks, optimization, data_dumper_),
      shadow_filter_(config.filter.length_blocks, optimization, data_dumper_),
      G_main_(config.filter.leakage_converged,
              config.filter.leakage_diverged,
              config.filter.main_noise_gate,
              config.filter.error_floor),
      G_shadow_(config.filter.shadow_rate, config.filter.shadow_noise_gate) {
  RTC_DCHECK(data_dumper_);
}

Subtractor::~Subtractor() = default;

void Subtractor::HandleEchoPathChange(
    const EchoPathVariability& echo_path_variability) {
  const auto full_reset = [&]() {
    main_filter_.HandleEchoPathChange();
    shadow_filter_.HandleEchoPathChange();
    G_main_.HandleEchoPathChange(echo_path_variability);
    G_shadow_.HandleEchoPathChange();
    converged_filter_ = false;
  };

  // TODO(peah): Add delay-change specific reset behavior.
  if ((echo_path_variability.delay_change ==
       EchoPathVariability::DelayAdjustment::kBufferFlush) ||
      (echo_path_variability.delay_change ==
       EchoPathVariability::DelayAdjustment::kDelayReset)) {
    full_reset();
  } else if (echo_path_variability.delay_change ==
             EchoPathVariability::DelayAdjustment::kNewDetectedDelay) {
    full_reset();
  } else if (echo_path_variability.delay_change ==
             EchoPathVariability::DelayAdjustment::kBufferReadjustment) {
    full_reset();
  }
}

void Subtractor::Process(const RenderBuffer& render_buffer,
                         const rtc::ArrayView<const float> capture,
                         const RenderSignalAnalyzer& render_signal_analyzer,
                         const AecState& aec_state,
                         SubtractorOutput* output) {
  RTC_DCHECK_EQ(kBlockSize, capture.size());
  rtc::ArrayView<const float> y = capture;
  FftData& E_main = output->E_main;
  FftData& E_main_nonwindowed = output->E_main_nonwindowed;
  FftData E_shadow;
  std::array<float, kBlockSize>& e_main = output->e_main;
  std::array<float, kBlockSize>& e_shadow = output->e_shadow;

  FftData S;
  FftData& G = S;

  // Form the output of the main filter.
  main_filter_.Filter(render_buffer, &S);
  PredictionError(fft_, S, y, &e_main, &output->s_main);
  fft_.ZeroPaddedFft(e_main, Aec3Fft::Window::kHanning, &E_main);
  fft_.ZeroPaddedFft(e_main, Aec3Fft::Window::kRectangular,
                     &E_main_nonwindowed);

  // Form the output of the shadow filter.
  shadow_filter_.Filter(render_buffer, &S);
  PredictionError(fft_, S, y, &e_shadow, nullptr);
  fft_.ZeroPaddedFft(e_shadow, Aec3Fft::Window::kHanning, &E_shadow);

  if (!converged_filter_) {
    const auto sum_of_squares = [](float a, float b) { return a + b * b; };
    const float e2_main =
        std::accumulate(e_main.begin(), e_main.end(), 0.f, sum_of_squares);
    const float e2_shadow =
        std::accumulate(e_shadow.begin(), e_shadow.end(), 0.f, sum_of_squares);
    const float y2 = std::accumulate(y.begin(), y.end(), 0.f, sum_of_squares);

    if (y2 > kBlockSize * 50.f * 50.f) {
      converged_filter_ = (e2_main > 0.3 * y2 || e2_shadow > 0.1 * y2);
    }
  }

  // Compute spectra for future use.
  E_main.Spectrum(optimization_, output->E2_main);
  E_main_nonwindowed.Spectrum(optimization_, output->E2_main_nonwindowed);
  E_shadow.Spectrum(optimization_, output->E2_shadow);

  // Update the main filter.
  std::array<float, kFftLengthBy2Plus1> X2;
  render_buffer.SpectralSum(main_filter_.SizePartitions(), &X2);
  G_main_.Compute(X2, render_signal_analyzer, *output, main_filter_,
                  aec_state.SaturatedCapture(), &G);
  main_filter_.Adapt(render_buffer, G);
  data_dumper_->DumpRaw("aec3_subtractor_G_main", G.re);
  data_dumper_->DumpRaw("aec3_subtractor_G_main", G.im);

  // Update the shadow filter.
  if (shadow_filter_.SizePartitions() != main_filter_.SizePartitions()) {
    render_buffer.SpectralSum(shadow_filter_.SizePartitions(), &X2);
  }
  G_shadow_.Compute(X2, render_signal_analyzer, E_shadow,
                    shadow_filter_.SizePartitions(),
                    aec_state.SaturatedCapture(), &G);
  shadow_filter_.Adapt(render_buffer, G);

  data_dumper_->DumpRaw("aec3_subtractor_G_shadow", G.re);
  data_dumper_->DumpRaw("aec3_subtractor_G_shadow", G.im);

  main_filter_.DumpFilter("aec3_subtractor_H_main");
  shadow_filter_.DumpFilter("aec3_subtractor_H_shadow");
}

}  // namespace webrtc
