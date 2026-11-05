const {test} = require('node:test');
const assert = require('node:assert/strict');
const names = require('../web/device-names.js');
const history = [
    {device:'stable-id',publication:1,source_boot:'a',updated:10,network:[{ip:'192.0.2.10',last_seen:10}]},
    {device:'stable-id',publication:2,source_boot:'b',updated:20,network:[{ip:'192.0.2.20',last_seen:20}]}
];
test('current display follows newer IP while historical transactions keep their recorded IP', () => {
    assert.equal(names.label({device:'stable-id'},{device_history:history}),'192.0.2.20');
    assert.equal(names.label({device:'stable-id',publication:1},{device_history:history},[],true),'192.0.2.10');
    assert.equal(names.label({device:'stable-id',publication:3},{device_history:history},[],true),'stable-id');
});
test('SOL-only and clock reports use peer addresses without changing stable keys', () => {
    const row={device:'mac-key',peer:'2001:db8::10'};
    assert.equal(names.label(row),'2001:db8::10');assert.equal(row.device,'mac-key');
    assert.equal(names.label({device:'mac-key'}, {}, [{...row,seen:30}]),'2001:db8::10');
});
test('non-address metadata never becomes an IP and separate devices are not merged', () => {
    assert.equal(names.label({device:'other'}, {device_history:history}),'other');
    assert.equal(names.label({device:'stable-id',peer:'独立事故附件'}, {device_history:history}),'192.0.2.20');
    assert.equal(names.ip('999.0.0.1'),'');
});
