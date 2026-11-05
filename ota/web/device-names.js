/* IP is a display name; stable device keys still bind requests and history. */
const DeviceNames = {
    ip(value) {
        if (typeof value !== 'string') return '';
        if (/^\d{1,3}(\.\d{1,3}){3}$/.test(value) && value.split('.').every(n => Number(n) <= 255)) return value;
        if (value.includes(':') && /^[0-9a-f:.]+$/i.test(value)) return value;
        return '';
    },
    network(row) {
        const records = Array.isArray(row?.network) ? row.network : row?.network ? [row.network] : [];
        return [...records].sort((a,b) => (b.last_seen || 0) - (a.last_seen || 0))
            .find(n => this.ip(n.ip) || this.ip(n.peer_ip)) || null;
    },
    address(row) {
        const network = this.network(row);
        return this.ip(network?.ip) || this.ip(network?.peer_ip) || this.ip(row?.ip) || this.ip(row?.peer);
    },
    label(row, data = {}, solRows = [], historical = false) {
        const direct = this.address(row);
        if (direct) return direct;
        const histories = (data?.device_history || []).filter(h => h.device === row.device &&
            (!historical || (h.publication === row.publication && (!row.source_boot || h.source_boot === row.source_boot))));
        const candidates = histories.map(h => ({ip:this.address(h), seen:this.network(h)?.last_seen || h.updated || 0}));
        if (!historical) {
            const rows = solRows.filter(r => r.device === row.device);
            const current = rows.filter(r => /(?:^|\n)current=yes(?:\n|$)/.test(r.meta || ''));
            for (const r of current.length ? current : rows) candidates.push({ip:this.address(r), seen:r.seen || 0});
        }
        return candidates.filter(r => r.ip).sort((a,b) => b.seen-a.seen)[0]?.ip || row.device || 'IP 未上报';
    }
};
if (typeof module !== 'undefined') module.exports = DeviceNames;
