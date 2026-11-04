"""Validate the host-only HWID display dictionary before embedding it."""
import json
from pathlib import Path
import re


def unique_keys(pairs):
    result = {}
    for key, value in pairs:
        if key in result:
            raise ValueError(f'duplicate HWID registry key: {key}')
        result[key] = value
    return result


def load_registry(path):
    data = json.loads(Path(path).read_text(encoding='utf-8'), object_pairs_hook=unique_keys)
    fields = {'schema_version', 'product_lines', 'projects', 'models'}
    if not isinstance(data, dict) or set(data) != fields:
        raise ValueError('HWID registry requires schema_version, product_lines, projects and models')
    if type(data['schema_version']) is not int or data['schema_version'] != 1:
        raise ValueError('unsupported HWID registry schema_version')
    for table, count in (('product_lines', 1), ('projects', 2), ('models', 3)):
        entries = data[table]
        if not isinstance(entries, dict):
            raise ValueError(f'{table} must be an object')
        for key, name in entries.items():
            parts = key.split('/')
            if (len(parts) != count or not re.fullmatch(r'[0-9a-f]{2}', parts[0])
                    or parts[0] == '00' or any(not re.fullmatch(r'[0-9a-f]{4}', value)
                    or value == '0000' for value in parts[1:])):
                raise ValueError(f'invalid {table} key: {key}')
            if (not isinstance(name, str) or not name.strip() or name != name.strip()
                    or len(name) > 128 or any(ord(char) < 32 or ord(char) == 127 for char in name)):
                raise ValueError(f'invalid {table} name: {key}')
            if count > 1 and parts[0] not in data['product_lines']:
                raise ValueError(f'unknown product line in {table}: {key}')
            if count > 2 and '/'.join(parts[:2]) not in data['projects']:
                raise ValueError(f'unknown project in {table}: {key}')
    return data
