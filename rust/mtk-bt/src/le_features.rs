//! Compatibility completion for bluetoothd's BCM LE feature-mask command.
//!
//! At startup bluetoothd sends `_BCM_WriteLELocalSupportedFeatures`: vendor
//! opcode 0xFD3C with an eight-byte mask. MediaTek answers Command Status
//! 0x01 (Unknown HCI Command) and the BLE advertiser never becomes ready
//! ("Stack is not ready"). For that exact command on a MediaTek controller
//! loaded by this kext, the rejection is rewritten in place into a successful
//! Command Complete with the same command credits:
//!
//! ```text
//! Command Status:   0F 04 01 NN 3C FD
//! Command Complete: 0E 04 NN 3C FD 00
//! ```
//!
//! The command still reaches the controller; no transfer is added. See
//! docs/BLE-ADVERTISING.md. Callers serialize access to [`LeFeatures`].

/// The startup command as captured. Opcode alone is not enough: other vendors
/// use 0xFD3C for different commands.
pub const FEATURE_COMMAND: [u8; 11] = [
    0x3C, 0xFD, 0x08, 0xFD, 0xFD, 0x01, 0xFF, 0x09, 0x00, 0x00, 0x00,
];
const HCI_RESET: [u8; 3] = [0x03, 0x0C, 0x00];

pub fn is_feature_command(command: &[u8]) -> bool {
    command == FEATURE_COMMAND
}

/// EP0 requests that may carry the feature command or an HCI Reset: class
/// OUT to the device or interface, all fields zero, 11 or 3 bytes.
pub fn is_candidate_request(
    bm_request_type: u8,
    b_request: u8,
    w_value: u16,
    w_index: u16,
    w_length: u16,
) -> bool {
    bm_request_type & 0xE0 == 0x20
        && bm_request_type & 0x1F <= 1
        && b_request == 0
        && w_value == 0
        && w_index == 0
        && (w_length as usize == FEATURE_COMMAND.len() || w_length as usize == HCI_RESET.len())
}

#[derive(Debug, Clone, Copy, PartialEq, Eq)]
pub enum Reply {
    /// No answer to a pending command in these events.
    None,
    /// The answer was found and is left alone (success or another error).
    Unchanged,
    /// The six bytes at `offset` were rewritten.
    Rewritten,
}

#[derive(Debug, Clone, Copy, PartialEq, Eq)]
pub struct Received {
    pub reply: Reply,
    pub offset: usize,
    /// The original six bytes at `offset`, to restore when writing the
    /// rewritten event back to the transfer fails.
    pub original: [u8; 6],
}

impl Received {
    const NONE: Self = Self {
        reply: Reply::None,
        offset: 0,
        original: [0; 6],
    };
}

/// Per-controller state. Generation tokens keep a late failed USB completion
/// from cancelling a newer command.
#[derive(Debug, Default, Clone, Copy, PartialEq, Eq)]
pub struct Pending {
    pub generation: u64,
    pub active: bool,
}

impl Pending {
    /// Starts tracking a command. Returns 0 if one is already pending (an
    /// overlapping command is not acknowledged twice).
    pub fn arm(&mut self) -> u64 {
        if self.active {
            return 0;
        }
        self.generation = self.generation.wrapping_add(1).max(1);
        self.active = true;
        self.generation
    }

    pub fn cancel(&mut self, token: u64) {
        if token == self.generation {
            self.active = false;
        }
    }

    pub fn reset(&mut self) {
        self.generation = self.generation.wrapping_add(1);
        self.active = false;
    }

    /// Looks for the answer to the pending command in a USB transfer of HCI
    /// events (no H4 prefix) and rewrites a rejection in place.
    pub fn receive(&mut self, events: &mut [u8]) -> Received {
        if !self.active {
            return Received::NONE;
        }
        // Validate complete framing first; never search for opcode bytes
        // inside payloads.
        let mut response = None;
        let mut off = 0;
        while off < events.len() {
            let rest = &events[off..];
            let Some(&len) = rest.get(1) else {
                return Received::NONE;
            };
            let size = len as usize + 2;
            if size > rest.len() {
                return Received::NONE;
            }
            let ev = &rest[..size];
            let status = ev.len() >= 6 && ev[..2] == [0x0F, 4] && ev[4..6] == [0x3C, 0xFD];
            let complete = ev.len() >= 5 && ev[0] == 0x0E && ev[1] >= 4 && ev[3..5] == [0x3C, 0xFD];
            if response.is_none() && (status || complete) {
                response = Some((off, status && ev[2] == 0x01));
            }
            off += size;
        }
        let Some((offset, unknown)) = response else {
            return Received::NONE;
        };
        self.active = false;
        let event = &mut events[offset..offset + 6];
        let mut original = [0; 6];
        original.copy_from_slice(event);
        if !unknown {
            return Received {
                reply: Reply::Unchanged,
                offset,
                original,
            };
        }
        let credits = event[3];
        event.copy_from_slice(&[0x0E, 4, credits, 0x3C, 0xFD, 0x00]);
        Received {
            reply: Reply::Rewritten,
            offset,
            original,
        }
    }
}

const MAX_CONTROLLERS: usize = 8;

#[derive(Debug, Default, Clone, Copy)]
struct Controller {
    /// IORegistry entry id of the USB device; 0 = free slot.
    registry_id: u64,
    /// Address of its HCI event (interrupt IN) pipe; a lookup key only.
    event_pipe: usize,
    pending: Pending,
}

/// All MediaTek controllers seen by the routes. Registry ids distinguish
/// replugged devices.
#[derive(Debug, Default)]
pub struct LeFeatures {
    controllers: [Controller; MAX_CONTROLLERS],
    generation: u64,
}

impl LeFeatures {
    pub const fn new() -> Self {
        Self {
            controllers: [Controller {
                registry_id: 0,
                event_pipe: 0,
                pending: Pending {
                    generation: 0,
                    active: false,
                },
            }; MAX_CONTROLLERS],
            generation: 0,
        }
    }

    fn find(&mut self, id: u64) -> Option<&mut Controller> {
        if id == 0 {
            return None;
        }
        self.controllers.iter_mut().find(|c| c.registry_id == id)
    }

    /// The controller's slot, created if needed: a free slot first, else the
    /// first one without a pending command.
    fn find_or_create(&mut self, id: u64) -> Option<&mut Controller> {
        if id == 0 {
            return None;
        }
        let slot = self
            .controllers
            .iter()
            .position(|c| c.registry_id == id)
            .or_else(|| self.controllers.iter().position(|c| c.registry_id == 0))
            .or_else(|| self.controllers.iter().position(|c| !c.pending.active))?;
        let c = &mut self.controllers[slot];
        if c.registry_id != id {
            *c = Controller {
                registry_id: id,
                ..Controller::default()
            };
        }
        Some(c)
    }

    /// EP0 command sent by bluetoothd to MediaTek device `id` (the caller has
    /// checked [`is_candidate_request`]). Returns a token for a tracked
    /// feature command, 0 otherwise. HCI Reset clears pending state.
    pub fn command(&mut self, id: u64, command: &[u8]) -> u64 {
        let reset = command == HCI_RESET;
        if !reset && !is_feature_command(command) {
            return 0;
        }
        // Tokens are unique across controllers.
        let token = self.generation.wrapping_add(1).max(1);
        let Some(c) = self.find_or_create(id) else {
            return 0;
        };
        if reset {
            c.pending.reset();
            return 0;
        }
        if c.pending.arm() == 0 {
            return 0;
        }
        c.pending.generation = token;
        self.generation = token;
        token
    }

    /// The EP0 submission or completion carrying `token` failed.
    pub fn cancel(&mut self, id: u64, token: u64) {
        if let Some(c) = self.find(id) {
            c.pending.cancel(token);
        }
    }

    /// Device whose event pipe is `pipe`, or 0.
    pub fn controller_for_pipe(&self, pipe: usize) -> u64 {
        if pipe == 0 {
            return 0;
        }
        self.controllers
            .iter()
            .find(|c| c.event_pipe == pipe)
            .map_or(0, |c| c.registry_id)
    }

    /// An HCI event pipe was handed out for device `id`. Any other mapping of
    /// the same pipe address is stale (the object was freed and reused).
    pub fn bind_event_pipe(&mut self, pipe: usize, id: u64, is_mediatek: bool) {
        for c in &mut self.controllers {
            if c.event_pipe == pipe && (!is_mediatek || c.registry_id != id) {
                c.event_pipe = 0;
                c.pending.reset();
            }
        }
        if is_mediatek && let Some(c) = self.find_or_create(id) {
            c.event_pipe = pipe;
        }
    }

    /// Events received on device `id`'s event pipe.
    pub fn receive(&mut self, id: u64, events: &mut [u8]) -> Received {
        match self.find(id) {
            Some(c) => c.pending.receive(events),
            None => Received::NONE,
        }
    }
}

#[cfg(test)]
mod tests {
    use super::*;
    use std::vec::Vec;

    fn status(error: u8, credits: u8) -> Vec<u8> {
        vec![0x0F, 4, error, credits, 0x3C, 0xFD]
    }

    fn armed() -> Pending {
        let mut p = Pending::default();
        assert_ne!(p.arm(), 0);
        p
    }

    #[test]
    fn exact_command_only() {
        assert!(is_feature_command(&FEATURE_COMMAND));
        for n in 0..FEATURE_COMMAND.len() {
            assert!(!is_feature_command(&FEATURE_COMMAND[..n]));
        }
        for pos in 0..FEATURE_COMMAND.len() {
            let mut c = FEATURE_COMMAND;
            c[pos] ^= 1;
            assert!(!is_feature_command(&c));
        }
    }

    #[test]
    fn every_status_and_credit() {
        for error in 0..=255u8 {
            let mut p = armed();
            let mut ev = status(error, 1);
            let before = ev.clone();
            let r = p.receive(&mut ev);
            assert!(!p.active);
            if error == 1 {
                assert_eq!(r.reply, Reply::Rewritten);
                assert_eq!(ev, [0x0E, 4, 1, 0x3C, 0xFD, 0]);
                assert_eq!(r.original.as_slice(), before.as_slice());
            } else {
                assert_eq!(r.reply, Reply::Unchanged);
                assert_eq!(ev, before);
            }
        }
        for credits in 0..=255u8 {
            let mut p = armed();
            let mut ev = status(1, credits);
            assert_eq!(p.receive(&mut ev).reply, Reply::Rewritten);
            assert_eq!(ev[2], credits);
            let rewritten = ev.clone();
            assert_eq!(p.receive(&mut ev).reply, Reply::None);
            assert_eq!(ev, rewritten);
        }
    }

    #[test]
    fn truncated_events_are_ignored() {
        for len in 0..6 {
            let mut p = armed();
            let mut ev = status(1, 1);
            assert_eq!(p.receive(&mut ev[..len]).reply, Reply::None);
            assert!(p.active);
            assert_eq!(ev, status(1, 1));
        }
        let mut p = armed();
        let mut ev = vec![0x0F, 4, 1, 1, 0x3C, 0xFD, 0xFF, 2, 0xAA];
        let before = ev.clone();
        assert_eq!(p.receive(&mut ev).reply, Reply::None);
        assert!(p.active);
        assert_eq!(ev, before);
    }

    #[test]
    fn tokens_reset_and_overlap() {
        let mut p = Pending::default();
        let old = p.arm();
        assert_eq!(
            p.arm(),
            0,
            "overlapping commands are not acknowledged twice"
        );
        p.reset();
        let new = p.arm();
        assert_ne!(new, old);
        p.cancel(old);
        assert!(p.active);
        p.cancel(new);
        assert_eq!(p.receive(&mut status(1, 1)).reply, Reply::None);
    }

    #[test]
    fn unrelated_and_complete_events() {
        let mut p = Pending::default();
        for mut unrelated in [
            vec![0x3E, 6, 0x0F, 4, 1, 1, 0x3C, 0xFD],
            vec![0x0F, 4, 1, 1, 0x3D, 0xFD],
            vec![0x04, 0x0F, 4, 1, 1, 0x3C, 0xFD],
        ] {
            p.arm();
            let before = unrelated.clone();
            assert_eq!(p.receive(&mut unrelated).reply, Reply::None);
            assert!(p.active);
            assert_eq!(unrelated, before);
            p.reset();
        }
        for error in [0, 1, 12] {
            p.arm();
            let mut complete = vec![0x0E, 4, 1, 0x3C, 0xFD, error];
            let before = complete.clone();
            assert_eq!(p.receive(&mut complete).reply, Reply::Unchanged);
            assert!(!p.active);
            assert_eq!(complete, before);
        }
    }

    #[test]
    fn coalesced_events() {
        let mut p = armed();
        let mut ev = vec![0xFF, 1, 0xAA, 0x0F, 4, 1, 7, 0x3C, 0xFD, 0xFF, 0];
        let r = p.receive(&mut ev);
        assert_eq!((r.reply, r.offset), (Reply::Rewritten, 3));
        assert_eq!(ev, [0xFF, 1, 0xAA, 0x0E, 4, 7, 0x3C, 0xFD, 0, 0xFF, 0]);
    }

    #[test]
    fn arbitrary_short_framing() {
        for kind in 0..=255u8 {
            for payload in 0..=255u8 {
                let mut p = armed();
                let mut ev = [kind, payload, 1, 1, 0x3C, 0xFD];
                p.receive(&mut ev);
            }
        }
    }

    #[test]
    fn controllers_are_independent() {
        let mut le = LeFeatures::new();
        let token = le.command(10, &FEATURE_COMMAND);
        assert_ne!(token, 0);
        assert_eq!(le.receive(11, &mut status(1, 1)).reply, Reply::None);
        le.bind_event_pipe(0x1000, 10, true);
        assert_eq!(le.controller_for_pipe(0x1000), 10);
        assert_eq!(le.receive(10, &mut status(1, 1)).reply, Reply::Rewritten);
    }

    #[test]
    fn stale_cancel_and_reset() {
        let mut le = LeFeatures::new();
        let first = le.command(10, &FEATURE_COMMAND);
        assert_eq!(le.command(10, &FEATURE_COMMAND), 0, "already pending");
        assert_eq!(le.command(10, &HCI_RESET), 0);
        let second = le.command(10, &FEATURE_COMMAND);
        assert_ne!(second, first);
        le.cancel(10, first);
        assert_eq!(le.receive(10, &mut status(1, 1)).reply, Reply::Rewritten);
        assert_eq!(le.command(10, &[0x3C, 0xFD, 0x08]), 0);
        assert_eq!(le.command(0, &FEATURE_COMMAND), 0);
    }

    #[test]
    fn pipe_rebinding() {
        let mut le = LeFeatures::new();
        le.bind_event_pipe(0x1000, 10, true);
        le.command(10, &FEATURE_COMMAND);
        // Same address handed to a non-MediaTek device: mapping and state cleared.
        le.bind_event_pipe(0x1000, 20, false);
        assert_eq!(le.controller_for_pipe(0x1000), 0);
        assert_eq!(le.receive(10, &mut status(1, 1)).reply, Reply::None);
    }

    #[test]
    fn slot_reuse_prefers_free_slots() {
        let mut le = LeFeatures::new();
        for id in 1..=8 {
            le.bind_event_pipe(id as usize * 0x100, id, true);
        }
        le.command(1, &FEATURE_COMMAND);
        // Table full: device 9 takes the first slot without a pending command.
        le.bind_event_pipe(0x900, 9, true);
        assert_eq!(le.controller_for_pipe(0x100), 1, "pending controller kept");
        assert_eq!(le.controller_for_pipe(0x200), 0);
        assert_eq!(le.controller_for_pipe(0x900), 9);
    }

    #[test]
    fn candidate_requests() {
        assert!(is_candidate_request(0x20, 0, 0, 0, 11));
        assert!(is_candidate_request(0x21, 0, 0, 0, 3));
        assert!(!is_candidate_request(0xA0, 0, 0, 0, 11), "IN");
        assert!(!is_candidate_request(0x40, 0, 0, 0, 11), "vendor");
        assert!(!is_candidate_request(0x22, 0, 0, 0, 11), "endpoint");
        assert!(!is_candidate_request(0x20, 0xE0, 0, 0, 11));
        assert!(!is_candidate_request(0x20, 0, 0, 0, 9));
    }

    /// Replays a PacketLogger capture: set MTK_BT_LE_CAPTURE to a .pklg
    /// recorded during bluetoothd startup (captures stay out of the repo,
    /// they contain addresses and keys).
    #[test]
    fn capture_replay() {
        let Ok(path) = std::env::var("MTK_BT_LE_CAPTURE") else {
            return;
        };
        let data = std::fs::read(&path).expect("read capture");
        let mut p = Pending::default();
        let (mut commands, mut rewritten) = (0, 0);
        let mut off = 0;
        while off < data.len() {
            let len = u32::from_be_bytes(data[off..off + 4].try_into().unwrap()) as usize;
            assert!((9..=1 << 20).contains(&len));
            let mut record = data[off + 4..off + 4 + len].to_vec();
            off += 4 + len;
            let (kind, body) = (record[8], &mut record[9..]);
            if kind == 0 && is_feature_command(body) {
                assert_ne!(p.arm(), 0);
                commands += 1;
            } else if kind == 1 && p.receive(body).reply == Reply::Rewritten {
                rewritten += 1;
            }
        }
        assert_eq!((commands, rewritten, p.active), (1, 1, false));
    }
}
