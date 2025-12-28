# Fault Tolerance and Access Request Testing

## Setup

You'll need **5 terminal windows**:
- Terminal 1: Name Server
- Terminal 2: Storage Server 1 (SS1) on port 9002/9003
- Terminal 3: Storage Server 2 (SS2) on port 9004/9005
- Terminal 4: Client (Alice)
- Terminal 5: Client (Bob)

## Part 1: Access Request System [5 Marks]

### Terminal 1 - Name Server
```bash
make run-ns
```

### Terminal 2 - Storage Server 1
```bash
make run-ss
# When prompted for NS IP: 127.0.0.1 (or your actual IP)
```

### Terminal 4 - Client (Bob - Owner)
```bash
make run-client
# Username: bob
```

**Bob creates a file:**
```
CREATE confidential.txt
WRITE confidential.txt 1
0 TopSecret
ETIRW
```

### Terminal 5 - Client (Alice - Requester)
```bash
make run-client
# Username: alice
```

**Alice tries to read (should fail):**
```
READ confidential.txt
```
Expected: `ERROR: Access denied`

**Alice requests access:**
```
REQACCESS confidential.txt
```
Expected: `ACK_SUCCESS: Access request sent to file owner`

### Back to Terminal 4 (Bob)

**Bob views requests:**
```
SHOW_REQUESTS
```
Expected: Shows alice's request for confidential.txt

**Bob approves:**
```
APPROVE_REQ confidential.txt alice
```
Expected: `ACK_SUCCESS: Access granted`

### Back to Terminal 5 (Alice)

**Alice reads again:**
```
READ confidential.txt
```
Expected: Should now successfully read "TopSecret"

---

## Part 2: Fault Tolerance - Kill & Read [15 Marks]

### Additional Setup - Start Second Storage Server

### Terminal 3 - Storage Server 2
For SS2, you need to modify ports. Either:
1. Edit `common.h` to use different ports, OR
2. Run on a different machine

**For testing on same machine, temporarily edit `common.h`:**
```c
#define SS_NS_PORT 9004        // Change from 9002
#define SS_CLIENT_PORT 9005    // Change from 9003
```

Then recompile and run:
```bash
make clean && make
./bin/storageserver
# When prompted: 127.0.0.1
```

### Test A: Kill & Read (Replication Verify)

### Terminal 4 (Client)
```
CREATE repl_test.txt
WRITE repl_test.txt 1
0 Data_is_Safe
ETIRW
```

Wait 2-3 seconds for replication to complete (check NS terminal for "→ Replication initiated" messages).

### Terminal 2 - Kill SS1
Press `Ctrl+C` to kill Storage Server 1

### Terminal 4 (Client)
```
READ repl_test.txt
```

**Expected Behavior:**
- Name Server detects SS1 is down
- Redirects read request to SS2 (replica)
- Client successfully reads "Data_is_Safe"
- NS terminal shows: "⚠ Primary SS 0 is down, using replica SS 1..."

---

## Test B: Phoenix Test (SS Recovery & Sync)

### Restart SS1 (Terminal 2)
```bash
./bin/storageserver
# When prompted: 127.0.0.1
```

### Terminal 3 - Kill SS2
Press `Ctrl+C`

Wait for heartbeat (5-15 seconds) until NS shows SS2 as DOWN.

### Terminal 4 (Client)
```
CREATE update.txt
WRITE update.txt 1
0 I_was_written_while_SS2_was_dead
ETIRW
```

Wait 2 seconds.

### Terminal 3 - Restart SS2
```bash
./bin/storageserver
```

Wait 3-5 seconds for reconnection and sync (watch NS terminal).

### Terminal 2 - Kill SS1
Press `Ctrl+C`

Wait for heartbeat to detect SS1 down.

### Terminal 4 (Client)
```
READ update.txt
```

**Pass Condition:** Output is "I_was_written_while_SS2_was_dead"
**Fail Condition:** "ERROR: No available storage servers" or "File not found"

---

## Observing the System

### Name Server Terminal Output to Watch For:

1. **Heartbeat monitoring:**
```
✓ Storage Server 0 came back ONLINE
⚠ Storage Server 1 marked as DOWN (heartbeat timeout)
```

2. **Replication:**
```
→ Replication initiated: 'repl_test.txt' to SS 1
✓ File 'repl_test.txt' created successfully (Primary SS: 0, Replicas: 2, Total files: X)
```

3. **Failover:**
```
⚠ Primary SS 0 is down, using replica SS 1 for file 'repl_test.txt'
```

### Storage Server Terminal Output:

```
→ Replicating file 'update.txt' from 10.85.162.202:9002
✓ Replicated file 'update.txt' (42 bytes)
```

---

## Troubleshooting

### If SS2 won't start on same machine:
The current implementation uses fixed ports. You have two options:
1. **Test on two different machines** (recommended for real distributed testing)
2. **Modify code** to accept port arguments

### If replication fails:
- Check that both SS instances registered successfully
- Look for "→ Replication initiated" messages in NS terminal
- Verify SS instances respond to PING (heartbeat)

### If heartbeat doesn't detect failures:
- Wait 15 seconds (HEARTBEAT_TIMEOUT)
- Check NS terminal for heartbeat messages
- Ensure PING/PONG implementation is working

---

## Implementation Status

### ✅ Implemented Features:
1. Access Request System (REQACCESS, SHOW_REQUESTS, APPROVE_REQ)
2. Multi-SS architecture with unique IDs
3. File replication on CREATE (async to all available SS)
4. Heartbeat monitoring (5s interval, 15s timeout)
5. Failure detection and marking SS as down
6. Read redirection to replica on primary SS failure
7. PING/PONG protocol in Storage Server

### ⚠️ Partial/Missing Features:
1. **SS Recovery Sync** - When SS reconnects, it doesn't automatically fetch missed files
   - Current: Relies on files created before SS was added
   - Needed: Active sync of files created while SS was offline
2. **Write Replication** - WRITE operations don't trigger async replication to replicas
   - Current: Only CREATE replicates
   - Needed: WRITE updates should replicate to all alive replicas
3. **Port Configuration** - Hardcoded ports make running multiple SS on same machine difficult

### To Fully Pass Phoenix Test:
The current implementation will pass if:
- update.txt was created AFTER SS2 came back online
- OR manual file copying is done

For automatic sync, additional implementation needed in SS registration handler to:
1. Request list of all files from NS
2. Request missing files from other SS instances
3. Sync checksums/timestamps
