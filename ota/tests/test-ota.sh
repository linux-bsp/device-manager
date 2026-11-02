#!/bin/sh
set -eu
src=$(CDPATH= cd -- "$(dirname -- "$0")/.." && pwd)
if [ -d "$src/../../../output/am625/host/bin" ]; then
    PATH="$src/../../../output/am625/host/bin:$PATH"
    export PATH
fi
for tool in curl sha256sum dtc fdtget mkimage dumpimage; do
    command -v "$tool" >/dev/null || { echo "Missing test dependency: $tool" >&2; exit 1; }
done
scratch=$(mktemp -d)
server_pid=
agent_pid=
cleanup()
{
    [ -z "$agent_pid" ] || { kill "$agent_pid" 2>/dev/null || :; wait "$agent_pid" 2>/dev/null || :; }
    [ -z "$server_pid" ] || { kill "$server_pid" 2>/dev/null || :; wait "$server_pid" 2>/dev/null || :; }
    rm -rf "$scratch"
}
trap cleanup EXIT
trap 'exit 1' INT TERM
mkdir "$scratch/source" "$scratch/feed" "$scratch/cache" "$scratch/wrong"
make_fixture()
{
    cp "$scratch/source/kernel.itb" "$scratch/kernel-payload"
    cat > "$scratch/kernel.dts" <<EOF
/dts-v1/;
/ { images { kernel { data = /incbin/("$scratch/kernel-payload"); }; };
configurations { default = "c"; c { fw,version = <65536>; }; }; };
EOF
    dtc -I dts -O dtb -o "$scratch/source/kernel.itb" "$scratch/kernel.dts"

    kernel_hash=$(sha256sum "$scratch/source/kernel.itb" | cut -d ' ' -f 1)
    rootfs_hash=$(sha256sum "$scratch/source/rootfs.itb" | cut -d ' ' -f 1)
    cat > "$scratch/manifest.dts" <<EOF
/dts-v1/;
/ {
    configurations {
        default = "conf-1";
        conf-1 {
            fw,type = "release";
            fw,version = <65536>;
            fw,product = "ti-am62x";
            fw,layout-id = "am62x-sd-ab-v1";
            fw,components = "kernel", "rootfs";
            fw,kernel-file = "kernel.itb";
            fw,rootfs-file = "rootfs.itb";
            fw,kernel-sha256 = "$kernel_hash";
            fw,rootfs-sha256 = "$rootfs_hash";
        };
    };
};
EOF
    dtc -I dts -O dtb -o "$scratch/source/release.itb" "$scratch/manifest.dts"
}
publish()
{
    "$src/device-server" publish --root "$scratch/feed" --source "$scratch/source" \
        --version "$1" --product ti-am62x --layout am62x-sd-ab-v1
}
agent()
{
    "$src/fwctld" --server "$url" --cache "$scratch/cache" \
        --product ti-am62x --layout am62x-sd-ab-v1 "$@"
}
http()
{
    curl --noproxy '*' --silent --show-error --max-time 5 "$@"
}
expect_fail()
{
    if "$@" >"$scratch/expected-failure.log" 2>&1; then
        echo "Unexpected success: $*" >&2
        exit 1
    fi
}
"$src/device-server" --root "$scratch/feed" --port 0 > "$scratch/server.log" 2>&1 &
server_pid=$!
i=0
while ! test -s "$scratch/server.log"; do
    i=$((i + 1))
    [ "$i" -lt 6 ] || exit 1
    sleep 1
done
url=$(sed -n 's|Device Manager: \(http://[^/]*/\).*|\1|p' "$scratch/server.log")
url=${url%/}
[ -n "$url" ]
[ "$(http -o /dev/null -w '%{http_code}' "$url/latest")" = 404 ]
http --fail "$url/healthz" | grep -qx ok
printf 'kernel fixture\n' > "$scratch/source/kernel.itb"
dd if=/dev/zero of="$scratch/rootfs-payload" bs=1024 count=257 2>/dev/null
cat > "$scratch/rootfs.its" <<EOF
/dts-v1/;
/ { description = "rootfs"; #address-cells = <1>;
images { rootfs { description = "rootfs"; data = /incbin/("$scratch/rootfs-payload"); type = "firmware"; arch = "arm64"; compression = "none"; }; };
configurations { default = "c"; c { firmware = "rootfs"; }; }; };
EOF
mkimage -f "$scratch/rootfs.its" "$scratch/source/rootfs.itb" >/dev/null
make_fixture
publish 1
http --fail "$url/" | grep -q 'upload-form'
http --fail "$url/api/state" | grep -q '"latest":1'
for f in release.itb kernel.itb rootfs.itb; do
    http --fail "$url/releases/1/$f" -o "$scratch/download"
    cmp "$scratch/download" "$scratch/source/$f"
done
http --fail -I "$url/releases/1/rootfs.itb" | tr -d '\r' | grep -q "Content-Length: $(stat -c %s "$scratch/source/rootfs.itb")"
for path in '/etc/passwd' '/releases/1/../manifest' '/releases/1/%2e%2e' '/releases/01/kernel.itb' '/releases/1/bootloader.itb'; do
    [ "$(http --path-as-is -o /dev/null -w '%{http_code}' "$url$path")" = 404 ]
done
[ "$(http -X POST -o /dev/null -w '%{http_code}' "$url/latest")" = 405 ]
agent --once
for f in release.itb kernel.itb rootfs.itb; do
    cmp "$scratch/cache/1/$f" "$scratch/source/$f"
done
record_inode=$(stat -c %i "$scratch/cache/current")
agent --once > "$scratch/repeat.log"
[ "$record_inode" = "$(stat -c %i "$scratch/cache/current")" ]
if grep -q Downloaded "$scratch/repeat.log"; then exit 1; fi
expect_fail publish 1
expect_fail "$src/fwctld" --server "$url" --cache "$scratch/wrong" \
    --product imx6ull --layout am62x-sd-ab-v1 --once
# Agent already running discovers the next publication without restarting.
"$src/fwctld" --server "$url" --cache "$scratch/cache" \
    --product ti-am62x --layout am62x-sd-ab-v1 --interval 1 > "$scratch/agent.log" 2>&1 &
agent_pid=$!
sleep 1
expect_fail agent --once
printf 'second kernel\n' > "$scratch/source/kernel.itb"
make_fixture
publish 2
i=0
while ! grep -q '^BSPOTA1 2 ' "$scratch/cache/current"; do
    i=$((i + 1))
    [ "$i" -lt 10 ] || { cat "$scratch/agent.log"; exit 1; }
    sleep 1
done
kill "$agent_pid"
wait "$agent_pid"
agent_pid=
http --fail "$url/releases/1/kernel.itb" -o "$scratch/old"
cmp "$scratch/old" "$scratch/cache/1/kernel.itb"
cp "$scratch/cache/current" "$scratch/previous"
# Publisher rejects mixed files before latest changes.
printf 'mismatched data\n' > "$scratch/source/kernel.itb"
expect_fail publish 3
http --fail "$url/latest" -o "$scratch/latest"
cmp "$scratch/latest" "$scratch/previous"
make_fixture
publish 3
printf 'corrupt\n' > "$scratch/feed/releases/3/rootfs.itb"
expect_fail agent --once
cmp "$scratch/cache/current" "$scratch/previous"
[ ! -e "$scratch/cache/3" ]
rm "$scratch/feed/releases/3/rootfs.itb"
ln -s "$scratch/source/rootfs.itb" "$scratch/feed/releases/3/rootfs.itb"
[ "$(http -o /dev/null -w '%{http_code}' "$url/releases/3/rootfs.itb")" = 404 ]
# The server must reject directory symlinks, not just file symlinks.
ln -s "$scratch/source" "$scratch/feed/releases/4"
[ "$(http -o /dev/null -w '%{http_code}' "$url/releases/4/kernel.itb")" = 404 ]
cp "$scratch/feed/releases/1/manifest" "$scratch/feed/latest"
expect_fail agent --once
cmp "$scratch/cache/current" "$scratch/previous"
printf 'invalid manifest\n' > "$scratch/feed/latest"
expect_fail agent --once
cmp "$scratch/cache/current" "$scratch/previous"
kill "$server_pid"
wait "$server_pid"
server_pid=
expect_fail agent --once
cmp "$scratch/cache/current" "$scratch/previous"
expect_fail "$src/device-server" --root "$scratch/feed" --port 70000
expect_fail "$src/fwctld" --server "$url" --cache "$scratch/cache" \
    --product ti-am62x --layout am62x-sd-ab-v1 --interval 0
printf 'PASS: publication, live polling, download integrity, cache preservation, locks and HTTP boundaries\n'
