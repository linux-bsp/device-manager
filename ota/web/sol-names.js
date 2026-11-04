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
    label(row) {
        if (!this.archived(row)) return row.boot + ((row.meta || '').includes('current=yes') ? '（当前）' : '');
        const time = this.stamp(row.archive_received_at ?? row.seen);
        const kind = (row.meta || '').includes('startup=healthy-current') ? '正常启动' : '上次未确认启动';
        return `${time?.label || '接收时间未知'} · ${kind} · ${this.short(row)}`;
    },
    filename(row, stage, exportedAt = Math.floor(Date.now() / 1000)) {
        const archived = this.archived(row);
        const time = this.stamp(archived ? (row.archive_received_at ?? row.seen) : exportedAt);
        const suffix = stage && stage !== 'all' ? '_' + stage.replace(/[^a-z0-9-]/gi, '') : '';
        return `${time?.file || 'time-unknown'}_${archived ? 'startup' : 'console'}_${this.short(row)}${suffix}.log`;
    }
};
if (typeof module !== 'undefined') module.exports = SolNames;
