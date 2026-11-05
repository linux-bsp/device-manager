/* All human calendar times use UTC+8, independently of the browser timezone.
 * API/database epochs remain UTC; durations and kernel uptime are unchanged. */
const DeviceTime = {
    format(seconds, unknown = '未知') {
        if (!Number.isFinite(seconds) || seconds < 1577836800 || seconds >= 4102444800)
            return unknown;
        const value = new Date(Math.floor(seconds * 1000));
        return new Intl.DateTimeFormat('zh-CN', {
            timeZone: 'Etc/GMT-8', year: 'numeric', month: '2-digit', day: '2-digit',
            hour: '2-digit', minute: '2-digit', second: '2-digit', hourCycle: 'h23'
        }).format(value) + ' +08:00';
    }
};
if (typeof module !== 'undefined') module.exports = DeviceTime;
