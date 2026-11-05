# Shared transfer component

`transfer/` is an in-process C library shared by OTA, firmware recovery, SOL and
clock HTTP clients. It adds no executable or daemon. Its public types do not
contain publication numbers, hardware IDs, boot IDs or firmware states.

## Build and integration

- `make -C transfer all` builds `libdm-transfer.a` (client) and
  `libdm-transfer-server.a` (HTTP server primitives).
- `make -C transfer test` runs real HTTP, socket, process-interruption and file
  tests. Pass dependency paths through `CPPFLAGS` and `LDFLAGS`.
- Consumers include `consumer.mk`, add `DM_TRANSFER_INCLUDE` and depend on the
  appropriate archive. Keep host and cross builds in separate source/build trees.
- Client links require libcurl and pthread; file verification additionally needs
  OpenSSL. Server file hashing needs OpenSSL, without a libcurl dependency.

## Request API

Always initialize `dm_transfer_request` with `dm_transfer_request_init()`;
unused file descriptors must be `-1`. `dm_transfer_perform()` supports bounded
memory responses, file/custom sinks, streaming file POST/PUT and HEAD/GET.
PUT requires an input descriptor. Supply immutable strings/buffers until the
synchronous call returns. Each concurrent call owns its request/result.

HTTP/HTTPS are enabled; TFTP is opt-in for firmware sources. Redirect following
is opt-in and HTTPS redirects cannot downgrade to HTTP. No environment proxy is
used on the device management network. HTTPS uses libcurl's certificate checks;
provision its trust store when using HTTPS. Timeouts, low-speed bounds, rate limits,
progress and cancellation are explicit options. Callbacks must not initiate a
nested request on the same object. Signal handlers only set cancellation flags.

The result separates transport/I/O errors from HTTP status. A successful exchange
can return HTTP 404/409; **the business caller decides which statuses are valid**.
POST is never replayed automatically: a lost response may follow a remote commit.
Do not call libcurl global cleanup while any consumer is running.

## Durable file downloads

`dm_transfer_download_file()` promotes only nonempty, fully received, verified
and synced files. A resumable download requires expected length and full SHA256.
The caller owns task directory selection, free-space/retention policy and locking.

`name.part` stores received bytes. `name.part.state` stores the expected identity,
length, durable offset and prefix SHA256, protected by a format/CRC check. Every
1 MiB or one second, received data is synced before atomically saving and syncing
the checkpoint. On restart, the prefix is rehashed and only a tail beyond that
checkpoint is discarded. A corrupt checkpoint/prefix is preserved and reported
as a storage failure. An identity change starts a fresh download.

Resume requires matching HTTP 206 Content-Range. If a server ignores Range and
returns 200, one safe full restart is allowed. A complete SHA mismatch is an
integrity failure and is never promoted. Verified files are fdatasynced and
renamed, then their directory is synced; the checkpoint is removed last.
File GET retries are opt-in and bounded; state-changing requests are not retried.
The default is one attempt so existing daemon retry policies remain in control.
Power-loss persistence requires a durable writable filesystem and functioning
storage flushes; RAM caches and warm-reset tests cannot establish that guarantee.

## Append upload and HTTP server

`dm_transfer_push_file()` asks an application callback for a committed cursor,
uses `pread()` for bounded batches and validates every acknowledgement. It supports
resume, bounded cursor correction, cancellation and a per-cycle batch budget.
The application still owns authentication, stream generation and final commit.
SOL clears its startup RAM banks **only after the existing archive commit ACK**;
a chunk ACK alone never clears them. The component does not alter serial bytes.

`transfer-server.h` provides bounded HTTP parsing, duplicate-header detection,
exact/deadline-limited body reads, synced file reception, responses and streaming
HEAD/Range file downloads. Routers retain origin/access checks and business
validation. Archive format, SQLite updates and OTA policy remain outside this
library. Log export data returned as JSON continues through the common response
writer; it is not turned into a firmware file protocol.

U-Boot uses its native network stack and is outside this POSIX/libcurl library.
Its recovery endpoints and firmware identity checks remain the same. Initramfs
links the client into fwctl and includes the resulting ELF dependency closure;
it needs neither the curl executable nor another transfer service.
