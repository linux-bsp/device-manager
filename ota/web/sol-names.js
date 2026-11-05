/* Archive time is the server's immutable first receipt, never a guessed boot
 * time. Show Asia/Shanghai explicitly, independent of the viewer's timezone. */
const SolNames = {
    stamp(seconds) {
        if (!Number.isSafeInteger(seconds) || seconds < 1577836800 || seconds >= 4102444800) return null;
        const parts = Object.fromEntries(new Intl.DateTimeFormat('en-GB', {
            timeZone: 'Asia/Shanghai', year: 'numeric', month: '2-digit', day: '2-digit',
            hour: '2-digit', minute: '2-digit', second: '2-digit', hourCycle: 'h23'
        }).formatToParts(new Date(seconds * 1000)).map(p => [p.type, p.value]));
        const day = `${parts.year}-${parts.month}-${parts.day}`;
        return {file: `${day}_${parts.hour}-${parts.minute}-${parts.second}+0800`,
            label: `${day} ${parts.hour}:${parts.minute}:${parts.second} +08:00`};
    },
    archived(row) { return /(?:^|\n)startup=/.test(row?.meta || ''); },
    short(row) { return (row?.boot || 'unknown').replace(/[^a-z0-9_-]/gi, '').slice(0, 8) || 'unknown'; },
    outcome(row) {
        const meta = Object.fromEntries((row.meta || '').split('\n').filter(line => line.includes('='))
            .map(line => [line.slice(0, line.indexOf('=')), line.slice(line.indexOf('=') + 1)]));
        if (meta['software-intent'] === 'health-rejected') return '异常启动';
        if (meta.startup === 'healthy-current' || meta['boot-health'] === 'confirmed') return '正常启动';
        if (meta.startup === 'previous-unconfirmed') return '异常启动';
        if (meta['boot-health'] === 'pending') return meta.current === 'no' ? '异常启动' : '启动中';
        return '启动状态未知';
    },
    label(row) {
        const time = this.stamp(row.first_connected_at ?? (this.archived(row) ? row.archive_received_at : row.bundle?.first_connected_at) ?? row.bundle?.received ?? row.seen);
        const title = time?.label || '接收时间未知';
        const current = /(?:^|\n)current=yes(?:\n|$)/.test(row.meta || '') ? '（当前）' : '';
        return `${title} · ${this.outcome(row)}${current}${row.bundleOnly ? ' · 完整日志包' : ''}`;
    },
    filename(row, stage, exportedAt = Math.floor(Date.now() / 1000)) {
        const archived = this.archived(row);
        const time = this.stamp(archived ? (row.archive_received_at ?? row.seen) : exportedAt);
        const suffix = stage && stage !== 'all' ? '_' + stage.replace(/[^a-z0-9-]/gi, '') : '';
        return `${time?.file || 'time-unknown'}_${archived ? 'startup' : 'console'}_${this.short(row)}${suffix}.log`;
    }
};
if (typeof module !== 'undefined') module.exports = SolNames;
