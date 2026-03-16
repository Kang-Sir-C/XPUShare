/**
 * Copyright 2020 Hung-Hsin Chen, LSA Lab, National Tsing Hua University
 * Copyright 2025 XPUShare Authors
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * You may obtain a copy of the License at
 *
 *     http://www.apache.org/licenses/LICENSE-2.0
 */

/**
 * Kernel burst and window period measurement/prediction utilities.
 *
 * v2 rewrite — key changes from the v1 predictor:
 *
 *   1. RecordKeeper now uses EMA + decayed-max instead of a monotone-deque
 *      that only tracked the running maximum.  The old approach caused a
 *      single long burst to pin the prediction high for the entire 3-second
 *      window, inflating every subsequent quota request.
 *
 *   2. The decayed-max shrinks toward the EMA with a configurable half-life
 *      (500 ms default), so outliers fade quickly while sustained patterns
 *      are tracked accurately.
 *
 *   3. get_predicted() returns  EMA + 0.5 * (decayed_max - EMA)  which gives
 *      a moderate headroom above the average without being dominated by
 *      spikes.  During cold-start (< 4 samples) it falls back to the raw
 *      decayed_max to avoid under-prediction.
 */

#include "predictor.h"

#include <cmath>
#include <climits>
#include <limits>
#include "debug.h"

// C++11 requires an out-of-class definition for static constexpr members
// that are ODR-used (e.g. passed by reference).  C++17 makes this implicit.
constexpr double RecordKeeper::DECAY_HALF_LIFE_MS;

using std::make_pair;
using std::chrono::duration_cast;
using std::chrono::microseconds;
using std::chrono::milliseconds;
using std::chrono::steady_clock;

// ---------------------------------------------------------------------------
// RecordKeeper
// ---------------------------------------------------------------------------

RecordKeeper::RecordKeeper(const int64_t valid_time)
    : VALID_TIME(valid_time),
      ema_(0.0),
      ema_alpha_(0.3),
      sample_count_(0),
      decayed_max_(0.0),
      last_update_tp_(timepoint_t::min()) {}

void RecordKeeper::add(const double data, const timepoint_t tp) {
  // --- Decay the max toward EMA based on elapsed time ---
  if (sample_count_ > 0 && last_update_tp_ != timepoint_t::min()) {
    double elapsed_ms =
        duration_cast<microseconds>(tp - last_update_tp_).count() / 1e3;
    if (elapsed_ms > 0.0 && DECAY_HALF_LIFE_MS > 0.0) {
      // decay_factor = 0.5 ^ (elapsed / half_life)
      double decay = std::pow(0.5, elapsed_ms / DECAY_HALF_LIFE_MS);
      // Shrink decayed_max toward ema_
      decayed_max_ = ema_ + (decayed_max_ - ema_) * decay;
    }
  }

  // --- Update EMA ---
  if (sample_count_ == 0) {
    ema_ = data;
  } else {
    ema_ = ema_alpha_ * data + (1.0 - ema_alpha_) * ema_;
  }
  // Prevent signed integer overflow (UB in C++).  Clamp before it happens.
  if (sample_count_ < INT_MAX) {
    sample_count_++;
  }

  // --- Update decayed max ---
  if (data > decayed_max_) {
    decayed_max_ = data;
  }

  last_update_tp_ = tp;
}

void RecordKeeper::drop_outdated(const timepoint_t tp) {
  // Apply time-based decay even when no new sample arrives.
  if (sample_count_ > 0 && last_update_tp_ != timepoint_t::min()) {
    double elapsed_ms =
        duration_cast<microseconds>(tp - last_update_tp_).count() / 1e3;
    if (elapsed_ms > VALID_TIME) {
      // All data is stale — reset.
      clear();
      return;
    }
    if (elapsed_ms > 0.0 && DECAY_HALF_LIFE_MS > 0.0) {
      double decay = std::pow(0.5, elapsed_ms / DECAY_HALF_LIFE_MS);
      decayed_max_ = ema_ + (decayed_max_ - ema_) * decay;
    }
  }
}

double RecordKeeper::get_predicted() {
  if (sample_count_ == 0) return 0.0;

  // Cold-start: not enough samples for a reliable EMA.
  if (sample_count_ < 4) return decayed_max_;

  // Blend: EMA + half the headroom from decayed_max.
  double headroom = decayed_max_ - ema_;
  if (headroom < 0.0) headroom = 0.0;
  return ema_ + 0.5 * headroom;
}

double RecordKeeper::get_max() {
  // Legacy alias.
  return get_predicted();
}

void RecordKeeper::clear() {
  ema_ = 0.0;
  sample_count_ = 0;
  decayed_max_ = 0.0;
  last_update_tp_ = timepoint_t::min();
}

// ---------------------------------------------------------------------------
// Predictor  (logic unchanged except it now benefits from the better
//             RecordKeeper underneath)
// ---------------------------------------------------------------------------

Predictor::Predictor(const char *name, const double thres)
    : MERGE_THRES(thres), normal_records(PREDICT_MAX_KEEP), long_records(PREDICT_MAX_KEEP) {
  mutex_ = PTHREAD_MUTEX_INITIALIZER;
  period_begin_ = timepoint_t::max();
  long_period_begin_ = timepoint_t::max();
  long_period_end_ = timepoint_t::min();
  upperbound_ = std::numeric_limits<double>::max();
  name_ = name;
}

Predictor::~Predictor() { pthread_mutex_destroy(&mutex_); }

bool Predictor::ongoing_unmerged() { return period_begin_ != timepoint_t::max(); }

bool Predictor::ongoing_merged() { return long_period_begin_ != timepoint_t::max(); }

void Predictor::record_stop() {
#ifndef NO_PREDICT
  double duration;
  timepoint_t tp;
  char* log_name = "/xpushare/log/predictor.log";
  pthread_mutex_lock(&mutex_);
  if (ongoing_unmerged()) {
    tp = steady_clock::now();
    duration = duration_cast<microseconds>(tp - period_begin_).count() / 1e3;
    normal_records.add(duration, tp);
    long_period_end_ = tp;
    long_records.add(
        duration_cast<microseconds>(long_period_end_ - long_period_begin_).count() / 1e3, tp);
    hDEBUG(log_name, __FILE__, (long)__LINE__, "%s: record stop (length: %.3f ms)", name_, duration);
  }
  period_begin_ = timepoint_t::max();
  pthread_mutex_unlock(&mutex_);
#endif
}

void Predictor::record_start() {
#ifndef NO_PREDICT
  double intv;
  char* log_name = "/xpushare/log/predictor.log";
  pthread_mutex_lock(&mutex_);
  if (!ongoing_unmerged()) {
    period_begin_ = steady_clock::now();

    intv = duration_cast<microseconds>(period_begin_ - long_period_end_).count() / 1e3;
    if (!ongoing_merged() || intv > MERGE_THRES) {
      long_period_begin_ = period_begin_;
      long_period_end_ = timepoint_t::min();
    }

    hDEBUG(log_name, __FILE__, (long)__LINE__, "%s: record start", name_);
  }
  pthread_mutex_unlock(&mutex_);
#endif
}

void Predictor::interrupt() {
#ifndef NO_PREDICT
  char* log_name = "/xpushare/log/predictor.log";
  pthread_mutex_lock(&mutex_);
  period_begin_ = timepoint_t::max();
  long_period_begin_ = timepoint_t::max();
  long_period_end_ = timepoint_t::min();
  hDEBUG(log_name, __FILE__, (long)__LINE__, "%s: interrupted", name_);
  pthread_mutex_unlock(&mutex_);
#endif
}

double Predictor::predict_unmerged() {
  double pred = 0.0;
#ifndef NO_PREDICT
  pthread_mutex_lock(&mutex_);
  normal_records.drop_outdated(steady_clock::now());
  pred = normal_records.get_predicted();
  pthread_mutex_unlock(&mutex_);
#endif
  return pred;
}

double Predictor::predict_merged() {
  double pred = 0.0;
#ifndef NO_PREDICT
  pthread_mutex_lock(&mutex_);
  long_records.drop_outdated(steady_clock::now());
  pred = long_records.get_predicted();
  pthread_mutex_unlock(&mutex_);
#endif
  return pred;
}

void Predictor::set_upperbound(const double bound) {
#ifndef NO_PREDICT
  pthread_mutex_lock(&mutex_);
  upperbound_ = bound;
  pthread_mutex_unlock(&mutex_);
#endif
}

void Predictor::reset() {
#ifndef NO_PREDICT
  pthread_mutex_lock(&mutex_);
  normal_records.clear();
  long_records.clear();
  period_begin_ = timepoint_t::max();
  long_period_begin_ = timepoint_t::max();
  long_period_end_ = timepoint_t::min();
  pthread_mutex_unlock(&mutex_);
#endif
}
