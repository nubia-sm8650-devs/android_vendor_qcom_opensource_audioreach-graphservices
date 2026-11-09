# Native topology preparation

`libar-topology` applies userspace default calibration through the emitted
graph SET controls before preparing a PCM. Initialize ACDB with the shipped
database files before using this library.

Configure reference controls, PCM-side paths and the required initial
hardware gates before opening the PCM. `pcm_open()` completes hardware
parameters without PREPARE, leaving the tuning window open. Obtain native
subgraph IDs and SET control names from the compiler binding file's
`tuning` entries.

Call `ar_topology::prepare()` under the session's ownership lock with only
newly acquired, untuned graphs. Do not reapply defaults to graphs already
tuned and shared by another client. The function requires ALSA SETUP state,
queries subgraph defaults with empty calibration-key vectors, preflights
all controls and calibration data, sends native SET records, then calls
`pcm_prepare()`. No calibration data is stored in the kernel or topology.
The overload accepting an initialization callback runs it after default
SETs and before PREPARE. Use it for cached, tagged or custom parameters.
The original three-argument API remains available.

On error, close the failed PCM and release that session's acquired routes
and graph ownership. Preparation does not run after a tuning failure.
Earlier acknowledged SETs are not rolled back. Shared owners must remain
held during cleanup.

Persistent or global-persistent defaults return `-EOPNOTSUPP` before any
SET or PREPARE. They require registration and acknowledged memory lifetime,
not conversion to ordinary SETs. The library does not replace PAL/AGM
ownership policy, implement live switching or provide restart replay.

## Shared session ownership

Construct `ar_topology::Bindings` from the compiler's compact JSON output,
then construct one `ar_topology::Runtime` per card with its mixer and
bindings. Keep both objects alive for the runtime's lifetime and use the
runtime as the sole writer of its reference, path and hardware controls.
Compact files must include reference ownership metadata. Older files
without it are rejected rather than inferred from control names.

`Bindings::select()` composes complete graph-key vectors before checking
directional reachability. Device fragments need not own a frontend alone.
References do not add missing peers; their DAPM paths become eligible only
when both peers belong to the composition. Failed selection leaves the
output plan unchanged.

Call `Runtime::open()` with graph-key vectors and explicit Tinyalsa PCM
configurations for every selected frontend. Start with a zero session ID
and an empty stream vector. The runtime reserves frontend roots, shares
controls, opens all PCMs, applies defaults only to newly acquired graphs,
then prepares the PCMs. Its optional configuration callback sees all open
PCMs in SETUP after default tuning and before any PREPARE. Callbacks must
not reenter the runtime. Configure compatible backend media formats before
opening a session; the runtime does not choose or arbitrate those formats.

On success, the stream handles are borrowed for data operations until
`Runtime::close()`. Do not close them separately. Closing one session keeps
controls and devices used by another. A failed open leaves no session and
no stream handles. `Runtime::close()` stops and closes every PCM, releases the
session's controls and forgets the session even when a step fails, and returns
the first error. The session ID is invalid afterwards.

An admission that could expose untuned processing to an already prepared
frontend returns `-EOPNOTSUPP` before route changes. This is not live
switching support. PCM closure follows Tinyalsa's close interface, which
does not report DSP teardown failures. Successful userspace cleanup does
not establish DSP CLOSE acknowledgement or restart recovery. PAL/AGM still
uses the stock backend.
