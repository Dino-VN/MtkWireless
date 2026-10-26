//! Wire protocol and scheduling primitives (`AwdlProtocol.hpp`).
//!
//! Integer behavior follows the C++ exactly, including 16-bit counter
//! wrap-around and the signed reinterpretations; those are spelled out with
//! `wrapping_*` and `as` casts.

use super::bytes::{at, copy, eq, fill, le16, le32, mac, put16, put32, set};
use mtk_common::Reader;

pub const BSSID: [u8; 6] = [0, 0x25, 0, 0xff, 0x94, 0x73];
pub const SNAP: [u8; 8] = [0xaa, 0xaa, 3, 0, 0x17, 0xf2, 8, 0];
pub const MAX_PEERS: usize = 16;
pub const SLOTS: usize = 16;
pub const PEER_LIFETIME_US: u64 = 3_000_000;
pub const AW_US: u32 = 16 * 1024;
/// Largest supported EAW is 64*16TU (1048.576ms); bounded below MT76's 5s.
pub const MAX_ROC_MS: u32 = 1100;
/// mt7921_set_roc waits one second for the firmware response.
pub const ROC_RESPONSE_TIMEOUT_US: u32 = 1_000_000;
pub const ROC_TOKEN_QUARANTINE_US: u32 = ROC_RESPONSE_TIMEOUT_US + MAX_ROC_MS * 1000;

const ACTION: [u8; 6] = [127, 0, 0x17, 0xf2, 8, 0x10];

/// Plain-data structs whose all-zero bit pattern is their `Default`.
///
/// # Safety
/// Implement only for `repr(C)` structs made of integers, `bool` and arrays
/// or structs of those.
unsafe trait Zeroable {}

/// Resets in place: `State` is ~2 KB and kernel stacks are 16 KB, so avoid
/// building a temporary.
fn zero<T: Zeroable>(value: &mut T) {
    // SAFETY: Zeroable guarantees all-zero is a valid value of T.
    unsafe { core::ptr::write_bytes(value as *mut T, 0, 1) }
}

/// Unicast, non-zero MAC at `f[i..i+6]`.
pub fn unicast(f: &[u8], i: usize) -> bool {
    let m = mac(f, i);
    f.len() >= i.saturating_add(6) && m[0] & 1 == 0 && m != [0; 6]
}

fn zero_padding(p: &[u8]) -> bool {
    p.len() < 3 && p.iter().all(|&b| b == 0)
}

#[repr(C)]
#[derive(Debug, Clone, Copy, Default, PartialEq, Eq)]
pub struct Sequence {
    pub channels: [u8; SLOTS],
    pub count: u8,
    pub step: u8,
}

/// Power-of-two EAW sizes divide the 16-bit AW counter without a partial
/// slot at rollover.
pub fn valid_presence(presence: u32) -> bool {
    presence != 0 && presence <= 64 && presence & (presence - 1) == 0
}

pub fn valid_sequence(s: &Sequence) -> bool {
    if s.count == 0 || s.count as usize > SLOTS || !valid_presence(s.step as u32 + 1) {
        return false;
    }
    s.channels[..s.count as usize].iter().all(|&c| c <= 165)
}

/// The 5 GHz channel of a host sequence: where AWDL data flows.
pub fn data_channel(s: &Sequence) -> u8 {
    if !valid_sequence(s) {
        return 0;
    }
    s.channels[..s.count as usize].iter().copied().find(|&c| c > 14).unwrap_or(0)
}

/// awdld's idle sequence (2026-10-26: 6,-,149,-,-,-,-,-,6,-,149,...) is
/// present on the data channel in 2 of 16 EAWs, and peers send us data only
/// there. During a transfer, offer the data channel in every idle EAW; the
/// host's channel-6 and other slots stay exactly as they are.
pub fn dense_sequence(s: &Sequence, channel: u8) -> Sequence {
    let mut d = *s;
    if !valid_sequence(s) || channel == 0 || channel > 165 {
        return d;
    }
    for c in &mut d.channels[..s.count as usize] {
        if *c == 0 {
            *c = channel;
        }
    }
    d
}

pub fn parse_sequence(p: &[u8]) -> Option<Sequence> {
    let n = p.len();
    if n < 6
        || at(p, 0) as usize >= SLOTS
        || at(p, 2) != 0
        || !valid_presence(at(p, 3) as u32 + 1)
        || le16(p, 4) != 0xffff
    {
        return None;
    }
    let count = at(p, 0) as usize + 1;
    let encoding = at(p, 1);
    let width = match encoding {
        0 => 1,
        1 | 3 => 2,
        _ => 0,
    };
    if width == 0 || count * width > n - 6 {
        return None;
    }
    let used = 6 + count * width;
    // Bound padding separately from TLV framing; never accept nonzero
    // extension bytes.
    if n - used > 3 || p[used..].iter().any(|&b| b != 0) {
        return None;
    }
    let mut s = Sequence {
        count: count as u8,
        step: at(p, 3),
        ..Sequence::default()
    };
    #[allow(clippy::needless_range_loop)] // i also indexes p
    for i in 0..count {
        let channel = at(p, 6 + i * width + (encoding == 1) as usize);
        s.channels[i] = channel;
        // 0 means an unavailable slot. A regulatory check is done by HAL.
        if channel > 165 {
            return None;
        }
        if encoding == 3 && channel != 0 {
            let opclass = at(p, 7 + i * width);
            // A 6 GHz channel number must not be mistaken for 2/5 GHz.
            let bad = if channel <= 14 {
                !(81..=84).contains(&opclass)
            } else {
                !(115..=130).contains(&opclass)
            };
            if bad {
                return None;
            }
        }
    }
    Some(s)
}

#[repr(C)]
#[derive(Debug, Clone, Copy, Default, PartialEq, Eq)]
pub struct Advertisement {
    pub sequence: Sequence,
    pub master: [u8; 6],
    pub sync: [u8; 6],
    pub period: u16,
    pub counter: u16,
    pub down: u16,
    pub presence: u8,
    pub metric: u32,
    pub distance: u32,
    pub master_counter: u32,
    pub self_counter: u32,
}

fn action_header_ok(f: &[u8]) -> bool {
    f.len() >= 40
        && at(f, 0) == 0xd0
        && at(f, 1) & 0xc7 == 0
        && at(f, 22) & 15 == 0
        && unicast(f, 10)
        && eq(f, 16, &BSSID)
        && eq(f, 24, &ACTION)
        && (at(f, 30) == 0 || at(f, 30) == 3)
}

/// Framing check for native peer discovery, without the scheduler's
/// narrower election/sequence policy.
pub fn valid_action_envelope(f: &[u8]) -> bool {
    let n = f.len();
    if n > 2048 || !action_header_ok(f) {
        return false;
    }
    let mut at_ = 40;
    while at_ < n {
        if n - at_ < 3 {
            return zero_padding(&f[at_..]);
        }
        let len = le16(f, at_ + 1) as usize;
        at_ += 3;
        if len > n - at_ {
            return false;
        }
        at_ += len;
    }
    n > 40
}

/// Parses a complete 802.11 action frame (no radiotap/FCS) into a new value,
/// so a truncated last TLV cannot partially update live state.
pub fn parse_action(f: &[u8]) -> Option<Advertisement> {
    let n = f.len();
    if !action_header_ok(f) {
        return None;
    }
    let mut a = Advertisement::default();
    let (mut sync, mut election, mut separate) = (false, false, false);
    let mut extra = Sequence::default();
    let mut election_master = [0u8; 6];
    let (mut embedded_encoding, mut separate_encoding) = (0u8, 0u8);
    let mut at_ = 40;
    while at_ < n {
        if n - at_ < 3 {
            if zero_padding(&f[at_..]) {
                break;
            }
            return None;
        }
        let kind = f[at_];
        let len = le16(f, at_ + 1) as usize;
        at_ += 3;
        if len > n - at_ {
            return None;
        }
        let p = &f[at_..at_ + len];
        match kind {
            4 => {
                if sync || len < 39 {
                    return None;
                }
                a.period = le16(p, 5);
                a.down = le16(p, 1);
                a.presence = p[27];
                a.counter = le16(p, 29);
                a.master = mac(p, 21);
                let total = a.presence as u32 * a.period as u32;
                if a.period != 16
                    || !valid_presence(a.presence as u32)
                    || a.down == 0
                    || a.down as u32 > total
                    || !unicast(&a.master, 0)
                {
                    return None;
                }
                a.sequence = parse_sequence(&p[33..])?;
                if a.sequence.step as u32 + 1 != a.presence as u32 {
                    return None;
                }
                // The down-counter and AW counter must describe the same EAW.
                if a.counter as u32 % a.presence as u32 != (total - a.down as u32) / a.period as u32
                {
                    return None;
                }
                embedded_encoding = p[34];
                sync = true;
            }
            24 => {
                if election || len != 40 || !unicast(p, 0) || !unicast(p, 6) {
                    return None;
                }
                a.sync = mac(p, 6);
                a.metric = le32(p, 20);
                a.distance = le32(p, 16);
                election_master = mac(p, 0);
                a.master_counter = le32(p, 12);
                a.self_counter = le32(p, 36);
                if a.distance > 10 {
                    return None;
                }
                election = true;
            }
            18 => {
                if separate {
                    return None;
                }
                extra = parse_sequence(p)?;
                separate_encoding = p[1];
                separate = true;
            }
            _ => {}
        }
        at_ += len;
    }
    if !sync {
        return None;
    }
    // Election v2 names the elected root, which can differ from the sync
    // parameters' transmitter during a tree transition.
    if election {
        a.master = election_master;
    }
    if separate {
        if extra.count != a.sequence.count || extra.step != a.sequence.step {
            return None;
        }
        // Prefer the separately validated opclass schedule (TLV 18) over the
        // legacy flags of the embedded one; do not invent a conversion.
        let count = extra.count as usize;
        if (embedded_encoding != 1 || separate_encoding != 3)
            && extra.channels[..count] != a.sequence.channels[..count]
        {
            return None;
        }
        a.sequence = extra;
    }
    if !election {
        a.sync = mac(f, 10);
    }
    Some(a)
}

#[repr(C)]
#[derive(Debug, Clone, Copy, Default, PartialEq, Eq)]
pub struct Peer {
    pub mac: [u8; 6],
    pub advertisement: Advertisement,
    pub seen: u64,
    pub epoch: u64,
    pub base_counter: u16,
    pub last_data: u16,
    pub replay: u64,
    pub used: bool,
    pub has_data: bool,
}

impl Peer {
    pub fn channel(&self, now: u64) -> u8 {
        let s = &self.advertisement.sequence;
        if !self.used
            || now < self.epoch
            || now < self.seen
            || now - self.seen > PEER_LIFETIME_US
            || !valid_sequence(s)
        {
            return 0;
        }
        let count = self
            .base_counter
            .wrapping_add(((now - self.epoch) / AW_US as u64) as u16);
        s.channels[(count as usize / (s.step as usize + 1)) % s.count as usize]
    }
}

/// Replay windows for AWDL data (`acceptData`).
pub fn accept_data(p: &mut Peer, seq: u16) -> bool {
    if !p.has_data {
        p.has_data = true;
        p.last_data = seq;
        p.replay = 1;
        return true;
    }
    let delta = seq.wrapping_sub(p.last_data) as i16;
    if delta > 0 {
        p.replay = if delta >= 64 {
            1
        } else {
            (p.replay << delta) | 1
        };
        p.last_data = seq;
        return true;
    }
    let back = p.last_data.wrapping_sub(seq) as u32;
    if back >= 64 || p.replay & (1u64 << back) != 0 {
        return false;
    }
    p.replay |= 1u64 << back;
    true
}

/// MSDU duplicate history, separate from native peer authorization. Captured
/// iPhones use independent AWDL counters for non-QoS and each QoS TID, and
/// restart a unicast counter after inactivity while keeping the same MAC.
#[repr(C)]
#[derive(Debug, Clone, Copy, Default, PartialEq, Eq)]
pub struct DataReplayWindow {
    pub bitmap: u64,
    pub last_rx: u64,
    pub sequence: u16,
    pub active: bool,
    // 0: no rollover observed, 12: observed 4095 -> 0, 16: upper bits seen.
    pub sequence_bits: u8,
}

impl DataReplayWindow {
    pub fn accept(&mut self, seq: u16, now: u64) -> bool {
        if !self.active || now < self.last_rx || now - self.last_rx > PEER_LIFETIME_US {
            self.active = true;
            self.sequence = seq;
            self.bitmap = 1;
            self.last_rx = now;
            self.sequence_bits = if seq > 0xfff { 16 } else { 0 };
            return true;
        }
        self.last_rx = now;
        // The 2026-10-26 large-upload trace reaches 4095, then resumes at
        // zero within the same active QoS stream. Treating it as a 16-bit
        // counter pins the high-water mark and drops every later datagram.
        // Learn this only at the boundary; streams using upper bits retain
        // full 16-bit arithmetic (including the captured TID 1 at 4096).
        if seq > 0xfff {
            self.sequence_bits = 16;
        } else if self.sequence_bits == 0 && self.sequence >= 0xfc0 && seq < 0x800 {
            self.sequence_bits = 12;
        }
        let mask = if self.sequence_bits == 12 { 0xfff } else { u16::MAX };
        let forward = seq.wrapping_sub(self.sequence) & mask;
        let delta = if forward <= mask / 2 { forward as i32 } else { forward as i32 - (mask as i32 + 1) };
        if delta > 0 {
            self.bitmap = if delta >= 64 { 1 } else { (self.bitmap << delta) | 1 };
            self.sequence = seq;
            return true;
        }
        let back = (self.sequence.wrapping_sub(seq) & mask) as u32;
        if back >= 64 || self.bitmap & (1u64 << back) != 0 {
            return false;
        }
        self.bitmap |= 1u64 << back;
        true
    }
}

#[repr(C)]
#[derive(Debug, Clone, Copy, Default, PartialEq, Eq)]
pub struct DataPeerEntry {
    pub replay_state: Peer,
    pub windows: [DataReplayWindow; 17], // TID 0..15; non-QoS at 16
    pub seen: u64,
    pub cached: bool,
    pub traffic: bool,
}

/// Native cache/traffic registration is authoritative for data membership;
/// replay state here survives the short-lived radio schedule.
#[repr(C)]
#[derive(Debug, Clone, Copy, Default, PartialEq, Eq)]
pub struct DataPeers {
    pub entries: [DataPeerEntry; MAX_PEERS],
}

// SAFETY: integers, bools and arrays of them only.
unsafe impl Zeroable for DataPeers {}
unsafe impl Zeroable for State {}

impl DataPeers {
    pub fn accept_data(&mut self, index: usize, stream: usize, seq: u16, now: u64) -> bool {
        let Some(entry) = self.entries.get_mut(index) else { return false };
        if !Self::live(entry, now) { return false; }
        entry.windows.get_mut(stream).is_some_and(|w| w.accept(seq, now))
    }

    pub fn reset(&mut self) {
        zero(self);
    }

    pub fn live(e: &DataPeerEntry, now: u64) -> bool {
        e.replay_state.used
            && (e.cached || e.traffic || (now >= e.seen && now - e.seen <= PEER_LIFETIME_US))
    }

    pub fn find(&self, address: &[u8; 6], now: u64) -> i32 {
        self.entries
            .iter()
            .position(|e| Self::live(e, now) && e.replay_state.mac == *address)
            .map_or(-1, |i| i as i32)
    }

    pub fn observe(&mut self, address: &[u8; 6], now: u64) -> i32 {
        if !unicast(address, 0) {
            return -1;
        }
        let mut i = self.find(address, now);
        if i < 0
            && let Some(j) = self.entries.iter().position(|e| !Self::live(e, now))
        {
            self.entries[j] = DataPeerEntry::default();
            self.entries[j].replay_state.mac = *address;
            self.entries[j].replay_state.used = true;
            i = j as i32;
        }
        if i >= 0 {
            self.entries[i as usize].seen = now;
        }
        i
    }

    pub fn can_observe(&self, address: &[u8; 6], now: u64) -> bool {
        unicast(address, 0)
            && (self.find(address, now) >= 0 || self.entries.iter().any(|e| !Self::live(e, now)))
    }

    pub fn set_cached(&mut self, address: &[u8; 6], value: bool, now: u64) -> bool {
        let i = if value {
            self.observe(address, now)
        } else {
            self.find(address, now)
        };
        if i < 0 {
            return !value;
        }
        let e = &mut self.entries[i as usize];
        e.cached = value;
        // Native cache removal revokes authorization and replay even if the
        // last advertisement is recent; a traffic registration may own it.
        if !value && !e.traffic {
            *e = DataPeerEntry::default();
        }
        true
    }

    pub fn set_traffic(&mut self, address: &[u8; 6], value: bool, now: u64) -> bool {
        let i = if value {
            self.observe(address, now)
        } else {
            self.find(address, now)
        };
        if i < 0 {
            return !value;
        }
        let e = &mut self.entries[i as usize];
        e.traffic = value;
        if !value && !e.cached {
            *e = DataPeerEntry::default();
        }
        true
    }
}

#[repr(C)]
#[derive(Debug, Clone, Copy, Default, PartialEq, Eq)]
pub struct Controls {
    pub sequence: Sequence,
    pub presence: u8,
    pub role: u8,
    pub forced: [u8; 6],
    pub parent: [u8; 6],
    pub parent_selected: bool,
}

/// Scheduler, election and software AW clock. Field order matches the C++
/// `mtkawdl::State`.
#[repr(C)]
#[derive(Debug, Clone, Copy, Default, PartialEq, Eq)]
pub struct State {
    pub mac: [u8; 6],
    pub master: [u8; 6],
    pub sync: [u8; 6],
    pub forced_sync: [u8; 6],
    pub sequence: Sequence,
    pub own_sequence: Sequence,
    pub peers: [Peer; MAX_PEERS],
    pub epoch: u64,
    pub base_counter: u16,
    pub data_counter: u16,
    pub frame_counter: u16,
    pub psf_period: u16,
    pub metric: u32,
    pub master_metric: u32,
    pub distance: u32,
    pub master_counter: u32,
    pub self_counter: u32,
    pub synchronized: bool,
    pub election_epoch: u64,
    pub presence: u8,
    pub own_presence: u8,
    pub sync_role: u8,
    pub min_extension: [u8; 4],
    pub native_election_managed: bool,
    pub native_parent_selected: bool,
    pub native_parent: [u8; 6],
    pub clock_sample_at: u64,
    pub clock_corrections: u64,
    pub clock_rejected: u64,
    /// Software equivalent of a firmware AW clock: phase discipline waits
    /// until the promised radio AW ends.
    pub clock_hold_until: u64,
    pub clock_deferred_at: u64,
    pub clock_deferred: u64,
    pub native_clock_selected: bool,
    pub native_timeline_started: bool,
    pub controls_held: bool,
    pub controls_dirty: bool,
    pub native_clock_holdover: bool,
    pub native_clock_validated: bool,
    pub native_clock_acquisition_pending: bool,
    pub clock_sample_rejected: bool,
    pub native_holdover_transitions: u64,
    pub native_clock_acquisitions: u64,
    pub clock_parent: [u8; 6],
    pub clock_root: [u8; 6],
    pub acquisition_parent: [u8; 6],
    pub acquisition_root: [u8; 6],
    pub acquisition_root_known: bool,
    pub controls_deferred: u64,
    pub pending_controls: Controls,
}

impl State {
    pub fn hold_clock(&mut self, until: u64) {
        self.clock_hold_until = until;
    }

    pub fn hold_controls(&mut self) {
        if self.controls_held {
            return;
        }
        self.pending_controls = Controls {
            sequence: self.own_sequence,
            presence: self.own_presence,
            role: self.sync_role,
            forced: self.forced_sync,
            parent: self.native_parent,
            parent_selected: self.native_parent_selected,
        };
        self.controls_held = true;
        self.controls_dirty = false;
    }

    pub fn discard_controls(&mut self) {
        self.controls_held = false;
        self.controls_dirty = false;
    }

    pub fn requested_role(&self) -> u8 {
        if self.controls_held {
            self.pending_controls.role
        } else {
            self.sync_role
        }
    }

    pub fn requested_parent_is(&self, address: &[u8; 6]) -> bool {
        if self.controls_held {
            self.pending_controls.parent_selected && self.pending_controls.parent == *address
        } else {
            self.native_parent_selected && self.native_parent == *address
        }
    }

    pub fn select_native_parent(
        &mut self,
        address: &[u8; 6],
        root: Option<&[u8; 6]>,
        acquire: bool,
    ) {
        self.native_election_managed = true;
        // Only an explicit native selection authorizes acquiring another AW
        // namespace; ordinary RX from the selected peer is drift.
        let keep_root = root.is_none()
            && self.native_clock_acquisition_pending
            && self.acquisition_parent == *address;
        self.native_clock_acquisition_pending = acquire
            || !self.native_clock_selected
            || self.clock_parent != *address
            || root.is_some_and(|r| self.clock_root != *r);
        if keep_root {
            self.native_clock_acquisition_pending = true;
        }
        self.acquisition_parent = *address;
        if !keep_root {
            self.acquisition_root_known = root.is_some();
        }
        if let Some(r) = root {
            self.acquisition_root = *r;
        }
        if self.controls_held {
            if !self.pending_controls.parent_selected || self.pending_controls.parent != *address {
                self.pending_controls.parent = *address;
                self.pending_controls.parent_selected = true;
                self.controls_dirty = true;
                self.controls_deferred += 1;
            }
        } else {
            if self.native_parent != *address {
                self.native_clock_holdover = false;
            }
            self.native_parent = *address;
            self.native_parent_selected = true;
        }
    }

    pub fn clear_native_parent(&mut self) {
        self.native_clock_acquisition_pending = false;
        self.acquisition_root_known = false;
        if self.controls_held {
            if self.pending_controls.parent_selected {
                self.pending_controls.parent_selected = false;
                self.controls_dirty = true;
                self.controls_deferred += 1;
            }
            return;
        }
        self.native_parent_selected = false;
        self.native_clock_selected = false;
        self.native_clock_validated = false;
        self.native_clock_holdover = false;
        self.clock_sample_rejected = false;
        self.synchronized = false;
        self.native_parent = [0; 6];
        self.clock_parent = [0; 6];
        self.clock_root = [0; 6];
        self.master = self.mac;
        self.sync = self.mac;
        self.sequence = self.own_sequence;
        self.presence = self.own_presence;
        self.master_metric = self.metric;
        self.master_counter = self.self_counter;
        self.distance = 0;
        // Keep the physical AW counter/phase through local-master promotion.
    }

    pub fn release_controls(&mut self, now: u64) -> bool {
        if !self.controls_held {
            return false;
        }
        let changed = self.controls_dirty;
        self.controls_held = false;
        self.controls_dirty = false;
        if changed {
            let pending = self.pending_controls;
            self.set_sync_role(pending.role as u32, Some(&pending.forced));
            self.set_sequence(&pending.sequence, now);
            self.native_parent = pending.parent;
            self.native_parent_selected = pending.parent_selected;
            if self.native_election_managed && !self.native_parent_selected {
                self.clear_native_parent();
            }
        }
        self.elect(now);
        changed
    }

    pub fn init(&mut self, address: &[u8; 6], now: u64) {
        zero(self);
        self.mac = *address;
        self.master = *address;
        self.sync = *address;
        self.epoch = now;
        self.election_epoch = now;
        self.metric = 1;
        self.master_metric = 1;
        self.presence = 4;
        self.own_presence = 4;
        self.sync_role = 1;
        self.psf_period = 110;
        self.sequence.count = SLOTS as u8;
        self.sequence.step = 3;
        self.sequence.channels = [6; SLOTS];
        self.own_sequence = self.sequence;
        self.min_extension = [3; 4];
    }

    pub fn counter(&self, now: u64) -> u16 {
        let elapsed = now.saturating_sub(self.epoch);
        self.base_counter
            .wrapping_add((elapsed / AW_US as u64) as u16)
    }

    pub fn until_next(&self, now: u64) -> u32 {
        if !valid_presence(self.presence as u32) {
            return 0;
        }
        let period = self.presence as u32 * AW_US;
        let elapsed = now.saturating_sub(self.epoch);
        let phase = ((self.base_counter as u64 % self.presence as u64) * AW_US as u64
            + elapsed % period as u64)
            % period as u64;
        period - phase as u32
    }

    pub fn channel(&self, now: u64) -> u8 {
        if !self.radio_schedule_ready() || !valid_sequence(&self.sequence) {
            return 0;
        }
        let s = &self.sequence;
        s.channels[(self.counter(now) as usize / (s.step as usize + 1)) % s.count as usize]
    }

    pub fn radio_schedule_ready(&self) -> bool {
        self.sync_role != 2
            || self.synchronized
            || (self.native_clock_holdover
                && self.native_election_managed
                && self.native_clock_selected
                && self.native_clock_validated
                && self.native_parent_selected
                && self.clock_parent == self.native_parent)
    }

    pub fn set_sync_role(&mut self, role: u32, address: Option<&[u8; 6]>) -> bool {
        let Some(address) = address else {
            return false;
        };
        let bad_address = match role {
            2 => *address == self.mac,
            3 => *address != self.mac,
            _ => !self.native_election_managed && *address != self.mac,
        };
        if !(1..=3).contains(&role) || !unicast(address, 0) || bad_address {
            return false;
        }
        let role = role as u8;
        if self.controls_held {
            if self.pending_controls.role != role || self.pending_controls.forced != *address {
                self.pending_controls.role = role;
                self.pending_controls.forced = *address;
                self.controls_dirty = true;
                self.controls_deferred += 1;
            }
            return true;
        }
        if self.sync_role == role && self.forced_sync == *address {
            return true;
        }
        self.hold_clock(0);
        self.sync_role = role;
        self.forced_sync = *address;
        self.synchronized = false;
        self.native_clock_holdover = false;
        self.sequence = self.own_sequence;
        self.presence = self.own_presence;
        self.master = self.mac;
        self.sync = self.mac;
        self.master_metric = self.metric;
        self.master_counter = self.self_counter;
        self.distance = 0;
        true
    }

    pub fn set_presence(&mut self, value: u8, now: u64) -> bool {
        if !valid_presence(value as u32) {
            return false;
        }
        if self.controls_held {
            if self.pending_controls.presence != value {
                self.pending_controls.presence = value;
                self.pending_controls.sequence.step = value - 1;
                self.controls_dirty = true;
                self.controls_deferred += 1;
            }
            return true;
        }
        self.own_presence = value;
        self.own_sequence.step = value - 1;
        if !self.synchronized {
            // Keep the current AW counter and sub-AW phase through a change
            // of EAW duration.
            let elapsed = now.saturating_sub(self.epoch);
            self.base_counter = self.counter(now);
            self.epoch = now - elapsed % AW_US as u64;
            self.presence = value;
            self.sequence.step = value - 1;
        }
        true
    }

    pub fn set_sequence(&mut self, value: &Sequence, now: u64) -> bool {
        if !valid_sequence(value) || !valid_presence(value.step as u32 + 1) {
            return false;
        }
        if self.controls_held {
            if self.pending_controls.sequence != *value {
                self.pending_controls.sequence = *value;
                self.pending_controls.presence = value.step + 1;
                self.controls_dirty = true;
                self.controls_deferred += 1;
            }
        } else {
            self.own_sequence = *value;
            self.set_presence(value.step + 1, now);
            self.sequence = *value;
            self.presence = value.step + 1;
        }
        true
    }

    pub fn set_social_channel(&mut self, index: u32, channel: u8, now: u64) {
        let mut desired = if self.controls_held {
            self.pending_controls.sequence
        } else {
            self.own_sequence
        };
        if index >= desired.count as u32 || index as usize >= SLOTS {
            return;
        }
        desired.channels[index as usize] = channel;
        self.set_sequence(&desired, now);
    }

    pub fn find(&self, address: &[u8; 6]) -> i32 {
        self.peers
            .iter()
            .position(|p| p.used && p.mac == *address)
            .map_or(-1, |i| i as i32)
    }

    /// Picks the synchronization parent and disciplines the AW clock.
    pub fn elect(&mut self, now: u64) {
        // Peer RX may refresh observations but cannot alter an open AW's
        // role, parent, channel or clock.
        if self.controls_held {
            return;
        }
        // Only a top master advances its own election counter.
        if now < self.election_epoch || self.synchronized || self.native_clock_holdover {
            self.election_epoch = now;
        } else {
            let intervals = (now - self.election_epoch) / 3_140_000;
            self.self_counter = self.self_counter.wrapping_add(intervals as u32);
            self.election_epoch += intervals * 3_140_000;
        }
        // A native interface receives its election result through advertisers.
        if self.native_election_managed && !self.native_parent_selected {
            return;
        }
        let mut best: Option<usize> = None;
        for i in 0..MAX_PEERS {
            {
                let p = &mut self.peers[i];
                if p.used && (now < p.seen || now - p.seen > PEER_LIFETIME_US) {
                    p.used = false;
                }
            }
            let p = &self.peers[i];
            if !p.used || p.advertisement.master == self.mac {
                continue;
            }
            let a = &p.advertisement;
            if a.distance >= 10 || a.sync == self.mac {
                continue;
            }
            if self.sync_role == 3 {
                continue;
            }
            if self.native_election_managed {
                if p.mac == self.native_parent {
                    best = Some(i);
                }
                continue;
            }
            if self.sync_role == 2 {
                if p.mac == self.forced_sync {
                    best = Some(i);
                }
                continue;
            }
            // iOS 26: metric precedes counter.
            let rank = a.master_counter.wrapping_sub(self.self_counter) as i32;
            if a.metric < self.metric
                || (a.metric == self.metric
                    && (rank < 0 || (rank == 0 && (a.distance != 0 || p.mac <= self.mac))))
            {
                continue;
            }
            let Some(b_index) = best else {
                best = Some(i);
                continue;
            };
            let b = &self.peers[b_index];
            let rank = a
                .master_counter
                .wrapping_sub(b.advertisement.master_counter) as i32;
            let ba = &b.advertisement;
            if a.metric > ba.metric
                || (a.metric == ba.metric
                    && (rank > 0
                        || (rank == 0
                            && (a.distance < ba.distance
                                || (a.distance == ba.distance && p.mac > b.mac)))))
            {
                best = Some(i);
            }
        }
        let Some(best) = best else {
            // Native selected this clock and still owns the configured
            // sequence: hold over instead of promoting ourselves.
            if self.native_election_managed
                && self.native_clock_selected
                && self.native_clock_validated
                && self.native_parent_selected
                && self.sync_role != 3
                && self.clock_parent == self.native_parent
            {
                if !self.native_clock_holdover {
                    self.native_holdover_transitions += 1;
                }
                self.native_clock_holdover = true;
                self.synchronized = false;
                self.sequence = self.own_sequence;
                self.presence = self.own_presence;
                self.master = self.clock_root;
                self.sync = self.clock_parent;
                return;
            }
            self.native_clock_holdover = false;
            // Keep the AW clock when an elected master expires, but restore
            // our own advertised channel sequence.
            if self.synchronized {
                self.sequence = self.own_sequence;
                self.presence = self.own_presence;
                self.hold_clock(0);
            }
            self.synchronized = false;
            self.master_metric = self.metric;
            self.master_counter = self.self_counter;
            self.distance = 0;
            self.master = self.mac;
            self.sync = self.mac;
            return;
        };
        let p = self.peers[best];
        let same_clock = if self.native_election_managed {
            self.native_clock_selected
                && self.clock_parent == p.mac
                && self.clock_root == p.advertisement.master
        } else {
            self.synchronized && self.sync == p.mac && self.master == p.advertisement.master
        };
        // A native selection authorizes this parent/root only.
        if self.native_election_managed
            && self.native_clock_acquisition_pending
            && (self.acquisition_parent != p.mac
                || (self.acquisition_root_known && self.acquisition_root != p.advertisement.master))
        {
            return;
        }
        let adopt = !self.native_election_managed
            || self.native_clock_acquisition_pending
            || (!self.native_clock_selected && !self.native_timeline_started);
        if self.native_election_managed && !same_clock && !adopt {
            // RX from another clock tree that native has not selected.
            if p.seen > self.clock_sample_at {
                self.clock_sample_at = p.seen;
                self.clock_rejected += 1;
            }
            self.clock_sample_rejected = true;
            self.synchronized = false;
            if !self.native_clock_holdover {
                self.native_holdover_transitions += 1;
            }
            self.native_clock_holdover = true;
            return;
        }
        if adopt && self.native_election_managed && now < self.clock_hold_until {
            if p.seen > self.clock_deferred_at {
                self.clock_deferred_at = p.seen;
                self.clock_deferred += 1;
            }
            return;
        }
        // A timer rechecking a rejected observation is not a new aligned RX.
        self.native_clock_holdover = self.clock_sample_rejected;
        self.synchronized = !self.clock_sample_rejected;
        self.master = p.advertisement.master;
        self.sync = p.mac;
        // A peer supplies the clock, not a replacement for the sequence the
        // native setter accepted.
        self.sequence = if self.native_election_managed {
            self.own_sequence
        } else {
            p.advertisement.sequence
        };
        self.presence = if self.native_election_managed {
            self.own_presence
        } else {
            p.advertisement.presence
        };
        self.master_metric = p.advertisement.metric;
        self.master_counter = p.advertisement.master_counter;
        self.distance = p.advertisement.distance.wrapping_add(1);
        if self.native_election_managed && !same_clock {
            self.native_clock_selected = true;
            self.native_clock_validated = false;
            self.clock_parent = p.mac;
            self.clock_root = p.advertisement.master;
        }
        if adopt {
            self.hold_clock(0);
            // Acquire the selected peer's event/wire clock at a safe edge.
            self.epoch = p.epoch;
            self.base_counter = p.base_counter;
            self.clock_sample_at = p.seen;
            if self.native_election_managed {
                self.native_clock_validated = true;
                self.native_clock_acquisition_pending = false;
                self.clock_sample_rejected = false;
                self.native_clock_holdover = false;
                self.synchronized = true;
                self.native_clock_acquisitions += 1;
            }
        } else if !same_clock || p.seen > self.clock_sample_at {
            let local = now.saturating_sub(self.epoch);
            let remote = now.saturating_sub(p.epoch);
            let peer_counter = p.base_counter.wrapping_add((remote / AW_US as u64) as u16);
            let delta = ((peer_counter as i32 - self.counter(now) as i32) as i16 as i32)
                .wrapping_mul(AW_US as i32)
                + (remote % AW_US as u64) as i32
                - (local % AW_US as u64) as i32;
            // Native permits 5 TU between consecutive matching starts. Slew at
            // most half a TU per fresh PSF; a discontinuous clock needs a new
            // selection.
            if !(-5 * 1024..=5 * 1024).contains(&delta) {
                self.clock_sample_at = p.seen;
                self.clock_rejected += 1;
                self.clock_sample_rejected = true;
                self.synchronized = false;
                if !self.native_clock_holdover {
                    self.native_holdover_transitions += 1;
                }
                self.native_clock_holdover = true;
                return;
            }
            if self.native_election_managed {
                self.native_clock_validated = true;
                self.clock_sample_rejected = false;
                self.native_clock_holdover = false;
                self.synchronized = true;
            }
            if now < self.clock_hold_until {
                if p.seen > self.clock_deferred_at {
                    self.clock_deferred_at = p.seen;
                    self.clock_deferred += 1;
                }
                // Do not consume the sample; the safe edge uses the newest.
                return;
            }
            self.clock_sample_at = p.seen;
            let correction = delta.clamp(-512, 512);
            if correction != 0 {
                let mut base = self.counter(now);
                let mut phase = (local % AW_US as u64) as i32 + correction;
                if phase < 0 {
                    base = base.wrapping_sub(1);
                    phase += AW_US as i32;
                } else if phase >= AW_US as i32 {
                    base = base.wrapping_add(1);
                    phase -= AW_US as i32;
                }
                if now >= phase as u32 as u64 {
                    self.epoch = now - phase as u32 as u64;
                    self.base_counter = base;
                    self.clock_corrections += 1;
                }
            }
        }
    }

    /// Records a peer's sync advertisement. Returns the peer slot or -1.
    pub fn receive_action(&mut self, f: &[u8], now: u64) -> i32 {
        let Some(a) = parse_action(f) else {
            return -1;
        };
        let transmitter = mac(f, 10);
        if transmitter == self.mac {
            return -1;
        }
        let elapsed = (a.presence as u64 * a.period as u64 - a.down as u64) * 1024;
        if now < elapsed {
            return -1;
        }
        let mut idx = self.find(&transmitter);
        if idx < 0 {
            idx = self
                .peers
                .iter()
                .position(|p| !p.used || now < p.seen || now - p.seen > PEER_LIFETIME_US)
                .map_or(-1, |i| i as i32);
        }
        if idx < 0 {
            return -1;
        }
        let p = &mut self.peers[idx as usize];
        if !p.used || now < p.seen || now - p.seen > PEER_LIFETIME_US || p.mac != transmitter {
            *p = Peer::default();
        }
        p.mac = transmitter;
        p.used = true;
        p.seen = now;
        p.advertisement = a;
        p.epoch = now - elapsed;
        p.base_counter = a.counter - a.counter % a.presence as u16;
        self.elect(now);
        idx
    }
}

pub fn write_sequence(p: &mut [u8], s: &Sequence) {
    set(p, 0, s.count.wrapping_sub(1));
    set(p, 1, 0);
    set(p, 2, 0);
    set(p, 3, s.step);
    put16(p, 4, 0xffff);
    copy(p, 6, &s.channels[..(s.count as usize).min(SLOTS)]);
}

pub fn write_sync(s: &State, p: &mut [u8], now: u64) {
    let channel = s.channel(now);
    set(p, 0, channel);
    set(p, 3, channel);
    put16(p, 1, s.until_next(now).div_ceil(1024) as u16);
    put16(p, 5, 16);
    put16(p, 11, 16);
    put16(p, 13, 16);
    let remaining = s.until_next(now);
    let duration = s.presence as u32 * AW_US;
    let elapsed = duration.wrapping_sub(remaining);
    put16(
        p,
        15,
        if elapsed < AW_US {
            ((AW_US - elapsed) / 1024) as u16
        } else {
            0
        },
    );
    // No advertised extension may cross the next scheduled channel change.
    for i in 0..4 {
        let e = s.min_extension[i];
        set(
            p,
            17 + i,
            if e < s.presence {
                e
            } else {
                s.presence.wrapping_sub(1)
            },
        );
    }
    copy(p, 21, &s.master);
    set(p, 27, s.presence);
    put16(p, 29, s.counter(now));
    put16(p, 31, s.counter(now));
    let tail = 33.min(p.len());
    write_sequence(&mut p[tail..], &s.sequence);
}

pub fn write_election(s: &State, p: &mut [u8]) {
    copy(p, 0, &s.master);
    copy(p, 6, &s.sync);
    put32(p, 12, s.master_counter);
    put32(p, 16, s.distance);
    put32(p, 20, s.master_metric);
    put32(p, 24, s.metric);
    put32(p, 36, s.self_counter);
}

fn next_frame_seq(s: &mut State) -> u16 {
    let v = ((s.frame_counter as u32) << 4) as u16;
    s.frame_counter = s.frame_counter.wrapping_add(1);
    v
}

/// Our own PSF. Returns its length, or 0 when `f` is too small or the
/// schedule cannot be advertised.
pub fn build_action(s: &mut State, f: &mut [u8], now: u64) -> usize {
    let count = s.sequence.count as usize;
    let sync_length = 39 + count + 2;
    let size = 40 + 3 + sync_length + 3 + 40 + 3 + 15 + 3 + 2 + 3 + 9;
    if f.len() < size
        || !valid_sequence(&s.sequence)
        || !valid_presence(s.presence as u32)
        || s.sequence.step as u32 + 1 != s.presence as u32
        || !s.radio_schedule_ready()
    {
        return 0;
    }
    let f = &mut f[..size];
    f.fill(0);
    f[0] = 0xd0;
    fill(f, 4, 6, 0xff);
    copy(f, 10, &s.mac);
    copy(f, 16, &BSSID);
    let seq = next_frame_seq(s);
    put16(f, 22, seq);
    f[24] = 127;
    f[26] = 0x17;
    f[27] = 0xf2;
    f[28] = 8;
    f[29] = 0x10;
    f[30] = 3;
    put32(f, 32, now as u32);
    put32(f, 36, now as u32);
    let mut at_ = 40;
    f[at_] = 4;
    put16(f, at_ + 1, sync_length as u16);
    {
        let p = &mut f[at_ + 3..];
        write_sync(s, p, now);
        put16(p, 7, s.psf_period);
        put16(p, 9, 0x1800);
    }
    at_ += 3 + sync_length;
    f[at_] = 24;
    put16(f, at_ + 1, 40);
    write_election(s, &mut f[at_ + 3..]);
    at_ += 43;
    f[at_] = 12;
    put16(f, at_ + 1, 15);
    {
        let p = &mut f[at_ + 3..];
        put16(p, 0, 0x0304);
        p[2] = b'X';
        p[3] = b'0';
        put16(p, 5, 7);
        copy(p, 7, &s.mac);
    }
    at_ += 18;
    f[at_] = 21;
    put16(f, at_ + 1, 2);
    f[at_ + 3] = 0x30;
    f[at_ + 4] = 1;
    at_ += 5;
    f[at_] = 6;
    put16(f, at_ + 1, 9);
    size
}

/// Supplemental TLVs of Tahoe's native SYNC_FRAME_TEMPLATE.
pub fn valid_template_tlvs(p: &[u8]) -> bool {
    let n = p.len();
    if n == 0 || n > 2048 {
        return false;
    }
    let mut election = false;
    let mut at_ = 0;
    while at_ < n {
        if n - at_ < 3 {
            return false;
        }
        let kind = p[at_];
        let length = le16(p, at_ + 1) as usize;
        at_ += 3;
        if length > n - at_ {
            return false;
        }
        match kind {
            4 | 18 => return false, // scheduler owns timing/sequence
            24 => {
                if election || length != 40 {
                    return false;
                }
                election = true;
            }
            5 if length != 21 => return false,
            20 if length % 6 != 0 => return false,
            _ => {}
        }
        at_ += length;
    }
    true
}

fn patch_legacy_election(s: &State, v: &mut [u8]) {
    set(v, 3, s.distance as u8);
    copy(v, 5, &s.master);
    put32(v, 11, s.master_metric);
    put32(v, 15, s.metric);
}

/// Our sync/election prefix followed by the native template TLVs (with the
/// election aligned to our clock and the stale sync-tree path dropped).
pub fn build_action_with_template(s: &mut State, f: &mut [u8], now: u64, p: &[u8]) -> usize {
    if !valid_template_tlvs(p) {
        return 0;
    }
    let n = p.len();
    // Validate capacity before build_action advances counters or writes.
    let prefix = 43 + 39 + s.sequence.count as usize + 2;
    if f.len() < prefix || n > f.len() - prefix {
        return 0;
    }
    let mut base = [0u8; 256];
    let base_length = build_action(s, &mut base, now);
    if base_length == 0 || prefix > base_length {
        return 0;
    }
    f[..prefix].copy_from_slice(&base[..prefix]);
    let mut dst = prefix;
    let mut at_ = 0;
    while at_ < n {
        let kind = p[at_];
        let len = le16(p, at_ + 1) as usize;
        if kind != 20 {
            f[dst..dst + 3 + len].copy_from_slice(&p[at_..at_ + 3 + len]);
            if kind == 24 {
                write_election(s, &mut f[dst + 3..dst + 3 + len]);
            }
            if kind == 5 {
                patch_legacy_election(s, &mut f[dst + 3..dst + 3 + len]);
            }
            dst += 3 + len;
        }
        at_ += 3 + len;
    }
    dst
}

pub const BLOCK_ACK_CATEGORY: u8 = 3;
pub const ADDBA_REQUEST: u8 = 0;
pub const ADDBA_RESPONSE: u8 = 1;
pub const DELBA: u8 = 2;
pub const STATUS_REQUEST_DECLINED: u16 = 37;

/// 802.11 Block Ack action frame (category 3) to `own` on the AWDL BSS.
pub fn block_ack_action(f: &[u8], own: &[u8; 6]) -> bool {
    f.len() >= 26
        && f[0] == 0xd0
        && f[1] & 0xc7 == 0
        && eq(f, 4, own)
        && unicast(f, 10)
        && !eq(f, 10, own)
        && eq(f, 16, &BSSID)
        && f[24] == BLOCK_ACK_CATEGORY
}

/// Largest RX Block Ack window offered to AWDL peers (MSDU replay window).
pub const RX_BA_WINDOW: u16 = 64;

/// An immediate-policy ADDBA request: TID, starting sequence number and
/// the requested window (0 means the peer leaves it to us).
#[derive(Debug, Clone, Copy, PartialEq, Eq)]
pub struct AddbaRequest {
    pub tid: u8,
    pub ssn: u16,
    pub window: u16,
}

pub fn addba_request(own: &[u8; 6], request: &[u8]) -> Option<AddbaRequest> {
    if request.len() < 24 + 9 || !block_ack_action(request, own) || request[25] != ADDBA_REQUEST {
        return None;
    }
    let params = le16(request, 27);
    // Delayed Block Ack (policy bit clear) is not supported by MT7921.
    if params & 2 == 0 {
        return None;
    }
    let tid = ((params >> 2) & 0xf) as u8;
    if tid > 7 {
        return None;
    }
    let requested = params >> 6;
    let window = if requested == 0 { RX_BA_WINDOW } else { requested.min(RX_BA_WINDOW) };
    Some(AddbaRequest { tid, ssn: le16(request, 31) >> 4, window })
}

/// A DELBA from the peer as originator ends our RX session for its TID.
pub fn delba_rx_tid(own: &[u8; 6], f: &[u8]) -> Option<u8> {
    if f.len() < 24 + 6 || !block_ack_action(f, own) || f[25] != DELBA {
        return None;
    }
    let params = le16(f, 26);
    (params & 0x0800 != 0).then_some((params >> 12) as u8 & 7)
}

fn build_addba_response(s: &mut State, request: &[u8], f: &mut [u8], status: u16, params: u16) -> usize {
    const SIZE: usize = 24 + 9;
    let f = &mut f[..SIZE];
    f.fill(0);
    f[0] = 0xd0;
    f[4..10].copy_from_slice(&request[10..16]);
    f[10..16].copy_from_slice(&s.mac);
    f[16..22].copy_from_slice(&BSSID);
    let seq = next_frame_seq(s);
    put16(f, 22, seq);
    f[24] = BLOCK_ACK_CATEGORY;
    f[25] = ADDBA_RESPONSE;
    f[26] = request[26];
    put16(f, 27, status);
    put16(f, 29, params);
    f[31..33].copy_from_slice(&request[29..31]);
    SIZE
}

/// Declines an ADDBA request (firmware BA session unavailable or disabled).
pub fn build_addba_decline(s: &mut State, request: &[u8], f: &mut [u8]) -> usize {
    if f.len() < 24 + 9
        || !block_ack_action(request, &s.mac)
        || request[25] != ADDBA_REQUEST
        || request.len() < 24 + 9
    {
        return 0;
    }
    let params = le16(request, 27);
    build_addba_response(s, request, f, STATUS_REQUEST_DECLINED, params)
}

/// Accepts an immediate ADDBA request with at most `window` MSDUs, after the
/// firmware recipient session is installed. Keeps the A-MSDU bit: A-MSDU
/// subframes inside A-MPDUs are decoded (1.0.2).
pub fn build_addba_accept(s: &mut State, request: &[u8], window: u16, f: &mut [u8]) -> usize {
    let Some(r) = addba_request(&s.mac, request) else { return 0 };
    if f.len() < 24 + 9 || window == 0 {
        return 0;
    }
    let window = window.min(r.window);
    let params = (le16(request, 27) & 0x3f) | (window << 6);
    build_addba_response(s, request, f, 0, params)
}

/// Refreshes a native action-frame template in place: our sync/sequence/
/// election values, TLVs shifted down. Validation runs first; a rejection
/// leaves the template and the state untouched.
pub fn refresh_action(s: &mut State, f: &mut [u8], now: u64) -> usize {
    let n = f.len();
    if n < 40
        || f[0] != 0xd0
        || f[1] & 0xc7 != 0
        || !eq(f, 24, &ACTION)
        || (f[30] != 0 && f[30] != 3)
        || !valid_sequence(&s.sequence)
        || s.sequence.step as u32 + 1 != s.presence as u32
        || !s.radio_schedule_ready()
    {
        return 0;
    }
    let count = s.sequence.count as usize;
    let (mut has_sync, mut has_election, mut has_sequence, mut has_legacy) =
        (false, false, false, false);
    let mut at_ = 40;
    while at_ < n {
        if n - at_ < 3 {
            return 0;
        }
        let kind = f[at_];
        let len = le16(f, at_ + 1) as usize;
        at_ += 3;
        if len > n - at_ {
            return 0;
        }
        let body = &f[at_..at_ + len];
        match kind {
            4 => {
                if has_sync || len < 39 || parse_sequence(&body[33..]).is_none() || len < 39 + count
                {
                    return 0;
                }
                has_sync = true;
            }
            18 => {
                if has_sequence || parse_sequence(body).is_none() || len < 6 + count {
                    return 0;
                }
                has_sequence = true;
            }
            24 => {
                if has_election || len != 40 {
                    return 0;
                }
                has_election = true;
            }
            5 => {
                if has_legacy || len != 21 {
                    return 0;
                }
                has_legacy = true;
            }
            20 if len % 6 != 0 => return 0,
            _ => {}
        }
        at_ += len;
    }
    if !has_sync {
        return 0;
    }
    let mut dst = 40;
    let mut src = 40;
    while src < n {
        let kind = f[src];
        let old = le16(f, src + 1) as usize;
        let len = match kind {
            4 => 39 + count,
            18 => 6 + count,
            _ => old,
        };
        // The template's election tree may belong to a previous master.
        if kind == 20 {
            src += 3 + old;
            continue;
        }
        // len <= old (validated above) and dst <= src: a forward move.
        f.copy_within(src..src + 3 + len, dst);
        put16(f, dst + 1, len as u16);
        {
            let p = &mut f[dst + 3..dst + 3 + len];
            match kind {
                4 => write_sync(s, p, now),
                18 => write_sequence(p, &s.sequence),
                24 => write_election(s, p),
                5 => patch_legacy_election(s, p),
                _ => {}
            }
        }
        src += 3 + old;
        dst += 3 + len;
    }
    f[10..16].copy_from_slice(&s.mac);
    f[16..22].copy_from_slice(&BSSID);
    let seq = next_frame_seq(s);
    put16(f, 22, seq);
    put32(f, 32, now as u32);
    put32(f, 36, now as u32);
    dst
}

pub fn valid_ipv6(p: &[u8]) -> bool {
    let n = p.len();
    if n < 40 || p[0] >> 4 != 6 {
        return false;
    }
    let payload = p[4] as usize * 256 + p[5] as usize;
    // Ethernet MTU bounds this path; IPv6 jumbograms are unsupported.
    n == 40 + payload
}

/// Ethernet (IPv6 only) to AWDL data frame. Returns the frame length or 0.
pub fn encode_data(out: &mut [u8], eth: &[u8], seq: u16) -> usize {
    let n = eth.len();
    if !(54..=1514).contains(&n)
        || eth[12] != 0x86
        || eth[13] != 0xdd
        || !unicast(eth, 6)
        || !valid_ipv6(&eth[14..])
        || out.len() < n + 26
    {
        return 0;
    }
    let out = &mut out[..n + 26];
    out[..40].fill(0);
    out[0] = 8;
    out[4..10].copy_from_slice(&eth[..6]);
    out[10..16].copy_from_slice(&eth[6..12]);
    out[16..22].copy_from_slice(&BSSID);
    put16(out, 22, ((seq as u32) << 4) as u16);
    out[24..32].copy_from_slice(&SNAP);
    put16(out, 32, 0x0403);
    put16(out, 34, seq);
    out[38..].copy_from_slice(&eth[12..]);
    n + 26
}

/// 802.11 header length of an AWDL data frame (24, 26 for QoS), or 0.
pub fn data_header_length(f: &[u8]) -> usize {
    if f.len() < 40
        || (f[0] != 8 && f[0] != 0x88)
        || f[1] & 0xc7 != 0
        || f[22] & 15 != 0
        || !unicast(f, 10)
        || !eq(f, 16, &BSSID)
    {
        return 0;
    }
    let hdr = if f[0] == 0x88 { 26 } else { 24 };
    // Fragment reassembly and HT-control/mesh headers remain unsupported.
    if hdr == 26 && f[25] & 1 != 0 {
        return 0;
    }
    hdr
}

/// MT7921 hardware deaggregation adds two bytes after the 802.11 header:
/// move the header up so the caller can strip two leading bytes.
pub fn relocate_deaggregated_header(f: &mut [u8]) -> bool {
    let n = f.len();
    let hdr = data_header_length(f);
    if hdr != 26 || f[24] & 0x80 == 0 || n < hdr + 2 + 14 + 56 {
        return false;
    }
    f.copy_within(0..hdr, 2);
    true
}

/// One MSDU of a data frame, as offsets into the frame.
#[derive(Debug, Clone, Copy, PartialEq, Eq)]
pub struct DataView {
    pub destination: usize,
    pub source: usize,
    pub payload: usize,
    pub length: usize,
    pub sequence: u16,
}

/// The MSDU at `at`; returns the bytes it uses (with A-MSDU padding) and
/// its view, or `None`.
pub fn data_view(f: &[u8], at_: usize) -> Option<(usize, DataView)> {
    let n = f.len();
    let hdr = data_header_length(f);
    if hdr == 0 || at_ < hdr || at_ >= n {
        return None;
    }
    let aggregate = hdr == 26 && f[24] & 0x80 != 0;
    let (mut dst, mut src, mut p) = (4, 10, at_);
    let mut len = n - at_;
    let mut used = len;
    if aggregate {
        if len < 14 {
            return None;
        }
        dst = p;
        src = p + 6;
        let body = f[p + 12] as usize * 256 + f[p + 13] as usize;
        if body > len - 14 || f[src..src + 6] != f[10..16] {
            return None;
        }
        used = 14 + body;
        p += 14;
        len = body;
        if used < n - at_ {
            let pad = (4 - used % 4) % 4;
            // Padding occurs only between complete subframes.
            if n - at_ - used < pad + 14 + 56 {
                return None;
            }
            used += pad;
        }
    } else {
        if at_ != hdr {
            return None;
        }
    }
    // iOS 27 captures: QoS RX carries RFC1042 LLC (zero OUI), a BE
    // length, then AWDL SNAP, also INSIDE each A-MSDU subframe. Bound the
    // reader to this MSDU, excluding aggregate padding and later subframes.
    // Never scan for an inner SNAP or relax AWDL/IPv6 validation below.
    if hdr == 26 {
        let mut reader = Reader::new(&f[p..p + len]);
        if reader.take(6).ok() == Some(&[0xaa, 0xaa, 3, 0, 0, 0][..]) {
            let body = reader.be16().ok()? as usize;
            if body != reader.remaining() {
                return None;
            }
            p += reader.position();
            len = body;
        }
    }
    let payload = &f[p..p + len];
    if !(56..=1516).contains(&len)
        || payload[..8] != SNAP
        || le16(payload, 8) != 0x0403
        || le16(payload, 12) != 0
        || payload[14] != 0x86
        || payload[15] != 0xdd
        || !valid_ipv6(&payload[16..])
    {
        return None;
    }
    Some((
        used,
        DataView {
            destination: dst,
            source: src,
            payload: p,
            length: len,
            sequence: le16(payload, 10),
        },
    ))
}

pub fn valid_data_frame(f: &[u8]) -> bool {
    let mut at_ = data_header_length(f);
    if at_ == 0 || f.len() > 11454 {
        return false;
    }
    while at_ < f.len() {
        let Some((used, _)) = data_view(f, at_) else {
            return false;
        };
        at_ += used;
    }
    true
}

/// Ethernet frame for one MSDU: destination, source, then the payload after
/// the SNAP/AWDL header. Returns its length or 0.
pub fn decode_data_view(
    out: &mut [u8],
    destination: &[u8],
    source: &[u8],
    payload: &[u8],
) -> usize {
    let length = payload.len();
    if !(56..=1516).contains(&length) || destination.len() < 6 || source.len() < 6 {
        return 0;
    }
    let len = length - 2;
    if out.len() < len {
        return 0;
    }
    out[..6].copy_from_slice(&destination[..6]);
    out[6..12].copy_from_slice(&source[..6]);
    out[12..len].copy_from_slice(&payload[14..]);
    len
}

/// Single-MSDU decode; fills `seq` on success.
pub fn decode_data(out: &mut [u8], f: &[u8], seq: &mut u16) -> usize {
    let hdr = data_header_length(f);
    if hdr == 0 || (hdr == 26 && f[24] & 0x80 != 0) {
        return 0;
    }
    let Some((_, v)) = data_view(f, hdr) else {
        return 0;
    };
    let len = decode_data_view(
        out,
        &f[v.destination..v.destination + 6],
        &f[v.source..v.source + 6],
        &f[v.payload..v.payload + v.length],
    );
    if len != 0 {
        *seq = v.sequence;
    }
    len
}

/// Reserved software PIDs distinguish AWDL frame classes in MT7921 TXS.
pub const TX_PID_UNICAST: u8 = 125;
pub const TX_PID_MULTICAST: u8 = 126;
pub const TX_PID_ACTION: u8 = 127;

/// ACK-driven unicast rate ladder, 20 MHz long GI, single stream:
/// OFDM 6/12/24/36/48/54 Mbps, then HT MCS 7 (65 Mbps). Every AWDL peer is
/// an 802.11n device; a rung it cannot receive fails, which steps back down.
/// MT7921 TX_RATE: mode at bit 6 (OFDM 1, HT 2), rate/MCS index below.
pub const TX_RATES: [u16; 7] = [0x4b, 0x4a, 0x49, 0x4d, 0x48, 0x4c, 0x87];
pub const TX_RATE_MBPS: [u16; 7] = [6, 12, 24, 36, 48, 54, 65];
const TX_RATE_OFDM_TOP: usize = 5;
const TX_PROBE_ACKS: u8 = 32;
const TX_CLEAN_ACKS: u8 = 128;
const TX_HOLD: u16 = 256;
const TX_STALE_LIMIT: u8 = 128;

#[repr(C)]
#[derive(Debug, Clone, Copy, Default, PartialEq, Eq)]
pub struct TxRate {
    pub index: u8,
    /// ACKs at this rate since it was selected or last failed.
    pub successes: u8,
    /// A failure within the last 128 ACKs. Single losses are usually the
    /// peer being off channel (1.0.3: ~1 in 2000), not the rate.
    pub failed: bool,
    /// Promoted and not yet confirmed by 32 ACKs.
    pub probing: bool,
    /// Failed probes in a row; the hold doubles with each, up to 16x.
    pub backoff: u8,
    /// Reports at another rate since the last matching one.
    pub stale: u8,
    pub probe_after: u16,
}

impl TxRate {
    fn top(ofdm_only: bool) -> usize {
        if ofdm_only { TX_RATE_OFDM_TOP } else { TX_RATES.len() - 1 }
    }

    pub fn current(&self, ofdm_only: bool) -> usize {
        (self.index as usize).min(Self::top(ofdm_only))
    }

    pub fn rate(&self, ofdm_only: bool) -> u16 {
        TX_RATES[self.current(ofdm_only)]
    }

    fn step_down(&mut self, ofdm_only: bool) {
        self.index = self.current(ofdm_only).saturating_sub(1) as u8;
        self.successes = 0;
        self.failed = false;
        self.probing = false;
        self.stale = 0;
        self.probe_after = TX_HOLD << self.backoff;
    }

    pub fn observe(&mut self, status: u32, ofdm_only: bool) {
        // Queued completions at the previous rate cannot promote or demote
        // the new one. Compare mode and index only (not NSS/STBC/BW bits).
        // If nothing matches for long, the firmware is not reporting this
        // rung as sent: step down rather than fly blind.
        if status & 0x3ff != self.rate(ofdm_only) as u32 & 0x3ff {
            self.stale = self.stale.saturating_add(1);
            if self.stale >= TX_STALE_LIMIT && self.current(ofdm_only) > 0 {
                self.step_down(ofdm_only);
            }
            return;
        }
        self.stale = 0;
        if status & 0x0007_0000 != 0 {
            if self.failed || self.probing {
                if self.probing {
                    self.backoff = (self.backoff + 1).min(4);
                }
                self.step_down(ofdm_only);
            } else {
                self.failed = true;
                self.successes = 0;
            }
            return;
        }
        self.successes = self.successes.saturating_add(1);
        if self.probing && self.successes >= TX_PROBE_ACKS {
            self.probing = false;
            self.backoff = 0;
        }
        if self.successes >= TX_CLEAN_ACKS {
            self.failed = false;
        }
        if self.probe_after != 0 {
            self.probe_after -= 1;
        } else if !self.failed && self.successes >= TX_PROBE_ACKS
            && self.current(ofdm_only) < Self::top(ofdm_only) {
            self.index = self.current(ofdm_only) as u8 + 1;
            self.successes = 0;
            self.probing = true;
        }
    }
}

#[repr(C)]
#[derive(Debug, Clone, Copy, Default, PartialEq, Eq)]
pub struct TxStatusCounters {
    pub data_acked: u64,
    pub data_failed: u64,
    pub multicast_completed: u64,
    pub multicast_failed: u64,
    pub action_completed: u64,
    pub action_failed: u64,
    pub last_status: u32,
    pub last_sequence: u16,
    pub last_wcid: u16,
    pub last_pid: u8,
    pub rates: [TxRate; 3], // unicast WCIDs 15..17
    /// Boot-arg amtk_awdl_ht=0: stop the ladder at 54 Mbps OFDM.
    pub ofdm_only: bool,
}

impl TxStatusCounters {
    pub fn rate(&self, wcid: u16) -> u16 {
        wcid.checked_sub(15).and_then(|i| self.rates.get(i as usize))
            .map_or(TX_RATES[0], |r| r.rate(self.ofdm_only))
    }

    pub fn rate_mbps(&self, peer: usize) -> u16 {
        self.rates.get(peer).map_or(0, |r| TX_RATE_MBPS[r.current(self.ofdm_only)])
    }

    /// Consumes a 32-byte TXS record when it belongs to an AWDL WCID
    /// (15..18). Returns whether it was AWDL's.
    pub fn observe(&mut self, p: &[u8]) -> bool {
        if p.len() < 32 {
            return false;
        }
        let status = le32(p, 0);
        let wcid = (le32(p, 8) >> 16) & 0x3ff;
        if !(15..=18).contains(&wcid) {
            return false;
        }
        let pid = (le32(p, 12) >> 24) as u8;
        // MT76 accepts only MPDU formats 0/1 here, not PPDU summaries.
        if (status >> 23) & 3 > 1 || !(TX_PID_UNICAST..=TX_PID_ACTION).contains(&pid) {
            return true;
        }
        let failed = status & 0x0007_0000 != 0; // queue, RTS, or ACK timeout
        self.last_status = status;
        self.last_sequence = (le32(p, 4) >> 20) as u16;
        self.last_wcid = wcid as u16;
        self.last_pid = pid;
        if pid == TX_PID_UNICAST && wcid < 18 {
            self.rates[(wcid - 15) as usize].observe(status, self.ofdm_only);
        }
        let counter = match (pid, failed) {
            (TX_PID_UNICAST, true) => &mut self.data_failed,
            (TX_PID_UNICAST, false) => &mut self.data_acked,
            (TX_PID_MULTICAST, true) => &mut self.multicast_failed,
            (TX_PID_MULTICAST, false) => &mut self.multicast_completed,
            (_, true) => &mut self.action_failed,
            (_, false) => &mut self.action_completed,
        };
        *counter += 1;
        true
    }
}

/// A matching, unexpired firmware grant is mandatory before any transmission.
#[repr(u8)]
#[derive(Debug, Clone, Copy, PartialEq, Eq)]
pub enum RocResult {
    Accepted,
    Malformed,
    NoGrant,
    WrongBss,
    FirmwareStatus,
    WrongType,
    Disabled,
    NotPending,
    Token,
    Channel,
    Late,
    Duration,
    Count,
}

#[repr(C)]
#[derive(Debug, Clone, Copy, Default, PartialEq, Eq)]
pub struct Reservation {
    pub deadline: u64,
    pub grant_observed_at: u64,
    pub requested_at: u64,
    pub token: u8,
    pub channel: u8,
    pub pending: bool,
    pub granted: bool,
    pub response_delay_us: u32,
}

impl Reservation {
    pub fn request(&mut self, token: u8, channel: u8, now: u64) {
        self.token = token;
        self.channel = channel;
        self.pending = true;
        self.granted = false;
        self.grant_observed_at = 0;
        self.requested_at = now;
        self.deadline = now + ROC_RESPONSE_TIMEOUT_US as u64;
        self.response_delay_us = 0;
    }

    pub fn accept(&mut self, token: u8, channel: u8, duration_ms: u32, now: u64) -> RocResult {
        if !self.pending {
            return RocResult::NotPending;
        }
        if token != self.token {
            return RocResult::Token;
        }
        if channel != self.channel {
            return RocResult::Channel;
        }
        if now < self.requested_at || now >= self.deadline {
            return RocResult::Late;
        }
        if duration_ms == 0 || duration_ms > MAX_ROC_MS {
            return RocResult::Duration;
        }
        self.response_delay_us = (now - self.requested_at) as u32;
        self.pending = false;
        self.granted = true;
        self.grant_observed_at = now;
        self.deadline = now + duration_ms as u64 * 1000;
        RocResult::Accepted
    }

    pub fn grant(&mut self, token: u8, channel: u8, duration_ms: u32, now: u64) -> bool {
        self.accept(token, channel, duration_ms, now) == RocResult::Accepted
    }

    pub fn can_transmit(&self, channel: u8, now: u64) -> bool {
        self.granted && self.channel == channel && now < self.deadline && self.deadline - now > 2000
    }

    pub fn cancel(&mut self) {
        self.pending = false;
        self.granted = false;
        self.deadline = 0;
        self.grant_observed_at = 0;
        self.requested_at = 0;
        self.response_delay_us = 0;
    }
}

#[repr(C)]
#[derive(Debug, Clone, Copy, Default, PartialEq, Eq)]
pub struct RocGrant {
    pub bss: u8,
    pub token: u8,
    pub status: u8,
    pub channel: u8,
    pub request_type: u8,
    pub duration_ms: u32,
}

/// Unified ROC event: four-byte header, then TLVs (tag u16, len u16 with
/// the header). MT7921's grant tag is 0 and 20 bytes long.
pub fn decode_roc_grant(p: &[u8], out: &mut RocGrant) -> RocResult {
    *out = RocGrant::default();
    let n = p.len();
    if n < 4 {
        return RocResult::Malformed;
    }
    let mut found = false;
    let mut at_ = 4;
    while at_ < n {
        if n - at_ < 4 {
            return RocResult::Malformed;
        }
        let tag = le16(p, at_);
        let len = le16(p, at_ + 2) as usize;
        if len < 4 || len > n - at_ {
            return RocResult::Malformed;
        }
        if tag == 0 {
            if found || len != 20 {
                return RocResult::Malformed;
            }
            found = true;
            *out = RocGrant {
                bss: p[at_ + 4],
                token: p[at_ + 5],
                status: p[at_ + 6],
                channel: p[at_ + 7],
                request_type: p[at_ + 13],
                duration_ms: le32(p, at_ + 16),
            };
        }
        at_ += len;
    }
    if found {
        RocResult::Accepted
    } else {
        RocResult::NoGrant
    }
}

pub fn receive_roc_grant(
    reservation: &mut Reservation,
    p: &[u8],
    bss: u8,
    enabled: bool,
    now: u64,
    out: &mut RocGrant,
) -> RocResult {
    let result = decode_roc_grant(p, out);
    if result != RocResult::Accepted {
        return result;
    }
    if out.bss != bss {
        return RocResult::WrongBss;
    }
    if out.status != 0 {
        return RocResult::FirmwareStatus;
    }
    if out.request_type != 1 {
        return RocResult::WrongType;
    }
    if !enabled {
        return RocResult::Disabled;
    }
    reservation.accept(out.token, out.channel, out.duration_ms, now)
}

#[cfg(test)]
mod tests {
    use super::*;
    use std::vec::Vec;

    fn qos_llc_frame(size: usize) -> (Vec<u8>, Vec<u8>) {
        let mut eth = std::vec![0; size];
        eth[..6].copy_from_slice(&[2, 1, 2, 3, 4, 5]);
        eth[6..12].copy_from_slice(&[2, 1, 2, 3, 4, 6]);
        eth[12..15].copy_from_slice(&[0x86, 0xdd, 0x60]);
        eth[18..20].copy_from_slice(&((size - 54) as u16).to_be_bytes());
        let mut wire = std::vec![0; size + 26];
        assert_eq!(encode_data(&mut wire, &eth, 65535), wire.len());
        let mut frame = wire[..24].to_vec();
        frame[0] = 0x88;
        frame.extend_from_slice(&[0, 0, 0xaa, 0xaa, 3, 0, 0, 0]);
        frame.extend_from_slice(&((wire.len() - 24) as u16).to_be_bytes());
        frame.extend_from_slice(&wire[24..]);
        (frame, eth)
    }

    #[test]
    fn qos_llc_length_envelope_decodes_without_changing_msdu() {
        // Captured rejected frames include both short packets and IPv6
        // packets up to MTU 1500. Test every size and original plain QoS.
        for size in 54..=1514 {
            let (frame, eth) = qos_llc_frame(size);
            assert!(valid_data_frame(&frame));
            let (used, view) = data_view(&frame, 26).unwrap();
            assert_eq!(used, frame.len() - 26);
            assert_eq!(view.payload, 34);
            assert_eq!(view.sequence, 65535);
            let mut out = std::vec![0; size];
            assert_eq!(decode_data(&mut out, &frame, &mut 0), size);
            assert_eq!(out, eth);
            assert_eq!(decode_data(&mut out[..size - 1], &frame, &mut 0), 0);
            let mut plain = frame[..26].to_vec();
            plain.extend_from_slice(&frame[34..]);
            assert!(valid_data_frame(&plain));
            assert_eq!(decode_data(&mut out, &plain, &mut 0), size);
            assert_eq!(out, eth);
        }
    }

    #[test]
    fn old_and_new_ios_framings_decode_the_same_msdu() {
        // iOS 18 and older put the AWDL SNAP right after the 802.11 header
        // (plain or QoS, or in an A-MSDU); iOS 27 adds the LLC length
        // envelope. All of them must keep decoding to the same packet.
        for size in [54, 60, 200, 1514] {
            let (envelope, eth) = qos_llc_frame(size);
            let qos = [&envelope[..26], &envelope[34..]].concat();
            let mut plain = [&envelope[..24], &envelope[34..]].concat();
            plain[0] = 8;
            let msdu = &envelope[34..];
            let mut amsdu = envelope[..26].to_vec();
            amsdu[24] = 0x80;
            amsdu.extend_from_slice(&envelope[4..10]);
            amsdu.extend_from_slice(&envelope[10..16]);
            amsdu.extend_from_slice(&(msdu.len() as u16).to_be_bytes());
            amsdu.extend_from_slice(msdu);
            for (name, frame) in [("non-QoS", &plain), ("QoS", &qos), ("iOS 27 envelope", &envelope)] {
                assert!(valid_data_frame(frame), "{name} {size}");
                let mut out = std::vec![0; size];
                assert_eq!(decode_data(&mut out, frame, &mut 0), size, "{name} {size}");
                assert_eq!(out, eth, "{name} {size}");
            }
            // A-MSDU goes through data_view per subframe, as the C++ RX loop.
            assert!(valid_data_frame(&amsdu), "A-MSDU {size}");
            let (used, v) = data_view(&amsdu, 26).unwrap();
            assert_eq!(used, amsdu.len() - 26);
            let mut out = std::vec![0; size];
            let len = decode_data_view(&mut out, &amsdu[v.destination..v.destination + 6],
                                       &amsdu[v.source..v.source + 6], &amsdu[v.payload..v.payload + v.length]);
            assert_eq!(len, size, "A-MSDU {size}");
            assert_eq!(out, eth, "A-MSDU {size}");
        }
    }

    fn append_subframe(frame: &mut Vec<u8>, envelope: &[u8], wrapped: bool) {
        if frame.len() != 26 {
            // A-MSDU padding is relative to each subframe, not the MAC header.
            let pad = (4 - (frame.len() - 26) % 4) % 4;
            frame.resize(frame.len() + pad, 0);
        }
        let msdu = &envelope[if wrapped { 26 } else { 34 }..];
        frame.extend_from_slice(&envelope[4..16]);
        frame.extend_from_slice(&(msdu.len() as u16).to_be_bytes());
        frame.extend_from_slice(msdu);
    }

    #[test]
    fn ios27_aggregate_llc_envelopes_keep_subframe_boundaries() {
        // 2026-10-26: iOS 27 QUIC upload MSDUs also carry the eight-byte
        // envelope inside A-MSDU subframes. The old non-aggregate-only gate
        // discarded these, including fully retained 146-byte frames.
        for size in 54..=1514 {
            let (envelope, eth) = qos_llc_frame(size);
            for wrapped in [true, false] {
                let mut frame = envelope[..26].to_vec();
                frame[24] = 0x80;
                append_subframe(&mut frame, &envelope, wrapped);
                append_subframe(&mut frame, &envelope, !wrapped);
                assert!(valid_data_frame(&frame), "size {size}, wrapped {wrapped}");
                let mut at = 26;
                for _ in 0..2 {
                    let (used, view) = data_view(&frame, at).unwrap();
                    let mut out = std::vec![0; size];
                    assert_eq!(decode_data_view(&mut out,
                        &frame[view.destination..view.destination + 6],
                        &frame[view.source..view.source + 6],
                        &frame[view.payload..view.payload + view.length]), size);
                    assert_eq!(out, eth);
                    assert_eq!(view.sequence, 65535);
                    at += used;
                }
                assert_eq!(at, frame.len());
            }
        }
    }

    #[test]
    fn ios27_aggregate_envelope_rejects_invalid_lengths_and_payloads() {
        let (envelope, _) = qos_llc_frame(96);
        let mut frame = envelope[..26].to_vec();
        frame[24] = 0x80;
        append_subframe(&mut frame, &envelope, true);
        // One subframe has exactly the captured short-frame shape: 26-byte
        // MAC + 14-byte subheader + 8-byte envelope + 98-byte AWDL MSDU.
        assert_eq!(frame.len(), 146);
        assert!(valid_data_frame(&frame));
        for n in 0..frame.len() {
            assert!(!valid_data_frame(&frame[..n]), "truncated at {n}");
        }
        // Subframe source, outer/inner lengths, LLC/OUI, SNAP, AWDL and IPv6.
        for offset in [32, 38, 39, 40, 41, 42, 43, 44, 45, 46, 47,
                       48, 52, 56, 60, 62, 64, 68] {
            let mut bad = frame.clone();
            bad[offset] ^= if offset == 64 { 0x10 } else { 1 };
            assert!(!valid_data_frame(&bad), "offset {offset}");
        }
        append_subframe(&mut frame, &envelope, true);
        let mut crosses_boundary = frame.clone();
        // An inner length covering subsequent subframes must be rejected.
        crosses_boundary[46..48].copy_from_slice(&((frame.len() - 48) as u16).to_be_bytes());
        assert!(!valid_data_frame(&crosses_boundary));
        let mut bad_second = frame.clone();
        let (used, _) = data_view(&frame, 26).unwrap();
        bad_second[26 + used + 14 + 8] ^= 1;
        assert!(!valid_data_frame(&bad_second));
        let mut trailing = frame.clone();
        trailing.push(0);
        assert!(!valid_data_frame(&trailing));
    }

    #[test]
    fn qos_llc_envelope_rejects_truncation_and_malformed_inner_data() {
        let (frame, _) = qos_llc_frame(1514);
        for n in 0..frame.len() {
            assert!(!valid_data_frame(&frame[..n]));
        }
        // LLC/OUI, BE length, inner SNAP, AWDL header and IPv6 length.
        for offset in [26, 27, 28, 29, 30, 31, 32, 33, 34, 38, 42, 46, 48, 50, 54] {
            let mut bad = frame.clone();
            bad[offset] ^= if offset == 50 { 0x10 } else { 1 };
            assert!(!valid_data_frame(&bad), "offset {offset}");
        }
        let mut trailing = frame.clone();
        trailing.push(0);
        assert!(!valid_data_frame(&trailing));
        let mut nested = frame[..34].to_vec();
        nested.extend_from_slice(&frame[26..]);
        let body = (nested.len() - 34) as u16;
        nested[32..34].copy_from_slice(&body.to_be_bytes());
        assert!(!valid_data_frame(&nested));
        let mut aggregate = frame.clone();
        aggregate[24] = 0x80;
        assert!(!valid_data_frame(&aggregate));
        let mut non_qos = frame[..24].to_vec();
        non_qos[0] = 8;
        non_qos.extend_from_slice(&frame[26..]);
        assert!(!valid_data_frame(&non_qos));
        let mut oversized = frame.clone();
        oversized.push(0);
        oversized[32..34].copy_from_slice(&1517u16.to_be_bytes());
        oversized[54..56].copy_from_slice(&1461u16.to_be_bytes());
        assert!(!valid_data_frame(&oversized));
    }

    #[test]
    fn independent_data_streams_and_idle_restart() {
        let mut peers = DataPeers::default();
        let mac = [2, 1, 2, 3, 4, 5];
        let start = 10_000_000;
        assert!(peers.set_cached(&mac, true, start));
        let index = peers.find(&mac, start) as usize;
        // 2026-10-24 trace: multicast 0..7 previously suppressed unicast
        // SYNs 0..7. TID 1 at 4096 then suppressed TID 0 at 193.
        for seq in 0..8 {
            assert!(peers.accept_data(index, 16, seq, start));
            assert!(peers.accept_data(index, 0, seq, start));
        }
        assert!(peers.accept_data(index, 0, 192, start));
        assert!(peers.accept_data(index, 1, 4096, start));
        assert!(peers.accept_data(index, 0, 193, start));
        assert!(peers.accept_data(index, 16, 8, start));
        assert!(!peers.accept_data(index, 0, 193, start));
        assert!(!peers.accept_data(index, 1, 4096, start));
        assert!(!peers.accept_data(index, 16, 8, start));
        // Advertisements / repeated cache adds cannot clear duplicate history.
        let later = start + PEER_LIFETIME_US;
        assert!(peers.set_cached(&mac, true, later));
        assert!(!peers.accept_data(index, 0, 0, later));
        // A live multicast stream cannot prevent idle unicast from restarting.
        let restart = later + PEER_LIFETIME_US + 1;
        assert!(peers.accept_data(index, 16, 49, restart - 1));
        assert!(peers.accept_data(index, 0, 0, restart));
        assert!(!peers.accept_data(index, 0, 0, restart));
        assert!(peers.accept_data(index, 0, 1, restart));
        assert!(peers.entries[index].cached);
        assert_eq!(peers.find(&mac, restart), index as i32);
        assert!(!peers.accept_data(index, 17, 0, restart));
        assert!(!peers.accept_data(MAX_PEERS, 0, 0, restart));
        assert!(peers.set_cached(&mac, false, restart));
        assert!(!peers.accept_data(index, 0, 2, restart));
        assert!(peers.set_cached(&mac, true, restart));
        assert!(peers.accept_data(index, 0, 0, restart));
    }

    #[test]
    fn data_stream_window_wrap_reorder_and_expiry() {
        let mut window = DataReplayWindow::default();
        assert!(window.accept(65535, 100));
        assert!(window.accept(0, 100));
        assert!(!window.accept(65535, 100));
        assert!(window.accept(65534, 100));
        assert!(!window.accept(65534, 100));
        assert!(window.accept(63, 100));
        assert!(window.accept(1, 100));
        assert!(!window.accept(65535, 100));
        assert!(!window.accept(1, 100 + PEER_LIFETIME_US));
        assert!(window.accept(1, 101 + 2 * PEER_LIFETIME_US));
        // A clock restart also cannot pin a window in the old epoch.
        assert!(window.accept(0, 1));
        assert!(!window.accept(0, 1));
    }

    #[test]
    fn large_upload_rolls_at_4096_without_idle_or_losing_duplicate_history() {
        let mut window = DataReplayWindow::default();
        // A sustained 32 MiB upload crosses this boundary several times.
        for packet in 0..30_000u64 {
            let seq = (packet & 0xfff) as u16;
            assert!(window.accept(seq, 100 + packet * 200), "packet {packet}");
            assert!(!window.accept(seq, 101 + packet * 200), "duplicate {packet}");
        }
        assert_eq!(window.sequence_bits, 12);
        let mut boundary = DataReplayWindow::default();
        assert!(boundary.accept(4094, 1));
        assert!(boundary.accept(0, 2));
        assert!(boundary.accept(4095, 3)); // reordered across rollover
        assert!(!boundary.accept(4095, 4));
        assert!(!boundary.accept(4094, 5));
        assert!(!boundary.accept(4032, 6)); // outside the 64-MSDU window
        // A channel switch can lose the first packets after rollover.
        let mut missed = DataReplayWindow::default();
        assert!(missed.accept(4095, 1));
        assert!(missed.accept(447, 2)); // first retained live rejection
        assert!(!missed.accept(447, 3));
    }

    #[test]
    fn sixteen_bit_streams_and_unrelated_backwards_jumps_stay_supported() {
        let mut window = DataReplayWindow::default();
        for packet in 0..70_000u64 {
            let seq = packet as u16;
            assert!(window.accept(seq, 100 + packet * 10));
            assert!(!window.accept(seq, 101 + packet * 10));
        }
        assert_eq!(window.sequence_bits, 16);
        let mut window = DataReplayWindow::default();
        assert!(window.accept(4000, 1));
        assert!(!window.accept(2000, 2));
        assert!(!window.accept(0, 3));
        assert_eq!(window.sequence_bits, 0);
        assert!(window.accept(4095, 4));
        assert!(window.accept(4096, 5));
        assert!(!window.accept(0, 6));
        assert_eq!(window.sequence_bits, 16);
    }

    #[test]
    fn dense_sequence_fills_only_idle_slots_with_the_data_channel() {
        let mut host = Sequence { count: 16, step: 3, ..Default::default() };
        for (i, c) in [(0, 6), (2, 149), (8, 6), (10, 149)] {
            host.channels[i] = c;
        }
        assert_eq!(data_channel(&host), 149);
        let d = dense_sequence(&host, 149);
        assert_eq!((d.count, d.step), (16, 3));
        for i in 0..16 {
            let want = if i == 0 || i == 8 { 6 } else { 149 };
            assert_eq!(d.channels[i], want, "slot {i}");
        }
        assert!(valid_sequence(&d));
        // Slots beyond count, 2.4 GHz-only and invalid sequences are untouched.
        let mut short = host;
        short.count = 4;
        assert_eq!(&dense_sequence(&short, 149).channels[4..], &host.channels[4..]);
        let mut social = Sequence { count: 16, step: 3, ..Default::default() };
        social.channels[0] = 6;
        assert_eq!(data_channel(&social), 0);
        assert_eq!(dense_sequence(&social, 0), social);
        assert_eq!(dense_sequence(&host, 200), host);
        let bad = Sequence { count: 17, ..host };
        assert_eq!(data_channel(&bad), 0);
        assert_eq!(dense_sequence(&bad, 149), bad);
    }

    fn txs(rate: u16, failed: bool, wcid: u16, pid: u8) -> [u8; 32] {
        let mut data = [0u8; 32];
        let status = 0x8000_8000 | rate as u32 | if failed { 0x10000 } else { 0 };
        data[..4].copy_from_slice(&status.to_le_bytes());
        data[8..12].copy_from_slice(&((wcid as u32) << 16).to_le_bytes());
        data[12..16].copy_from_slice(&((pid as u32) << 24).to_le_bytes());
        data
    }

    fn acks(c: &mut TxStatusCounters, wcid: u16, n: usize) {
        for _ in 0..n {
            let rate = c.rate(wcid);
            assert!(c.observe(&txs(rate, false, wcid, TX_PID_UNICAST)));
        }
    }

    #[test]
    fn unicast_rates_climb_to_ht_and_ignore_isolated_losses() {
        let mut c = TxStatusCounters::default();
        assert_eq!(c.rate(15), 0x4b); // 6 Mbps
        acks(&mut c, 15, 32 * 6);
        assert_eq!(c.rate(15), 0x87); // HT MCS 7
        assert_eq!(c.rate_mbps(0), 65);
        assert_eq!(c.rate(16), 0x4b); // independent peer
        for wcid in [0, 14, 18, u16::MAX] {
            assert_eq!(c.rate(wcid), 0x4b);
        }
        // The 1.0.3 capture: one loss per ~2000 ACKs must not cost a rung.
        for _ in 0..20 {
            c.observe(&txs(0x87, true, 15, TX_PID_UNICAST));
            acks(&mut c, 15, 2000);
            assert_eq!(c.rate(15), 0x87);
        }
        // Two losses within 128 ACKs do.
        c.observe(&txs(0x87, true, 15, TX_PID_UNICAST));
        acks(&mut c, 15, 100);
        c.observe(&txs(0x87, true, 15, TX_PID_UNICAST));
        assert_eq!(c.rate(15), 0x4c); // 54 Mbps
        assert_eq!(c.rates[0].probe_after, 256);
    }

    #[test]
    fn unicast_rate_probe_failures_back_off_exponentially() {
        let mut c = TxStatusCounters { ofdm_only: true, ..Default::default() };
        acks(&mut c, 15, 32 * 4);
        assert_eq!(c.rate(15), 0x48); // 48 Mbps
        acks(&mut c, 15, 32);
        assert_eq!(c.rate(15), 0x4c); // 54: probing
        c.observe(&txs(0x4c, true, 15, TX_PID_UNICAST));
        assert_eq!(c.rate(15), 0x48);
        assert_eq!(c.rates[0].probe_after, 512);
        acks(&mut c, 15, 512 + 32);
        assert_eq!(c.rate(15), 0x4c);
        c.observe(&txs(0x4c, true, 15, TX_PID_UNICAST));
        assert_eq!(c.rates[0].probe_after, 1024);
        // A confirmed probe resets the backoff.
        acks(&mut c, 15, 1024 + 32 + 32);
        assert_eq!(c.rate(15), 0x4c);
        assert_eq!(c.rates[0].backoff, 0);
        // ofdm_only never offers HT.
        acks(&mut c, 15, 4096);
        assert_eq!(c.rate(15), 0x4c);
        assert_eq!(c.rate_mbps(0), 54);
    }

    #[test]
    fn unicast_rate_ignores_stale_and_foreign_reports() {
        let mut c = TxStatusCounters::default();
        acks(&mut c, 15, 64);
        assert_eq!(c.rate(15), 0x49); // 24 Mbps
        let before = c.rates[0];
        // Late completions at older rates, other frame classes and PPDU
        // summaries must not change the 24 Mbps history.
        for _ in 0..64 {
            c.observe(&txs(0x4a, true, 15, TX_PID_UNICAST));
            c.observe(&txs(0x49, true, 15, TX_PID_ACTION));
            c.observe(&txs(0x49, true, 15, TX_PID_MULTICAST));
            let mut ppdu = txs(0x49, true, 15, TX_PID_UNICAST);
            ppdu[2] |= 1; ppdu[3] |= 1; // format 2
            c.observe(&ppdu);
        }
        assert_eq!(c.rates[0].index, before.index);
        assert_eq!(c.rates[0].successes, before.successes);
        assert!(!c.rates[0].failed);
        // Firmware that never reports the selected rung: step down.
        for _ in 0..64 {
            c.observe(&txs(0x4a, false, 15, TX_PID_UNICAST));
        }
        assert_eq!(c.rate(15), 0x4a);
        // NSS/STBC bits above the mode field still match.
        acks(&mut c, 15, 256 + 1);
        assert_eq!(c.rate(15), 0x49); // hold over: probing 24 again
        c.observe(&txs(0x49 | 0x2000, true, 15, TX_PID_UNICAST));
        assert_eq!(c.rate(15), 0x4a);
        let snapshot = c;
        for n in 0..32 {
            assert!(!c.observe(&[0u8; 32][..n]));
        }
        assert!(!c.observe(&txs(0x49, true, 14, TX_PID_UNICAST)));
        assert_eq!(c, snapshot);
    }

    fn addba(state: &State, params: u16, ssc: u16) -> [u8; 33] {
        let mut f = [0u8; 33];
        f[0] = 0xd0;
        f[4..10].copy_from_slice(&state.mac);
        f[10..16].copy_from_slice(&[0x02, 1, 2, 3, 4, 5]);
        f[16..22].copy_from_slice(&BSSID);
        f[24] = BLOCK_ACK_CATEGORY;
        f[25] = ADDBA_REQUEST;
        f[26] = 9;
        f[27..29].copy_from_slice(&params.to_le_bytes());
        f[29..31].copy_from_slice(&5000u16.to_le_bytes());
        f[31..33].copy_from_slice(&ssc.to_le_bytes());
        f
    }

    #[test]
    fn addba_requests_are_accepted_with_a_bounded_window() {
        let mut s = State::default();
        s.mac = [0x02, 9, 9, 9, 9, 9];
        // A-MSDU, immediate, TID 5, 256 MSDUs, SSN 1234.
        let req = addba(&s, 1 | 2 | 5 << 2 | 256 << 6, 1234 << 4);
        assert_eq!(addba_request(&s.mac, &req), Some(AddbaRequest { tid: 5, ssn: 1234, window: 64 }));
        let mut f = [0u8; 40];
        assert_eq!(build_addba_accept(&mut s, &req, 64, &mut f), 33);
        assert_eq!(&f[4..10], &req[10..16]);
        assert_eq!(&f[10..16], &s.mac);
        assert_eq!((f[24], f[25], f[26]), (BLOCK_ACK_CATEGORY, ADDBA_RESPONSE, 9));
        assert_eq!(le16(&f, 27), 0);
        assert_eq!(le16(&f, 29), 1 | 2 | 5 << 2 | 64 << 6);
        assert_eq!(le16(&f, 31), 5000);
        // Smaller requests are honoured; 0 means ours.
        let req = addba(&s, 2 | 32 << 6, 0);
        assert_eq!(addba_request(&s.mac, &req).unwrap().window, 32);
        assert_eq!(addba_request(&s.mac, &addba(&s, 2, 0)).unwrap().window, 64);
        // Delayed BA, foreign destinations and short frames are refused.
        assert_eq!(addba_request(&s.mac, &addba(&s, 0, 0)), None);
        let delayed = addba(&s, 0, 0);
        assert_eq!(build_addba_accept(&mut s, &delayed, 64, &mut f), 0);
        let mut other = addba(&s, 2, 0);
        other[4] ^= 1;
        assert_eq!(addba_request(&s.mac, &other), None);
        for n in 0..33 {
            assert_eq!(addba_request(&s.mac, &req[..n]), None);
            assert_eq!(build_addba_accept(&mut s, &req[..n], 64, &mut f), 0);
        }
        assert_eq!(build_addba_accept(&mut s, &req, 64, &mut f[..32]), 0);
        // Decline keeps the peer's parameters and status 37.
        assert_eq!(build_addba_decline(&mut s, &req, &mut f), 33);
        assert_eq!(le16(&f, 27), STATUS_REQUEST_DECLINED);
        assert_eq!(le16(&f, 29), 2 | 32 << 6);
    }

    #[test]
    fn delba_from_the_originator_names_the_rx_tid() {
        let mut s = State::default();
        s.mac = [0x02, 9, 9, 9, 9, 9];
        let mut f = addba(&s, 0, 0);
        f[25] = DELBA;
        f[26..28].copy_from_slice(&(0x0800u16 | 6 << 12).to_le_bytes());
        assert_eq!(delba_rx_tid(&s.mac, &f[..30]), Some(6));
        f[26..28].copy_from_slice(&(6u16 << 12).to_le_bytes()); // recipient
        assert_eq!(delba_rx_tid(&s.mac, &f[..30]), None);
        assert_eq!(delba_rx_tid(&s.mac, &f[..29]), None);
    }

    fn fixture(name: &str) -> Vec<u8> {
        let path = std::path::Path::new(env!("CARGO_MANIFEST_DIR"))
            .join("../../kext/Wifi/vendor/tests/awdl/fixtures")
            .join(name);
        std::fs::read(path).expect("AWDL fixture")
    }

    /// Offset of the election-v2 TLV (type 24) body in an action frame.
    fn election_body(f: &[u8]) -> usize {
        let mut at_ = 40;
        while at_ + 3 <= f.len() {
            let len = le16(f, at_ + 1) as usize;
            if f[at_] == 24 {
                return at_ + 3;
            }
            at_ += 3 + len;
        }
        panic!("no election TLV");
    }

    #[test]
    fn captured_psf_parses() {
        let f = fixture("iphone-v9-psf.bin");
        let a = parse_action(&f).expect("captured PSF");
        assert!(valid_sequence(&a.sequence));
        assert!(valid_action_envelope(&f));
    }

    #[test]
    fn election_distance_bound() {
        let f = fixture("iphone-election-transition.bin");
        assert!(parse_action(&f).is_some());
        let at_ = election_body(&f);
        for (distance, ok) in [(10u32, true), (11, false), (u32::MAX, false)] {
            let mut g = f.clone();
            g[at_ + 16..at_ + 20].copy_from_slice(&distance.to_le_bytes());
            assert_eq!(parse_action(&g).is_some(), ok, "distance {distance}");
        }
    }

    #[test]
    fn truncations_never_panic_or_parse_partially() {
        for name in ["iphone-v9-psf.bin", "iphone-election-transition.bin"] {
            let f = fixture(name);
            for n in 0..f.len() {
                let _ = parse_action(&f[..n]);
                let _ = valid_action_envelope(&f[..n]);
                let mut s = State::default();
                s.init(&[2, 1, 2, 3, 4, 5], 10_000_000);
                let _ = s.receive_action(&f[..n], 10_000_000);
                let mut t = f[..n].to_vec();
                let _ = refresh_action(&mut s, &mut t, 10_000_000);
            }
        }
    }
}
