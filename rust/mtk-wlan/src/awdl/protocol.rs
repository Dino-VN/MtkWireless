//! Wire protocol and scheduling primitives (`AwdlProtocol.hpp`).
//!
//! Integer behavior follows the C++ exactly, including 16-bit counter
//! wrap-around and the signed reinterpretations; those are spelled out with
//! `wrapping_*` and `as` casts.

use super::bytes::{at, copy, eq, fill, le16, le32, mac, put16, put32, set};

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
}

impl DataReplayWindow {
    pub fn accept(&mut self, seq: u16, now: u64) -> bool {
        if !self.active || now < self.last_rx || now - self.last_rx > PEER_LIFETIME_US {
            self.active = true;
            self.sequence = seq;
            self.bitmap = 1;
            self.last_rx = now;
            return true;
        }
        self.last_rx = now;
        let delta = seq.wrapping_sub(self.sequence) as i16;
        if delta > 0 {
            self.bitmap = if delta >= 64 { 1 } else { (self.bitmap << delta) | 1 };
            self.sequence = seq;
            return true;
        }
        let back = self.sequence.wrapping_sub(seq) as u32;
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

/// Declines an ADDBA request (no firmware BA session for AWDL peers).
pub fn build_addba_decline(s: &mut State, request: &[u8], f: &mut [u8]) -> usize {
    const SIZE: usize = 24 + 9;
    if f.len() < SIZE
        || !block_ack_action(request, &s.mac)
        || request[25] != ADDBA_REQUEST
        || request.len() < SIZE
    {
        return 0;
    }
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
    put16(f, 27, STATUS_REQUEST_DECLINED);
    f[29..31].copy_from_slice(&request[27..29]);
    f[31..33].copy_from_slice(&request[29..31]);
    SIZE
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
    } else if at_ != hdr {
        return None;
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
}

impl TxStatusCounters {
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
