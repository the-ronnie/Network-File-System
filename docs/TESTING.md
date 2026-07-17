# Testing Guide

This guide walks through manual test procedures for the two advanced subsystems: the **access request workflow** and **fault tolerance with replication**. For implementation details behind each behavior, see [FAULT_TOLERANCE.md](FAULT_TOLERANCE.md).

## Prerequisites

Build the project from the repository root:

```bash
make
```

You will need multiple terminal windows:

| Terminal | Role |
|----------|------|
| 1 | Name Server |
| 2 | Storage Server 1 |
| 3 | Storage Server 2 (fault tolerance tests only) |
| 4 | Client — user `bob` |
| 5 | Client — user `alice` |

---

## Test 1: Access Request System

### Setup

```bash
# Terminal 1 — Name Server
make run-ns

# Terminal 2 — Storage Server
make run-ss
# When prompted for the NS IP, enter: 127.0.0.1

# Terminal 4 — Client (Bob)
make run-client
# Username: bob
```

### Bob creates a confidential file

```
CREATE confidential.txt
WRITE confidential.txt 1
0 TopSecret
ETIRW
```

### Alice requests access

```bash
# Terminal 5 — Client (Alice)
make run-client
# Username: alice
```

```
READ confidential.txt          # Should FAIL with "Access denied"
REQACCESS confidential.txt    # Request access
```

Expected: `ACK_SUCCESS: Access request sent to file owner`

### Bob reviews and approves

```
SHOW_REQUESTS                          # Shows alice's pending request
APPROVE_REQ confidential.txt alice     # Approve it
```

Expected: `ACK_SUCCESS: Access granted`

### Alice reads again

```
READ confidential.txt          # Should now succeed and print "TopSecret"
```

**Pass condition:** Alice can read the file only after Bob's approval.

---

## Test 2: Fault Tolerance — Kill & Read (Replication + Failover)

> **Note on ports:** Storage Servers use fixed ports defined in `include/common.h` (9002/9003). To run two Storage Servers you must either use two separate machines (recommended) or temporarily change `SS_NS_PORT`/`SS_CLIENT_PORT` (e.g. to 9004/9005), then run `make rebuild` for the second instance.

### Setup

Start the Name Server (Terminal 1) and both Storage Servers (Terminals 2 and 3), each entering the NS IP when prompted.

### Create a file and let it replicate

```
# Terminal 4 (client)
CREATE repl_test.txt
WRITE repl_test.txt 1
0 Data_is_Safe
ETIRW
```

Wait 2–3 seconds and watch the Name Server terminal for:

```
→ Replication initiated: 'repl_test.txt' to SS 1
✓ File 'repl_test.txt' created successfully (Primary SS: 0, Replicas: 2, ...)
```

### Kill the primary and read

1. In Terminal 2, press `Ctrl+C` to kill Storage Server 1.
2. In Terminal 4, run `READ repl_test.txt`.

**Expected behavior:**
- The Name Server detects that SS 0 is down and redirects the read to the replica:
  `⚠ Primary SS 0 is down, using replica SS 1 for file 'repl_test.txt'`
- The client successfully reads `Data_is_Safe`.

---

## Test 3: Fault Tolerance — Phoenix Test (Recovery & Sync)

This verifies that a Storage Server which was offline catches up on files it missed.

1. **Restart SS1** (Terminal 2) and confirm it re-registers with the NS.
2. **Kill SS2** (Terminal 3, `Ctrl+C`). Wait 5–15 seconds until the NS marks it DOWN:
   `⚠ Storage Server 1 marked as DOWN (heartbeat timeout)`
3. **Create a file while SS2 is dead** (Terminal 4):
   ```
   CREATE update.txt
   WRITE update.txt 1
   0 I_was_written_while_SS2_was_dead
   ETIRW
   ```
4. **Restart SS2** (Terminal 3). Watch the NS terminal for the recovery sync:
   ```
   → Initiating recovery sync for SS 1...
   ✓ Recovery sync initiated: 1 files to replicate to SS 1
   ```
5. **Kill SS1** (Terminal 2) and wait for the heartbeat to mark it down.
6. **Read the file** (Terminal 4): `READ update.txt`

**Pass condition:** the output is `I_was_written_while_SS2_was_dead` — served from SS2, which received the file via recovery sync.
**Fail condition:** `ERROR: No available storage servers` or `File not found`.

---

## What to Watch For

### Name Server output

```
✓ Storage Server 0 came back ONLINE
⚠ Storage Server 1 marked as DOWN (heartbeat timeout)
→ Replication initiated: 'repl_test.txt' to SS 1
⚠ Primary SS 0 is down, using replica SS 1 for file 'repl_test.txt'
✓ Access request: alice → confidential.txt (owner: bob)
✓ Approved: alice granted READ access to confidential.txt
```

### Storage Server output

```
Received command from NS: PING        # heartbeat — responds with PONG
→ Replicating file 'update.txt' from 10.85.162.202:9002
✓ Replicated file 'update.txt' (42 bytes)
```

---

## Troubleshooting

**A second Storage Server won't start on the same machine.**
The implementation uses fixed ports. Test on two machines, or temporarily change the port definitions in `include/common.h` and rebuild for the second instance.

**Replication doesn't happen.**
- Verify both Storage Servers registered successfully (check NS startup logs).
- Look for `→ Replication initiated` messages in the NS terminal.
- Confirm the servers respond to heartbeat PINGs.

**Heartbeat doesn't detect a failure.**
- Failure detection takes up to 15 seconds (`HEARTBEAT_TIMEOUT`); wait before concluding it failed.
- Check the NS terminal for heartbeat warnings.

---

## Verification Checklist

- [ ] Access request: Alice can request access to a file she doesn't own
- [ ] Access request: Bob sees pending requests with `SHOW_REQUESTS`
- [ ] Access request: Bob can approve with `APPROVE_REQ`
- [ ] Access request: Alice gains READ access after approval
- [ ] Replication: created files report a replica count in NS logs
- [ ] Heartbeat: NS logs show DOWN/ONLINE transitions when servers die/return
- [ ] Failover: READ succeeds from a replica when the primary is down
- [ ] Recovery: a reconnecting Storage Server syncs files it missed
