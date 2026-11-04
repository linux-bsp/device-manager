const {test} = require('node:test');
const assert = require('node:assert/strict');
const hwid = require('../web/hwid.js');

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
