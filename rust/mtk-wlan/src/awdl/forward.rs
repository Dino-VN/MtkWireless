//! Selective forward admission for channels starved by greedy ROC scheduling.
//! C++ owns progress, intent and call lifetimes. This pure planner receives only
//! eligible schedule entries and returns fixed coordinates, never RF ownership.
use super::protocol::{
    AW_US, MAX_ROC_MS, ROC_RESPONSE_TIMEOUT_US, SLOTS, State, valid_presence, valid_sequence,
};
use super::timeline::{ROC_LEAD_US, ROC_PREPARE_US, RadioPlan, RocChannelTiming, current_window};

/// `eligible` is one flag per schedule entry, NOT a channel-number bitmap. The
/// caller marks only reachable channels with no native window for a full cycle
/// and no recent recovery attempt. Rotate over DISTINCT eligible channels, so
/// repeated fast-channel slots cannot buy additional recovery turns.
///
/// `release_at` protects open native windows and the original requested window
/// of a live grant. Surplus grant airtime is not a future native promise. The
/// caller may abort it at preparation, but only after that original window ends.
pub fn plan_forward(
    s: &State,
    now: u64,
    release_at: u64,
    after_channel: u8,
    timing: &RocChannelTiming,
    eligible: &[u8; SLOTS],
    home: u8,
) -> Option<RadioPlan> {
    if !valid_sequence(&s.sequence)
        || !valid_presence(s.presence as u32)
        || s.sequence.step as u32 + 1 != s.presence as u32
        || !s.radio_schedule_ready()
        || s.controls_dirty
        || s.native_clock_acquisition_pending
        || now < s.epoch
    {
        return None;
    }
    let width = s.presence as u64 * AW_US as u64;
    let cap = |ch| {
        if home != 0 && ch != home {
            300u32
        } else {
            MAX_ROC_MS
        }
    };
    let mut first = 0u8;
    let mut next = 0u8;
    for (i, &ch) in s.sequence.channels[..s.sequence.count as usize]
        .iter()
        .enumerate()
    {
        if ch == 0 || eligible[i] == 0 || width > cap(ch) as u64 * 1000 + 10 * 1024 {
            continue;
        }
        if first == 0 || ch < first {
            first = ch;
        }
        if ch > after_channel && (next == 0 || ch < next) {
            next = ch;
        }
    }
    let channel = if next != 0 { next } else { first };
    if channel == 0 {
        return None;
    }
    let lead = timing
        .lead_retained(channel, ROC_LEAD_US)
        .clamp(ROC_LEAD_US, ROC_RESPONSE_TIMEOUT_US);
    let earliest = release_at
        .max(now)
        .checked_add(lead as u64)?
        .checked_add(ROC_PREPARE_US)?;
    // current_window uses timestamp addition. Reject unrepresentable host
    // coordinates before calling it, including every search/coalescing step.
    earliest.checked_add((4 * SLOTS as u64 + 4) * width)?;
    let mut target = current_window(s, earliest)?;
    // A count need not divide the 16-bit AW counter. Include another rotation
    // across counter wrap rather than assuming indefinitely periodic indices.
    for _ in 0..(2 * SLOTS + 2) {
        if target.channel == channel && target.start >= earliest {
            let request_at = target.start.checked_sub(lead as u64)?;
            let mut end = target.end;
            let mut cursor = end;
            // Same-channel/idle coalescing, including home-channel trailing
            // idle. Competing channels are never relabeled as idle here.
            for _ in 1..s.sequence.count {
                let Some(n) = current_window(s, cursor) else {
                    break;
                };
                if n.channel != 0 && n.channel != channel {
                    break;
                }
                cursor = n.end;
                if n.channel == channel || (home != 0 && channel == home) {
                    end = n.end;
                }
                if cursor - request_at >= cap(channel) as u64 * 1000 {
                    break;
                }
            }
            let duration_ms = (end - request_at + 4000)
                .div_ceil(1000)
                .min(cap(channel) as u64) as u32;
            return Some(RadioPlan {
                window: target,
                request_at,
                duration_ms,
            });
        }
        target = current_window(s, target.end)?;
    }
    None
}

#[cfg(test)]
mod tests {
    use super::super::protocol::Sequence;
    use super::*;

    fn state(p: u8, n: u8, base: u16) -> State {
        let mut s = State::default();
        s.init(&[2, 1, 2, 3, 4, 5], 10_000_000);
        let mut q = Sequence {
            count: n,
            step: p - 1,
            ..Sequence::default()
        };
        for i in 0..n as usize {
            q.channels[i] = i as u8 + 1;
        }
        assert!(s.set_sequence(&q, 10_000_000));
        s.base_counter = base;
        s
    }

    #[test]
    fn every_presence_count_and_counter_wrap_has_bounded_distinct_turns() {
        let timing = RocChannelTiming::default();
        for p in [1, 2, 4, 8, 16, 32, 64] {
            for n in 1..=16 {
                for base in [0, 1, 65500, 65535] {
                    let s = state(p, n, base);
                    let now = 10_100_000;
                    let release = now + 1_100_000;
                    let mut cursor = 0;
                    for expected in 1..=n {
                        let a = plan_forward(&s, now, release, cursor, &timing, &[1; SLOTS], 0)
                            .unwrap();
                        assert_eq!(a.window.channel, expected);
                        assert!(a.request_at - ROC_PREPARE_US >= release);
                        assert!(
                            a.window.start
                                <= release
                                    + ROC_RESPONSE_TIMEOUT_US as u64
                                    + ROC_PREPARE_US
                                    + (2 * SLOTS as u64 + 2) * p as u64 * AW_US as u64
                        );
                        assert!(a.duration_ms <= MAX_ROC_MS);
                        cursor = expected;
                    }
                    assert_eq!(
                        plan_forward(&s, now, release, cursor, &timing, &[1; SLOTS], 0)
                            .unwrap()
                            .window
                            .channel,
                        1
                    );
                }
            }
        }
    }

    #[test]
    fn duplicates_do_not_gain_turns_and_only_eligible_channels_are_selected() {
        let mut s = state(4, 16, 0);
        s.sequence.channels = [
            36, 36, 36, 36, 36, 36, 36, 36, 44, 1, 44, 44, 44, 11, 11, 36,
        ];
        let t = RocChannelTiming::default();
        let eligible = [0, 0, 0, 0, 0, 0, 0, 0, 1, 0, 1, 1, 1, 1, 1, 0];
        let mut cursor = 0;
        for expected in [11, 44, 11, 44] {
            let plan = plan_forward(&s, 10_000_000, 0, cursor, &t, &eligible, 36).unwrap();
            assert_eq!(plan.window.channel, expected);
            assert!(plan.duration_ms <= 300);
            cursor = expected;
        }
    }

    #[test]
    fn impossible_offchannel_dwell_is_not_a_false_native_window() {
        let s = state(32, 3, 0);
        let t = RocChannelTiming::default();
        let plan = plan_forward(&s, 10_000_000, 0, 0, &t, &[1; SLOTS], 2).unwrap();
        assert_eq!(plan.window.channel, 2); // 524ms cannot fit a 300ms ROC.
        assert!(plan_forward(&s, 10_000_000, 0, 0, &t, &[1; SLOTS], 9).is_none());
    }

    #[test]
    fn idle_unready_invalid_and_unrepresentable_inputs_fail_closed() {
        let mut s = state(4, 4, 0);
        let t = RocChannelTiming::default();
        assert!(plan_forward(&s, u64::MAX - 100, 0, 0, &t, &[1; SLOTS], 0).is_none());
        assert!(plan_forward(&s, 1, 0, 0, &t, &[1; SLOTS], 0).is_none());
        assert!(plan_forward(&s, 10_000_000, 0, 0, &t, &[0; SLOTS], 0).is_none());
        s.controls_dirty = true;
        assert!(plan_forward(&s, 10_000_000, 0, 0, &t, &[1; SLOTS], 0).is_none());
        s.controls_dirty = false;
        s.sequence.count = 255;
        assert!(plan_forward(&s, 10_000_000, 0, 0, &t, &[1; SLOTS], 0).is_none());
        s.sequence.count = 4;
        s.sequence.channels = [0; SLOTS];
        assert!(plan_forward(&s, 10_000_000, 0, 0, &t, &[1; SLOTS], 0).is_none());
    }

    #[test]
    fn settling_grace_requires_current_context_and_more_than_issue_margin() {
        let mut context = RocChannelTiming::default();
        let mut legacy = RocChannelTiming::default();
        assert!(!context.recovery_grace(&legacy, 44));
        context.observe_retained(44, 110_000, 0);
        legacy.observe(44, 110_000, 0);
        assert!(!context.recovery_grace(&legacy, 44)); // A slow channel, not an outlier yet.
        context.observe_retained(44, 102_000, 0);
        legacy.observe(44, 102_000, 0);
        assert!(!context.recovery_grace(&legacy, 44)); // Exactly the 8ms margin.
        context.observe_retained(44, 101_999, 0);
        legacy.observe(44, 101_999, 0);
        let before = (context, legacy);
        assert!(context.recovery_grace(&legacy, 44));
        for channel in [0, 6, 166, 255] {
            assert!(!context.recovery_grace(&legacy, channel));
        }
        assert_eq!((context, legacy), before); // A hint, never a timing mutation.
        let mut replacement = RocChannelTiming::default();
        replacement.observe_retained(44, 12_000, 0);
        assert!(!replacement.recovery_grace(&legacy, 44)); // Foreign-context peak.
        assert!(!context.recovery_grace(&RocChannelTiming::default(), 44));
        legacy.observe(44, 110_000, 0);
        assert!(!context.recovery_grace(&legacy, 44)); // New response supports the peak.
    }

    #[test]
    fn settling_grace_keeps_eight_other_observation_aging_across_wrap() {
        let mut context = RocChannelTiming::default();
        let mut legacy = RocChannelTiming::default();
        for response in [500_000, 12_000] {
            context.observe_retained(44, response, 0);
            legacy.observe(44, response, 0);
        }
        context.observations = u64::MAX;
        legacy.observations = u64::MAX;
        context.entries[0].observed = u64::MAX;
        legacy.entries[0].observed = u64::MAX;
        assert!(context.recovery_grace(&legacy, 44));
        for _ in 0..7 {
            context.observe_retained(6, 12_000, 0);
            legacy.observe(6, 12_000, 0);
            assert!(context.recovery_grace(&legacy, 44));
        }
        context.observe_retained(6, 12_000, 0);
        legacy.observe(6, 12_000, 0);
        assert!(!context.recovery_grace(&legacy, 44));
        assert_eq!(legacy.lead(44, ROC_LEAD_US), ROC_LEAD_US);
        assert_eq!(context.lead_retained(44, 0), 508_000);
        legacy.observe(44, 12_000, 0);
        context.observe_retained(44, 12_000, 0);
        assert!(!context.recovery_grace(&legacy, 44)); // Retained peak alone cannot impose grace.
        assert_eq!(legacy.lead(44, 0), ROC_LEAD_US);
    }

    #[test]
    fn retained_samples_are_explicit_and_legacy_aging_still_recovers_outliers() {
        let mut legacy = RocChannelTiming::default();
        let mut retained = RocChannelTiming::default();
        legacy.observe(44, 990000, 20000);
        retained.observe_retained(44, 990000, 20000);
        for _ in 0..100 {
            legacy.observe(1, 12000, 0);
            retained.observe_retained(1, 12000, 0);
        }
        assert_eq!(legacy.lead(44, ROC_LEAD_US), ROC_LEAD_US);
        assert_eq!(retained.lead_retained(44, 0), 1_000_000);
        legacy.observe(44, 12000, 0);
        assert_eq!(legacy.lead(44, 0), ROC_LEAD_US);
        for i in 0..8 {
            retained.observe_retained(44, 12000, 0);
            assert_eq!(
                retained.lead_retained(44, 0),
                if i == 7 { ROC_LEAD_US } else { 1_000_000 }
            );
        }
        let count = retained.observations;
        retained.observe_retained(0, 900000, 0);
        retained.observe_retained(166, 900000, 0);
        assert_eq!(retained.observations, count);
        retained = RocChannelTiming::default();
        assert_eq!(retained.lead_retained(44, 123), 123);
    }
}
