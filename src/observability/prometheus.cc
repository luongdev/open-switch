/*
 * src/observability/prometheus.cc
 *
 * Prometheus text exposition format rendering.
 * See include/osw/observability/prometheus.h for the public contract.
 *
 * SPDX-License-Identifier: AGPL-3.0-or-later
 */

#include "osw/observability/prometheus.h"

#include <algorithm>
#include <cstdint>
#include <sstream>
#include <string>

#include "osw/observability/log.h"

namespace osw::observability::prometheus {

namespace {
constexpr const char* kSubsystem = "metrics.prometheus";
}

// ---------------------------------------------------------------------------
// Label rendering
// ---------------------------------------------------------------------------

std::string RenderLabels(const Labels& labels) {
    if (labels.empty()) {
        return {};
    }
    std::string out;
    out.reserve(64);
    out += '{';
    for (std::size_t i = 0; i < labels.size(); ++i) {
        if (i > 0) {
            out += ',';
        }
        out += labels[i].key;
        out += "=\"";
        // Escape backslash, double-quote and newline in label values per
        // the Prometheus text format spec.
        for (char c : labels[i].value) {
            if (c == '\\') {
                out += "\\\\";
            } else if (c == '"') {
                out += "\\\"";
            } else if (c == '\n') {
                out += "\\n";
            } else {
                out += c;
            }
        }
        out += '"';
    }
    out += '}';
    return out;
}

// ---------------------------------------------------------------------------
// Counter
// ---------------------------------------------------------------------------

void Counter::Render(std::string& out) const {
    out += "# HELP ";
    out += name_;
    out += ' ';
    out += help_;
    out += '\n';
    out += "# TYPE ";
    out += name_;
    out += " counter\n";
    out += name_;
    out += RenderLabels(labels_);
    out += ' ';
    out += std::to_string(value_.load(std::memory_order_relaxed));
    out += '\n';
}

// ---------------------------------------------------------------------------
// Gauge
// ---------------------------------------------------------------------------

void Gauge::Render(std::string& out) const {
    out += "# HELP ";
    out += name_;
    out += ' ';
    out += help_;
    out += '\n';
    out += "# TYPE ";
    out += name_;
    out += " gauge\n";
    out += name_;
    out += RenderLabels(labels_);
    out += ' ';
    out += std::to_string(value_.load(std::memory_order_relaxed));
    out += '\n';
}

// ---------------------------------------------------------------------------
// Histogram
// ---------------------------------------------------------------------------

Histogram::Histogram(std::string name,
                     std::string help,
                     Labels labels,
                     const std::vector<double>& bounds)
    : name_(std::move(name)),
      help_(std::move(help)),
      labels_(std::move(labels)),
      bounds_(bounds),
      // +1 slot for the implicit +Inf bucket
      bucket_counts_(bounds.size() + 1) {
    for (auto& b : bucket_counts_) {
        b.store(0, std::memory_order_relaxed);
    }
    if (!std::is_sorted(bounds_.begin(), bounds_.end())) {
        osw::log::Warn(kSubsystem,
                       "Histogram '%s': bounds are not sorted; results will be incorrect",
                       name_.c_str());
    }
}

void Histogram::Observe(double value) noexcept {
    // Find the first bucket whose upper bound >= value and increment all
    // cumulative buckets from that point through +Inf. Prometheus
    // histograms are cumulative: bucket[i] counts observations <= bound[i].
    for (std::size_t i = 0; i < bounds_.size(); ++i) {
        if (value <= bounds_[i]) {
            bucket_counts_[i].fetch_add(1, std::memory_order_relaxed);
        }
    }
    // Always increment the +Inf bucket.
    bucket_counts_.back().fetch_add(1, std::memory_order_relaxed);
    count_.fetch_add(1, std::memory_order_relaxed);

    // Convert to nanoseconds and accumulate into sum_ns_. This is an
    // approximation (integer arithmetic on a double value) but is
    // acceptable for a scrape endpoint.
    const auto ns = static_cast<std::uint64_t>(value * 1e9);
    sum_ns_.fetch_add(ns, std::memory_order_relaxed);
}

std::vector<std::uint64_t> Histogram::GetBucketCounts() const {
    std::vector<std::uint64_t> out;
    out.reserve(bucket_counts_.size());
    for (const auto& b : bucket_counts_) {
        out.push_back(b.load(std::memory_order_relaxed));
    }
    return out;
}

double Histogram::GetSum() const noexcept {
    return static_cast<double>(sum_ns_.load(std::memory_order_relaxed)) / 1e9;
}

void Histogram::Render(std::string& out) const {
    out += "# HELP ";
    out += name_;
    out += '_';
    out += "bucket ";
    out += help_;
    out += '\n';
    out += "# TYPE ";
    out += name_;
    out += " histogram\n";

    const std::string base_labels = RenderLabels(labels_);

    // Per-bucket lines. For each finite bound we render
    // name_bucket{le="<bound>",<extra>} <count>
    for (std::size_t i = 0; i < bounds_.size(); ++i) {
        out += name_;
        out += "_bucket{le=\"";
        // Format the bound value: use a compact decimal representation.
        // std::to_string on doubles can produce trailing zeros; use
        // a stringstream with reduced precision for clean output.
        std::ostringstream ss;
        ss << bounds_[i];
        out += ss.str();
        out += '"';
        // Append extra labels (if any) after the le label.
        if (!labels_.empty()) {
            out += ',';
            // Strip the leading '{' from base_labels and append the rest.
            out += std::string_view(base_labels).substr(1);
        } else {
            out += '}';
        }
        out += ' ';
        out += std::to_string(bucket_counts_[i].load(std::memory_order_relaxed));
        out += '\n';
    }

    // +Inf bucket
    out += name_;
    out += "_bucket{le=\"+Inf\"";
    if (!labels_.empty()) {
        out += ',';
        out += std::string_view(base_labels).substr(1);
    } else {
        out += '}';
    }
    out += ' ';
    out += std::to_string(bucket_counts_.back().load(std::memory_order_relaxed));
    out += '\n';

    // Sum line
    out += name_;
    out += "_sum";
    out += base_labels;
    out += ' ';
    {
        std::ostringstream ss;
        ss << GetSum();
        out += ss.str();
    }
    out += '\n';

    // Count line
    out += name_;
    out += "_count";
    out += base_labels;
    out += ' ';
    out += std::to_string(count_.load(std::memory_order_relaxed));
    out += '\n';
}

// ---------------------------------------------------------------------------
// Registry
// ---------------------------------------------------------------------------

Counter* Registry::AddCounter(std::string name, std::string help, Labels labels) {
    std::lock_guard<std::mutex> lk(mu_);
    counters_.push_back(
        std::make_unique<Counter>(std::move(name), std::move(help), std::move(labels)));
    return counters_.back().get();
}

Gauge* Registry::AddGauge(std::string name, std::string help, Labels labels) {
    std::lock_guard<std::mutex> lk(mu_);
    gauges_.push_back(std::make_unique<Gauge>(std::move(name), std::move(help), std::move(labels)));
    return gauges_.back().get();
}

Histogram* Registry::AddHistogram(std::string name,
                                  std::string help,
                                  Labels labels,
                                  const std::vector<double>& bounds) {
    std::lock_guard<std::mutex> lk(mu_);
    histograms_.push_back(
        std::make_unique<Histogram>(std::move(name), std::move(help), std::move(labels), bounds));
    return histograms_.back().get();
}

Histogram* Registry::AddLatencyHistogram(std::string name, std::string help, Labels labels) {
    std::vector<double> bounds(kDefaultLatencyBuckets.begin(), kDefaultLatencyBuckets.end());
    return AddHistogram(std::move(name), std::move(help), std::move(labels), bounds);
}

std::string Registry::Render() const {
    std::string out;
    out.reserve(4096);

    // Capture pointers under the lock but render outside it so we
    // don't hold the mutex during string formatting.
    std::vector<const Counter*> ctrs;
    std::vector<const Gauge*> gauges;
    std::vector<const Histogram*> hists;
    {
        std::lock_guard<std::mutex> lk(mu_);
        ctrs.reserve(counters_.size());
        for (const auto& c : counters_) {
            ctrs.push_back(c.get());
        }
        gauges.reserve(gauges_.size());
        for (const auto& g : gauges_) {
            gauges.push_back(g.get());
        }
        hists.reserve(histograms_.size());
        for (const auto& h : histograms_) {
            hists.push_back(h.get());
        }
    }

    for (const auto* c : ctrs) {
        c->Render(out);
    }
    for (const auto* g : gauges) {
        g->Render(out);
    }
    for (const auto* h : hists) {
        h->Render(out);
    }
    return out;
}

void Registry::ClearForTesting() {
    std::lock_guard<std::mutex> lk(mu_);
    counters_.clear();
    gauges_.clear();
    histograms_.clear();
}

}  // namespace osw::observability::prometheus
