---
name: koios-sync
description: Harvest new commits from the koios/matrx reference repos (matrx-fw, koios-sdk, kd_common, esp_websocket_client, kd_pixdriver), triage them for relevance to this firmware, and propose ports. Use when asked to sync, check, or harvest koios or matrx changes.
---

# koios-sync

This firmware shares ancestry with the Koios Digital MATRX ecosystem. Their fixes regularly apply to us verbatim or conceptually (past harvests fixed an ntp task stack overflow, a websocket TLS teardown use-after-free, and OTA download fragility we had inherited). This skill runs one harvest cycle.

## State

`last-sync.json` next to this file maps repo name to the last triaged commit SHA. Only commits after that SHA need review. Update the file at the end of a run.

## Repos and lineage map

Local clones live in `inspiration/` at the repo root (gitignored). All five repos in `last-sync.json` have a clone there, `kd_common` included. Fall back to `https://github.com/koiosdigital/kd_common/commit/<sha>.patch` only if a clone is missing.

| Reference repo | Maps to us |
|---|---|
| matrx-fw `main/webp_player` | `main/webp_player` (same lineage, near verbatim) |
| matrx-fw `main/scheduler` | `main/scheduler` (concept-level, ours is a different FSM) |
| matrx-fw `main/sockets` | `main/network/sockets|handlers|messages` (ours is JSON, theirs protobuf) |
| matrx-fw `main/display` | `main/display` |
| matrx-fw `main/config` | `main/config` + `main/system/quiet_hours*` |
| matrx-fw `components/esp-hub75` | our fork at `~/Code/esp-hub75` (consumed via git pin in `main/idf_component.yml`) |
| koios-sdk `core/cloudlink.c` | `main/network/sockets.cpp` (outbox, escalation, reconnect policy) |
| koios-sdk `core/ota.c` + ports | `main/system/ota*` |
| kd_common `src/ntp.c` | `main/system/ntp.cpp` (same TZ-fetch lineage) |
| kd_common heap tracing | `main/system/heap_monitor*` |
| kd_common `src/wifi.c`, `src/provisioning.c`, `src/net.c` | `main/network/wifi*` + `main/system/event_bus` (concept-level; their connect/disconnect callback registry is our event bus). Easy to miss because their feature commits refactor this core in passing. |
| kd_common `src/kd_common.c` | `main/startup/runtime_orchestrator` (boot phase ordering) |
| esp_websocket_client | our pinned dependency in `main/idf_component.yml` (bump the pin) |
| kd_pixdriver | no mapping (no addressable LED hardware) |

## Procedure

1. Read `last-sync.json`.
2. For each clone in `last-sync.json`: `git -C inspiration/<repo> fetch origin --quiet`, then `git log <last-sha>..origin/main --oneline`. Run this gathering in a sandbox or subagent; only the commit lists need to reach the conversation.
3. Triage every new commit: read the diff (`git show`) and classify. Read the **whole** diff, not just the files the lineage map names: their feature commits routinely refactor shared core (WiFi, provisioning, boot ordering) in passing, and a `--stat` that looks like "eth support" can still be hiding a behavioural change to code we share. Classify as:
   - **port**: fixes or improves code we share or have already ported (use the lineage map; check whether our copy has the same defect before assuming)
   - **concept**: their implementation differs but the idea transfers
   - **skip**: their-stack-specific (protobuf, mTLS/PKI, BLE, kd_pixdriver, submodule bumps, version bumps, sdkconfig noise)
4. Present the triage as a table with a one-line rationale per commit. Wait for the user to approve which ports to do, then implement them following the repo conventions, build-verify (fresh `-B` build dir, see CLAUDE.md), and commit in conventional style.
5. After the run (regardless of how many ports were approved), update `last-sync.json` to the new origin/main SHAs so triaged-but-skipped commits are not re-reviewed.

## Gotchas

- Watch for commits that fix bugs in code WE ported from them (webp frame diffing, quiet hours, outbox); those are the highest-value ports and easy to miss because our file layout differs.
- Their commit messages are often uninformative ("fix", "heap", "eh"); always read the diff.
- esp-hub75 changes go to our fork first (commit and push there), then bump the pin in `main/idf_component.yml` and clear the component cache if the fetch resolves stale (`compote cache clear`, delete root `dependencies.lock`).
