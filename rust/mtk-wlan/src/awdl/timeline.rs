//! Availability-window timeline and ROC planning (`AwdlTimeline.hpp`).
//! Host observations use monotonic microseconds; AW counters are schedule
//! coordinates, never timestamps.

use super::protocol::{
    AW_US, MAX_ROC_MS, ROC_RESPONSE_TIMEOUT_US, Reservation, SLOTS, Sequence, State,
    valid_presence, valid_sequence,
};

#[repr(C)]
#[derive(Debug, Clone, Copy, Default, PartialEq, Eq)]
pub struct Window {
    pub start: u64,
    pub end: u64,
    pub counter: u16,
    pub channel: u8,
    pub presence: u8,
}

pub fn current_window(s: &State, now: u64) -> Option<Window> {
    if !valid_presence(s.presence as u32) || !valid_sequence(&s.sequence) {
        return None;
    }
    let length = s.presence as u32 * AW_US;
    let remaining = s.until_next(now);
    if remaining == 0 || remaining > length || now < (length - remaining) as u64 {
        return None;
    }
    let counter = s.counter(now);
    Some(Window {
        start: now - (length - remaining) as u64,
        end: now + remaining as u64,
        counter: counter - counter % s.presence as u16,
        channel: s.channel(now),
        presence: s.presence,
    })
}

/// AWDL multicast rendezvous uses EAW slots 0 and 10 of the 16-slot cycle.
pub fn multicast_ready(s: &State, now: u64) -> bool {
    let Some(w) = current_window(s, now) else {
        return false;
    };
    if w.channel == 0 {
        return false;
    }
    let slot = (s.counter(now) as usize / s.presence as usize) % SLOTS;
    if slot != 0 && slot != 10 {
        return false;
    }
    let guard = (16 * 1024u64).min((w.end - w.start) / 4);
    now - w.start >= guard && w.end - now >= guard
}

fn cycle_us(sequence: &Sequence) -> u32 {
    sequence.count as u32 * (sequence.step as u32 + 1) * AW_US
}

/// Lifetime budget of a queued data packet, snapshotted on enqueue: MCU
/// admission, ROC response and one full channel cycle plus the 8ms margin.
pub fn data_queue_lifetime_us(s: &State, destination: &[u8; 6]) -> u32 {
    let mut cycle = SLOTS as u32 * 4 * AW_US;
    for sequence in [&s.sequence, &s.own_sequence] {
        if valid_sequence(sequence) {
            cycle = cycle.max(cycle_us(sequence));
        }
    }
    let peer = s.find(destination);
    if peer >= 0 {
        let sequence = &s.peers[peer as usize].advertisement.sequence;
        if valid_sequence(sequence) {
            cycle = cycle.max(cycle_us(sequence));
        }
    }
    4 * ROC_RESPONSE_TIMEOUT_US + cycle + 8000
}

/// Lead before an EAW edge for the BSS and off-channel arbitration.
pub const ROC_LEAD_US: u32 = 55_000;

/// Bounded history of accepted ROC response/preparation delays.
#[repr(C)]
#[derive(Debug, Clone, Copy, PartialEq, Eq)]
pub struct RocLeadTiming {
    pub samples: [u32; 8],
    pub lead_us: u32,
    pub count: u32,
    pub next: u32,
    pub updates: u64,
    pub decreases: u64,
}

impl Default for RocLeadTiming {
    fn default() -> Self {
        Self {
            samples: [0; 8],
            lead_us: ROC_LEAD_US,
            count: 0,
            next: 0,
            updates: 0,
            decreases: 0,
        }
    }
}

impl RocLeadTiming {
    pub fn observe(&mut self, response_us: u32, preparation_us: u64) -> u32 {
        let need = (response_us as u64 + preparation_us + 8000)
            .clamp(ROC_LEAD_US as u64, ROC_RESPONSE_TIMEOUT_US as u64);
        self.samples[self.next as usize % 8] = need as u32;
        self.next = (self.next + 1) % 8;
        if self.count < 8 {
            self.count += 1;
        }
        let value = self.samples[..self.count as usize]
            .iter()
            .copied()
            .fold(ROC_LEAD_US, u32::max);
        if value < self.lead_us {
            self.decreases += 1;
        }
        self.lead_us = value;
        self.updates += 1;
        value
    }
}

#[repr(C)]
#[derive(Debug, Clone, Copy, Default, PartialEq, Eq)]
pub struct RocChannelEntry {
    pub channel: u8,
    pub observed: u64,
    pub timing: RocLeadTiming,
}

/// Independent lead histories per destination channel; a channel ages out
/// after eight newer grants on other channels.
#[repr(C)]
#[derive(Debug, Clone, Copy, Default, PartialEq, Eq)]
pub struct RocChannelTiming {
    pub entries: [RocChannelEntry; SLOTS],
    pub next: u32,
    pub observations: u64,
}

impl RocChannelTiming {
    pub fn lead(&self, channel: u8, fallback: u32) -> u32 {
        self.entries
            .iter()
            .find(|e| {
                e.channel == channel
                    && e.timing.count != 0
                    && self.observations.wrapping_sub(e.observed) < 8
            })
            .map_or(fallback, |e| e.timing.lead_us)
    }

    pub fn observe(&mut self, channel: u8, response_us: u32, preparation_us: u64) {
        if channel == 0 || channel > 165 {
            return;
        }
        let index = match self.entries.iter().position(|e| e.channel == channel) {
            Some(i) => i,
            None => {
                let i = self.next as usize % SLOTS;
                self.next = (self.next + 1) % SLOTS as u32;
                self.entries[i] = RocChannelEntry {
                    channel,
                    ..RocChannelEntry::default()
                };
                i
            }
        };
        let observations = self.observations;
        let entry = &mut self.entries[index];
        if observations.wrapping_sub(entry.observed) >= 8 {
            entry.timing = RocLeadTiming::default();
        }
        self.observations += 1;
        entry.observed = self.observations;
        entry.timing.observe(response_us, preparation_us);
    }
}

#[repr(C)]
#[derive(Debug, Clone, Copy, Default, PartialEq, Eq)]
pub struct RadioPlan {
    pub window: Window,
    pub request_at: u64,
    pub duration_ms: u32,
}

/// Plans the next ROC: the reachable window, when to request it and for how
/// long (one grant covers adjacent equal-channel windows).
pub fn plan_radio(
    s: &State,
    now: u64,
    lead_us: u32,
    skip_current: bool,
    timing: Option<&RocChannelTiming>,
) -> Option<RadioPlan> {
    let mut target = current_window(s, now)?;
    if skip_current || target.channel == 0 || now - target.start > 8000 {
        for _ in 0..s.sequence.count {
            target = current_window(s, target.end)?;
            if target.channel != 0 {
                break;
            }
        }
    }
    if target.channel == 0 {
        return None;
    }
    // Skip windows the measured lead cannot reach, judging each candidate by
    // its own channel's latency.
    let mut budget =
        ROC_RESPONSE_TIMEOUT_US / (s.presence as u32 * AW_US) + s.sequence.count as u32 + 2;
    let selected = loop {
        let lead = timing.map_or(lead_us, |t| t.lead(target.channel, ROC_LEAD_US));
        let unreachable = lead > ROC_LEAD_US
            && lead > s.presence as u32 * AW_US + 8000
            && target.start + 8000 < now + lead as u64;
        if target.channel != 0 && !unreachable {
            break lead;
        }
        if budget == 0 {
            return None;
        }
        budget -= 1;
        target = current_window(s, target.end)?;
    };
    let at_ = target.start.saturating_sub(selected as u64).max(now);
    let mut end = target.end;
    let mut cursor = end;
    for _ in 1..s.sequence.count {
        let Some(next) = current_window(s, cursor) else {
            break;
        };
        if next.channel != 0 && next.channel != target.channel {
            break;
        }
        // Idle slots may sit inside one grant, but never extend into
        // trailing idle time.
        cursor = next.end;
        if next.channel == target.channel {
            end = next.end;
        }
        if cursor - at_ >= MAX_ROC_MS as u64 * 1000 {
            break;
        }
    }
    let duration = (end - at_ + 4000).div_ceil(1000).min(MAX_ROC_MS as u64);
    Some(RadioPlan {
        window: target,
        request_at: at_,
        duration_ms: duration as u32,
    })
}

pub fn grant_covers_window(w: &Window, grant: &Reservation, now: u64) -> bool {
    let at_ = grant.grant_observed_at.max(w.start);
    let check = now.max(w.start);
    let end = grant.deadline.min(w.end);
    w.channel != 0
        && grant.grant_observed_at != 0
        && grant.grant_observed_at <= check
        && grant.can_transmit(w.channel, check)
        && end > at_
        && end - at_ + 10 * 1024 >= w.presence as u64 * AW_US as u64
}

/// An early grant keeps its original reservation, as long as the actual
/// schedule still has that window.
pub fn grant_covers_reserved_window(
    s: &State,
    reserved: &Window,
    requested_channel: u8,
    grant: &Reservation,
    now: u64,
) -> bool {
    if requested_channel == 0
        || requested_channel != reserved.channel
        || now >= reserved.start
        || grant.grant_observed_at == 0
        || grant.grant_observed_at > now
        || !grant_covers_window(reserved, grant, reserved.start)
    {
        return false;
    }
    current_window(s, reserved.start).is_some_and(|a| a == *reserved)
}

#[repr(C)]
#[derive(Debug, Clone, Copy, PartialEq, Eq)]
pub struct Timeline {
    pub active: Window,
    pub open: bool,
    pub emitted: bool,
    pub radio_end: u64,
    pub role: u8,
    pub token: u8,
    pub grant_observed_at: u64,
    pub last_start: u64,
    pub last_window_start: u64,
    pub starts: u64,
    pub ends: u64,
    pub late: u64,
    pub short_grants: u64,
    pub last_counter: u16,
    pub sync: [u8; 6],
    pub root: [u8; 6],
    pub last_sync: [u8; 6],
    pub last_root: [u8; 6],
    pub last_presence: u8,
    pub schedule: Sequence,
    pub schedule_recorded: bool,
}

impl Default for Timeline {
    fn default() -> Self {
        Self {
            active: Window::default(),
            open: false,
            emitted: false,
            radio_end: 0,
            role: 1,
            token: 0,
            grant_observed_at: 0,
            last_start: 0,
            last_window_start: 0,
            starts: 0,
            ends: 0,
            late: 0,
            short_grants: 0,
            last_counter: 0,
            sync: [0; 6],
            root: [0; 6],
            last_sync: [0; 6],
            last_root: [0; 6],
            last_presence: 0,
            schedule: Sequence::default(),
            schedule_recorded: false,
        }
    }
}

impl Timeline {
    pub fn matches_schedule(&self, s: &State) -> bool {
        self.active.presence == s.presence
            && self.role == s.sync_role
            && self.sync == s.sync
            && self.root == s.master
            && (!self.schedule_recorded || self.schedule == s.sequence)
    }

    /// Reports an actual grant near the EAW edge, tracking the grant's real
    /// end when it is shorter than the requested dwell.
    #[allow(clippy::too_many_arguments)]
    pub fn begin(
        &mut self,
        w: &Window,
        grant: &Reservation,
        now: u64,
        parent: &[u8; 6],
        master: &[u8; 6],
        native_role: u8,
        radio_schedule: Option<&Sequence>,
    ) -> bool {
        if self.open
            || w.channel == 0
            || now < w.start
            || now >= w.end
            || now - w.start > 8000
            || !grant.can_transmit(w.channel, now)
            || grant.grant_observed_at == 0
            || grant.grant_observed_at > now
        {
            self.late += 1;
            return false;
        }
        let start = grant.grant_observed_at.max(w.start);
        let end = grant.deadline.min(w.end);
        // Native sanityCheckAWEnd allows +/-10 TU, not a partial dwell.
        if end.wrapping_sub(start) + 10 * 1024 < w.presence as u64 * AW_US as u64 {
            self.short_grants += 1;
            return false;
        }
        if self.emitted && now < self.active.end {
            return false;
        }
        // Identify the last notified window by counter/presence/master too;
        // RX clock corrections can move an edge by a fraction of a TU.
        let edge_delta = w.start.abs_diff(self.last_window_start);
        if self.emitted
            && self.last_counter == w.counter
            && self.last_presence == w.presence
            && self.last_sync == *parent
            && self.last_root == *master
            && edge_delta < w.presence as u64 * AW_US as u64
        {
            return false;
        }
        self.active = *w;
        self.open = true;
        self.emitted = true;
        self.last_window_start = w.start;
        self.radio_end = end;
        self.role = native_role;
        self.token = grant.token;
        self.grant_observed_at = grant.grant_observed_at;
        self.schedule_recorded = radio_schedule.is_some();
        if let Some(schedule) = radio_schedule {
            self.schedule = *schedule;
        }
        self.last_start = start;
        self.last_counter = w.counter;
        self.last_presence = w.presence;
        self.starts += 1;
        self.sync = *parent;
        self.root = *master;
        self.last_sync = *parent;
        self.last_root = *master;
        true
    }

    pub fn due(&self, now: u64) -> bool {
        self.open && now >= self.radio_end
    }

    pub fn switch_lead_us(&self) -> u32 {
        let late_start = self.last_start.wrapping_sub(self.active.start);
        if late_start < 10 * 1024 {
            (10 * 1024 - late_start) as u32
        } else {
            0
        }
    }

    pub fn close(&mut self) {
        if self.open {
            self.open = false;
            self.ends += 1;
        }
    }

    pub fn reset(&mut self) {
        self.open = false;
        self.emitted = false;
        self.last_window_start = 0;
        self.radio_end = 0;
    }
}
