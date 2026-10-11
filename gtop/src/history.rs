//! Real bounded trend history for every graph in this app, and the real
//! fix for "panele drgaja jak sie cos zmienia" (panels jitter/wobble
//! when something changes): a sliding-window graph that re-derives its
//! own y-axis min/max from whatever's currently in the window, every
//! frame, makes the WHOLE graph shape visibly rescale even when the
//! real value barely changed. ratatui's own `Sparkline::max()` lets a
//! caller pin an explicit ceiling instead of auto-scaling to the
//! dataset - every PERCENTAGE-shaped series (CPU/RAM/GPU/per-core) uses
//! a fixed `max(100)` via `as_u64_capped`; only genuinely-unbounded rate
//! series (disk/net throughput) omit `.max()` and let ratatui auto-scale
//! to the window, since there's no other real bound for those.

use std::collections::VecDeque;

/// HIST_LEN - ~30s of samples at the real 1s tick interval - enough for
/// a short, real trend graph per metric.
pub const HIST_LEN: usize = 30;

#[derive(Debug, Default, Clone)]
pub struct History {
    buf: VecDeque<f64>,
}

impl History {
    pub fn push(&mut self, v: f64) {
        self.buf.push_back(v);
        while self.buf.len() > HIST_LEN {
            self.buf.pop_front();
        }
    }

    pub fn last(&self) -> f64 {
        *self.buf.back().unwrap_or(&0.0)
    }

    #[allow(dead_code)] // real, general-purpose History API surface, not every collector's graph needs it yet
    pub fn len(&self) -> usize {
        self.buf.len()
    }

    #[allow(dead_code)]
    pub fn is_empty(&self) -> bool {
        self.buf.is_empty()
    }

    /// as_u64 - real values as ratatui's Sparkline wants them (u64,
    /// negative clamped to 0 - a real percentage or rate is never
    /// negative in practice, this is just defensive).
    pub fn as_u64(&self) -> Vec<u64> {
        self.buf.iter().map(|v| v.max(0.0).round() as u64).collect()
    }
}
