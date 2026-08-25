# Production-oriented service operations

These procedures cover deployment, coordination, and recovery of the v1 service.
Repotraverse supplies facts and reviewed interpretations, not stability conclusions.

For the CLI experiment PoC, including separate output-directory rules and evidence
collection, use [`poc-runbook.md`](poc-runbook.md).

## Deployment

Use the generic Windows x64 package on the intended 8-vCPU, 32-GB VM. Extract
the ZIP to a versioned directory, copy `config/service.example.json` to
an operations-owned location, and replace every path and repository ID. Keep
the catalog and scratch roots on fast local storage; never place either in a VM
image.

Initialize each instance once with
`repotraverse identity init --catalog C:\repotraverse\catalog`, record the
generated producer ID, and place the IDs of permitted
peer instances in `trusted_producers`. An empty production allowlist trusts only
the local producer. Grant the analysis Git credentials access to
`repotraverse/v1/coordination/tasks/`, `repotraverse/v1/coordination/results/`, and
`repotraverse/v1/coordination/claims/`, plus the configured knowledge refs.
Register the service from an elevated shell:

```powershell
.\tools\install-service.ps1 -Config C:\repotraverse\service.json
```

Readiness is `GET http://127.0.0.1:7341/v1/health/ready`; service identity and
status are available at `/v1/status`, and local metrics are available at
`/v1/metrics`. Configure an HTTPS OTLP endpoint to export logs, metrics, and
request-planning spans. Export failure is counted locally and never blocks
analysis.

## Recovery

The Service Control Manager restarts the process. Dispatching and processing
tasks return to `pending` when the catalog reopens; Git leases prevent duplicate
publication. Transient failures use exponential retry, while incompatible or
repeated failures enter quarantine. Inspect their diagnostic fingerprint and
use the CLI `work.retry` query only after correcting the cause.

Non-v1 and unversioned prototype catalogs are deliberately unsupported.
For catalog corruption, stop the service, preserve the directory for incident
analysis, create a new catalog directory, and restart. PR imports, receipts,
inference proposals, decisions, and lineage reviews are re-imported from Git;
compiler manifests are cache entries, and build captures must
be re-imported because raw captures are intentionally not shared artifacts.
Durable local request state should be included in catalog backups.

## Failure handling

- Git unavailable: queries remain partial, tasks stay unpublished, and workers
  do not start them. Restore the remote and allow the coordination loop to retry.
- Extractor timeout or output limit: the worker keeps retryable operational state;
  increase a limit only after capacity review.
- Disk pressure: the workspace pool evicts idle sparse revisions by byte and count
  limits. Full capture workspaces are temporary. If a revision cannot fit while
  preserving the configured reserve, it fails with `disk_space_insufficient`; reduce
  concurrency, increase the workspace volume, or provide complete dependency capture.
  Temporary result refs are pruned after the completed lease expires.
- Progressive budget exhausted: retain `progressive-screening.v1.json` and the pilot
  report. Git and syntax facts remain valid, but semantic history is partial. Increase
  only the exhausted explicit cap on a new output directory.
- Invalid producer record: remove the producer from the allowlist, preserve the
  offending ref, and investigate before reenrollment.
- Telemetry outage: use `/v1/metrics` and host-captured structured stderr;
  analysis continues with bounded telemetry queues.

## Upgrade and rollback

Run the complete test suite and catalog recovery drill before promotion. Stop
the service, back up the catalog, replace the versioned program directory, and
restart. Future schema migrations must be transactional and forward-only.
Rollback is permitted only when the previous binary supports the current
catalog schema; otherwise restore the matching backup. Verify readiness,
producer identity, queue depth, quarantine count, and a representative cached
and uncached query after every change.
