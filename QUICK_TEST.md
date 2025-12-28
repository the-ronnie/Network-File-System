# Quick Testing Guide

## Test 1: Access Request System (5 marks)

### Setup (3 terminals)
```bash
# Terminal 1 - Name Server
make run-ns

# Terminal 2 - Storage Server
make run-ss
# Enter NS IP: 127.0.0.1

# Terminal 3 - Client (Bob)
make run-client
# Username: bob
```

### Bob creates confidential file
```
CREATE confidential.txt
WRITE confidential.txt 1
0 TopSecret
ETIRW
```

### Terminal 4 - Client (Alice)
```bash
make run-client
# Username: alice
```

### Alice requests access
```
READ confidential.txt          # Should FAIL with "Access denied"
REQACCESS confidential.txt     # Request access
```

### Back to Bob (Terminal 3)
```
SHOW_REQUESTS                           # See alice's request
APPROVE_REQ confidential.txt alice      # Approve it
```

### Back to Alice (Terminal 4)
```
READ confidential.txt          # Should NOW WORK - see "TopSecret"
```

✅ **PASS** if Alice can read after approval

---

## Test 2: Basic Replication (Part of 15 marks)

**Note:** For full multi-SS testing, you need to run two Storage Servers on different ports or different machines. The current implementation uses fixed ports (9002/9003), so for same-machine testing, you'd need to modify `common.h` temporarily.

### Simple Same-Machine Test (Conceptual)

With only ONE storage server running:

```bash
# Terminal 3 (Client)
CREATE test.txt
WRITE test.txt 1
0 Hello
ETIRW
```

Watch Terminal 1 (Name Server) output:
- Should show: `✓ File 'test.txt' created successfully (Primary SS: 0, Replicas: 1, Total files: X)`
- With only 1 SS, replica count will be 1 (just the primary)

When a SECOND SS connects:
- NS should show: `→ Initiating recovery sync for SS 1...`
- NS should show: `→ Replication initiated: 'test.txt' to SS 1`

### Full Multi-SS Test (Requires Code Modification)

See `TEST_FAULT_TOLERANCE.md` for detailed instructions on testing with 2 Storage Servers.

---

## Observing Implementation Features

### Name Server Output to Watch:

1. **Heartbeat (every 5 seconds)**
```
(Silent if all SS alive, shows warnings if SS down)
```

2. **File Creation with Replication**
```
✓ File 'test.txt' created successfully (Primary SS: 0, Replicas: 2, Total files: 3)
→ Replication initiated: 'test.txt' to SS 1
```

3. **SS Registration**
```
[2025-11-20 XX:XX:XX] Storage Server registered:
  ID: 0
  IP: 127.0.0.1
  NS Port: 9002
  Client Port: 9003
  Total SS count: 1
```

4. **Access Requests**
```
✓ Access request: alice → confidential.txt (owner: bob)
✓ Approved: alice granted READ access to confidential.txt
```

### Storage Server Output:

1. **PING Response**
```
Received command from NS: PING
(Responds with PONG automatically)
```

2. **Replication Receipt**
```
→ Replicating file 'test.txt' from 127.0.0.1:9002
✓ Replicated file 'test.txt' (42 bytes)
```

---

## Implementation Summary

### ✅ COMPLETED Features:

**Access Request System (5 marks):**
- REQACCESS command - users can request access to files
- SHOW_REQUESTS command - owners see pending requests
- APPROVE_REQ command - owners approve requests, grants READ access
- Request queue stored in FileInfo structure (MAX_ACCESS_REQUESTS = 20)

**Fault Tolerance & Replication (15 marks):**
- Multi-SS architecture with unique IDs and alive status tracking
- Heartbeat monitoring (5s interval, 15s timeout, PING/PONG protocol)
- Automatic file replication on CREATE to all available SS instances
- Read redirection - if primary SS down, uses replica
- SS recovery sync - when SS reconnects, syncs all missing files
- Failure detection - marks SS as down after heartbeat timeout
- Storage Server handles PING, REPLICATE, GET_FILE_CONTENT commands

### 📝 Notes:

**Limitations:**
1. Fixed ports in `common.h` - testing multiple SS on same machine requires code modification
2. WRITE operations create file on primary SS but async replication happens during SS sync
3. DELETE operations don't remove replicas (would need distributed DELETE)

**For Production Use Would Need:**
- Configurable ports (command-line arguments)
- WRITE replication (trigger replication after WRITE completes)
- DELETE replication (remove file from all replicas)
- Consistency mechanisms (vector clocks, quorum writes)
- Better conflict resolution

**Testing Recommendations:**
- Test Access Requests on single SS (easy, fully functional)
- Test Replication/Failover on two separate machines (or modify ports)
- Use network monitoring tools to observe PING/PONG traffic
- Check file system on both SS directories to verify replication

---

## Quick Verification Checklist

- [ ] Access Request: Alice can request access ✓
- [ ] Access Request: Bob sees requests ✓
- [ ] Access Request: Bob can approve ✓
- [ ] Access Request: Alice gains access after approval ✓
- [ ] Replication: Files created are marked with replica count
- [ ] Heartbeat: NS logs show periodic health checks (if SS goes down)
- [ ] Failover: READ works from replica when primary is down
- [ ] Recovery: SS reconnection triggers sync of missing files

Total Potential Marks: **20** (5 for Access Requests + 15 for Fault Tolerance)
