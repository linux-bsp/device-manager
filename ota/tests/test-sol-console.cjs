const {test} = require('node:test');
const assert = require('node:assert/strict');
const SolConsole = require('../web/sol-console.js');
const bytes = text => Buffer.from(text).toString('latin1');
const frame = (seq, time, data) => `[seq=${seq} mono=${time.toFixed(9)} wall=0 bytes=${data.length} source=console] ${data}`;
function push(view, stage, text, chunk = 4096, gen = 1, start = 0) {
    for (let i = 0; i < text.length; i += chunk)
        view.push({stage, gen, off:start+i, text:text.slice(i,i+chunk)});
}
test('preserves indentation, blank lines, UTF-8 and partial lines at every byte boundary', () => {
    const text = bytes('U-Boot\r\n\r\n   Description:  内核\nStarting CAN: OK\n# ');
    for (const chunk of [1, 2, 7, 4096]) {
        const view = new SolConsole();
        const data = frame(1, 1, text.slice(0, 41)) + frame(2, 2, text.slice(41));
        push(view, 'services', data, chunk);
        assert.equal(view.text(), 'U-Boot\n\n   Description:  内核\nStarting CAN: OK\n# ');
    }
});
test('only removes framing, never a literal header in new payloads', () => {
    const view = new SolConsole(), literal = '[seq=4 mono=1.0 wall=0] application text\n';
    push(view, 'services', frame(1,1,literal), 3);
    assert.equal(view.text(),literal);
});
test('kernel uses six-digit native timestamp, dictionary stripped, messages retained', () => {
    const view = new SolConsole();
    push(view,'kernel',frame(1,9,'6,12,12345,-;hello\\x20world\n SUBSYSTEM=platform\n'),2);
    assert.equal(view.text(),'[    0.012345] hello world\n');
});
test('sorts boot stages and native kernel time, not network arrival order', () => {
    const view = new SolConsole();
    push(view,'services',frame(5,2,'Starting CAN: OK\n'));
    push(view,'kernel',frame(4,7,'6,1,1000000,-;Run /init as init process\n')+frame(6,9,'6,2,3000000,-;Link is Up\n'));
    push(view,'uboot',frame(3,7,'U-Boot\n'));
    push(view,'r5-spl',frame(1,7,'R5 SPL\n'));
    push(view,'a53-spl',frame(2,7,'A53 SPL\n'));
    assert.equal(view.text(),'R5 SPL\nA53 SPL\nU-Boot\n[    1.000000] Run /init as init process\nStarting CAN: OK\n[    3.000000] Link is Up\n');
});
test('legacy metadata, source labels and collector events stay out of default view', () => {
    const view = new SolConsole();
    push(view,'services','[seq=1 mono=1.000 wall=1] source=fwctld.log\n[seq=2 mono=1.001 wall=1] [fwctl] [INFO] ready\n',1);
    push(view,'events',frame(3,2,'collector internal'));
    assert.equal(view.text(),'[fwctl] [INFO] ready\n');
    assert.equal(view.text('events'),'collector internal');
});
test('rotation and retained partial streams do not concatenate unrelated frames', () => {
    const view = new SolConsole();
    push(view,'services',frame(1,1,'first\n'),5,1);
    push(view,'services','discarded tail\n'+frame(2,2,'second\n'),7,2,500);
    assert.equal(view.text(),'first\nsecond\n');
    assert(view.truncated);
});
test('bounded view, invalid frame length and incomplete data do not leak metadata', () => {
    const view = new SolConsole(8);
    push(view,'services',frame(1,1,'123456\n')+frame(2,2,'last\n'));
    assert.equal(view.text(),'last\n'); assert(view.truncated);
    view.limit = 1024;
    push(view,'kernel','[seq=3 mono=3.000 wall=3 bytes=9999999 source=console] '+frame(4,4,'5,4,4,-;ok\n'));
    assert.equal(view.text('kernel'),'[    0.000004] ok\n');
});
test('carriage return and backspace act like a plain serial terminal', () => {
    assert.equal(SolConsole.terminal('count 3\rcount 2\rcount 1\nabc\bX\n'), 'count 1\nabX\n');
});

test('imported initramfs block cannot split an early rcS partial line', () => {
    const view = new SolConsole();
    push(view,'services',frame(1,7,'=== Starting rcS ===\nStarting CAN: ')+frame(4,8,'OK\n'));
    push(view,'initramfs',frame(2,7.1,'[fw-init] [INFO] switch_root\n'));
    assert.equal(view.text(),'[fw-init] [INFO] switch_root\n=== Starting rcS ===\nStarting CAN: OK\n');
});

test('raw startup archives preserve serial bytes and stage order', () => {
    const c = new SolConsole();
    c.push({id: 4, stage: 'kernel', raw: true, text: '[    0.000000] Linux\r\n'});
    c.push({id: 1, stage: 'r5-spl', raw: true, text: 'SPL\r\n'});
    c.push({id: 3, stage: 'uboot', raw: true, text: 'U-Boot\n'});
    assert.equal(c.text(), 'SPL\nU-Boot\n[    0.000000] Linux\n');
});
