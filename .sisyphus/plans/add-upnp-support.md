# Plan: Add UPnP Support to raknet_proxy

## TL;DR

> **Quick Summary**: Add UPnP (Universal Plug and Play) port mapping support to `raknet_proxy` to automatically configure router port forwarding, improving NAT traversal success rate.
>
> **Deliverables**:
> - Modified `raknet_proxy.cpp` with UPnP integration
> - New `-upnp` command-line flag
> - Automatic port mapping cleanup on exit
>
> **Estimated Effort**: Short (1-2 hours)
> **Parallel Execution**: NO - single file modification
> **Critical Path**: Modify raknet_proxy.cpp → Test compilation → Verify UPnP functionality

---

## Context

### Original Request
User asked to add UPnP support to `raknet_proxy` to improve NAT hole-punching success rate.

### Current State
- `raknet_proxy.cpp` is a C++ application using RakNet library
- Uses `NatPunchthroughClient` plugin for NAT traversal
- System has `miniupnpc` library installed (`/usr/include/miniupnpc/`, `/usr/lib/libminiupnpc.so`)
- No UPnP integration currently exists

### Technical Background
UPnP allows applications to automatically configure router port mappings. For `raknet_proxy`, this means:
1. The listening port can be explicitly opened on the router
2. External clients can connect directly without NAT traversal
3. Combined with existing NAT punchthrough, improves overall connectivity

---

## Work Objectives

### Core Objective
Add UPnP port mapping functionality to `raknet_proxy` to automatically forward the listening port on the router.

### Concrete Deliverables
- Modified `raknet_proxy.cpp` with:
  - UPnP header includes
  - `-upnp` command-line flag
  - `AddUPnPPortMapping()` function
  - `RemoveUPnPPortMapping()` function
  - Integration in main() startup and shutdown

### Definition of Done
- [x] Code compiles successfully with `-lminiupnpc` flag
- [x] `-upnp` flag is recognized and UPnP is attempted
- [x] Port mapping is added on startup (if router supports UPnP)
- [x] Port mapping is removed on shutdown
- [x] Graceful fallback if UPnP fails (non-blocking)

### Must Have
- Non-blocking UPnP discovery (timeout)
- Graceful error handling (UPnP failure doesn't crash program)
- Port mapping cleanup on exit
- Clear console output for UPnP status

### Must NOT Have
- Hard dependency on UPnP (must work without it)
- Blocking the main loop for UPnP operations
- Security vulnerabilities (expose only necessary ports)

---

## Verification Strategy

### Test Decision
- **Infrastructure exists**: NO (no test framework)
- **Automated tests**: None
- **Agent-Executed QA**: YES - manual compilation and runtime testing

### QA Policy
Every task includes agent-executed QA scenarios. Evidence saved to `.sisyphus/evidence/`.

---

## Execution Strategy

### Sequential Execution
This is a single-file modification, so tasks are sequential:

```
Task 1: Add UPnP includes and helper functions
    ↓
Task 2: Add -upnp command-line option
    ↓
Task 3: Implement UPnP port mapping in main()
    ↓
Task 4: Test compilation and runtime
```

---

## TODOs

- [x] 1. Add UPnP Includes and Helper Functions

  **What to do**:
  - Add `#include <miniupnpc/miniupnpc.h>` and related headers
  - Create `AddUPnPPortMapping()` function that:
    1. Calls `upnpDiscover()` to find UPnP devices
    2. Calls `UPNP_GetExternalIPAddress()` to get external IP
    3. Calls `UPNP_AddPortMapping()` to open the port
    4. Returns success/failure status
  - Create `RemoveUPnPPortMapping()` function that:
    1. Calls `UPNP_DeletePortMapping()` to remove the mapping
    2. Cleans up resources

  **Recommended Agent Profile**:
  - **Category**: `quick`
  - **Skills**: []

  **References**:
  - `/usr/include/miniupnpc/miniupnpc.h` - UPnP discovery API
  - `/usr/include/miniupnpc/upnpcommands.h` - Port mapping commands
  - `/home/yuey1ng/mini/raknet-proxy/RakNet/DependentExtensions/miniupnpc-1.6.20120410/upnpc.c` - Example usage

  **Acceptance Criteria**:
  - [x] UPnP headers are included
  - [x] `AddUPnPPortMapping()` function exists
  - [x] `RemoveUPnPPortMapping()` function exists
  - [x] Functions compile without errors

  **QA Scenarios**:

  ```
  Scenario: UPnP function compilation
    Tool: Bash (g++)
    Preconditions: Source file modified
    Steps:
      1. Run: g++ -c raknet_proxy.cpp -Iraknet_proxy/RakNet/Source -o /dev/null
      2. Check for compilation errors
    Expected Result: No compilation errors related to UPnP code
    Evidence: .sisyphus/evidence/task-1-compile.txt
  ```

  **Commit**: NO (will commit after all tasks)

- [x] 2. Add -upnp Command-Line Option

  **What to do**:
  - Add `bool useUPnP` field to `ProgramArgs` struct
  - Add `-upnp` flag parsing in `ParseArgs()`
  - Update `PrintUsage()` to show `-upnp` option
  - Default to `false` (UPnP disabled by default)

  **Recommended Agent Profile**:
  - **Category**: `quick`
  - **Skills**: []

  **References**:
  - `/home/yuey1ng/mini/raknet-proxy/raknet_proxy.cpp:26-36` - ProgramArgs struct
  - `/home/yuey1ng/mini/raknet-proxy/raknet_proxy.cpp:52-112` - ParseArgs function

  **Acceptance Criteria**:
  - [x] `useUPnP` field exists in ProgramArgs
  - [x] `-upnp` flag is parsed
  - [x] Help text shows `-upnp` option
  - [x] Default value is `false`

  **QA Scenarios**:

  ```
  Scenario: UPnP flag parsing
    Tool: Bash
    Preconditions: Binary compiled
    Steps:
      1. Run: ./raknet_proxy --help
      2. Check output contains "-upnp"
    Expected Result: Help text shows UPnP option
    Evidence: .sisyphus/evidence/task-2-help.txt
  ```

  **Commit**: NO

- [x] 3. Integrate UPnP in main() Startup and Shutdown

  **What to do**:
  - After RakNet startup (line ~195), if `args.useUPnP` is true:
    1. Call `AddUPnPPortMapping(args.localPort)`
    2. Print external IP if successful
  - Before shutdown (line ~451), if UPnP was enabled:
    1. Call `RemoveUPnPPortMapping(args.localPort)`
  - Ensure UPnP failure doesn't block or crash the program

  **Recommended Agent Profile**:
  - **Category**: `quick`
  - **Skills**: []

  **References**:
  - `/home/yuey1ng/mini/raknet-proxy/raknet_proxy.cpp:187-197` - RakNet startup
  - `/home/yuey1ng/mini/raknet-proxy/raknet_proxy.cpp:450-458` - Shutdown cleanup

  **Acceptance Criteria**:
  - [x] UPnP port mapping attempted on startup (if -upnp flag)
  - [x] External IP printed if UPnP succeeds
  - [x] UPnP port mapping removed on shutdown
  - [x] Program continues even if UPnP fails

  **QA Scenarios**:

  ```
  Scenario: UPnP startup integration
    Tool: Bash
    Preconditions: Binary compiled, router with UPnP available
    Steps:
      1. Run: ./raknet_proxy -local_port 60000 -upnp -nat_ip 1.2.3.4 -nat_port 61111 -target_ip 5.6.7.8 -target_port 7000
      2. Check console output for UPnP messages
      3. Press Ctrl+C to exit
    Expected Result: 
      - "UPnP: 正在发现设备..." message appears
      - "UPnP: 外部IP: x.x.x.x" message appears (if successful)
      - "UPnP: 端口映射已添加" message appears (if successful)
      - "UPnP: 端口映射已删除" message appears on exit
    Evidence: .sisyphus/evidence/task-3-upnp-runtime.txt

  Scenario: UPnP graceful failure
    Tool: Bash
    Preconditions: Binary compiled, no UPnP router available
    Steps:
      1. Run: ./raknet_proxy -local_port 60000 -upnp -nat_ip 1.2.3.4 -nat_port 61111 -target_ip 5.6.7.8 -target_port 7000
      2. Check console output
    Expected Result: 
      - "UPnP: 发现设备失败" or similar warning appears
      - Program continues running normally
    Evidence: .sisyphus/evidence/task-3-upnp-failure.txt
  ```

  **Commit**: YES
  - Message: `feat(upnp): add UPnP port mapping support`
  - Files: `raknet_proxy.cpp`
  - Pre-commit: `g++ raknet_proxy.cpp -Iraknet_RakNet/Source -Lraknet_RakNet/Lib/LibStatic -lRakNetStatic -lminiupnpc -lpthread -o raknet_proxy`

---

## Final Verification Wave

- [x] F1. **Compilation Test** — `quick`
  Verify the code compiles without errors using the standard compilation command.
  Output: `Compilation [PASS/FAIL] | VERDICT`

- [x] F2. **Runtime Test** — `quick`
  Run the binary with `-upnp` flag and verify UPnP discovery is attempted.
  Output: `UPnP Discovery [PASS] | Port Mapping [PASS] (graceful failure) | VERDICT: PASS`

---

## Commit Strategy

- **Task 3**: `feat(upnp): add UPnP port mapping support` - raknet_proxy.cpp

---

## Success Criteria

### Verification Commands
```bash
# Compile with UPnP support
g++ raknet_proxy.cpp -I./RakNet/Source -L./RakNet/Lib/LibStatic -lRakNetStatic -lminiupnpc -lpthread -o raknet_proxy

# Test help output
./raknet_proxy --help  # Should show -upnp option

# Test UPnP functionality (requires UPnP-capable router)
./raknet_proxy -local_port 60000 -upnp -nat_ip 1.2.3.4 -nat_port 61111 -target_ip 5.6.7.8 -target_port 7000
```

### Final Checklist
- [x] Code compiles with `-lminiupnpc`
- [x] `-upnp` flag is recognized
- [x] UPnP discovery is attempted on startup
- [x] Port mapping is added (if router supports it)
- [x] Port mapping is removed on shutdown
- [x] Program works without `-upnp` flag
- [x] Program continues if UPnP fails

---

## Implementation Notes

### miniupnpc API Usage

```cpp
#include <miniupnpc/miniupnpc.h>
#include <miniupnpc/upnpcommands.h>
#include <miniupnpc/upnperrors.h>

bool AddUPnPPortMapping(int port) {
    struct UPNPDev* devlist = NULL;
    char externalIP[16] = {0};
    int error = 0;
    
    // Discover UPnP devices (2 second timeout)
    devlist = upnpDiscover(2000, NULL, NULL, 0, 0, &error);
    if (!devlist) {
        printf("UPnP: 发现设备失败\n");
        return false;
    }
    
    struct UPNPUrls urls;
    struct IGDdatas data;
    
    // Get valid IGD (Internet Gateway Device)
    int igd = UPNP_GetValidIGD(devlist, &urls, &data, NULL, 0);
    if (igd <= 0) {
        printf("UPnP: 未找到有效的IGD设备\n");
        freeUPNPDevlist(devlist);
        return false;
    }
    
    // Get external IP
    if (UPNP_GetExternalIPAddress(urls.controlURL, data.first.servicetype, externalIP) == 0) {
        printf("UPnP: 外部IP: %s\n", externalIP);
    }
    
    // Add port mapping
    char portStr[16], internalAddr[64];
    snprintf(portStr, sizeof(portStr), "%d", port);
    
    // Get local IP
    // ... (need to get local IP address)
    
    int result = UPNP_AddPortMapping(
        urls.controlURL,
        data.first.servicetype,
        portStr,      // external port
        portStr,      // internal port
        internalAddr, // internal IP
        "raknet_proxy", // description
        "UDP",        // protocol
        NULL,         // remote host
        "0"           // lease duration (0 = permanent)
    );
    
    if (result == 0) {
        printf("UPnP: 端口映射已添加 (UDP %d)\n", port);
    } else {
        printf("UPnP: 端口映射失败: %s\n", strupnperror(result));
    }
    
    // Cleanup
    FreeUPNPUrls(&urls);
    freeUPNPDevlist(devlist);
    
    return (result == 0);
}
```

### Compilation Command

```bash
g++ raknet_proxy.cpp \
    -I./RakNet/Source \
    -L./RakNet/Lib/LibStatic \
    -lRakNetStatic \
    -lminiupnpc \
    -lpthread \
    -o raknet_proxy
```

---

## Risk Assessment

| Risk | Impact | Mitigation |
|------|--------|------------|
| No UPnP router available | Low | Graceful fallback, program continues |
| UPnP discovery timeout | Low | 2-second timeout, non-blocking |
| Port mapping fails | Low | Warning message, continue operation |
| Security concerns | Medium | Only map necessary port, use descriptive name |

---

## Dependencies

- System: `miniupnpc` library (`/usr/lib/libminiupnpc.so`)
- Headers: `/usr/include/miniupnpc/`
- Compilation: `-lminiupnpc` flag
