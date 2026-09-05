# Local automation API (version 1)

Every viewer exposes `POST http://127.0.0.1:<port>/rpc` on an OS-assigned port. CLI
and HTTP requests share the same command slot. Control commands execute before SDL
initialization, so they do not create a second window or renderer.

## Instance discovery

Use `woby ctl instances --json` to list live instances. Each result includes `id`, `pid`,
`url`, `apiVersion`, and `ready`. Discovery checks the authenticated endpoint and ignores
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
{"jsonrpc":"2.0","id":2,"result":{"instance":"review","path":"C:\\output\\view.png"}}
```

The result path reflects `.png` extension normalization. Parent directories are created;
existing files are overwritten. Output matches the UI screenshot: 1920 × 1800, current
camera, scene and helpers, no controls. Only one automation capture is outstanding at a
time. HTTP handlers never access UiState or GPU handles; the main thread takes the
request, uses the existing screenshot pipeline, and signals completion.

Internally, validated requests become typed `AutomationCommand` payloads. The main
thread takes them through `takeAutomationCommand` and reports a typed result or error
through `completeAutomationCommand`. Each command has an internal ID independent of
the client's JSON-RPC ID; stale or duplicate completions cannot finish another command.
The runtime currently allows one outstanding command, retaining its slot until started
work completes even if the HTTP deadline expires. Screenshot is the first command type.
Instance discovery remains a runtime-only query and does not wait for main-thread work.

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

IDs are independent of names, paths, vector indices, and visibility. They remain valid
through edits, tree reordering, saves, and removal of other objects. Removing an object
(including a folder pruned when its last file is removed) invalidates its ID. Reopening
or replacing a scene gives its loaded objects new IDs, including when reopening the
same `.woby` file. A newly added object never reuses a removed object's ID.

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
| -32002 | An automation command is already pending |
| -32003 | Command deadline expired |
| -32004 | Main-thread capture failed, including busy loading/UI capture or file-write errors |
| -32005 | Unknown or stale object ID |

A timeout cancels a request that has not started. GPU work already submitted may still
save its PNG. Disconnecting the HTTP client is not cancellation; set a suitable deadline
and avoid automatically retrying a timed-out capture with the same output path.

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

Version 1 exposes instance discovery, screenshot capture, object enumeration, and object
lookup. Scene editing, job/event APIs, and MCP integration can use the same runtime boundary.
