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

Route handlers and database changes remain serialized in this first phase. This
avoids introducing concurrent catalog/publication/quarantine mutations before
their transaction ownership is redesigned. A slow request body, slow download,
storage stall or expensive handler can still block handlers and delay admission
deadline processing. This is not a complete request-priority or load-isolation
implementation, and the limits do not promise availability under all floods.
Several legitimate clients behind one NAT share the per-peer limit.

Next phase: bounded body staging and download execution, control-request priority,
global/per-device resource accounting, and explicit transactional ownership across
publication, quarantine, SOL deletion and archive receipt operations. Long-running
database/storage failures also require a defined deadline and cancellation policy.

Validation: `python3 ota/tests/test-admission.py` exercises idle headers, exact
binary-body boundaries, malformed input, fixed deadlines, limits, disconnect churn
and service stop. Existing transfer/OTA/SOL tests cover parser and mutation behavior.
