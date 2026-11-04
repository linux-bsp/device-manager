#!/usr/bin/env python3
"""Check the single display registry, scoped keys, and its embedded representation."""
import copy
import json
from pathlib import Path
import re
import subprocess
import sys
import tempfile
import unittest

ROOT = Path(__file__).resolve().parents[2]
sys.path.insert(0, str(ROOT / 'identity'))
from registry import load_registry


class HWIDRegistry(unittest.TestCase):
    def setUp(self):
        self.temp = tempfile.TemporaryDirectory(prefix='hwid-registry-')
        self.addCleanup(self.temp.cleanup)
        self.path = Path(self.temp.name) / 'registry.json'
        self.data = load_registry(ROOT / 'config/hwid-registry.json')

    def load(self, data):
        self.path.write_text(json.dumps(data, ensure_ascii=False), encoding='utf-8')
        return load_registry(self.path)

    def test_registered_hardware_and_scoped_model_codes(self):
        self.assertEqual(self.data['product_lines']['01'], '计算载荷')
        self.assertEqual(self.data['projects']['01/0001'], 'H200')
        self.assertEqual(self.data['models']['01/0001/0001'], 'CCM 桥接板')
        self.data['models']['01/0002/0001'] = '项目二专用板'
        self.assertEqual(self.load(self.data)['models']['01/0002/0001'], '项目二专用板')

    def test_invalid_schema_codes_names_and_hierarchy_are_rejected(self):
        changes = [lambda d: d.update(schema_version=True),
                   lambda d: d.update(schema_version=2),
                   lambda d: d.update(unknown={}),
                   lambda d: d.update(projects=[]),
                   lambda d: d['product_lines'].update({'00': '保留'}),
                   lambda d: d['product_lines'].update({'0A': '大写编码'}),
                   lambda d: d['projects'].update({'02/0001': '无所属产品线'}),
                   lambda d: d['projects'].update({'01/0000': '保留项目'}),
                   lambda d: d['models'].update({'01/0003/0001': '无所属项目'}),
                   lambda d: d['models'].update({'0001': '缺少作用域'}),
                   lambda d: d['models'].update({'01/0001/0001': ''}),
                   lambda d: d['models'].update({'01/0001/0001': ' 尾空格 '}),
                   lambda d: d['models'].update({'01/0001/0001': '控制\n字符'}),
                   lambda d: d['models'].update({'01/0001/0001': 123})]
        for change in changes:
            data = copy.deepcopy(self.data)
            change(data)
            with self.subTest(change=change), self.assertRaises(ValueError):
                self.load(data)

    def test_duplicate_codes_are_not_silently_replaced(self):
        self.path.write_text('{"schema_version":1,"product_lines":{"01":"A","01":"B"},'
                             '"projects":{},"models":{}}')
        with self.assertRaisesRegex(ValueError, 'duplicate'):
            load_registry(self.path)

    def test_embedded_endpoint_data_is_exactly_the_validated_dictionary(self):
        header = Path(self.temp.name) / 'page.h'
        subprocess.run([sys.executable, str(ROOT / 'ota/web/embed.py'), str(header)], check=True)
        array = re.search(r'hwid_registry_json\[\] = \{([^}]+)\}', header.read_text())[1]
        payload = bytes(map(int, re.findall(r'\d+', array)))
        self.assertEqual(payload[-1], 0)
        self.assertEqual(json.loads(payload[:-1]), self.data)


if __name__ == '__main__':
    unittest.main()
