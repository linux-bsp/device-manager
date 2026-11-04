const {test, beforeEach} = require('node:test');
const assert = require('node:assert/strict');
const hwid = require('../web/hwid.js');
const registry = require('../../config/hwid-registry.json');

beforeEach(() => { hwid.registry = null; hwid.registryRequest = null; });

test('board HWID fields preserve their widths and exact PCB silk screen version', () => {
    assert.deepEqual(hwid.parse('1010001000110000'), {
        value:'1010001000110000',format:'1',line:'01',project:'0001',model:'0001',
        pcb:'10',reserved:'000',pcbVersion:'v1.0',grouped:'1-01-0001-0001-10-000'
    });
    assert.match(hwid.describe('1010001000110000'), /项目 0001 · 产品型号 0001 · PCB v1\.0/);
    assert.equal(hwid.product('1010001000110000'), '产品线 01 · 项目 0001 · 型号 0001 · PCB v1.0');
});
test('high-bit identities keep all 64 bits without numeric conversion', () => {
    const value='1ffffffffffff000';
    assert.equal(hwid.parse(value).value,value);
    assert.equal(hwid.parse(value).pcbVersion,'v15.15');
    assert.equal(hwid.parse('101000100011a000').pcbVersion,'v1.10');
});
test('missing or invalid identities are never displayed as generic hardware support', () => {
    for(const value of [null,undefined,123,'101000100011000','2010001000110000','1010001000110001','1000001000110000','1010000000110000','1010001000010000','101000100011A000','<script>'])
        assert.equal(hwid.parse(value),null,String(value));
    assert.equal(hwid.product(null), '未知硬件');
});

test('registered field names keep exact HWID and PCB codes available', () => {
    hwid.setRegistry(registry);
    assert.equal(hwid.product('1010001000110000'), '计算载荷 · H200 · CCM 桥接板 · PCB v1.0');
    assert.match(hwid.describe('1010001000110000'), /项目 0001（H200）/);
    assert.match(hwid.describe('1010001000110000'), /产品型号 0001（CCM 桥接板）/);
    assert.match(hwid.describe('1010001000110000'), /1-01-0001-0001-10-000/);
    assert.equal(hwid.product('101000100011a000'), '计算载荷 · H200 · CCM 桥接板 · PCB v1.10');
});

test('unknown fields fall back individually and model names never cross projects', () => {
    hwid.setRegistry(registry);
    assert.equal(hwid.product('1010002000110000'), '计算载荷 · 飞腾 S5000C · 型号 0001 · PCB v1.0');
    assert.equal(hwid.product('1020001000110000'), '产品线 02 · 项目 0001 · 型号 0001 · PCB v1.0');
    const scoped = structuredClone(registry);
    scoped.models['01/0002/0001'] = '项目二专用板';
    hwid.setRegistry(scoped);
    assert.equal(hwid.product('1010002000110000'), '计算载荷 · 飞腾 S5000C · 项目二专用板 · PCB v1.0');
});

test('registry is fetched once and an unavailable endpoint preserves raw-code display', async t => {
    const original = global.fetch;
    t.after(() => { global.fetch = original; });
    let requests = 0;
    global.fetch = async url => {
        assert.equal(url, '/api/hwid-registry');
        requests++;
        return {ok: requests > 1, json: async () => registry};
    };
    await hwid.loadRegistry();
    assert.equal(hwid.product('1010001000110000'), '产品线 01 · 项目 0001 · 型号 0001 · PCB v1.0');
    await Promise.all([hwid.loadRegistry(), hwid.loadRegistry()]);
    await hwid.loadRegistry();
    assert.equal(requests, 2);
    assert.match(hwid.product('1010001000110000'), /CCM 桥接板/);
});

test('malformed registry responses cannot replace the previous valid dictionary', () => {
    hwid.setRegistry(registry);
    for (const value of [null, {}, {...registry, schema_version: 2}, {...registry, models: []}])
        assert.throws(() => hwid.setRegistry(value));
    assert.match(hwid.product('1010001000110000'), /CCM 桥接板/);
});
