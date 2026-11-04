const {test} = require('node:test');
const assert = require('node:assert/strict');
const names = require('../web/sol-names.js');
const received = Date.UTC(2026, 10, 4, 0, 36, 15) / 1000;
const archive = {boot:'df6e92a1-61fa-44b6-86e0-bab2799ce871', seen:received+86400,
    archive_received_at:received, meta:'startup=previous-unconfirmed\n'};
test('archive labels and downloads use the immutable server receipt in Beijing time', () => {
    assert.match(names.label(archive), /^2026-11-04 08:36:15 \+08:00/);
    assert.equal(names.filename(archive, 'all', received+86400), '2026-11-04_08-36-15+0800_startup_df6e92a1.log');
    assert.equal(names.filename(archive, 'kernel'), '2026-11-04_08-36-15+0800_startup_df6e92a1_kernel.log');
});
test('same-second records have distinct names and retain their kind', () => {
    const other={...archive,boot:'ed0abacf-e728-47d6-a601-3929d05b0295',meta:'startup=healthy-current\n'};
    assert.notEqual(names.filename(archive,'all'),names.filename(other,'all'));
    assert.match(names.label(archive),/上次未确认启动/);assert.match(names.label(other),/正常启动/);
});
test('unknown device or server clock never appears as a fabricated 1970 date', () => {
    assert.equal(names.stamp(0),null);assert.equal(names.stamp(NaN),null);
    assert.match(names.filename({...archive,archive_received_at:0},'all'),/^time-unknown_/);
    assert.match(names.label({...archive,archive_received_at:0}),/时间未知/);
});
test('live console snapshots use export time instead of last heartbeat', () => {
    const row={boot:'live-boot',seen:received-200,meta:'current=yes\n'};
    assert.equal(names.filename(row,'all',received),'2026-11-04_08-36-15+0800_console_live-boo.log');
    assert.match(names.label(row),/当前/);
});

test('Beijing midnight rolls into the next date without a 24-hour filename', () => {
    const stamp = names.stamp(Date.UTC(2026,10,3,16,0,0)/1000);
    assert.equal(stamp.file,'2026-11-04_00-00-00+0800');
});
