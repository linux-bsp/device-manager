/* Byte-oriented archive decoder. Transport metadata stays out of the console.
 * The API's strings are Latin-1 byte containers, not decoded UTF-8 strings. */
class SolConsole {
    constructor(limit = 2 * 1024 * 1024) {
        this.streams = new Map();
        this.records = [];
        this.size = 0;
        this.limit = limit;
        this.truncated = false;
    }
    push(row) {
        const key = row.stage + '/' + (row.gen ?? 0);
        let stream = this.streams.get(key);
        if (!stream) {
            stream = {stage: row.stage, buffer: '', frame: null, offset: row.off ?? 0};
            this.streams.set(key, stream);
        }
        if (row.off !== undefined && row.off !== stream.offset) {
            stream.buffer = ''; stream.frame = null;
            this.truncated = true;
        }
        stream.offset += row.text.length;
        stream.buffer += row.text;
        this.parse(stream);
    }
    save(stream, data) {
        this.records.push({...stream.frame, stage: stream.stage, data});
        this.size += data.length;
        while (this.size > this.limit && this.records.length) {
            this.size -= this.records.shift().data.length;
            this.truncated = true;
        }
    }
    parse(stream) {
        const header = /\[seq=(\d+) mono=(\d+(?:\.\d+)?) wall=(-?\d+)(?: bytes=(\d+) source=([a-z]+))?\] /;
        for (;;) {
            if (!stream.frame) {
                const match = header.exec(stream.buffer);
                if (!match) {
                    // A header may span HTTP chunks. Bound incomplete/corrupt input.
                    if (stream.buffer.length > 1024) {
                        stream.buffer = stream.buffer.slice(-1024);
                        this.truncated = true;
                    }
                    return;
                }
                if (match.index) this.truncated = true;
                stream.buffer = stream.buffer.slice(match.index + match[0].length);
                stream.frame = {seq: Number(match[1]), time: Number(match[2]),
                    length: match[4] === undefined ? null : Number(match[4]), legacy: match[4] === undefined};
                if (stream.frame.length > 262144) {
                    stream.frame = null; this.truncated = true; continue;
                }
            }
            const frame = stream.frame;
            if (frame.length !== null) {
                if (stream.buffer.length < frame.length) return;
                this.save(stream, stream.buffer.slice(0, frame.length));
                stream.buffer = stream.buffer.slice(frame.length);
            } else {
                // Old archives have no length. Their added newlines are irreversible.
                const next = header.exec(stream.buffer);
                if (!next) {
                    if (stream.buffer.length > 262144) {
                        stream.buffer = stream.buffer.slice(-1024);
                        stream.frame = null; this.truncated = true;
                    }
                    return;
                }
                this.save(stream, stream.buffer.slice(0, next.index));
                stream.buffer = stream.buffer.slice(next.index);
            }
            stream.frame = null;
        }
    }
    text(stage = 'all') {
        const records = this.records.slice();
        // Preview the last legacy record without waiting for the next message.
        for (const stream of this.streams.values()) {
            if (stream.frame?.legacy && stream.buffer) {
                const end = stream.buffer.lastIndexOf('\n');
                if (end >= 0) records.push({...stream.frame, stage: stream.stage,
                    data: stream.buffer.slice(0, end + 1)});
            }
        }
        // Early initramfs text is imported as a block after the rcS spool.
        // It belongs before rcS, even though its receipt timestamp is later.
        const firstService = Math.min(...records.filter(r => r.stage === 'services').map(r => r.time));
        const output = [];
        const bootOrder = {'r5-spl': 0, 'a53-spl': 1, uboot: 2};
        for (const record of records) {
            if (stage !== 'all' && stage !== record.stage) continue;
            if (stage === 'all' && record.stage === 'events') continue;
            let {data, time} = record;
            if (record.stage === 'initramfs' && Number.isFinite(firstService))
                time = Math.min(time, firstService - 0.000001);
            if (record.legacy && record.stage === 'services' && /^source=[^\n]*\n$/.test(data)) continue;
            if (record.stage === 'kernel') {
                const match = /^\d+,\d+,(\d+),[^;]*;([^]*?)(?:\n |$)/.exec(data);
                if (match) {
                    const us = Number(match[1]); time = us / 1000000;
                    // /dev/kmsg escapes nonprintable bytes and carries dictionary
                    // lines after the message; neither is a serial-console prefix.
                    const message = match[2].replace(/\n$/, '').replace(/\\x([0-9a-f]{2})/gi,
                        (_, hex) => String.fromCharCode(parseInt(hex, 16)));
                    const stamp = '[' + String(Math.floor(us / 1000000)).padStart(5) + '.' +
                        String(us % 1000000).padStart(6, '0') + '] ';
                    data = message.split('\n').map(line => stamp + line).join('\n') + '\n';
                }
            }
            output.push({...record, data, time, group: bootOrder[record.stage] ?? 3});
        }
        // Upload order is stage/file order, not console order. Kernel timestamps
        // are native; userspace timestamps are receipt times (best effort merge).
        output.sort((a, b) => a.group - b.group || a.time - b.time || a.seq - b.seq);
        const bytes = output.map(r => r.data).join('');
        const text = new TextDecoder().decode(Uint8Array.from(bytes, c => c.charCodeAt(0)));
        return SolConsole.terminal(text);
    }
    static terminal(text) {
        // Plain-text console export: CR updates the current line and backspace
        // moves left. No synthetic labels or blank lines between network chunks.
        const lines = []; let line = [], column = 0;
        text = text.replace(/\x1b\[[0-9;]*m/g, '');
        for (const ch of text) {
            if (ch === '\n') { lines.push(line.join('')); line = []; column = 0; }
            else if (ch === '\r') column = 0;
            else if (ch === '\b') column = Math.max(0, column - 1);
            else if (ch !== '\0') line[column++] = ch;
        }
        return lines.join('\n') + (lines.length ? '\n' : '') + line.join('');
    }
}
if (typeof module !== 'undefined') module.exports = SolConsole;
