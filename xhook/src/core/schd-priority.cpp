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
 * Scheduling priority comparator — v2 rewrite.
 *
 * Original problem:
 *   The old comparator used  missing / (missing + usage)  when both candidates
 *   had positive missing values.  This is a *relative* metric that ignores the
 *   absolute size of each client's request fraction.  A client with
 *   request=0.1 and usage=0.0 (missing=0.1) would rank equally with a client
 *   with request=0.5 and usage=0.0 (missing=0.5), because both have
 *   missing/(missing+usage) = 1.0.  This broke proportional fairness.
 *
 * New approach — deficit-ratio based priority:
 *
 *   deficit_ratio = (require - usage) / require
 *
 *   This normalizes the deficit by each client's own request, so a client
 *   that has received 0% of its request (deficit_ratio=1.0) always beats one
 *   that has received 50% (deficit_ratio=0.5), regardless of absolute sizes.
 *
 *   IMPORTANT: The comparator must be a strict weak ordering (required by
 *   std::sort).  This means NO epsilon-based "tie zones" — they break
 *   transitivity.  Instead, we use exact comparisons with FIFO as the
 *   ultimate tie-breaker (arrived_time is unique per candidate).
 */

#include "scheduler.h"
#include <cmath>

bool schd_priority(const valid_candidate_t &a, const valid_candidate_t &b) {
  // Phase 1: Any client below its guaranteed request beats one that isn't.
  bool a_starved = (a.missing > 0);
  bool b_starved = (b.missing > 0);

  if (a_starved && !b_starved) return true;
  if (!a_starved && b_starved) return false;

  if (a_starved && b_starved) {
    // Phase 2: Both are below their request — higher deficit_ratio wins.
    if (a.deficit_ratio != b.deficit_ratio) {
      return a.deficit_ratio > b.deficit_ratio;
    }
    // Tie-break: FIFO.
    return a.arrived_time < b.arrived_time;
  }

  // Phase 3: Both are above their request — more remaining headroom wins.
  if (a.remaining != b.remaining) {
    return a.remaining > b.remaining;
  }
  // Tie-break: less usage first.
  if (a.usage != b.usage) {
    return a.usage < b.usage;
  }
  // Ultimate tie-break: FIFO (arrived_time is unique).
  return a.arrived_time < b.arrived_time;
}
