#!/bin/sh
# Transport smoke test against an existing Buildroot FIT release; never flashes.
set -eu
src=$(CDPATH= cd -- "$(dirname -- "$0")/.." && pwd)
release=${1:?Usage: test-real-release.sh PATH_TO_IMAGES_RELEASE}
PATH="$src/../../output/am625/host/bin:$PATH"
export PATH
scratch=$(mktemp -d)
pid=
cleanup()
{
    [ -z "$pid" ] || { kill "$pid" 2>/dev/null || :; wait "$pid" 2>/dev/null || :; }
    rm -rf "$scratch"
}
trap cleanup EXIT
trap 'exit 1' INT TERM
"$src/device-server" publish --root "$scratch/feed" --source "$release" \
    --version 1
"$src/device-server" --root "$scratch/feed" --port 0 > "$scratch/server.log" 2>&1 &
pid=$!
i=0
while ! test -s "$scratch/server.log"; do
    i=$((i + 1))
    [ "$i" -lt 6 ] || exit 1
    sleep 1
done
url=$(sed -n 's|Device Manager: \(http://[^/]*/\).*|\1|p' "$scratch/server.log")
"$src/fwctld" --server "$url" --cache "$scratch/cache" \
    --hwid 1010001000110000 --once
for file in release.itb kernel.itb rootfs.itb; do
    cmp "$release/$file" "$scratch/cache/1010001000110000/1/$file"
done
printf 'PASS: actual AM62x release published, downloaded and byte-compared (no flashing)\n'
