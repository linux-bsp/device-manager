const {test} = require('node:test');
const assert = require('node:assert/strict');
const time = require('../web/time-format.js');
test('UTC+8 calendar time is independent of viewer timezone', () => {
    const epoch = Date.UTC(2026, 10, 5, 16, 1, 2) / 1000;
    const old = process.env.TZ;
    try {
        for (const zone of ['UTC', 'America/New_York', 'Asia/Tokyo']) {
            process.env.TZ = zone;
            assert.equal(time.format(epoch), '2026/11/06 00:01:02 +08:00');
        }
    } finally { if (old === undefined) delete process.env.TZ; else process.env.TZ = old; }
});
test('uninitialized clock is explicit, epoch remains numeric UTC', () => {
    for (const value of [undefined, null, NaN, Infinity, 0, 4102444800])
        assert.equal(time.format(value, '未校准'), '未校准');
    assert.equal(time.format(1793894462), '2026/11/06 00:01:02 +08:00');
});
