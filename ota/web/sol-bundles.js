/* Full closed-boot packages are separate from the bounded live text window. */
async function solWithBundles(rows) {
    const packages = await solGet('/api/sol/bundles');
    const combined = rows.map(row => ({...row, bundle: null}));
    for (const bundle of packages) {
        const existing = combined.find(row => row.device === bundle.device && row.boot === bundle.boot);
        if (existing) existing.bundle = bundle;
        else combined.push({device: bundle.device, boot: bundle.boot, order: bundle.order,
            seen: bundle.received, peer: bundle.peer, meta: '', bundle, bundleOnly: true});
    }
    return combined.sort((a, b) => a.device.localeCompare(b.device) || b.order - a.order);
}

function solBundleButton() {
    const row = sol.rows.find(row => row.device === $('sol-device').value && row.boot === $('sol-boot').value);
    $('sol-bundle').disabled = !row?.bundle;
    $('sol-bundle').title = row?.bundle ? '下载经过完整 SHA256 校验的全部阶段日志包' : '尚未收到整包归档确认';
}

async function solDownloadBundle() {
    const row = sol.rows.find(row => row.device === $('sol-device').value && row.boot === $('sol-boot').value);
    if (!row?.bundle) return;
    const button = $('sol-bundle');
    button.disabled = true;
    try {
        const path = '/api/sol/bundles/' + [row.device, row.boot].map(encodeURIComponent).join('/');
        const response = await fetch(path, {headers: sol.token ? {Authorization: 'Bearer ' + sol.token} : {}});
        if (!response.ok) throw Error('归档包下载失败，请刷新后重试。');
        const url = URL.createObjectURL(await response.blob());
        const a = document.createElement('a');
        a.href = url;
        a.download = `${SolNames.stamp(row.bundle.received)?.file || 'time-unknown'}_boot-${SolNames.short(row)}.tar.gz`;
        a.click();
        setTimeout(() => URL.revokeObjectURL(url), 1000);
    } catch (error) { $('sol-status').textContent = error.message; }
    finally { solBundleButton(); }
}
