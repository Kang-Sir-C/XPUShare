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

#ifndef PREDICTOR_H
#define PREDICTOR_H

#include <pthread.h>

#include <chrono>
#include <deque>

typedef std::chrono::time_point<std::chrono::_V2::steady_clock> timepoint_t;

const int64_t PREDICT_MAX_KEEP = 3000;  // maximum time a record will be kept (in milliseconds)

/**
 * EMA-based record keeper.
 *
 * The original RecordKeeper used a monotone-deque that only kept the running
 * maximum over the last VALID_TIME ms.  This caused burst predictions to be
 * dominated by rare outliers, inflating quota requests far beyond what was
 * actually needed.
 *
 * The new implementation maintains:
 *   - An exponential moving average (EMA) for smooth tracking.
 *   - A decayed maximum that shrinks toward the EMA over time, so a single
 *     spike doesn't pin the prediction high forever.
 *   - A raw sample count for cold-start detection.
 */
class RecordKeeper {
 public:
  RecordKeeper(const int64_t valid_time);
  void add(const double data, const timepoint_t tp);
  void drop_outdated(const timepoint_t tp);
  void clear();

  /// Returns the predicted value: EMA + headroom derived from the decayed max.
  double get_predicted();

  /// Legacy alias — returns the same as get_predicted().
  double get_max();

 private:
  const int64_t VALID_TIME;

  // EMA state
  double ema_;
  double ema_alpha_;       // smoothing factor (0,1)
  int    sample_count_;

  // Decayed-max state
  double decayed_max_;
  timepoint_t last_update_tp_;

  // Decay half-life in milliseconds — the decayed max loses half its excess
  // over the EMA every DECAY_HALF_LIFE_MS ms.
  static constexpr double DECAY_HALF_LIFE_MS = 500.0;
};

class Predictor {
 public:
  Predictor(const char *name = "", const double thres = 0.0);
  ~Predictor();
  void record_stop();
  void record_start();
  void interrupt();
  bool ongoing_unmerged();
  bool ongoing_merged();
  double predict_unmerged();
  double predict_merged();
  void set_upperbound(const double bound);
  void reset();

 private:
  const char *name_;
  // two consecutive period with interval less than this value will be merged
  const double MERGE_THRES;
  pthread_mutex_t mutex_;
  timepoint_t period_begin_;
  timepoint_t long_period_begin_, long_period_end_;
  RecordKeeper normal_records, long_records;
  double upperbound_;
};

#endif
