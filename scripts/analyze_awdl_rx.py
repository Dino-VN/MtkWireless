#!/usr/bin/env python3
"""Decode bounded pre-replay RX traces from an AWDL collector folder."""
import argparse
import collections
import csv
import hashlib
import plistlib
import struct
from pathlib import Path

RECORD = struct.Struct('<QQHHHHBBbB6s6s26s80s')
assert RECORD.size == 146


def decode(raw):
    if len(raw) != RECORD.size:
        raise ValueError('Incomplete AWDL RX record')
    (time, bitmap, seq, previous, mpdu, length, result, channel, rssi,
     size, source, destination, header, prefix) = RECORD.unpack(raw)
    if not time:
        return None
    if result not in (1, 2, 3) or not 56 <= length <= 1516 or size > 80 or size > length:
        raise ValueError('Invalid AWDL RX record')
    prefix = prefix[:size]
    if size < 16 or struct.unpack_from('<H', prefix, 10)[0] != seq:
        raise ValueError('AWDL sequence does not match header')
    row = dict(time_us=time, source=source.hex(':'), destination=destination.hex(':'),
               sequence=seq, previous=previous, back=(previous-seq) & 65535,
               bitmap=hex(bitmap), mpdu=mpdu, result={1: 'accepted', 2: 'duplicate', 3: 'too-old'}[result],
               channel=channel, rssi=rssi, tid=header[24] & 15 if header[0] == 0x88 else -1,
               length=length, prefix_hash=hashlib.sha256(prefix).hexdigest()[:16],
               protocol='', src_port='', dst_port='', tcp_sequence='', tcp_ack='', tcp_flags='')
    # Prefix begins with SNAP/AWDL (16 bytes), followed by the IPv6 packet.
    if size >= 56 and prefix[16] >> 4 == 6:
        protocol = prefix[22]
        row['protocol'] = protocol
        if protocol in (6, 17) and size >= 60:
            row['src_port'], row['dst_port'] = struct.unpack_from('>HH', prefix, 56)
        if protocol == 6 and size >= 70:
            row['tcp_sequence'], row['tcp_ack'] = struct.unpack_from('>II', prefix, 60)
            row['tcp_flags'] = hex(prefix[69])
    return row


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('folder', type=Path)
    args = parser.parse_args()
    records = {}
    for path in sorted(args.folder.glob('*.plist')):
        for entry in plistlib.loads(path.read_bytes()):
            if entry.get('MTKAWDLDataRxTraceVersion') != 1:
                continue
            for key in ('MTKAWDLDataRxTrace', 'MTKAWDLDataRxLastRejected'):
                data = entry.get(key, b'')
                if len(data) % RECORD.size:
                    raise ValueError(f'{path}: truncated {key}')
                for offset in range(0, len(data), RECORD.size):
                    raw = data[offset:offset+RECORD.size]
                    row = decode(raw)
                    if row:
                        records[raw] = row
    if not records:
        raise SystemExit('No pre-replay RX trace: this requires the 0.1.10 diagnostic Wi-Fi kext.')
    rows = sorted(records.values(), key=lambda row: row['time_us'])
    target = args.folder / 'rx-trace.csv'
    with target.open('w', newline='') as output:
        writer = csv.DictWriter(output, fieldnames=list(rows[0]))
        writer.writeheader()
        writer.writerows(rows)
    print(f'{len(rows)} distinct sampled records; ring sampling may omit frames. CSV: {target}')
    counts = collections.Counter((row['source'], row['destination'], row['tid'], row['result']) for row in rows)
    for (src, dst, tid, result), count in sorted(counts.items()):
        print(f'{src} -> {dst}, TID {tid}: {result}={count}')
    print('Last 12 rejected records:')
    for row in [r for r in rows if r['result'] != 'accepted'][-12:]:
        print(' '.join(f'{key}={row[key]}' for key in ('time_us', 'source', 'destination', 'tid',
              'sequence', 'previous', 'back', 'result', 'src_port', 'dst_port', 'tcp_ack')))


if __name__ == '__main__':
    main()
