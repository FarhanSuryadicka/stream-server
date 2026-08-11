// Streaming statistics for the measurement harness.
//
// Metric formulas are defined in experiment/docs/01-harness-spec.md §5. They
// are implemented once, here, so every protocol is scored by identical
// arithmetic — a per-protocol reimplementation is exactly how a comparison
// stops being a comparison.

#ifndef UCV_STATS_H
#define UCV_STATS_H

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <deque>
#include <set>
#include <vector>

namespace ucv {

// Percentiles need the full sample set, and a 120 s run at 30 fps is only
// ~3600 samples, so we keep them all rather than approximating. Exact beats
// clever at this scale.
class Distribution {
 public:
  void Add(double v) {
    samples_.push_back(v);
    // Percentile() sorts in place for exact reporting. Live reports call it
    // while samples are still arriving, so every subsequent append makes the
    // vector unsorted again and must invalidate the cached state.
    sorted_ = false;
  }

  bool   empty() const { return samples_.empty(); }
  size_t count() const { return samples_.size(); }

  // Linear-interpolated percentile on the sorted sample set.
  // p is a fraction in [0,1].
  double Percentile(double p) {
    if (samples_.empty()) return 0.0;
    EnsureSorted();
    if (p <= 0.0) return samples_.front();
    if (p >= 1.0) return samples_.back();
    const double idx = p * static_cast<double>(samples_.size() - 1);
    const size_t lo  = static_cast<size_t>(std::floor(idx));
    const size_t hi  = static_cast<size_t>(std::ceil(idx));
    if (lo == hi) return samples_[lo];
    const double frac = idx - static_cast<double>(lo);
    return samples_[lo] * (1.0 - frac) + samples_[hi] * frac;
  }

  double Min() { EnsureSorted(); return samples_.empty() ? 0.0 : samples_.front(); }
  double Max() { EnsureSorted(); return samples_.empty() ? 0.0 : samples_.back(); }

  double Mean() const {
    if (samples_.empty()) return 0.0;
    double sum = 0.0;
    for (double v : samples_) sum += v;
    return sum / static_cast<double>(samples_.size());
  }

  double StdDev() const {
    if (samples_.size() < 2) return 0.0;
    const double m = Mean();
    double acc = 0.0;
    for (double v : samples_) {
      const double d = v - m;
      acc += d * d;
    }
    return std::sqrt(acc / static_cast<double>(samples_.size() - 1));
  }

  const std::vector<double>& samples() const { return samples_; }

 private:
  void EnsureSorted() {
    if (!sorted_) {
      std::sort(samples_.begin(), samples_.end());
      sorted_ = true;
    }
  }
  std::vector<double> samples_;
  bool                sorted_ = false;
};

// RFC 3550 interarrival jitter (harness spec §5.6).
//
// Worth noting: D(i) is a difference of differences, so the phone/PC clock
// offset cancels out entirely. Jitter stays trustworthy even when the
// clock-sync estimate in §4 is imperfect — which is why it is a better
// discriminator than raw one-way latency when two protocols are close.
class JitterEstimator {
 public:
  void Update(int64_t t_sent_ns, int64_t t_recv_ns) {
    if (have_prev_) {
      const int64_t d = (t_recv_ns - prev_recv_ns_) - (t_sent_ns - prev_sent_ns_);
      const double  ad = std::fabs(static_cast<double>(d));
      jitter_ns_ += (ad - jitter_ns_) / 16.0;
    }
    prev_sent_ns_ = t_sent_ns;
    prev_recv_ns_ = t_recv_ns;
    have_prev_    = true;
  }

  double jitter_ms() const { return jitter_ns_ / 1e6; }

 private:
  int64_t prev_sent_ns_ = 0;
  int64_t prev_recv_ns_ = 0;
  double  jitter_ns_    = 0.0;
  bool    have_prev_    = false;
};

// Sequence tracking: loss, reordering, duplicates (harness spec §5.5).
//
// Deliberately does NOT report loss as a percentage: the authoritative
// frames_sent comes from the sender log, because a tail-end loss is invisible
// to gap analysis. This class reports observed gaps only, and the analysis
// step reconciles the two.
class SequenceTracker {
 public:
  // Returns false if the frame is a duplicate and should not be counted.
  bool Observe(uint32_t seq) {
    if (!seen_any_) {
      seen_any_    = true;
      first_seq_   = seq;
      max_seq_     = seq;
      received_++;
      return true;
    }
    if (seq == max_seq_ || (seq < max_seq_ && recent_.count(seq))) {
      duplicates_++;
      return false;
    }
    if (seq < max_seq_) {
      reorder_events_++;
    } else if (seq > max_seq_ + 1) {
      gap_frames_ += (seq - max_seq_ - 1);
      gap_events_++;
    }
    if (seq > max_seq_) max_seq_ = seq;
    Remember(seq);
    received_++;
    return true;
  }

  uint64_t received() const       { return received_; }
  uint64_t gap_frames() const     { return gap_frames_; }
  uint64_t gap_events() const     { return gap_events_; }
  uint64_t reorder_events() const { return reorder_events_; }
  uint64_t duplicates() const     { return duplicates_; }
  uint32_t first_seq() const      { return first_seq_; }
  uint32_t max_seq() const        { return max_seq_; }

 private:
  // Bounded memory of recent sequence numbers for duplicate detection. A
  // full set would grow without limit on a long run; 4096 covers any
  // plausible reordering window on a LAN.
  void Remember(uint32_t seq) {
    recent_.insert(seq);
    order_.push_back(seq);
    if (order_.size() > 4096) {
      recent_.erase(order_.front());
      order_.pop_front();
    }
  }

  bool     seen_any_       = false;
  uint32_t first_seq_      = 0;
  uint32_t max_seq_        = 0;
  uint64_t received_       = 0;
  uint64_t gap_frames_     = 0;
  uint64_t gap_events_     = 0;
  uint64_t reorder_events_ = 0;
  uint64_t duplicates_     = 0;

  std::set<uint32_t>   recent_;
  std::deque<uint32_t> order_;
};

}  // namespace ucv

#endif  // UCV_STATS_H
