# HTTP admission and scheduling boundaries

Device Manager collects incomplete request headers using a bounded poll loop.
One silent or slowly fragmented header no longer blocks other complete requests.
There are at most 32 pending connections and 16 per IPv4 peer, with an 8191-byte
header limit and a 15-second deadline measured from acceptance. Progress does not
extend that deadline. Excess admissions get a best-effort 503/Retry-After; expired
headers get 408. Error responses never wait for the peer to read them.

The shared transfer component consumes at most 2048 header bytes per step. It
stops exactly at CRLFCRLF, leaving binary upload bodies on the socket. Parsing
continues to reject ambiguous lengths and transfer encodings. Listener admission
is capped at eight accepts per turn and clients receive rotating service order.
SIGTERM closes pending clients without waiting for their header deadlines.
Completed inline responses half-close their write side and drain at most 64 KiB
for at most one second, using the same nonblocking loop. This lets a small error
response reach a client that has already started sending a rejected body, instead
of immediately resetting the socket. Deferred transfers retain socket ownership
outside the admission pool and are not half-closed by that pool.

Route handlers and database changes remain serialized. This
avoids introducing concurrent catalog/publication/quarantine mutations before
their transaction ownership is redesigned. A slow request body, a blocked small
control response, storage/export preparation or an expensive handler can still block handlers and delay admission
deadline processing. This is not a complete request-priority or load-isolation
implementation, and the limits do not promise availability under all floods.
Several legitimate clients behind one NAT share the per-peer limit.

## Download workers (S5b)

File responses larger than 64 KiB run in at most four read-only worker processes
per server instance. Each worker retains only its socket and artifact descriptor;
it does not retain the listener, catalog descriptors or other client sockets.
HEAD and small metadata remain in the control path. A full worker pool returns
503. Every worker has a 180-second total lifetime plus the existing socket timeout;
client disconnect, parent death and service shutdown terminate killable workers.
Uninterruptible kernel IO can delay process exit; shutdown never waits indefinitely.
The four-worker bound is per running server instance, not across repeated server
restarts while orphaned tasks remain in kernel D state.

The parent reaps workers and updates download counts only after successful file
transmission. Counts may appear on the next event-loop iteration; they indicate
successful HTTP transmission, not device receipt, SHA validation or OTA success.
HEAD, invalid ranges, aborted transfers and killed workers do not count. Existing
Range semantics are preserved: a completed partial response counts as one transfer.
If the server itself crashes after transmission and before its completion update,
that statistic can be lost; it is not an exactly-once download receipt protocol.

Workers serving OTA publication artifacts are stopped when the parent observes
quarantine or cannot verify that policy. Checks use cached catalog payload hashes,
not repeated full-file hashing. Bytes already sent cannot be recalled. Policy
checks run after handlers and during normal loop iterations; a blocked handler
can still delay detection. Manual forensic package exports remain available under
their existing policy, independently of automatic OTA distribution.

## Upload reception workers (S5c)

Firmware archive reception uses at most two worker processes per server instance.
Each retains only its client socket and controlled temporary file, with a
180-second absolute lifetime and the existing 512 MiB compressed body limit.
Progress does not reset the lifetime. A full pool returns 503; the client may
retry later. This endpoint still requires a complete upload, not upload resumption.

The parent retains the response connection and exclusive ownership of archive
validation, draft registration and database changes. Only a successful receive
worker can enter validation; incomplete, disconnected or timed-out bodies are
removed without creating a draft. Fully received invalid archives return 422.
Only validated drafts return 201, and publication remains a separate operation.
Graceful shutdown terminates receivers and removes their partial files; parent
death kills killable children. Uninterruptible kernel IO and abrupt server death
can leave `.upload-*` directories requiring inspection and cleanup. The worker
limit is per running instance, not across orphaned kernel tasks after restarts.

This isolates slow network reception, not storage or validation work. Temporary
file creation/cleanup, decompression, FIT inspection and catalog updates remain
in the parent and can delay other requests. The two body limits do not reserve
disk capacity or establish a global storage quota. Existing decompressed-size
and archive-entry checks remain in force.

## Policy query identity reuse (S5d)

Device policy queries and publication artifact requests resolve their full
payload hashes through the persistent catalog. Existing failure history no longer
causes every request to extract FIT payloads and calculate their hashes again.
Quarantine markers remain live checks; creating a marker blocks the next request
even when identity data is cached. Missing or inconsistent identity fails closed.

The catalog checks device/inode, size and nanosecond mtime/ctime before reuse.
A changed or replaced file requires complete verification before its identity
can be refreshed. These metadata checks are cache invalidation, not continuous
media-integrity scrubbing or protection from privileged file/database tampering.
Publication and automatic fallback retain their full byte verification; clients
continue validating full SHA256 after download and on-device partition writes.
Tests count FIT tool invocations across repeated requests and server restart,
and verify live quarantine and corruption detection with preserved mtime.

Next phase: control-request priority,
global/per-device resource accounting, and explicit transactional ownership across
publication, quarantine, SOL deletion and archive receipt operations. Long-running
database/storage failures also require a defined deadline and cancellation policy.

Validation: `python3 ota/tests/test-admission.py` exercises idle headers, exact
binary-body boundaries, malformed input, fixed deadlines, limits, disconnect churn
and service stop. `test-download-workers.py` covers slow readers, worker limits,
exact bytes/Range, accounting, quarantine cancellation, descriptor isolation and
shutdown; the lifetime test accelerates the worker alarm in an isolated fixture.
`test-upload-workers.py` covers slow receivers, capacity rejection, exact valid
archive completion, malformed/truncated bodies, descriptor ownership, shutdown
and an accelerated lifetime deadline.
Existing transfer/OTA/SOL tests cover parser and mutation behavior.
