/* HWID is a fixed-width string; never round a 64-bit value through Number. */
const FirmwareHardware = {
    parse(value) {
        if (typeof value !== 'string' || !/^1[0-9a-f]{12}000$/.test(value)) return null;
        const line = value.slice(1, 3), project = value.slice(3, 7), model = value.slice(7, 11);
        if (line === '00' || project === '0000' || model === '0000') return null;
        const pcb = value.slice(11, 13);
        return {value, format: value[0], line, project, model, pcb, reserved: value.slice(13),
            pcbVersion: `v${parseInt(pcb[0], 16)}.${parseInt(pcb[1], 16)}`,
            grouped: `${value[0]}-${line}-${project}-${model}-${pcb}-${value.slice(13)}`};
    },
    describe(value) {
        const h = this.parse(value);
        return h ? `${h.grouped} · 编码 v${h.format} · 产品线 ${h.line} · 项目 ${h.project} · 产品型号 ${h.model} · PCB ${h.pcbVersion} · 保留 ${h.reserved}` : 'HWID 编码无效';
    }
};
if (typeof module !== 'undefined') module.exports = FirmwareHardware;
