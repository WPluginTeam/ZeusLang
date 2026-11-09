# NexoraCode reference source

This is a byte-for-byte reference snapshot of the first remote-control migration slice. It is kept separate from the Zeus implementation and is not imported by the sample runner.

Copied from the current working tree at `F:/Code/AI/ChatDB/NexoraCode`: `core/remote_connection.py`, `model/ConversationStore.py`, `model/StreamRuntime.py`, `model/SessionJournal.py`, and `tests/test_remote_whitelist.py`. `SOURCE_MANIFEST.sha256` records the copied file hashes.

Runtime data, credentials, configuration data, logs, and the full Flask server/route tree were not copied. The first Zeus slice is the WSS state machine and JSON envelopes; RPC dispatch and task-event relay will be migrated behind explicit host interfaces in later steps.
