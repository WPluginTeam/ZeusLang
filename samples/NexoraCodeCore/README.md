# NexoraCodeCore sample

This sample is the isolated migration workspace for NexoraCode's backend. The
initial Zeus slice covers remote-control WSS, RPC envelopes, and task lifecycle
forwarding: device authentication, heartbeat, RPC frame parsing, result
correlation, bounded lifecycle-event queuing, and the WebSocket close handshake.

The standalone UI-free HTTP backend is now in `backend_main.zeu`; see
[`BACKEND.md`](BACKEND.md) for its process setup and API contract.

`reference/` contains byte-for-byte copies of the Python remote-connection and
task-event source used as the migration baseline. The sample runner does not
import that code. Runtime data, credentials, provider configuration, and logs
stay outside this sample. The independent Zeus HTTP API and process entrypoint
are described in `BACKEND.md`. The lifecycle buffer is the
host callback boundary: it receives a host lock implementing `acquire` and
`release`; task-event sources call `enqueue`, and the socket loop calls
`nexora_core_flush_task_events`, which drains the queue under that lock and
writes the frames outside it. A rejected write propagates to the socket loop and
the unsent frames go away with the connection, which is what the baseline's
per-run list does. It accepts only `task_started` and `task_finished`, retaining
the newest 64 frames, and reads absent payload fields as empty strings the way
the baseline's `dict.get()` did. Task-stream replay remains a separate slice
because it needs host session-state and cursor interfaces.

`connection.zeu` carries the long-lived session: `NexoraCoreClient` opens the
connection, authenticates, sends a heartbeat on `heartbeatMillis`, flushes the
lifecycle buffer each pass, and reads with `receiveMillis` as a soft timeout, so
a quiet peer never blocks the loop. A failed session closes only when the client
is stopping, waits `backoffMillis`, doubles it up to `backoffCapMillis`, and
retries; `requestStop`/`join` shut it down within one receive window. The frame
hooks (`NexoraCoreHandlers`) are the seam the RPC dispatch and task-stream
slices fill. `connectFn` lets a test substitute the connection factory, which is
why the whole state machine is covered without sockets.

`rpc.zeu` turns that seam into the remote-control RPC path. `NexoraCoreRpcHandler`
takes a host `localApi` with one method - `dispatch(method, path, query, body)
-> {"status": int, "body": ...}` - and answers every `rpc` frame with an
`rpc_result` that echoes the request id. The whitelist check stays in the sample:
an unlisted method/path returns 403 without touching the host, a host exception
returns 500, and a host answer that is not a status-carrying dictionary returns
502, matching the baseline's three cases. The handler receives the connection
alongside the frame, because that is what it needs to send the reply.

`task_stream.zeu` covers the task relay. `NexoraCoreTaskStreamHandler` takes a
host `taskStream` with `getMeta(stream_id)` and `iterChunks(stream_id, fromSeq)`
and reproduces the baseline frame sequence: `task_stream_error` when the stream
is unknown, a leading `task_state` while the task runs, one `task_event` per
chunk, `task_stream_gap` (with `head_seq`) when the cursor missed entries, and a
closing `task_state`. Subscriptions are keyed by a 32-character lower-case hex
`subscription_id`; `task_unsubscribe` stops one, `stopAll` mirrors the
baseline's disconnect cleanup, and `pump` re-runs a pass from the cursor the
relay advanced to. The frame sequence is the contract; live tailing is
`pump` on whatever cadence the host wants.

`journal.zeu` is the host persistence boundary. `NexoraCoreTaskJournal` keeps one
task under `<root>/<stream>.json` (metadata, replaced atomically through a
temporary file and `rename`) plus `<stream>.jsonl` (one event per line, each
carrying `_stream_seq`). `saveMeta`/`append`/`read` mirror the baseline
`SessionJournal`: `read(after, limit)` returns the requested window and advances
`last_seq` to the highest sequence it saw, a task left `running` or `cancelling`
by a previous process is reported as `interrupted`, `recent` orders the task
list newest-first, and `purgeExpired(days, maxFiles)` drops expired or
overflowing logs while never touching a task this process still owns. Task ids
are validated as 32-character lower-case hex before they reach a path.
`NexoraCoreJournalTaskStream` adapts the journal to the two methods the task
stream already asks for, so the relay slice runs unchanged on top of real files.

`agent_events.zeu` is the first AgentLoop slice: pure event functions with no
IO, threads, or network. `nexora_group_tool_calls` groups provider
`tool_call` deltas by `index` (later `id`/`name` win, non-object arguments
fall back to `{}`), `nexora_translate_event` maps `AgentLoop` events to the
SSE frontend shape (`Routes._translate_event`, unknown types pass through by
copy), and `nexora_extract_usage_io`/`nexora_accumulate_usage` port the
provider usage accounting (`Provider._extract_usage_io`, OpenAI prompt-port
vs Anthropic input-port, cache-miss derivation). Provider streaming and tool
execution stay on the host; Zeus only normalizes the events.

`provider_config.zeu` reads NexoraCode's `data/providers.json` (endpoint,
model, window, timeout, bearer header) and builds `/chat/completions` URLs
with the same three rules as `ProviderClient._completions_url`.
`context.zeu` ports the no-compression subset of `ContextManager`:
`message_payload` normalization, `build_request_messages`, token estimate
(character-based heuristic; provider usage stays the calibration source),
`request_output_limit` arithmetic, anchor-based `measure_request_basis`,
and the `prepare` go/no-go threshold. Compression itself stays a later slice.

`chat_cli.zeu` + `chat_cli.py` is the CLI smoke test: one non-streaming
`POST` through `zeus.requests` with a real provider entry, printing the
model's reply. Usage: `python samples/NexoraCodeCore/chat_cli.py
--provider USST --question "..."`. Timeout seconds are rounded up with pure
integer arithmetic because AOT rejects fractional values at native `int`
positions (the VM rejects them too); see `__nexora_ceil_nonneg`.

`sse.zeu` + `stream_session.zeu` + `test_sse.zeu` cover server-sent events
over the new `zeus.requests.stream()` API: `ResponseStream.readLineSlice`
returns one de-framed line per call (chunked bodies are de-chunked
incrementally, unknown framing is rejected like the buffered path),
`nexora_sse_next` folds lines into `{type: event}` / `{type: done}` frames,
and `NexoraStreamSession` adds the lifecycle a loop needs — `next()` returns
`nil` for a soft timeout slice so the caller can check its own cancel flag,
`cancel()` is an `Event` (checked between slices, never mid-`recv`, so a
cancel from another thread cannot leave the parser on half a frame), and
`close()` owns the socket. The online suite exercises it against a local mock
peer that splits one chunk across two TCP writes, plus two socket-free cases
through a fake line source (cancel-before-read, multi-line `data:` joining).
Ownership rule: `stream()` hands the open socket to the returned object. That
works because `Socket` and `SSLSocket` are reference types (`operator[@=]`) —
they name one entry in the socket handle table, and their finalizer closes it,
so a value-semantic copy that dies first would close a live connection. Do not
give either wrapper value semantics again without re-checking teardown.

`provider_stream.zeu` + `provider_stream_test.zeu` are the OpenAI-compatible
streaming layer: `nexora_chat_stream_payload` builds the `stream: true` body
(with `stream_options.include_usage` so token counts still arrive),
`nexora_chat_stream_apply` folds one chunk into a running state (`text`,
`toolCalls`, `finishReason`, `usage`) and reports what changed, and
`nexora_chat_stream_message` collapses that state into a standard assistant
message. Tool calls matter here: providers stream them by `index` and slice
`function.arguments` into fragments, so the merger re-reads the slot, appends
the fragment, and writes it back instead of trusting array-element aliasing.
`nexora_chat_stream_tool_requests` parses the arguments into a dict and keeps
the raw text; a parse failure becomes a `{parse_error, raw}` placeholder
rather than dropping the call.

`agent_loop.zeu` + `agent_loop_test.zeu` are the round loop itself
(`nexora_agent_run`). Zeus owns the round count, the event folding, the usage
totals, the tool dispatch and the termination rule; three host seams own
everything it cannot know — `openRound(messages, maxTokens)` returns a session with
`next(sliceMillis)` / `close()` (and optionally `cancel()`), `executeTool(call)`
returns the tool output, and `saveMessage(message)` persists (optional).
`onEvent(event)` and `shouldCancel()` are optional observers.

`onEvent` sees a stable vocabulary: `round_start` / `round_end` (with
`finishReason`, `toolCalls`, `textLength`, `truncated`), `text` and `thinking`
deltas, `usage`, and `function_result`. Two behaviours worth knowing:

* Tool output is capped by `maxToolOutput` (default 20000 bytes) before it is
  fed back into the next request; the truncation point walks back to a UTF-8
  character boundary, and the marker states how many bytes were dropped. The
  `function_result` event reports `truncated` and `originalLength`.
* Cancellation is propagated down to the session (`session.cancel()`) instead
  of only breaking the read loop, so the peer stops streaming and the socket
  does not linger. Sessions that do not implement `cancel()` are fine.

`thinking` covers reasoning models (`reasoning_content`): it is accumulated
separately and deliberately **not** echoed back in the assistant message,
because several providers reject that field on input.

The loop stops at `done` (no tool calls left), `awaiting_approval`, `cancelled`,
`max_rounds`, `error`, or `context_overflow`, and returns the final assistant message, the
round and tool counters, whether any round hit `max_tokens` (`truncated`), the
per-round `finishReasons`, the accumulated usage, and the full message list.
Provider stream failures and empty final responses use separate configured
retry limits. Stream retries preserve already delivered text as continuation
context; empty-response retry prompts stay out of persisted history.

`turn_injection.zeu` + `turn_injection_test.zeu` implement Modified Injection
for the Agent Loop. `injectionSources` accepts separate
`project_conventions` and `custom_instructions` text. The first turn adds each
source in full; later turns add only line-level changes, while unchanged
sources add no message. An updated `permissionMode` adds its own system message.
The optional `loadInjectionState()` / `saveInjections(messages, baseline,
permissionMode, skipUserMessage)` callbacks load and persist the source
baseline with the messages. Injection metadata stays in history and is removed
by `nexora_message_payload` before requests reach a provider. After history is
folded, `nexora_agent_run` rebuilds the full current injection immediately
after the summary and before the current user message; the optional
`saveContext(record, fullInjections, requestMessages)` callback lets the host
commit the summary, cut position, and injection together after checking that
the uncompressed request history still matches the stored conversation.
`NexoraCoreConversationStore`
also exposes `insertInjections` to save injected messages, versioned baseline,
and permission mode in one write.

`compression.zeu` + `compression_test.zeu` implement the deterministic half of
the Python `ContextManager.prepare`: `nexora_compress(messages, window,
output, tools, summarize)` returns `direct` when the request fits, `trimmed`
when halving the longest `tool` results is enough, `folded` when the history
before the current turn has to be replaced by a summary, and `overflow` when
the current turn alone is too big. Trimming only touches a copy — the caller's
history keeps the original tool output — and every cut walks back to a UTF-8
character boundary so the request body never carries half a character. Folding
keeps leading `system` messages and the current user turn verbatim, and drops
only whole earlier turns, so tool-call pairing is never split. The baseline
builds the summary with a batching model call; here that is an optional
`summarize(messages) -> string` seam, and without it a deterministic digest
(one line per dropped message) is used, so a conversation degrades instead of
getting stuck. `nexora_agent_run` calls this whenever the budget check fails
and emits a `context_compression` event with `before` / `estimate` / `budget` /
`droppedMessages`.

`agent_cli.zeu` + `agent_cli.py` is the end-to-end proof: real provider entry,
streaming chat completions, one real tool (`read_file`, sandboxed to a
temporary work directory), and two rounds (the model calls the tool, the loop
returns the file contents, the model answers). Run it with:

```powershell
python samples/NexoraCodeCore/agent_cli.py --provider USST
```

It prints each streamed text fragment, the tool result, the round count, the
token usage, and the final reply. `--max-rounds` caps the loop; `--no-tool`
relaxes the assertion that `read_file` was actually called.

From the Zeus2 repository root, run:

```powershell
python samples/NexoraCodeCore/run_test.py --offline
python samples/NexoraCodeCore/run_test.py
```

The `--offline` run compiles the policy, task-event, connection-state, RPC-dispatch,
AgentLoop, Modified Injection, compression, context, conversation-store,
task-stream, task-journal and JSON checks as an optimized AOT executable
without opening sockets: retry with
backoff, heartbeat cadence, lifecycle flush before the first read, reconnect
after the peer closes, and stop latency. It reports separate JSON parse-only and
stringify-only microbenchmarks against Python's `json` module using the same
ASCII payload and operation count. The default run exercises two WSS scenarios
through a local mock peer and a temporary self-signed certificate: the transport
slice (SSL wrapping, device authentication, RPC exchange, lifecycle filtering,
frame overflow, close handshake) and the connection slice (heartbeat cadence,
queued lifecycle frame on the first pass, the RPC hook, and a clean stop with
close code 1000). A third slice exercises chunked SSE through `stream()` and
`sse.zeu` against a plain-HTTP mock peer (one chunk split across two TCP
writes, `[DONE]` terminator, clean close).
The separate `ZeusZTest/run_websocket_wss_test.py` regression covers rejection
of an untrusted certificate by the default context, an insecure-context
WebSocket text roundtrip, and the close handshake. Unicode is checked in the
JSON path and again in the RPC round trip. The benchmarks measure JSON routines,
not end-to-end Nexora request latency.
