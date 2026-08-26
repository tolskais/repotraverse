# On-demand catalog operations

Repotraverse has no permanent service or inbound network listener. Install the
Windows package in a versioned directory, copy `config/catalog.example.json` to
an operations-owned location, and keep the catalog and scratch directories on
fast local storage.

Every catalog-backed command requires `--config`. It opens SQLite directly,
returns a schema-v1 snapshot envelope, transactionally queues missing work, and
starts one detached hidden runner when needed. The runner heartbeats every five
seconds, processes ordinary work in FIFO order, runs maintenance after the
ordinary queue drains, publishes final facts, and releases its singleton lease
in the same transaction as its final empty-queue check.

Use `--wait` to watch work associated with one invocation. Ctrl-C only stops the
wait. Inspect redacted state with `work-status`; request cancellation with
`work-cancel --work-id ID`. Retry by rerunning the originating command.

## Credentials and connectors

Credential configuration contains references, never secret values. An
environment-backed credential is available only if the command that launches
the runner has that variable. File and Windows credential references are
resolved by the runner. SQLite stores only reference names as capabilities.
Incompatible work remains `waiting_for_credential` until a later compatible
command interacts with the catalog.

Connector access is explicit:

```powershell
repotraverse connector-sync --config C:\repotraverse\catalog.json `
  --connector bitbucket-enterprise
repotraverse connector-status --config C:\repotraverse\catalog.json `
  --connector bitbucket-enterprise
```

Ordinary source, history, and symbol commands never synchronize Bitbucket or
Jira. Only normalized fields are published; tokens, headers, cookies, comments,
attachments, worklogs, and raw provider responses are not persisted.

## Recovery

A later CLI command recovers a launching or active lease whose heartbeat is more
than 30 seconds old and makes interrupted work runnable again. PID existence is
not used as proof of ownership. There is no watchdog or supervisor. If detached
process creation fails, only the still-matching launch token is cleared and work
remains pending.

Non-v1 and unversioned prototype catalogs are unsupported. For corruption,
preserve the directory for incident analysis and create a new catalog. Analysis
Git restores shared facts and decisions; compile contexts, cursors, leases, and
caches remain catalog-local.

OTLP remains outbound-only. Connector and telemetry failures are represented by
redacted fingerprints. The Windows long-path manifest remains part of every
shipped executable.
