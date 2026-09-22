"""Parse read-only show flows snapshots without rounding uint64 labels."""
from collections import Counter
import io
import ipaddress
import json
import re

MAX_FLOW_REPLY = 8 * 1024 * 1024
MAX_ROWS = 2000
MAX_ROWS_BYTES = 2 * 1024 * 1024
STACK_KEYS = {'labels', 'client_labels', 'server_labels', 'client_saved_labels',
              'server_saved_labels', 'client_divert_body', 'server_divert_body'}
LABEL_KEYS = STACK_KEYS - {'client_divert_body', 'server_divert_body'}
BASE_KEYS = {'table', 'ip_version', 'src', 'dst', 'protocol', 'src_port', 'dst_port', 'idle_ms', 'path'}
CONTEXT_KEYS = {f'{side}_{field}' for side in ('client', 'server')
                for field in ('cookie', 'chain', 'step', 'origin', 'action', 'reverse')}
ADMISSION = {'existing_tcp', 'diverted_tcp', 'existing_udp'}


def unsigned(value, maximum=2**64-1):
    if not re.fullmatch(r'[0-9]{1,20}', value) or int(value) > maximum:
        raise ValueError('invalid unsigned flow value')
    return value


def parse_row(line):
    row = {}
    for item in line.split()[1:]:
        key, sep, value = item.partition('=')
        if not sep or key in row or key not in BASE_KEYS | STACK_KEYS | CONTEXT_KEYS:
            raise ValueError('unknown or duplicate flow field')
        if key in STACK_KEYS:
            if value == 'unknown' and key == 'labels':
                row[key] = None
            else:
                if not re.fullmatch(r'\[(?:0x[0-9a-fA-F]{16}(?:,0x[0-9a-fA-F]{16})*)?\]', value):
                    raise ValueError('invalid label stack')
                row[key] = value[1:-1].lower().split(',') if value != '[]' else []
        elif key in ('src', 'dst'):
            row[key] = str(ipaddress.ip_address(value))
        elif key == 'table':
            if not re.fullmatch(r'[a-z0-9_]{1,64}', value):
                raise ValueError('invalid flow table')
            row[key] = value
        else:
            row[key] = unsigned(value, 65535 if key.endswith('_port') else 255 if key=='protocol' else 2**64-1)
    if not {'table', 'ip_version', 'src', 'dst'} <= row.keys() or row['ip_version'] not in ('4', '6'):
        raise ValueError('incomplete flow key')
    if any(ipaddress.ip_address(row[key]).version != int(row['ip_version']) for key in ('src', 'dst')):
        raise ValueError('flow address family mismatch')
    if any(key in row for key in ('protocol', 'src_port', 'dst_port')) and not {'protocol', 'src_port', 'dst_port'} <= row.keys():
        raise ValueError('incomplete transport key')
    return row


def parse_flows(text, expected_pid):
    if len(text.encode()) > MAX_FLOW_REPLY:
        raise ValueError('flow snapshot exceeds Fabric reply limit')
    header, rows, tables, labels = {}, [], Counter(), Counter()
    count = stored_bytes = 0
    footer = None
    for line in io.StringIO(text):
        line = line.strip()
        if not line:
            continue
        if footer is not None:
            raise ValueError('data after flow_count footer')
        if line.startswith('flow '):
            if not {'format', 'format_version', 'view', 'pid', 'tracking'} <= header.keys():
                raise ValueError('missing flow header')
            row = parse_row(line)
            count += 1
            tables[row['table']] += 1
            unique = {label for key in LABEL_KEYS for label in (row.get(key) or [])}
            labels.update(unique)
            size = len(json.dumps(row))
            if len(rows) < MAX_ROWS and stored_bytes + size <= MAX_ROWS_BYTES:
                rows.append(row)
                stored_bytes += size
        else:
            key, sep, value = line.partition('=')
            if key == 'flow_count' and sep:
                footer = int(unsigned(value))
            elif sep and not count and key in ('format', 'format_version', 'view', 'pid', 'tracking') and key not in header:
                header[key] = value
            else:
                raise ValueError('invalid flow header')
    if (header.get('format') != 'txt' or header.get('format_version') != '1' or
            header.get('view') != 'flows' or (not re.fullmatch(r'[0-9]+', header.get('pid', '')) or (expected_pid is not None and header.get('pid') != str(expected_pid))) or
            header.get('tracking') not in ('none', 'retained') or footer != count or
            (header['tracking'] == 'none' and count)):
        raise ValueError('inconsistent flow snapshot')
    return {'tracking': header['tracking'], 'flow_count': count, 'rows': rows,
            'returned_count': len(rows), 'truncated': count != len(rows), 'row_limit': MAX_ROWS,
            'table_counts': dict(tables), 'admission_count': sum(tables[t] for t in ADMISSION),
            'label_count': len(labels), 'labels': [{'value': label, 'rows': n} for label, n in labels.most_common(128)],
            'labels_truncated': len(labels) > 128}
