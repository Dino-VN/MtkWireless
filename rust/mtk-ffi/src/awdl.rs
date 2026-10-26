//! C ABI of the AWDL core. The C++ side is `kext/Wifi/vendor/include/AWDL`:
//! the original headers, now declaring the same structs and forwarding every
//! function and method here. Layouts are checked by `mtk_awdl_layout`.

use mtk_wlan::awdl::advertisers::{AdvertiserRecord, decode_advertiser_record};
use mtk_wlan::awdl::protocol::{
    self as p, Advertisement, DataPeerEntry, DataPeers, DataReplayWindow, Peer, Reservation, RocGrant, RocResult,
    Sequence, State, TxRate, TxStatusCounters,
};
use mtk_wlan::awdl::timeline::{
    self as t, RadioPlan, RocChannelTiming, RocLeadTiming, Timeline, Window,
};

unsafe fn bytes<'a>(ptr: *const u8, len: usize) -> &'a [u8] {
    if ptr.is_null() || len == 0 {
        &[]
    } else {
        unsafe { core::slice::from_raw_parts(ptr, len) }
    }
}

unsafe fn bytes_mut<'a>(ptr: *mut u8, len: usize) -> &'a mut [u8] {
    if ptr.is_null() || len == 0 {
        &mut []
    } else {
        unsafe { core::slice::from_raw_parts_mut(ptr, len) }
    }
}

unsafe fn mac<'a>(ptr: *const u8) -> Option<&'a [u8; 6]> {
    unsafe { ptr.cast::<[u8; 6]>().as_ref() }
}

macro_rules! export {
    ($(fn $name:ident($($arg:ident: $ty:ty),*) $(-> $ret:ty)? $body:block)*) => {
        $(
            /// # Safety
            /// Pointers are valid for the sizes given (6 bytes for MACs) and
            /// structs come from the matching C++ declarations.
            #[unsafe(no_mangle)]
            #[allow(unused_unsafe)]
            pub unsafe extern "C" fn $name($($arg: $ty),*) $(-> $ret)? { unsafe { $body } }
        )*
    };
}

const ZERO_MAC: [u8; 6] = [0; 6];

export! {
    fn mtk_awdl_valid_presence(presence: u32) -> bool { p::valid_presence(presence) }
    fn mtk_awdl_valid_sequence(s: *const Sequence) -> bool { s.as_ref().is_some_and(p::valid_sequence) }
    fn mtk_awdl_parse_sequence(data: *const u8, n: usize, out: *mut Sequence) -> bool {
        match (p::parse_sequence(bytes(data, n)), out.as_mut()) {
            (Some(s), Some(out)) => { *out = s; true }
            _ => false,
        }
    }
    fn mtk_awdl_valid_action_envelope(f: *const u8, n: usize) -> bool {
        !f.is_null() && p::valid_action_envelope(bytes(f, n))
    }
    fn mtk_awdl_parse_action(f: *const u8, n: usize, out: *mut Advertisement) -> bool {
        match (p::parse_action(bytes(f, n)), out.as_mut()) {
            (Some(a), Some(out)) => { *out = a; true }
            _ => false,
        }
    }
    fn mtk_awdl_peer_channel(peer: *const Peer, now: u64) -> u8 { peer.as_ref().map_or(0, |x| x.channel(now)) }

    fn mtk_awdl_data_peers_reset(d: *mut DataPeers) { if let Some(d) = d.as_mut() { d.reset() } }
    fn mtk_awdl_data_peers_accept(d: *mut DataPeers, index: usize, stream: usize, seq: u16, now: u64) -> bool {
        d.as_mut().is_some_and(|d| d.accept_data(index, stream, seq, now))
    }
    fn mtk_awdl_data_peers_live(e: *const DataPeerEntry, now: u64) -> bool { e.as_ref().is_some_and(|e| DataPeers::live(e, now)) }
    fn mtk_awdl_data_peers_find(d: *const DataPeers, address: *const u8, now: u64) -> i32 {
        match (d.as_ref(), mac(address)) { (Some(d), Some(a)) => d.find(a, now), _ => -1 }
    }
    fn mtk_awdl_data_peers_observe(d: *mut DataPeers, address: *const u8, now: u64) -> i32 {
        match (d.as_mut(), mac(address)) { (Some(d), Some(a)) => d.observe(a, now), _ => -1 }
    }
    fn mtk_awdl_data_peers_can_observe(d: *const DataPeers, address: *const u8, now: u64) -> bool {
        match (d.as_ref(), mac(address)) { (Some(d), Some(a)) => d.can_observe(a, now), _ => false }
    }
    fn mtk_awdl_data_peers_set_cached(d: *mut DataPeers, address: *const u8, value: bool, now: u64) -> bool {
        match (d.as_mut(), mac(address)) { (Some(d), Some(a)) => d.set_cached(a, value, now), _ => !value }
    }
    fn mtk_awdl_data_peers_set_traffic(d: *mut DataPeers, address: *const u8, value: bool, now: u64) -> bool {
        match (d.as_mut(), mac(address)) { (Some(d), Some(a)) => d.set_traffic(a, value, now), _ => !value }
    }

    fn mtk_awdl_state_hold_controls(s: *mut State) { if let Some(s) = s.as_mut() { s.hold_controls() } }
    fn mtk_awdl_state_requested_parent_is(s: *const State, address: *const u8) -> bool {
        match (s.as_ref(), mac(address)) { (Some(s), Some(a)) => s.requested_parent_is(a), _ => false }
    }
    fn mtk_awdl_state_select_native_parent(s: *mut State, address: *const u8, root: *const u8, acquire: bool) {
        if let (Some(s), Some(a)) = (s.as_mut(), mac(address)) { s.select_native_parent(a, mac(root), acquire) }
    }
    fn mtk_awdl_state_clear_native_parent(s: *mut State) { if let Some(s) = s.as_mut() { s.clear_native_parent() } }
    fn mtk_awdl_state_release_controls(s: *mut State, now: u64) -> bool { s.as_mut().is_some_and(|s| s.release_controls(now)) }
    fn mtk_awdl_state_init(s: *mut State, address: *const u8, now: u64) {
        if let Some(s) = s.as_mut() { s.init(mac(address).unwrap_or(&ZERO_MAC), now) }
    }
    fn mtk_awdl_state_counter(s: *const State, now: u64) -> u16 { s.as_ref().map_or(0, |s| s.counter(now)) }
    fn mtk_awdl_state_until_next(s: *const State, now: u64) -> u32 { s.as_ref().map_or(0, |s| s.until_next(now)) }
    fn mtk_awdl_state_channel(s: *const State, now: u64) -> u8 { s.as_ref().map_or(0, |s| s.channel(now)) }
    fn mtk_awdl_state_radio_schedule_ready(s: *const State) -> bool { s.as_ref().is_some_and(|s| s.radio_schedule_ready()) }
    fn mtk_awdl_state_set_sync_role(s: *mut State, role: u32, address: *const u8) -> bool {
        s.as_mut().is_some_and(|s| s.set_sync_role(role, mac(address)))
    }
    fn mtk_awdl_state_set_presence(s: *mut State, value: u8, now: u64) -> bool { s.as_mut().is_some_and(|s| s.set_presence(value, now)) }
    fn mtk_awdl_state_set_sequence(s: *mut State, value: *const Sequence, now: u64) -> bool {
        match (s.as_mut(), value.as_ref()) { (Some(s), Some(v)) => s.set_sequence(v, now), _ => false }
    }
    fn mtk_awdl_state_set_social_channel(s: *mut State, index: u32, channel: u8, now: u64) {
        if let Some(s) = s.as_mut() { s.set_social_channel(index, channel, now) }
    }
    fn mtk_awdl_state_find(s: *const State, address: *const u8) -> i32 {
        match (s.as_ref(), mac(address)) { (Some(s), Some(a)) => s.find(a), _ => -1 }
    }
    fn mtk_awdl_state_elect(s: *mut State, now: u64) { if let Some(s) = s.as_mut() { s.elect(now) } }
    fn mtk_awdl_state_receive_action(s: *mut State, f: *const u8, n: usize, now: u64) -> i32 {
        s.as_mut().map_or(-1, |s| s.receive_action(bytes(f, n), now))
    }
    fn mtk_awdl_accept_data(peer: *mut Peer, seq: u16) -> bool { peer.as_mut().is_some_and(|x| p::accept_data(x, seq)) }

    fn mtk_awdl_build_action(s: *mut State, f: *mut u8, capacity: usize, now: u64) -> usize {
        s.as_mut().map_or(0, |s| p::build_action(s, bytes_mut(f, capacity), now))
    }
    fn mtk_awdl_valid_template_tlvs(data: *const u8, n: usize) -> bool { !data.is_null() && p::valid_template_tlvs(bytes(data, n)) }
    fn mtk_awdl_build_action_with_template(s: *mut State, f: *mut u8, capacity: usize, now: u64, data: *const u8, n: usize) -> usize {
        if data.is_null() { return 0; }
        s.as_mut().map_or(0, |s| p::build_action_with_template(s, bytes_mut(f, capacity), now, bytes(data, n)))
    }
    fn mtk_awdl_block_ack_action(f: *const u8, n: usize, own: *const u8) -> bool {
        !f.is_null() && mac(own).is_some_and(|m| p::block_ack_action(bytes(f, n), m))
    }
    fn mtk_awdl_data_channel(s: *const Sequence) -> u8 {
        s.as_ref().map_or(0, p::data_channel)
    }
    fn mtk_awdl_dense_sequence(s: *const Sequence, channel: u8, out: *mut Sequence) {
        if let (Some(s), Some(out)) = (s.as_ref(), out.as_mut()) { *out = p::dense_sequence(s, channel) }
    }
    fn mtk_awdl_addba_request(own: *const u8, request: *const u8, n: usize, tid: *mut u8, ssn: *mut u16,
                              window: *mut u16) -> bool {
        let Some(own) = mac(own) else { return false };
        if tid.is_null() || ssn.is_null() || window.is_null() {
            return false;
        }
        match p::addba_request(own, bytes(request, n)) {
            Some(r) => { *tid = r.tid; *ssn = r.ssn; *window = r.window; true }
            None => false,
        }
    }
    fn mtk_awdl_delba_rx_tid(own: *const u8, f: *const u8, n: usize, tid: *mut u8) -> bool {
        let Some(own) = mac(own) else { return false };
        if tid.is_null() {
            return false;
        }
        match p::delba_rx_tid(own, bytes(f, n)) {
            Some(t) => { *tid = t; true }
            None => false,
        }
    }
    fn mtk_awdl_build_addba_accept(s: *mut State, request: *const u8, n: usize, window: u16, f: *mut u8,
                                   capacity: usize) -> usize {
        s.as_mut().map_or(0, |s| p::build_addba_accept(s, bytes(request, n), window, bytes_mut(f, capacity)))
    }
    fn mtk_awdl_build_addba_decline(s: *mut State, request: *const u8, n: usize, f: *mut u8, capacity: usize) -> usize {
        s.as_mut().map_or(0, |s| p::build_addba_decline(s, bytes(request, n), bytes_mut(f, capacity)))
    }
    fn mtk_awdl_refresh_action(s: *mut State, f: *mut u8, n: usize, now: u64) -> usize {
        s.as_mut().map_or(0, |s| p::refresh_action(s, bytes_mut(f, n), now))
    }
    fn mtk_awdl_encode_data(out: *mut u8, capacity: usize, eth: *const u8, n: usize, seq: u16) -> usize {
        p::encode_data(bytes_mut(out, capacity), bytes(eth, n), seq)
    }
    fn mtk_awdl_data_header_length(f: *const u8, n: usize) -> usize { p::data_header_length(bytes(f, n)) }
    fn mtk_awdl_relocate_deaggregated_header(f: *mut u8, n: usize) -> bool { p::relocate_deaggregated_header(bytes_mut(f, n)) }
    fn mtk_awdl_data_view(f: *const u8, n: usize, at: usize, out: *mut DataView) -> usize {
        match (p::data_view(bytes(f, n), at), out.as_mut()) {
            (Some((used, v)), Some(out)) => {
                *out = DataView {
                    destination: f.add(v.destination),
                    source: f.add(v.source),
                    payload: f.add(v.payload),
                    length: v.length,
                    sequence: v.sequence,
                };
                used
            }
            _ => 0,
        }
    }
    fn mtk_awdl_valid_data_frame(f: *const u8, n: usize) -> bool { !f.is_null() && p::valid_data_frame(bytes(f, n)) }
    fn mtk_awdl_decode_data_view(out: *mut u8, capacity: usize, view: *const DataView) -> usize {
        let Some(v) = view.as_ref() else { return 0 };
        if out.is_null() || v.payload.is_null() || v.source.is_null() || v.destination.is_null() || !(56..=1516).contains(&v.length) {
            return 0;
        }
        // The view's pointers come from mtk_awdl_data_view on a live frame.
        p::decode_data_view(bytes_mut(out, capacity), bytes(v.destination, 6), bytes(v.source, 6), bytes(v.payload, v.length))
    }
    fn mtk_awdl_decode_data(out: *mut u8, capacity: usize, f: *const u8, n: usize, seq: *mut u16) -> usize {
        let mut s = 0u16;
        let len = p::decode_data(bytes_mut(out, capacity), bytes(f, n), &mut s);
        if len != 0 && let Some(seq) = seq.as_mut() { *seq = s }
        len
    }
    fn mtk_awdl_tx_status_observe(c: *mut TxStatusCounters, data: *const u8, n: usize) -> bool {
        !data.is_null() && c.as_mut().is_some_and(|c| c.observe(bytes(data, n)))
    }
    fn mtk_awdl_tx_rate(c: *const TxStatusCounters, wcid: u16) -> u16 {
        c.as_ref().map_or(p::TX_RATES[0], |c| c.rate(wcid))
    }
    fn mtk_awdl_tx_rate_mbps(c: *const TxStatusCounters, peer: usize) -> u16 {
        c.as_ref().map_or(0, |c| c.rate_mbps(peer))
    }
    fn mtk_awdl_reservation_request(r: *mut Reservation, token: u8, channel: u8, now: u64) {
        if let Some(r) = r.as_mut() { r.request(token, channel, now) }
    }
    fn mtk_awdl_reservation_accept(r: *mut Reservation, token: u8, channel: u8, duration_ms: u32, now: u64) -> RocResult {
        r.as_mut().map_or(RocResult::NotPending, |r| r.accept(token, channel, duration_ms, now))
    }
    fn mtk_awdl_reservation_can_transmit(r: *const Reservation, channel: u8, now: u64) -> bool {
        r.as_ref().is_some_and(|r| r.can_transmit(channel, now))
    }
    fn mtk_awdl_reservation_cancel(r: *mut Reservation) { if let Some(r) = r.as_mut() { r.cancel() } }
    fn mtk_awdl_decode_roc_grant(data: *const u8, n: usize, out: *mut RocGrant) -> RocResult {
        let mut g = RocGrant::default();
        let r = if data.is_null() { RocResult::Malformed } else { p::decode_roc_grant(bytes(data, n), &mut g) };
        if let Some(out) = out.as_mut() { *out = g }
        r
    }
    fn mtk_awdl_receive_roc_grant(r: *mut Reservation, data: *const u8, n: usize, bss: u8, enabled: bool, now: u64, out: *mut RocGrant) -> RocResult {
        let (Some(r), Some(out)) = (r.as_mut(), out.as_mut()) else { return RocResult::Malformed };
        if data.is_null() { *out = RocGrant::default(); return RocResult::Malformed; }
        p::receive_roc_grant(r, bytes(data, n), bss, enabled, now, out)
    }

    fn mtk_awdl_current_window(s: *const State, now: u64, out: *mut Window) -> bool {
        match (s.as_ref().and_then(|s| t::current_window(s, now)), out.as_mut()) {
            (Some(w), Some(out)) => { *out = w; true }
            _ => false,
        }
    }
    fn mtk_awdl_multicast_ready(s: *const State, now: u64) -> bool { s.as_ref().is_some_and(|s| t::multicast_ready(s, now)) }
    fn mtk_awdl_data_queue_lifetime_us(s: *const State, destination: *const u8) -> u32 {
        s.as_ref().map_or(0, |s| t::data_queue_lifetime_us(s, mac(destination).unwrap_or(&ZERO_MAC)))
    }
    fn mtk_awdl_roc_lead_observe(l: *mut RocLeadTiming, response_us: u32, preparation_us: u64) -> u32 {
        l.as_mut().map_or(t::ROC_LEAD_US, |l| l.observe(response_us, preparation_us))
    }
    fn mtk_awdl_roc_channel_lead(c: *const RocChannelTiming, channel: u8, fallback: u32) -> u32 {
        c.as_ref().map_or(fallback, |c| c.lead(channel, fallback))
    }
    fn mtk_awdl_roc_channel_observe(c: *mut RocChannelTiming, channel: u8, response_us: u32, preparation_us: u64) {
        if let Some(c) = c.as_mut() { c.observe(channel, response_us, preparation_us) }
    }
    fn mtk_awdl_plan_radio(s: *const State, now: u64, out: *mut RadioPlan, lead_us: u32, skip_current: bool, timing: *const RocChannelTiming, only_channel: u8, home_channel: u8) -> bool {
        match (s.as_ref().and_then(|s| t::plan_radio(s, now, lead_us, skip_current, timing.as_ref(), only_channel, home_channel)), out.as_mut()) {
            (Some(plan), Some(out)) => { *out = plan; true }
            _ => false,
        }
    }
    fn mtk_awdl_grant_covers_window(w: *const Window, grant: *const Reservation, now: u64) -> bool {
        match (w.as_ref(), grant.as_ref()) { (Some(w), Some(g)) => t::grant_covers_window(w, g, now), _ => false }
    }
    fn mtk_awdl_grant_covers_reserved_window(s: *const State, reserved: *const Window, requested_channel: u8, grant: *const Reservation, now: u64) -> bool {
        match (s.as_ref(), reserved.as_ref(), grant.as_ref()) {
            (Some(s), Some(w), Some(g)) => t::grant_covers_reserved_window(s, w, requested_channel, g, now),
            _ => false,
        }
    }
    fn mtk_awdl_timeline_matches_schedule(tl: *const Timeline, s: *const State) -> bool {
        match (tl.as_ref(), s.as_ref()) { (Some(tl), Some(s)) => tl.matches_schedule(s), _ => false }
    }
    fn mtk_awdl_timeline_begin(tl: *mut Timeline, w: *const Window, grant: *const Reservation, now: u64, parent: *const u8, master: *const u8, native_role: u8, radio_schedule: *const Sequence) -> bool {
        match (tl.as_mut(), w.as_ref(), grant.as_ref(), mac(parent), mac(master)) {
            (Some(tl), Some(w), Some(g), Some(parent), Some(master)) => tl.begin(w, g, now, parent, master, native_role, radio_schedule.as_ref()),
            _ => false,
        }
    }
    fn mtk_awdl_decode_advertiser_record(raw: *const u8, n: usize, out: *mut AdvertiserRecord) -> bool {
        if raw.is_null() { return false; }
        match (decode_advertiser_record(bytes(raw, n)), out.as_mut()) {
            (Some(r), Some(out)) => { *out = r; true }
            _ => false,
        }
    }
}

/// C++ `mtkawdl::DataView`: pointers into the frame.
#[repr(C)]
pub struct DataView {
    pub destination: *const u8,
    pub source: *const u8,
    pub payload: *const u8,
    pub length: usize,
    pub sequence: u16,
}

macro_rules! layout {
    ($($ty:ty { $($field:ident),* })*) => {
        &[$(
            core::mem::size_of::<$ty>(),
            core::mem::align_of::<$ty>(),
            $(core::mem::offset_of!($ty, $field),)*
        )*]
    };
}

/// Sizes, alignments and field offsets, in the order of the table in
/// `AwdlLayout.hpp`; the C++ side compares them at startup and in tests.
static LAYOUT: &[usize] = layout! {
    Sequence { channels, count, step }
    Advertisement { sequence, master, sync, period, counter, down, presence, metric, distance, master_counter, self_counter }
    Peer { mac, advertisement, seen, epoch, base_counter, last_data, replay, used, has_data }
    DataReplayWindow { bitmap, last_rx, sequence, active, sequence_bits }
    DataPeerEntry { replay_state, windows, seen, cached, traffic }
    DataPeers { entries }
    State { mac, master, sync, forced_sync, sequence, own_sequence, peers, epoch, base_counter, data_counter,
        frame_counter, psf_period, metric, master_metric, distance, master_counter, self_counter, synchronized,
        election_epoch, presence, own_presence, sync_role, min_extension, native_election_managed,
        native_parent_selected, native_parent, clock_sample_at, clock_corrections, clock_rejected, clock_hold_until,
        clock_deferred_at, clock_deferred, native_clock_selected, native_timeline_started, controls_held,
        controls_dirty, native_clock_holdover, native_clock_validated, native_clock_acquisition_pending,
        clock_sample_rejected, native_holdover_transitions, native_clock_acquisitions, clock_parent, clock_root,
        acquisition_parent, acquisition_root, acquisition_root_known, controls_deferred, pending_controls }
    TxRate { index, successes, failed, probing, backoff, stale, probe_after }
    TxStatusCounters { data_acked, data_failed, multicast_completed, multicast_failed, action_completed,
        action_failed, last_status, last_sequence, last_wcid, last_pid, rates, ofdm_only }
    Reservation { deadline, grant_observed_at, requested_at, token, channel, pending, granted, response_delay_us }
    RocGrant { bss, token, status, channel, request_type, duration_ms }
    DataView { destination, source, payload, length, sequence }
    Window { start, end, counter, channel, presence }
    RocLeadTiming { samples, lead_us, count, next, updates, decreases }
    RocChannelTiming { entries, next, observations }
    RadioPlan { window, request_at, duration_ms }
    Timeline { active, open, emitted, radio_end, role, token, grant_observed_at, last_start, last_window_start,
        starts, ends, late, short_grants, last_counter, sync, root, last_sync, last_root, last_presence, schedule,
        schedule_recorded }
    AdvertiserRecord { aw_counter, down_counter, sync_contention, af_period, raw8, raw10, aw_period, rssi,
        reserved15, presence, reserved17, raw18, reserved19, fw_af_rx_time, peer_self_metric, root_metric,
        root_master, immediate_peer }
};

/// Entry `index` of the layout table, or `usize::MAX` past its end.
#[unsafe(no_mangle)]
pub extern "C" fn mtk_awdl_layout(index: usize) -> usize {
    LAYOUT.get(index).copied().unwrap_or(usize::MAX)
}

#[unsafe(no_mangle)]
pub extern "C" fn mtk_awdl_layout_len() -> usize {
    LAYOUT.len()
}
