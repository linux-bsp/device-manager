/* HWID is a fixed-width string; never round a 64-bit value through Number. */
const FirmwareHardware = {
    registry: null,
    registryRequest: null,
    setRegistry(value) {
        if (!value || value.schema_version !== 1 ||
            ['product_lines', 'projects', 'models'].some(key => !value[key] ||
                typeof value[key] !== 'object' || Array.isArray(value[key])))
            throw new Error('HWID 名称字典格式无效');
        this.registry = value;
    },
    loadRegistry() {
        if (!this.registryRequest) {
            const controller = new AbortController();
            const timer = setTimeout(() => controller.abort(), 3000);
            this.registryRequest = fetch('/api/hwid-registry', {cache: 'no-store', signal: controller.signal})
                .then(response => {
                    if (!response.ok) throw new Error('HWID 名称字典暂不可用');
                    return response.json();
                }).then(value => this.setRegistry(value))
                .catch(() => { this.registryRequest = null; })
                .finally(() => clearTimeout(timer));
        }
        return this.registryRequest;
    },
    label(table, key, fallback) {
        const value = this.registry?.[table]?.[key];
        return typeof value === 'string' && value ? value : fallback;
    },
    parse(value) {
        if (typeof value !== 'string' || !/^1[0-9a-f]{12}000$/.test(value)) return null;
        const line = value.slice(1, 3), project = value.slice(3, 7), model = value.slice(7, 11);
        if (line === '00' || project === '0000' || model === '0000') return null;
        const pcb = value.slice(11, 13);
        return {value, format: value[0], line, project, model, pcb, reserved: value.slice(13),
            pcbVersion: `v${parseInt(pcb[0], 16)}.${parseInt(pcb[1], 16)}`,
            grouped: `${value[0]}-${line}-${project}-${model}-${pcb}-${value.slice(13)}`};
    },
    product(value) {
        const h = this.parse(value);
        if (!h) return '未知硬件';
        const line = this.label('product_lines', h.line, `产品线 ${h.line}`);
        const project = this.label('projects', `${h.line}/${h.project}`, `项目 ${h.project}`);
        const model = this.label('models', `${h.line}/${h.project}/${h.model}`, `型号 ${h.model}`);
        return `${line} · ${project} · ${model} · PCB ${h.pcbVersion}`;
    },
    describe(value) {
        const h = this.parse(value);
        if (!h) return 'HWID 编码无效';
        const field = (table, key, label, code) => {
            const name = this.label(table, key, '');
            return `${label} ${code}${name ? `（${name}）` : ''}`;
        };
        return `${h.grouped} · 编码 v${h.format} · ` +
            field('product_lines', h.line, '产品线', h.line) + ' · ' +
            field('projects', `${h.line}/${h.project}`, '项目', h.project) + ' · ' +
            field('models', `${h.line}/${h.project}/${h.model}`, '产品型号', h.model) +
            ` · PCB ${h.pcbVersion} · 保留 ${h.reserved}`;
    }
};
if (typeof module !== 'undefined') module.exports = FirmwareHardware;
