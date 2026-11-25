"""Analyze host-side AirDrop traffic from classic Ethernet pcap files."""
from array import array
from collections import Counter, defaultdict
import csv
import heapq
import ipaddress
import json
import math
import socket
import struct
from typing import NamedTuple

NS = 1_000_000_000
MIB = 1024 * 1024
MAGIC = {b'\xd4\xc3\xb2\xa1': ('<', 1000), b'\xa1\xb2\xc3\xd4': ('>', 1000),
         b'\x4d\x3c\xb2\xa1': ('<', 1), b'\xa1\xb2\x3c\x4d': ('>', 1)}
DISCOVERY_PORTS = {5353, 5355, 1900}
PACKET_FIELDS = ('interface', 'capture_index', 'wall_ns', 'elapsed_ms', 'selected',
                 'direction', 'transport', 'local_ip', 'local_port', 'peer_ip', 'peer_port',
                 'frame_bytes', 'captured_bytes', 'ip_bytes', 'transport_payload_bytes',
                 'flow_direction_gap_ms', 'tx_payload_gap_ms', 'action_phase', 'tcp_flags',
                 'tcp_seq', 'tcp_ack')
TIMELINE_FIELDS = ('start_ms', 'seconds', 'tx_packets', 'rx_packets', 'tx_payload_bytes',
                   'rx_payload_bytes', 'tx_payload_mib_s', 'rx_payload_mib_s')


class CaptureError(ValueError):
    pass


class Packet(NamedTuple):
    src: bytes
    dst: bytes
    sport: int
    dport: int
    proto: str
    ip_bytes: int
    payload: int
    flags: int
    seq: int
    ack: int


def pcap_records(path, follow=None):
    with path.open('rb') as stream:
        magic = MAGIC.get(read_record_bytes(stream, 4, follow))
        header = read_record_bytes(stream, 20, follow)
        if not magic or len(header) != 20:
            raise CaptureError('missing or unsupported pcap header')
        order, scale = magic
        major, minor, _, _, snaplen, linktype = struct.unpack(order + 'HHIIII', header)
        if (major, minor) != (2, 4) or linktype & 0xffff != 1:
            raise CaptureError(f'unsupported pcap version or linktype ({major}.{minor}, {linktype})')
        index = 0
        while True:
            record = read_record_bytes(stream, 16, follow)
            if not record:
                break
            if len(record) != 16:
                raise CaptureError('partial pcap record header')
            sec, fraction, saved, frame_len = struct.unpack(order + 'IIII', record)
            if saved > min(snaplen, frame_len, 2**20) or fraction * scale >= NS:
                raise CaptureError('invalid pcap record size or timestamp')
            frame = read_record_bytes(stream, saved, follow)
            if len(frame) != saved:
                raise CaptureError('partial pcap frame')
            index += 1
            yield index, sec * NS + fraction * scale, frame_len, frame


def read_record_bytes(stream, count, follow):
    data = stream.read(count)
    while len(data) < count and follow is not None and not follow.wait(.05):
        data += stream.read(count - len(data))
    return data


def decode_frame(frame):
    if len(frame) < 14:
        return None
    ether_type = int.from_bytes(frame[12:14], 'big')
    offset = 14
    while ether_type in (0x8100, 0x88a8, 0x9100):
        if len(frame) < offset + 4:
            return None
        ether_type = int.from_bytes(frame[offset + 2:offset + 4], 'big')
        offset += 4
    if ether_type == 0x0800 and len(frame) >= offset + 20:
        if frame[offset] >> 4 != 4 or int.from_bytes(frame[offset + 6:offset + 8], 'big') & 0x3fff:
            return None
        header = (frame[offset] & 0xf) * 4
        size = int.from_bytes(frame[offset + 2:offset + 4], 'big')
        proto = frame[offset + 9]
        src, dst = frame[offset + 12:offset + 16], frame[offset + 16:offset + 20]
        if header < 20:
            return None
    elif ether_type == 0x86dd and len(frame) >= offset + 40:
        if frame[offset] >> 4 != 6:
            return None
        header = 40
        size = 40 + int.from_bytes(frame[offset + 4:offset + 6], 'big')
        proto = frame[offset + 6]
        src, dst = frame[offset + 8:offset + 24], frame[offset + 24:offset + 40]
        while proto in (0, 43, 44, 51, 60):
            pos = offset + header
            if pos + 2 > min(len(frame), offset + size):
                return None
            if proto == 44:
                # An atomic fragment has neither offset nor More Fragments.
                if pos + 8 > min(len(frame), offset + size) or frame[pos + 2:pos + 4] != b'\0\0':
                    return None
                extension = 8
            elif proto == 51:
                extension = (frame[pos + 1] + 2) * 4
            else:
                extension = (frame[pos + 1] + 1) * 8
            proto = frame[pos]
            header += extension
    else:
        return None

    start = offset + header
    available = size - header
    if proto == 17 and available >= 8 and len(frame) >= start + 8:
        sport, dport, length = struct.unpack_from('!HHH', frame, start)
        if 8 <= length <= available:
            return Packet(src, dst, sport, dport, 'UDP', size, length - 8, 0, 0, 0)
    elif proto == 6 and available >= 20 and len(frame) >= start + 20:
        sport, dport, seq, ack, data_flags = struct.unpack_from('!HHIIH', frame, start)
        length = (data_flags >> 12) * 4
        if 20 <= length <= available:
            return Packet(src, dst, sport, dport, 'TCP', size, available - length,
                          data_flags & 0x1ff, seq, ack)
    return None


def packed_addresses(values):
    return {ipaddress.ip_address(value.split('%')[0]).packed for value in values}


def capture_packets(directory, config, quality=None, follow=None):
    for interface, capture in config['captures'].items():
        local_ips = packed_addresses(capture['addresses'])
        macs = {bytes.fromhex(value.replace(':', '')) for value in capture['macs']}
        issues = Counter()
        previous = None
        try:
            for index, stamp, length, frame in pcap_records(directory / capture['pcap'], follow):
                if follow is not None and follow.is_set():
                    break
                issues['records'] += 1
                issues['header_only_records'] += int(len(frame) < length)
                issues['timestamp_regressions'] += int(previous is not None and stamp < previous)
                previous = stamp
                if stamp < config['start_ns'] or (follow is None and stamp > config['end_ns']):
                    issues['outside_window'] += 1
                    continue
                packet = decode_frame(frame)
                if packet is None:
                    issues['other_or_undecoded'] += 1
                    continue
                if (packet.sport in DISCOVERY_PORTS or packet.dport in DISCOVERY_PORTS or
                        frame[0] & 1 or packet.src == packet.dst):
                    issues['discovery_or_group'] += 1
                    continue
                src_local = packet.src in local_ips or frame[6:12] in macs
                dst_local = packet.dst in local_ips or frame[:6] in macs
                if src_local == dst_local:
                    issues['unknown_direction'] += 1
                    continue
                if src_local:
                    direction, local, lp, peer, pp = 'tx', packet.src, packet.sport, packet.dst, packet.dport
                else:
                    direction, local, lp, peer, pp = 'rx', packet.dst, packet.dport, packet.src, packet.sport
                issues['usable_packets'] += 1
                yield (interface, packet.proto, local, lp, peer, pp), index, stamp, length, len(frame), direction, packet
        except (CaptureError, OSError) as error:
            if quality is not None:
                quality.setdefault(interface, {})['error'] = str(error)
        if quality is not None:
            quality.setdefault(interface, {}).update(issues)


def flow_details(key):
    interface, transport, local, lp, peer, pp = key
    return dict(interface=interface, transport=transport, local_ip=socket.inet_ntop(
        socket.AF_INET if len(local) == 4 else socket.AF_INET6, local), local_port=lp,
        peer_ip=str(ipaddress.ip_address(peer)), peer_port=pp)


def distribution(values, divisor=1):
    if not values:
        return None
    ordered = sorted(values)
    count = len(ordered)
    return dict(count=count, min=ordered[0] / divisor, mean=sum(ordered) / count / divisor,
                p50=ordered[math.ceil(count * .50) - 1] / divisor,
                p95=ordered[math.ceil(count * .95) - 1] / divisor,
                p99=ordered[math.ceil(count * .99) - 1] / divisor, max=ordered[-1] / divisor)


def choose_flow(flows, file_bytes, outcome, manual):
    if manual:
        if manual not in range(1, len(flows) + 1):
            raise ValueError(f'flow {manual} is outside 1–{len(flows)}')
        return flows[manual - 1], 'manual'
    if not flows:
        return None, 'no_local_unicast_tcp_udp'
    dominant = flows[0]
    tx = dominant[1]['tx_payload_bytes']
    # Size is a plausibility check, not a proof of recipient identity.
    floor = min(8192, file_bytes)
    if tx < (max(floor, file_bytes // 2) if outcome == 'shared' else floor):
        return None, 'insufficient_bulk_traffic'
    if len(flows) > 1 and tx < 4 * flows[1][1]['tx_payload_bytes']:
        return None, 'multiple_bulk_candidates'
    return dominant, 'dominant_transmit_candidate'


def save_analysis(directory, config, analysis_dir, manual=None):
    quality = {}
    flows = defaultdict(Counter)
    for key, _, stamp, _, _, direction, packet in capture_packets(directory, config, quality):
        flow = flows[key]
        flow[direction + '_packets'] += 1
        flow[direction + '_payload_bytes'] += packet.payload
        if 'first_ns' not in flow:
            flow['first_ns'] = stamp
        flow['last_ns'] = stamp
    ordered = sorted(flows.items(), key=lambda item: (-item[1]['tx_payload_bytes'], item[0]))
    chosen, attribution = choose_flow(ordered, config['file_bytes'], config['status'], manual)
    selected_key = chosen[0] if chosen else None
    order_ids = {key: index for index, (key, _) in enumerate(ordered, 1)}
    origin = config['start_ns']
    previous = {}
    selected_gaps = {name: array('q') for name in ('tx', 'rx', 'tx_payload')}
    biggest = []
    bins = defaultdict(Counter)
    first_tx = last_tx = None
    selected_ns = config.get('recipient_selected_ns')
    first_packet_after_select = first_tx_after_select = None
    last_before_action = first_after_action = None
    action = config.get('action') or {}
    action_start, action_end = action.get('start_ns'), action.get('end_ns')
    action_payload_packets = Counter()
    syn = None
    syn_ambiguous = False
    syn_rtt_ns = None
    flow_addresses = {key: flow_details(key) for key in flows}
    with (analysis_dir / 'packets.csv').open('w', newline='') as stream:
        writer = csv.DictWriter(stream, ('flow', *PACKET_FIELDS))
        writer.writeheader()
        for key, index, stamp, wire, saved, direction, packet in capture_packets(directory, config):
            selected = key == selected_key
            channel = (key, direction)
            before = previous.get(channel)
            gap = stamp - before if before is not None else None
            previous[channel] = stamp
            tx_gap = None
            action_phase = ''
            if action_start is not None:
                action_phase = ('before' if stamp < action_start else
                                'during' if action_end is None or stamp < action_end else 'after')
            if selected:
                if selected_ns is not None and stamp >= selected_ns:
                    if first_packet_after_select is None:
                        first_packet_after_select = stamp
                    if direction == 'tx' and packet.payload and first_tx_after_select is None:
                        first_tx_after_select = stamp
                if gap is not None and gap >= 0:
                    selected_gaps[direction].append(gap)
                bucket = bins[(stamp - origin) // NS]
                bucket[direction + '_packets'] += 1
                bucket[direction + '_payload_bytes'] += packet.payload
                if direction == 'tx' and packet.payload:
                    if action_phase:
                        action_payload_packets[action_phase] += 1
                        if stamp < action_start:
                            last_before_action = stamp
                        elif first_after_action is None:
                            first_after_action = stamp
                    if last_tx is None:
                        first_tx = stamp
                    else:
                        tx_gap = stamp - last_tx
                        if tx_gap >= 0:
                            selected_gaps['tx_payload'].append(tx_gap)
                            heapq.heappush(biggest, (tx_gap, last_tx, stamp, index))
                            if len(biggest) > 10:
                                heapq.heappop(biggest)
                    last_tx = stamp
                if packet.proto == 'TCP' and packet.flags & 0x02:
                    if direction == 'tx' and not packet.flags & 0x10:
                        if syn is None:
                            syn = (packet.seq, stamp)
                        else:
                            syn_ambiguous = True
                    if (direction == 'rx' and packet.flags & 0x10 and syn is not None and
                            packet.ack == (syn[0] + 1) & 0xffffffff and
                            stamp >= syn[1] and syn_rtt_ns is None and not syn_ambiguous):
                        syn_rtt_ns = stamp - syn[1]
            writer.writerow(dict(flow_addresses[key], flow=order_ids[key], capture_index=index,
                                 wall_ns=stamp, elapsed_ms=(stamp - origin) / 1e6,
                                 selected=int(selected), direction=direction, frame_bytes=wire,
                                 captured_bytes=saved, ip_bytes=packet.ip_bytes,
                                 transport_payload_bytes=packet.payload,
                                 flow_direction_gap_ms=gap / 1e6 if gap is not None else '',
                                 tx_payload_gap_ms=tx_gap / 1e6 if tx_gap is not None else '',
                                 action_phase=action_phase,
                                 tcp_flags=hex(packet.flags) if packet.proto == 'TCP' else '',
                                 tcp_seq=packet.seq if packet.proto == 'TCP' else '',
                                 tcp_ack=packet.ack if packet.proto == 'TCP' and packet.flags & 0x10 else ''))

    flow_rows = [dict(flow_addresses[key], flow=order_ids[key], selected=int(key == selected_key),
                      tx_packets=data['tx_packets'], rx_packets=data['rx_packets'],
                      tx_payload_bytes=data['tx_payload_bytes'], rx_payload_bytes=data['rx_payload_bytes'],
                      first_ms=(data['first_ns'] - origin) / 1e6, last_ms=(data['last_ns'] - origin) / 1e6)
                 for key, data in ordered]
    with (analysis_dir / 'flows.csv').open('w', newline='') as stream:
        writer = csv.DictWriter(stream, ('flow', 'selected', 'interface', 'transport', 'local_ip',
                                'local_port', 'peer_ip', 'peer_port', 'tx_packets', 'rx_packets',
                                'tx_payload_bytes', 'rx_payload_bytes', 'first_ms', 'last_ms'))
        writer.writeheader()
        writer.writerows(flow_rows)

    timeline = []
    if selected_key is not None and bins:
        for second in range(math.ceil((config['end_ns'] - origin) / NS)):
            bucket = bins[second]
            seconds = min(1.0, (config['end_ns'] - origin) / NS - second)
            if seconds <= 0:
                continue
            timeline.append(dict(start_ms=second * 1000, seconds=seconds,
                                 **{key: bucket[key] for key in TIMELINE_FIELDS[2:6]},
                                 **{f'{d}_payload_mib_s': bucket[d + '_payload_bytes'] / MIB / seconds
                                    for d in ('tx', 'rx')}))
    with (analysis_dir / 'timeline.csv').open('w', newline='') as stream:
        writer = csv.DictWriter(stream, TIMELINE_FIELDS)
        writer.writeheader()
        writer.writerows(timeline)
    active_s = (last_tx - first_tx) / NS if first_tx is not None else None
    active_bins = [row['tx_payload_mib_s'] for row in timeline if first_tx is not None and
                   row['start_ms'] * 1_000_000 >= first_tx - origin and
                   (row['start_ms'] + 1000) * 1_000_000 <= last_tx - origin]
    return {
        'capture_quality': quality, 'flow_count': len(flows), 'attribution': attribution,
        'selected_flow': flow_rows[order_ids[selected_key] - 1] if chosen else None,
        'gap_ms': {name: distribution(values, 1e6) for name, values in selected_gaps.items()},
        'largest_tx_payload_gaps': [
            dict(gap_ms=gap / 1e6, start_ms=(start - origin) / 1e6,
                 end_ms=(end - origin) / 1e6, next_capture_index=index)
            for gap, start, end, index in sorted(biggest, reverse=True)],
        'first_tx_payload_ms': (first_tx - origin) / 1e6 if first_tx is not None else None,
        'last_tx_payload_ms': (last_tx - origin) / 1e6 if last_tx is not None else None,
        'tx_payload_span_seconds': active_s,
        'tx_payload_mib_s_over_span': (
            chosen[1]['tx_payload_bytes'] / MIB / active_s if chosen and active_s and active_s > 0 else None),
        'interior_full_second_tx_payload_mib_s': distribution(active_bins),
        'tcp_syn_to_synack_ms': syn_rtt_ns / 1e6 if syn_rtt_ns is not None and not syn_ambiguous else None,
        'recipient_to_first_flow_ms': ((first_packet_after_select - selected_ns) / 1e6
                                       if first_packet_after_select is not None else None),
        'recipient_to_first_tx_payload_ms': ((first_tx_after_select - selected_ns) / 1e6
                                             if first_tx_after_select is not None else None),
        'action_tx_payload_packets': dict(action_payload_packets) if action_start is not None and chosen else None,
        'action_crossing_gap_ms': ((first_after_action - last_before_action) / 1e6
                                   if first_after_action is not None and last_before_action is not None else None),
        'action_to_next_tx_payload_ms': ((first_after_action - action_start) / 1e6
                                         if first_after_action is not None else None),
    }, timeline


def write_json(path, value):
    path.write_text(json.dumps(value, indent=2, allow_nan=False) + '\n', encoding='utf-8')
