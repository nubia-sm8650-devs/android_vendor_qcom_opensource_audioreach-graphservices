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

## Device startup

Native AGM and PAL startup share the names in `ar_topology_startup.h`. The
`ro.vendor.audio.native_topology.card` property names the ALSA card by its
mixer name, and `ro.vendor.audio.native_topology.alias` gives PAL the stock
card name it uses for its platform check and configuration file names. The
compiler's binding file is installed at
`/vendor/etc/audio/audioreach-native-bindings.json`.

`ar_topology::find_card()` returns the index of the only card with the given
mixer name. `ar_topology::load_bindings()` parses and validates the binding
file.

## Shared session ownership

Construct `ar_topology::Bindings` from the compiler's compact JSON output,
then construct one `ar_topology::Runtime` per card with its mixer and
bindings. Keep both objects alive for the runtime's lifetime and use the
runtime as the sole writer of its reference, path and hardware controls.
Compact files must include reference ownership metadata and module instance
IDs in every `tuning` entry's `modules` array. Regenerate older files that
lack this metadata; ownership is not inferred from control names.

Each frontend keeps its binding mode, `pcm`, `hostless` or `compress`.
A duplex hostless PCM appears as a playback and a capture frontend with the
same PCM ID and root.
`Bindings::select()` composes complete graph-key vectors before checking
directional reachability. Device fragments need not own a frontend alone.
References do not add missing peers; their DAPM paths become eligible only
when both peers belong to the composition. Failed selection leaves the
output plan unchanged.
`Bindings::frontends()` returns the frontends shared by every selection that
contains a partial key set, for callers that know their device keys only
later.

Call `Runtime::open()` with graph-key vectors and explicit Tinyalsa PCM
configurations for every selected frontend. Start with a zero session ID
and an empty stream vector. The runtime reserves frontend roots, shares
controls, opens all PCMs, applies defaults only to newly acquired graphs,
then prepares the PCMs. A compressed frontend must be the plan's only
frontend and carry its `snd_compr_params` in `Pcm::compress`.
The runtime opens its compress device for writing, or for reading when the
frontend captures, applies the defaults while the DSP graph is open, and sets
the parameters when preparing, which starts a playback graph. Starting does
nothing further. The client reads or writes, triggers and sets gapless
metadata on the descriptor `Runtime::compress_fd()` lends, and must
not use it after closing the session. A duplex PCM needs a playback and a capture configuration with
its ID and opens a stream for each, and calls that name only that PCM return
`-EOPNOTSUPP`. The optional configuration callback sees all open
PCMs in SETUP after default tuning and before any PREPARE. Callbacks must
not reenter the runtime. This API expects externally configured
backend formats. Use managed acquisition below to reserve and program explicit
formats through qualified backend bindings.

On success, the stream handles are borrowed for data operations until
`Runtime::close()`. Do not close them separately. Closing one session keeps
controls and devices used by another. A failed open leaves no session and
no stream handles. `Runtime::close()` stops and closes every PCM, releases the
session's controls and forgets the session even when a step fails, and returns
the first error. The session ID is invalid afterwards.

An admission that could expose untuned processing to an already prepared
frontend of another session returns `-EOPNOTSUPP` before route changes. PCM
closure follows Tinyalsa's close interface, which
does not report DSP teardown failures. Successful userspace cleanup does
not establish DSP CLOSE acknowledgement or restart recovery. PAL/AGM still
uses the stock backend.

## Rerouting an open session

`Runtime::reroute()` replaces the selections of an open session while its
PCMs stay open and keep running. The new selections must resolve to the same
frontends, otherwise it returns `-EXDEV`. It releases the controls only the old
selections used, enables the references and paths the new ones add, sends
default tuning to the graphs that are new to the session and calls the
optional configuration callback with a writer scoped to the new selections,
then enables the added hardware gates, which
starts the new graphs of a prepared PCM. Pass managed backend media for
sessions acquired with it. A backend port whose device graph stays in the
session keeps its media, and a different format for it returns `-EBUSY`. A
port that only changes its device graph takes the new media.

A failed reroute leaves the session on the new selections with the completed
part of the route enabled. Call it again with the same selections to finish,
or close the session.

## Separate lifecycle phases

Use `Runtime::acquire()` to open the selected PCMs and apply first-use
defaults while leaving them in SETUP. Cached, tagged and custom parameters
can then be sent without another default application overwriting them.
`Runtime::prepare()` runs the optional configuration callback before the
first PCM PREPARE. A successful callback is not repeated when retrying a
partial PCM preparation failure. Preparation failure retains the session
for retry or close. `Runtime::open()` remains the acquire-and-prepare
convenience API, with cleanup on preparation failure.

`Runtime::start()` requires prepared PCMs and starts their data paths.
Playback clients must supply data before starting. Hostless PCMs need no
data, since the runtime raises their stop threshold to the ALSA boundary
before starting them. A partial multi-PCM start failure keeps the started
PCMs running. Retrying starts only the remaining prepared PCMs, without
assuming rollback. `Runtime::stop()` drops the PCMs but retains graph and
route ownership. Prepare again before restarting, without reapplying
defaults to shared graphs. Borrowed handles remain valid until close.

These phases require the matching native kernel lifecycle implementation.
PCM PREPARE prepares host and processing graphs without START. PCM START
starts them after the backend, and STOP releases that client's starts
before the backend stops. Shared graphs remain running for other clients.

`Runtime::stop()` drops every PCM of the session and returns the first error.
It does not block later calls.

Forced teardown and restart recovery still need separate handling. The AGM
parameter adapter uses these preparation phases without selecting a native
backend. SUSPEND, property-scoped operations and live switching remain
unconnected.

## Initialization parameters

After `Runtime::acquire()`, use `Runtime::prepare_parameters()` to receive a
borrowed `Parameters&` in the initialization callback. Defaults have already
been applied and no PCM PREPARE runs until the callback succeeds. Use the
writer directly inside the callback, not the owner-locked runtime setters.
Do not retain it or reenter the runtime. Choose either this callback or the
original `Runtime::prepare()` callback for the session's first preparation.

`Parameters::set()` routes native module parameter records through the
selected graphs' SET controls. It validates the entire batch and preflights
all required controls before writing. Contiguous records for one graph may
share a write, but records are never reordered across graphs. The optional
`applied` byte cursor advances only after an acknowledged write. Retain the
same bytes and cursor when retrying initialization after partial failure.
Earlier acknowledged writes are not rolled back.

`Parameters::resolve_tag()` uses `ACDB_CMD_GET_MODULE_TAG_DATA`. A nonempty
GKV must exactly match an opened selection; an empty GKV scopes the request
to every current member. Returned records must target modules in that
scope. Resolution and SET are separate so an initialization queue can retain
resolved data across a failed SET instead of querying it again.

Outside the callback, use `Runtime::set_parameters()` and
`Runtime::set_tagged_parameters()` for synchronous updates under the owner
lock. A failed batch may leave an acknowledged prefix applied. These APIs
do not register persistent data. VCPM records, instance 4, go to the graph
of the session's frontend, and return `-EOPNOTSUPP` for sessions with
frontends on several graphs. Instance 3 returns `-EOPNOTSUPP`.

`Runtime::get_parameters()` queries one parameter record of a module the
session owns. Pass the record header followed by room for the payload. It is
filled in place. The query goes through the `get_control` named in the
graph's bindings and returns `-EOPNOTSUPP` when the bindings name none, and
`-EIO` when the module reports an error.

## Calibration keys

`Runtime::acquire()` takes optional calibration keys for each requested
selection. Newly acquired graphs receive ACDB calibration for the keys of the
first selection that reaches them, and the session keeps each selection's
keys. `Runtime::set_calibration()` sends the calibration that differs between
a selection's kept keys and new ones to every graph of that selection, or of
the first selection when the graph keys are empty. New keys must have as many
keys as the kept ones and are kept only after they are sent, as stock
`gsl_set_cal()` does.

## Managed backend media

Use the `Runtime::acquire()` overload accepting `HardwareConfig` records to
configure hardware before routes activate or frontend hardware setup runs.
Each record names a complete opened GKV selection and supplies its rate,
channel count, native data-format identity and ALSA sample-format name.
An optional `HardwareConfig::backend` anchors the request to an exact native
provider and DAI identity instead of accepting any port in that selection.
A record without one needs a selection with one physical port. A selection
with several ports, such as a loopback's playback and capture devices, takes
one record naming each backend. Every selected hardware gate must have a
qualified backend binding and a supplied configuration.
No frontend format, device alias or topology data-format default is inferred.

The runtime preflights all enum values before writing, shares compatible
physical-port configurations and rejects conflicting requests until every
owner closes. Stopped sessions retain ownership. Managed and
externally configured hardware sessions cannot coexist on the same runtime.
The caller remains responsible for supplying the device's intended format;
this API does not discover devices or implement live reconfiguration.

Control writes stage backend configuration without sending DSP commands.
An error prevents PCM admission, but already staged enum writes are not
rolled back. A later admission preflights the controls again.

Bindings may provide a read-only `channel_map` control instead of a writable
`channels` enum. Admission requires its live channel count to match the
requested media and retains the ordered codec slots with the session. Shared
admission and PREPARE/START reject a changed map with `-ESTALE`; close and
reacquire before using the new map. The kernel backend retains map programming
ownership. Userspace does not replace slots or turn this check into an atomic
map lease across ALSA calls.

`Bindings::aifs()` exposes explicitly declared physical AIF identities for
device discovery. Repeated hardware gates retain their memberships while
sharing one canonical AIF entry. Conflicting IDs, names or native ports are
rejected; absent declarations do not generate stock aliases.

## PCM events and data buffer descriptor

Use `Runtime::register_events()` with one native module-event registration
record and the owning PCM ID. The module must belong to the opened session.
The overload without a PCM ID resolves the module's directional frontend
owner from the selected DAPM paths. It requires exactly one owner and rejects
shared-module ambiguity instead of choosing a frontend by occurrence order.
Use `Runtime::read_event()` to consume one queued native module-event payload.
An empty queue returns `-EAGAIN`; queue overflow returns `-EOVERFLOW` without
replacing the caller's output.

`Runtime::subscribe_events()` returns an owned nonblocking CLOEXEC ALSA
control descriptor after checking frontend ownership. Watch it for `POLLIN`
and drain module payloads with `Runtime::read_event()` until `-EAGAIN`, also
draining once before the first wait. Card-wide notifications may be unrelated
to this frontend. Use `consume_pcm_notifications()` to consume notification
records without replacing the queued module payload read. A lost-event error
does not discard the remaining module queue. Close the subscription before
closing its session. These APIs do not start a listener or invoke callbacks.

`Runtime::export_buffer()` returns an owned CLOEXEC descriptor that only maps
the data buffer of an open PCM, from `SNDRV_PCM_IOCTL_ANONYMOUS_DUP`, and the
buffer size in bytes. Initialize the descriptor output to `-1` and close it
when no longer needed. The descriptor and every mapping made through it keep
the kernel stream open, so the PCM stays busy after session close until they
are released. Stream position comes from `Runtime::sync_pcm()`.

`Runtime::sync_pcm()` passes ALSA `snd_pcm_sync_ptr` flags and application
pointer/availability controls to the owned PCM. Use `SNDRV_PCM_SYNC_PTR_HWSYNC`
for a hardware-position refresh, and the ALSA APPL/AVAIL_MIN flags to choose
which controls to read rather than update. Successful replies contain the
kernel PCM state, hardware pointer and timestamp. Inspect that state for XRUN
or suspension. An ioctl error is returned without replacing the caller's
snapshot. No automatic prepare, start or restart occurs.

`Runtime::start()` issues ALSA START without Tinyalsa's cached MMAP control
commit, preserving application pointers committed through `Runtime::sync_pcm()`.

`Runtime::pcm_poll_fd()` returns the borrowed PCM descriptor for data readiness,
not the card-wide module subscription. Poll outside runtime/control locks and
serialize use with session close. Do not close the borrowed descriptor.
`PCM_NOIRQ` disables period wakeups; returning its descriptor does not create
a wakeup source for that mode. These local APIs do not export DSP position
memory. AGM and PAL provide separate NOIRQ MMAP integration; public backend
selection and device qualification remain incomplete.

These operations require a PCM in the session's bindings, derive its
direction from those bindings and reject pending STOP/close cleanup. They
address ALSA controls by full PCM identity, not by name or numeric-ID cache.
The lower-level functions in `ar_topology_pcm_controls.h` provide the same
transport without session ownership policy.
