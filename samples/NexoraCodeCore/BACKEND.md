# Standalone Zeus backend

`backend_main.zeu` is the first standalone, UI-free NexoraCodeCore process. It
owns the local HTTP server, conversation store, Agent Loop tasks, and task
journal. A frontend can call its JSON API without loading Zeus modules itself.

## Build and run

From the Zeus2 repository root on Windows:

```powershell
./samples/NexoraCodeCore/build_ncode.bat
```

The script builds `dist/NexoraCodeCoreBackend.exe` and `dist/ncode.exe`. On the
first build it copies an existing root `ncode.config.json`, or otherwise
`samples/NexoraCodeCore/ncode.config.template`, beside the CLI as the local,
ignored `dist/ncode.config.json`. Later builds keep that file unchanged so path
edits survive. The CLI locates it from its own exe path, so it works when
launched from another current directory.

`dist/ncode.config.json` contains `backend_url`, `backend_executable`, and
`backend_working_directory`. The CLI checks `/health`; if the backend is not
already running, it starts the configured executable with the configured working
directory and waits for readiness. The default working directory points at the
existing NexoraCode checkout, so the backend reads its original
`data/providers.json` directly and does not duplicate provider credentials.
Conversations and task journals are stored in that checkout's
`data/nexoracode_core/` directory. Keep the backend executable path and working
directory aligned with the local checkout.

The server binds to `127.0.0.1:8765` by default. It is intended for a local
frontend process; authentication and remote binding are not part of this first
slice.

## JSON API

| Method | Path | Purpose |
| --- | --- | --- |
| GET | `/health` | Backend readiness |
| GET | `/api/models` | Public provider names and model limits |
| GET | `/api/conversations` | List conversations |
| POST | `/api/conversations` | Create a conversation with optional `title` |
| GET | `/api/conversations/get?conversation_id=...` | Read one conversation |
| POST | `/api/conversations/settings` | Set `permission_mode` for a conversation |
| GET/POST | `/api/local/permissions` | Read or save global defaults or conversation permissions |
| GET | `/api/local/permissions/auto-approval-events?limit=50` | Read recent automatic approval decisions |
| POST | `/api/chat` | Start a chat task |
| GET | `/api/tasks` | List tasks, or read one task with `stream_id` and optional `after_seq` |
| POST | `/api/tasks/cancel` | Request cancellation with `stream_id` |
| POST | `/api/tasks/approval` | Approve or deny a pending tool call |

Start a chat with a JSON body such as:

```json
{"message":"Find the cached file","provider":"USST"}
```

The response returns `stream_id`, `conversation_id`, and the conversation's
permission mode. Poll
`/api/tasks?stream_id=...&after_seq=0` for task metadata and sequenced events.
The `after_seq` cursor reports a gap if the requested events have left the
bounded in-memory window. When `gap` is true, the response also includes the
accumulated assistant text as `content` so a client can catch up. Completed
events are also written to the task journal.

`/api/models` intentionally omits API keys and provider endpoints. The chat
worker uses a tool registry with schema validation and supports workspace-scoped
file reading, directory listing, bounded tree search, text file writing, short
local shell commands, and persistent terminal sessions. Read operations run
directly. Writes and commands default to `confirm`, pause the current task, and
publish a `permission_required` event containing an opaque approval id and a
short summary. Command approvals show the command and working directory. Tool
arguments stay in the backend.
Resolve the request with `POST /api/tasks/approval` and `{ "stream_id": "...",
"approval_id": "...", "decision": "approve" }` or `"deny"`; the response
returns the new continuation stream id. Approval ids are bound to their task,
expire after 30 minutes, and allow idempotent retries.

Set per-conversation tool behavior with
`POST /api/conversations/settings` and `{ "conversation_id": "...",
"permission_mode": "read_only|confirm|full" }`. New conversations use
the current global permission defaults. `GET /api/local/permissions` returns
the global mode and configured approval-model choices; pass `conversation_id`
to read one conversation's settings. `POST /api/local/permissions` saves either
the global default or one conversation's `mode` and `approval_model_id`.
Supported modes are `read_only`, `confirm`, `full`, and `auto`; `auto` requires
a configured approval model. In `auto`, write, command, and network operations
are checked by the configured `provider-id/model` at temperature 0 with no
tools enabled; request errors, cancellation, tool calls, and unparseable output
deny the operation. The model must return `1,reason` or `0,reason`; an invalid
format gets one strict-format retry. Read operations remain available in every
mode, and unknown operation kinds are denied. Automatic decisions are recorded
to the local JSONL audit file and can be read through the events route. New
conversations snapshot the global defaults, so later global changes do not
rewrite existing conversations. File tools block sensitive paths and paths outside the configured
workspace. Shell commands use `cmd.exe`; `cwd` defaults to the workspace and
may name another existing directory. Their timeout is 1 to 300 seconds, with a
30-second default.

`local_terminal` supports `run`, `read`, and `terminate`. `run` waits 10 seconds
by default, then returns a session id and up to 8,000 characters of output.
Later `read` calls return the next output slice. `terminate` stops the Windows
process tree. Terminal reads are allowed directly; starting or terminating a
session follows the conversation's command permission mode. Terminal standard
input is not supported. Captured output stays in a temporary file until the
process exits and all captured output has been read; the active capture size is
not yet bounded by a ring buffer. Browser tools, plugin hosting, and event
streaming over the HTTP response remain to be migrated; the current task
transport uses polling.

## Terminal client

The terminal client is written in Zeus and compiled into one Windows console
executable. It uses Zeus's built-in HTTP and process APIs and has no Python
runtime dependency.

Build both executables from the Zeus2 repository root:

    ./samples/NexoraCodeCore/build_ncode.bat

Run the client from any working directory:

    ./dist/ncode.exe

The client starts the backend automatically when needed, selects the first
configured provider at startup, and supports `/help`, `/backend`, `/models`,
`/model NAME`, `/permissions [read_only|confirm|full]`, `/ctx`, `/history`,
`/open ID`, `/new [title]`, and `/quit`. When a write needs confirmation, the
CLI shows its summary and asks `[y/N]`; only `y` or `yes` approves it.

The Agent Loop backend exposes `local_file_read`, `local_file_list`,
`local_file_search_tree`, `local_file_write`, `local_shell_exec`, and
`local_terminal` to the configured provider.

The provider receives a stable NexoraCode system prompt with the configured
workspace path and the backend's actual tool and permission boundaries. At each
chat start and approval continuation, the backend reads the first non-empty
file in this order: `AGENTS.md`, `NEXORA.md`, `.nexora/AGENTS.md`. It resolves
the candidate before reading and skips paths that resolve outside the workspace.
The selected file is limited to 8,000 Unicode characters. Its first version is
stored as a project-conventions system message; later edits are stored as
line-level changes, and unchanged content is not repeated. Injection state and
messages are committed with the conversation. `/ctx` estimates the same stable
system prompt and any pending project-instruction change.

The backend loads NexoraCode local settings from `data/config.json` in its
working directory. `/api/local/settings` exposes and updates the general
settings for username, custom prompt, project-instruction reading, tool
auto-expand, telemetry, theme, and the two retry limits. Writes are atomic and
preserve unrelated keys. Its provider section exposes the configured provider
fields and a `has_api_key` flag without returning `api_key`. Provider updates
are written atomically to `data/providers.json`; an existing key is retained
only when its provider ID and endpoint still match, and a key source can be
copied only to the same endpoint or preset. Presets use backend-owned endpoint
values. Invalid templates and context budgets are rejected before saving.
`GET /api/local/provider-templates` returns the bundled preset list.
`POST /api/local/provider-models` discovers models for a custom or preset
provider. Presets use their bundled catalog URL; custom providers derive
`/models` from the Chat Completions endpoint. HTTPS is required except for
custom loopback HTTP endpoints. Requests do not follow redirects, use a
20-second timeout, and keep API keys in the request header. Catalogs are
normalized to unique, sorted `{id, name}` rows; OpenRouter modality and
OpenCode Chat Completions filters are applied before returning them.
Custom text is capped at 8,000 Unicode characters when saved and trimmed for
prompt injection. Retry limits accept integers from 0 to 10. Invalid UTF-8 in
project instruction files is replaced with `�`, matching the Python backend's
reader.

Provider stream failures retry up to `auto_retry_count` times (default 5) with
250/500/1,000 ms bounded backoff. Partial visible text is sent as continuation
context, and the output allowance is reduced by the added input estimate. An
empty final response retries up to `empty_response_retry_count` times (default
2), except when the provider reports `finish_reason: tool_calls` without a
usable tool call. Retry instructions are request-only and do not enter stored
conversation history.
