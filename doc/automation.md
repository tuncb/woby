# Local automation API (version 1)

Every viewer exposes `POST http://127.0.0.1:<port>/rpc` on an OS-assigned port. CLI
and HTTP requests share the same bounded FIFO command queue. Control commands execute before SDL
initialization, so they do not create a second window or renderer.

## Instance discovery

For all supported CLI commands, RPC names, parameters, and target scopes, see the
[CTL command reference](ctl-commands.md). This includes scene inspection, property
editing, camera navigation, model/importer management, diagnostics, and pane controls.

Use `woby ctl instances --json` to list live instances. Each result includes `id`, `pid`,
`url`, `apiVersion`, `ready`, `queuedCommands`, and `activeSequence` (a string, or null).
The queued count excludes the active command. Discovery checks the authenticated endpoint and ignores
stale records, including records left by a crashed process. An instance may be listed
as `ready: false` while startup loads models. Wait until ready before requesting capture.

Per-user discovery records live in:

- Windows: `%LOCALAPPDATA%\woby\instances`
- Linux/macOS with `XDG_RUNTIME_DIR`: `$XDG_RUNTIME_DIR/woby/instances`
- Linux fallback: `$HOME/.local/state/woby/instances`
- macOS fallback: `$HOME/Library/Application Support/woby/instances`

`instance-<id>.json` contains `version`, `id`, `pid`, `port`, and a random `token`.
These records are in a directory restricted to the current user. Treat the token as a
credential; the CLI does not print it. The process holds an OS lock on `instance-<id>.lock`
for its lifetime, which prevents duplicate names even during startup. The OS releases
the lock after a crash. Empty lock files are intentionally retained; registry JSON is
removed on normal shutdown and replaced when the same ID starts again.

IDs are case-sensitive lowercase ASCII names, limited to 64 characters, with letters,
digits, `-`, and `_`; the first character must be a letter or digit. Automatically generated
IDs have the form `woby-<16 random hex digits>`. The ID is runtime metadata and does not
change when opening or saving a scene.

## HTTP requests

Send `Content-Type: application/json` and `Authorization: Bearer <token>`. Use
`127.0.0.1` in the URL. The server validates the Host header and rejects requests with
an Origin header. Browser/CORS access and remote network access are not supported.
The request body is limited to 16 KiB. Tokens change on every launch, including when
reusing a custom ID.

This API supports individual JSON-RPC 2.0 requests with an `id` and named parameters.
Batch requests are unsupported. Notifications (requests without an `id`) return HTTP 204
and perform no work. Every executed command has an acknowledged result or error.

### Inspect an instance

```json
{"jsonrpc":"2.0","id":1,"method":"instance.info","params":{}}
```

The result has the same public metadata as CLI discovery. It never includes the token.

### Capture the scene

```json
{"jsonrpc":"2.0","id":2,"method":"screenshot.capture","params":{"path":"C:\\output\\view.png","timeoutSeconds":60}}
```

`path` must be an absolute filename on the machine running Woby. `timeoutSeconds` is an
optional integer from 1 to 3600 (default 60). Unknown parameters are rejected. The HTTP
response waits for GPU readback and PNG writing, then returns:

```json
{"jsonrpc":"2.0","id":2,"result":{"instance":"review","path":"C:\\output\\view.png","sequence":"2","commandId":"cmd-0123456789abcdef0123456789abcdef-2","state":"succeeded"}}
```

The result path reflects `.png` extension normalization. Parent directories are created;
existing files are overwritten. Output matches the UI screenshot: 1920 × 1800, current
camera, scene and helpers, no controls. Only one automation command executes at a
time. HTTP handlers never access UiState or GPU handles; the main thread takes the
request, uses the existing screenshot pipeline, and signals completion.

Internally, validated requests become typed `AutomationCommand` payloads. The main
thread takes them through `takeAutomationCommand` and reports a typed result or error
through `completeAutomationCommand`. Each command has an internal ID independent of
the client's JSON-RPC ID; stale or duplicate completions cannot finish another command.
The runtime retains the active command until started work completes even if the HTTP
deadline expires. Subsequent commands stay queued until that completion.
Instance discovery remains a runtime-only query and does not wait for main-thread work.

### Command ordering and concurrency

Every successful scene command returns a `sequence`, `commandId`, and `state` alongside
its result. Sequences are decimal strings assigned in increasing order when commands are admitted to the
queue, independently of the client's JSON-RPC `id`. Strings preserve all 64 bits in
clients that represent JSON numbers as floating point.

Commands execute in FIFO admission order, with at most eight admitted commands total
(active plus queued). A full queue returns `-32002`. Admission order across concurrent
connections is defined by the returned sequence, not by client send time or response
arrival time. Dependent scripts should wait for each response before submitting the
next command. The runtime leaves HTTP workers available for discovery while scene
requests wait.

HTTP handlers only validate and enqueue commands. Scene commands, UI input, and load
commits execute on the main thread. Query results own their data, and object IDs are
resolved afresh at execution. A screenshot holds the execution slot through GPU
readback and PNG writing, so subsequent commands cannot overtake it. Manual screenshot
requests also reject a capture if another is pending.

Automation assumes a single coordinating client and no manual changes during a
workflow. Multiple clients and UI edits can interleave safely, but the application
does not detect intervening changes or guarantee a consistent scene across separate
requests. Commands use the state present when they execute. A later query waits for
earlier admitted commands, but does not wait for unrelated background loads. Separate
requests are not an atomic batch.

Timeouts include queue waiting. Expired queued commands are removed without execution;
started commands retain the execution slot until completion. Later commands retain
their relative order when earlier commands expire or fail. Accepted-command timeouts
include their `sequence`, `commandId`, and execution `state` in error `data`; sequence
gaps are expected. Disconnecting a client is not cancellation. Viewer shutdown releases both active and queued clients.
CLI JSON errors expose the `error` message, RPC `code`, and available `data`.

Scene revision tracking has been removed. `scene.revision` is an unknown method
(`-32601`), `ifRevision` is an unknown parameter (`-32602`), and the CLI rejects
`revision` and `--if-revision`. Results no longer include a `revision` field.

For future mutating handlers, update logical state and bounds/dirty tracking on the
main thread before completing the command. Render code must continue to read
already-updated state.

### Recover commands and retry safely

All queued scene methods accept an optional `requestKey` (`--request-key KEY` in the
CLI). Keys contain 1-128 ASCII letters, digits, `-`, `_`, `.`, or `:`. Choose a unique
key for each intended operation and keep it before submitting the request.

```powershell
woby ctl --instance review screenshot C:\output\view.png --request-key capture-1 --timeout 60 --json
woby ctl --instance review command --request-key capture-1 --json
woby ctl --instance review command COMMAND_ID --json
```

The equivalent lookup requests are:

```json
{"jsonrpc":"2.0","id":5,"method":"command.get","params":{"requestKey":"capture-1"}}
{"jsonrpc":"2.0","id":6,"method":"command.get","params":{"id":"COMMAND_ID"}}
```

Replace `COMMAND_ID` with the opaque `commandId` from a response. Unlike `sequence`,
this ID includes a random launch identity; IDs from another viewer or a restarted
viewer cannot refer to new commands. Lookup takes exactly one selector, returns
immediately, and bypasses the scene queue. It consumes no admission slot or sequence.
It returns `instance`, `commandId`, `sequence`, optional `requestKey`, and `state`:

| State | Meaning |
| --- | --- |
| `queued` | Admitted but not started |
| `running` | Started; may complete after the HTTP deadline |
| `succeeded` | Finished; the original response result is in nested `result` |
| `failed` | Finished; the original structured error is in nested `error` |
| `expired-before-start` | Deadline expired without execution; original timeout error is in nested `error` |

A successful lookup has CLI exit code 0 even when the inspected command failed;
inspect `state` and nested `error`. Lookup failures use exit code 1 and the normal
structured CLI error format.

Retrying the same method and validated operation parameters with the same key never
admits a second command in that viewer launch. Finished requests replay their original
result or error with the retry's JSON-RPC `id`. Queued/running duplicates return
`-32008` immediately with command metadata; they do not hold another HTTP worker.
Use `command.get` to poll, or resubmit the identical keyed request later to recover its
original response. A conflicting method or operation parameter returns `-32006`.
Screenshot paths are compared after lexical normalization; different spellings that
refer to the same file are not otherwise guaranteed to match. Object queries replay
the original snapshot; use a new key or no key for a fresh query.

`timeoutSeconds` is excluded from command identity. Only the first admission sets the
execution deadline, including queue waiting; retries cannot extend it. A `-32003`
response with `state: running` is a wait timeout, not the command's terminal outcome.
The slot stays occupied until actual completion. `expired-before-start` is terminal
and replayed on retries; a caller wanting to try that operation again must use a new
key. The timeout message and eventual result both identify the same command.

Completed results are retained in memory in completion order, up to 128 records and
8 MiB of serialized responses combined. There is no time-based retention guarantee.
An oversized response is delivered to its original waiter but is not retained.
Active/queued records are retained independently. Evicted results return `-32009`,
which means the outcome is unavailable, **not** that no work occurred.

Used keys and their parameter fingerprints remain reserved for the entire viewer
launch, even after result eviction. This ledger accepts at most 1,024 distinct keys;
after that, new keyed commands return `-32010` before admission. Existing keys remain
queryable/replayable subject to result retention, and unkeyed commands still work.
Validation and admission failures do not reserve keys. Reusing an evicted key never
starts new work; reconcile the outcome before choosing a fresh key.

All history and key reservations disappear on viewer shutdown. The guarantee covers
one viewer launch, not crashes or restarts, and is not durable exactly-once execution.
An unknown key in a new launch says nothing about work performed by an earlier launch.
After a lost connection, inspect using the saved key in the original launch; after a
restart or unavailable result, reconcile the output before submitting another operation.
JSON-RPC `id` is only response correlation and does not deduplicate commands.

For future scene-editing methods, expose absolute setters (for example `visible=false`
and `opacity=0.5`) and return actual values after operation-boundary validation and
clamping. Complete edits only after updating bounds and dirty tracking. Relative
operations and external side effects still require a retry key to prevent duplicate
application. These are handler requirements for future property-editing methods.

### Discover and resolve scene objects

```powershell
woby ctl --instance review objects --json
woby ctl --instance review object OBJECT_ID --json
```

These synchronous queries also accept `--timeout SECONDS` (1-3600, default 60).
Without `--json`, their results are printed as indented JSON. The equivalent HTTP methods are:

```json
{"jsonrpc":"2.0","id":3,"method":"objects.list","params":{}}
{"jsonrpc":"2.0","id":4,"method":"object.get","params":{"id":"OBJECT_ID","timeoutSeconds":60}}
```

Replace `OBJECT_ID` with an opaque string returned by `objects.list`. Listing returns
`{"instance":"review","objects":[...]}`; lookup returns
`{"instance":"review","object":{...}}`. Every object has `id`, `kind` (`folder`,
`file`, or `group`), and `name`. Files also have `path`; groups have `fileId`, which
identifies their owning file. The list is a flat inventory, not a hierarchy or a list
of only visible objects. It includes loaded files and groups even if a saved scene's
tree does not reference them. Repeated tree references share the underlying file or
group ID and do not create duplicate inventory entries.

`object.get` additionally returns `settings`, relevant counts/importer/local-bounds
data, and an `occurrences` array containing effective visibility, opacity, and world
matrices for each hierarchy occurrence. `scene.tree` exposes the ordered hierarchy
with the same occurrence information. The flat `objects.list` inventory retains its
compact identity-only format. See [inspection details](ctl-commands.md#visibility-rendering-transforms-and-appearance).

IDs are independent of names, paths, vector indices, and visibility. They remain valid
through edits, tree reordering, saves, and removal of other objects. Removing an object
(including a folder pruned when its last file is removed) invalidates its ID. Reopening
or replacing a scene gives its loaded objects new IDs, including when reopening the
same `.woby` file. A newly added object never reuses a removed object's ID.
Scene Undo restores removed objects with their original IDs and comparison
references; an ID is unavailable while its object is absent. New/Open clears scene
history, so IDs from a replaced scene cannot be restored.

IDs are scoped to one viewer launch. Another viewer, or a restarted viewer using the
same instance name, rejects them. Treat IDs as opaque strings; do not derive or parse
them. They are metadata rather than user-editable scene properties, are not persisted
in `.woby` files, and do not affect dirty tracking. Both queries read the current
committed scene on the main thread; they do not wait for a background load to finish.
An object can disappear after listing, so clients must handle stale lookup errors.

Malformed IDs return `-32602`. Unknown, removed, replaced, and foreign-session IDs
return `-32005` rather than falling back to a name, path, or index.

Authentication failures use HTTP 401, rejected Host/Origin uses 403, incorrect content
type uses 415, and excessive payloads use 413. JSON-RPC results and errors use HTTP 200.
In addition to standard parse/request/method/parameter/internal errors:

| Code | Meaning |
| --- | --- |
| -32001 | Instance is starting or shutting down |
| -32002 | Automation queue is full (eight admitted commands) |
| -32003 | Command deadline expired |
| -32004 | Main-thread capture failed, including busy loading/UI capture or file-write errors |
| -32005 | Unknown or stale object ID |
| -32006 | Request key already belongs to different command parameters |
| -32007 | Unknown command ID or request key in this viewer launch |
| -32008 | Duplicate request is still queued/running; inspect with `command.get` |
| -32009 | Command result was evicted; outcome is unavailable and the key cannot execute again |
| -32010 | Session request-key capacity reached; no new command was admitted |

A timeout cancels a request that has not started. GPU work already submitted may still
save its PNG. Disconnecting the HTTP client is not cancellation. Use a request key and
command lookup to recover the outcome; repeating an unkeyed capture can write another PNG.

## PowerShell example

This calls the HTTP API directly; normal CLI use handles discovery and credentials automatically.

```powershell
$instance = Get-Content "$env:LOCALAPPDATA\woby\instances\instance-review.json" | ConvertFrom-Json
$request = @{
    jsonrpc = '2.0'
    id = 1
    method = 'screenshot.capture'
    params = @{ path = 'C:\output\view.png'; timeoutSeconds = 60 }
} | ConvertTo-Json -Depth 4
Invoke-RestMethod -Method Post -Uri "http://127.0.0.1:$($instance.port)/rpc" `
    -Headers @{ Authorization = "Bearer $($instance.token)" } `
    -ContentType 'application/json' -Body $request -TimeoutSec 65
```

Version 1 exposes instance discovery, screenshot capture, object enumeration and
detailed lookup, scene persistence/replacement, quit, scene/property/camera controls,
model and importer management, diagnostics, and pane settings. These commands share
FIFO ordering, retry keys, and command status/result recovery. Job/event APIs and
MCP integration remain future work.

## Scene persistence and shutdown

See the [scene lifecycle contract](scene-lifecycle.md) for the complete policy,
completion, failure, and object-ID rules. These commands are available through RPC
and CLI:

```powershell
woby ctl --instance review scene save-as C:\output\review.woby --request-key save-1 --json
woby ctl --instance review scene save --request-key save-2 --json
woby ctl --instance review scene open C:\scenes\next.woby --on-dirty save --request-key open-1 --json
woby ctl --instance review scene new --on-dirty discard --request-key new-1 --json
woby ctl --instance review quit --on-dirty save --save-path C:\output\last.woby --request-key quit-1 --json
```

Save-as and explicit `--save-path` destinations require `--overwrite` to replace
existing files. An ordinary save replaces the current file. Open/new/quit default to
an actionable dirty-scene error and never display confirmation dialogs for CTL.
All commands accept `--timeout`, `--wait`, `--request-key`, and `--json`.

Example RPC (all paths must be absolute):

```json
{"jsonrpc":"2.0","id":1,"method":"scene.open","params":{"path":"C:/scenes/next.woby","onDirty":"save","savePath":"C:/output/previous.woby","overwrite":true,"requestKey":"open-1"}}
```

Errors expose `data.reason`, the current `data.path` and `data.dirty`, plus command
metadata. CLI JSON preserves these fields. For example, `dirty_scene` (-32011) calls
for a save/discard policy, while `save_path_required` (-32012) calls for a save path.
A failed command is retained: use a new request key when correcting its parameters.
Quit success means `quitAccepted: true`; observe process exit separately. Lookup
history and request keys disappear when the viewer exits.
