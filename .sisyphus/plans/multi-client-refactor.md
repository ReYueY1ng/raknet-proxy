# Multi-Client RakNet Proxy Refactor

## TL;DR

> **Quick Summary**: Refactor single-client NAT punchthrough proxy to multi-client, with per-client GUID impersonation and silent forwarding to a shared target server.
>
> **Deliverables**:
> - Multi-client connection tracking (std::map<GUID, ClientContext>)
> - Per-client child RakPeer with GUID impersonation
> - Unified poll loop for main + N child peers
> - Graceful SIGINT shutdown
> - Silent forwarding (errors + key events only)
>
> **Estimated Effort**: Medium
> **Parallel Execution**: YES - 3 waves
> **Critical Path**: Wave 1 (structs + signal) → Wave 2 (peer lifecycle) → Wave 3 (poll loop + polish)

---

## Context

### Original Request
Refactor `raknet_proxy.cpp` (~440 lines) from single-client to multi-client:
1. 多客户端 → 每个客户端独立转发路径
2. GUID 伪装 → 子 RakPeer 用 SetMyGUID(clientGUID) 连目标服务器
3. 安静转发 → 去掉 PrintPacketData()，只留错误和关键事件日志
4. 共用目标 → 所有客户端共享一个 targetServerIP:port

### Interview Summary
- **GUID 用途**: proxy 伪装成客户端 GUID 连接目标服务器（SetMyGUID + Startup + Connect）
- **目标服务器**: 所有客户端共用一个 targetServerIP:port
- **日志级别**: 只保留连接/断开/打洞结果/错误，去掉 hex dump 和 "转发完成" 等
- **架构**: 主 peer（NAT 打洞） + N 个子 peer（目标服务器连接），单线程 poll

### Metis Review
**Critical Finding — NatPunchthroughClient 单状态限制**:
`NatPunchthroughClient` 内部使用单一 `SendPing sp` 状态机。两个客户端**同时**打洞时，第二个 `ID_NAT_CONNECT_AT_TIME` 会覆盖第一个的 `sp`，导致第一个打洞失败。客户端需要重试 → 实际表现为串行化打洞，延迟增加但不丢连接。

**Identified Gaps** (addressed):
- **信号处理**: 需添加 `SIGINT` handler 优雅关闭（当前 Ctrl+C 直接杀进程，无 cleanup）
- **子 peer 安全**: `SetMaximumIncomingConnections(0)` 防止意外连入
- **端口策略**: 子 peer 用 `port=0` 让 OS 自动分配，避免端口冲突
- **目标服务器多连接**: 如果目标服务器限制同 IP 多连接，需用户配合放宽

---

## Work Objectives

### Core Objective
将单客户端代理重构为多客户端代理，每个客户端通过 NAT 打洞到达后，proxy 伪装成该客户端的 GUID 连接到目标服务器，双向转发数据。

### Concrete Deliverables
- 改写 `raknet_proxy.cpp`：单文件，从 ~440 行扩展到 ~600 行
- 新增 `ClientContext` 结构体：跟踪每个客户端的状态
- 新增 `std::map<RakNetGUID, ClientContext>` 客户端表
- 新增 SIGINT 信号处理器
- 删除 `PrintPacketData()` 函数和调用
- 精简所有 `printf` 为关键事件级别

### Definition of Done
- [x] `g++ -c -lpthread -g -I RakNet/Source raknet_proxy.cpp -o raknet_proxy.o` 编译通过
- [x] 完整链接通过：`g++ --std=c++11 -pthread -g -I RakNet/Source raknet_proxy.cpp RakNet/Source/*.cpp -o raknet_proxy -fpermissive`
- [x] 运行时: 多客户端打洞后均能转发数据
- [x] 运行时: 不同客户端在目标服务器端显示不同 GUID
- [x] 运行时: Ctrl+C 后所有子 peer 被 Shutdown + DestroyInstance

### Must Have
- 多客户端支持（N 个客户端各自独立转发）
- GUID 伪装（子 peer 用客户端 GUID 连接目标服务器）
- 客户端断开时清理对应子 peer
- SIGINT 信号处理 → 优雅关闭
- 静默转发（无包数据打印，无"转发完成"日志）

### Must NOT Have (Guardrails)
- 不要修改 RakNet 源文件
- 不要 per-client 目标服务器（共享一个目标）
- 不要多线程（保持单线程 poll）
- 不要配置文件支持
- 不要加密/安全特性
- 不要给子 peer 附加 NatPunchthroughClient 插件（只主 peer 需要）
- 不要过度抽象（不需要 Forwarder 类，不需要单独头文件）
- 不要添加统计/计数打印

---

## Verification Strategy

> **ZERO HUMAN INTERVENTION** - ALL verification is agent-executed.

### Test Decision
- **Infrastructure exists**: NO
- **Automated tests**: None (no framework, no CI)
- **Framework**: None needed — verification via compile + runtime behavior check
- **QA**: Agent-executed curl/tmux scenarios

### QA Policy
Every task includes agent-executed QA scenarios. Evidence saved to `.sisyphus/evidence/task-{N}-{scenario-slug}.{ext}`.

- **API/Backend**: Use Bash (g++ build + process launch + signal test)
- **CLI**: Use interactive_bash (tmux) for runtime behavior verification

---

## Execution Strategy

### Parallel Execution Waves

```
Wave 1 (Start Immediately - data structures + cleanup prep):
├── Task 1: Remove PrintPacketData and reduce log verbosity [quick]
├── Task 2: Add ClientContext struct + client map + cleanup helpers [quick]
└── Task 3: Add SIGINT signal handler for graceful shutdown [quick]

Wave 2 (After Wave 1 - core lifecycle logic):
├── Task 4: Refactor ID_NEW_INCOMING_CONNECTION → create child peer + ClientContext [unspecified-high]
├── Task 5: Refactor ID_CONNECTION_LOST / ID_DISCONNECTION → cleanup child peer [unspecified-high]
└── Task 6: Implement per-client data forwarding logic [unspecified-high]

Wave 3 (After Wave 2 - integrate + polish):
├── Task 7: Rewrite main poll loop for N+1 peers [unspecified-high]
├── Task 8: Handle ID_CONNECTION_ATTEMPT_FAILED on child peers [quick]
└── Task 9: Final cleanup: remove dead code, verify all printf levels [quick]

Critical Path: Task 1 → Task 4 → Task 6 → Task 7 → Task 9
Parallel Speedup: ~50% (Wave 1 = 3 parallel, Wave 2 = 3 parallel)
Max Concurrent: 3
```

### Dependency Matrix

- **1, 2, 3**: - - 4-9, 1 (all three independent)
- **4**: 2 - 6, 7, 2
- **5**: 2 - 7, 2
- **6**: 2, 4 - 7, 2
- **7**: 1, 2, 3, 4, 5, 6 - 9, 3
- **8, 9**: 7 - -, 3

### Agent Dispatch Summary

- **Wave 1**: 3 tasks → `quick` (straightforward code removal/addition)
- **Wave 2**: 3 tasks → `unspecified-high` (logic-heavy, need careful RakNet API usage)
- **Wave 3**: 3 tasks → `quick` + `unspecified-high`

---

## TODOs

- [x] 1. Remove PrintPacketData and reduce log verbosity

  **What to do**:
  - Delete `PrintPacketData()` function entirely (lines 99-108)
  - Rewrite `ID_NEW_INCOMING_CONNECTION`: shorten to one line `printf("[客户端] %s (GUID: %s)\n", sender, guid)`
  - Rewrite `ID_DISCONNECTION_NOTIFICATION`: shorten to one line
  - Rewrite `ID_CONNECTION_LOST`: shorten to one line
  - Rewrite `ID_NAT_PUNCHTHROUGH_SUCCEEDED`: shorten to one line with GUID
  - Rewrite `ID_NAT_PUNCHTHROUGH_FAILED`: shorten to one line
  - Rewrite other NAT error cases: keep brief
  - Rewrite `default` forwarding branch: remove per-packet prints entirely, just do the Send silently
  - Keep startup/configuration prints (lines 131-143) and fatal error prints
  - Keep "服务器已关闭" and "NAT打洞客户端插件已附加" messages
  - Remove `#include <iostream>` (unused) — keep only needed headers

  **Must NOT do**:
  - Do not remove connection/disconnection/punchthrough result logs
  - Do not remove startup info or error messages
  - Do not change header includes beyond removing unused ones

  **Recommended Agent Profile**:
  > Editor-heavy refactoring. Quick profile for straightforward code removal.
  - **Category**: `quick`
  - **Skills**: []
  - **Skills Evaluated but Omitted**: (none needed — pure text editing)

  **Parallelization**:
  - **Can Run In Parallel**: YES
  - **Parallel Group**: Wave 1 (with Tasks 2, 3)
  - **Blocks**: Task 4, Task 7 (they reference log lines being rewritten)
  - **Blocked By**: None

  **References**:
  - `raknet_proxy.cpp:99-108` — `PrintPacketData()` function to delete
  - `raknet_proxy.cpp:131-143` — startup prints to KEEP
  - `raknet_proxy.cpp:217-425` — switch statement with all case blocks to rewrite

  **Acceptance Criteria**:
  - [ ] `PrintPacketData` function removed from file
  - [ ] `#include <iostream>` removed (if unused after other changes)
  - [ ] `printf("  转发完成\n")` removed from all branches
  - [ ] `printf("[#%d] 收到用户数据来自: ...")` removed from forwarding path
  - [ ] `printf("  转发客户端数据到目标服务器 ...")` removed from forwarding path
  - [ ] Startup banner, error messages, connect/disconnect events still present
  - [ ] All NAT punchthrough result messages still logged (one line each)

  **QA Scenarios**:

  ```
  Scenario: Build verifies PrintPacketData is gone
    Tool: Bash
    Preconditions: raknet_proxy.cpp edited
    Steps:
      1. Run: g++ -c -lpthread -g -I RakNet/Source raknet_proxy.cpp -o raknet_proxy.o
      2. Assert exit code 0
      3. Run: grep -c "PrintPacketData" raknet_proxy.cpp
    Expected Result: grep returns 0 (no matches)
    Evidence: .sisyphus/evidence/task-1-no-printpacketdata.txt

  Scenario: Build verifies "转发完成" gone
    Tool: Bash
    Preconditions: raknet_proxy.cpp edited
    Steps:
      1. Run: grep -c "转发完成" raknet_proxy.cpp
    Expected Result: grep returns 0
    Evidence: .sisyphus/evidence/task-1-no-forward-done.txt
  ```

  **Commit**: NO (part of combined commit)

- [x] 2. Add ClientContext struct, client map, and cleanup helpers

  **What to do**:
  - Add `#include <map>` at top
  - Add `#include <signal.h>` at top (for SIGINT)
  - Define `ClientContext` struct before `main()`:
    ```cpp
    struct ClientContext {
        RakNet::RakPeerInterface* childPeer;
        RakNet::RakNetGUID clientGuid;
        RakNet::SystemAddress clientAddress;
        RakNet::SystemAddress targetAddress;
    };
    ```
  - Replace `RakNet::SystemAddress clientAddress` singleton with:
    ```cpp
    std::map<RakNet::RakNetGUID, ClientContext> clients;
    ```
  - Keep `RakNet::SystemAddress targetServerAddress` — only set when the first client's child peer successfully connects to target
  - Add helper function before `main()`:
    ```cpp
    void DestroyClient(RakNet::RakPeerInterface* childPeer) {
        if (childPeer) {
            childPeer->Shutdown(300);
            RakNet::RakPeerInterface::DestroyInstance(childPeer);
        }
    }
    ```
  - Add helper function to find client by address:
    ```cpp
    RakNet::RakNetGUID FindClientByAddress(const RakNet::SystemAddress& addr) {
        for (auto& pair : g_clients) {
            if (pair.second.clientAddress == addr) return pair.first;
        }
        return RakNet::UNASSIGNED_RAKNET_GUID;
    }
    ```

  **Must NOT do**:
  - Do not remove `targetServerAddress` variable — still needed as fallback
  - Do not define `ClientContext` in a separate header file
  - Do not over-engineer: no class, no destructor, no RAII wrapper

  **Recommended Agent Profile**:
  > Straightforward struct + map + helpers. Quick profile.
  - **Category**: `quick`
  - **Skills**: []

  **Parallelization**:
  - **Can Run In Parallel**: YES
  - **Parallel Group**: Wave 1 (with Tasks 1, 3)
  - **Blocks**: Task 4, 5, 6
  - **Blocked By**: None

  **References**:
  - `raknet_proxy.cpp:1-16` — current includes to extend
  - `raknet_proxy.cpp:208-212` — current variable declarations to replace
  - `RakNet/Source/RakNetTypes.h` — `RakNetGUID` type reference

  **Acceptance Criteria**:
  - [ ] `ClientContext` struct defined with all 4 fields
  - [ ] `std::map<RakNet::RakNetGUID, ClientContext> clients` declared
  - [ ] `DestroyClient(peer)` helper function exists
  - [ ] Old `RakNet::SystemAddress clientAddress` removed
  - [ ] Compiles cleanly: `g++ -c -lpthread -g -I RakNet/Source raknet_proxy.cpp`

  **QA Scenarios**:

  ```
  Scenario: Compile with new data structures
    Tool: Bash
    Preconditions: Task 1 complete
    Steps:
      1. Run: g++ -c -lpthread -g -I RakNet/Source raknet_proxy.cpp -o raknet_proxy.o
      2. Assert exit code 0
    Expected Result: Compile succeeds, no warnings about unused variables
    Evidence: .sisyphus/evidence/task-2-compile.txt
  ```

  **Commit**: NO (part of combined commit)

- [x] 3. Add SIGINT signal handler for graceful shutdown

  **What to do**:
  - Add `volatile bool g_running = true;` before `main()`
  - Add `void SignalHandler(int) { g_running = false; }` before `main()`
  - In `main()`, after variable declarations: `signal(SIGINT, SignalHandler);`
  - Change `while (true)` to `while (g_running)`
  - After the while loop (before shutdown section): iterate `clients` map and call `DestroyClient()` for each child peer
  - Keep the existing main peer shutdown code after client cleanup

  **Must NOT do**:
  - Do not use `sigaction` — `signal()` is sufficient for Ctrl+C
  - Do not add cleanup for `natClient` in the signal handler itself (do it after the loop)

  **Recommended Agent Profile**:
  > Simple signal handling + loop change. Quick profile.
  - **Category**: `quick`
  - **Skills**: []

  **Parallelization**:
  - **Can Run In Parallel**: YES
  - **Parallel Group**: Wave 1 (with Tasks 1, 2)
  - **Blocks**: Task 7 (poll loop depends on g_running)
  - **Blocked By**: None

  **References**:
  - `raknet_proxy.cpp:201` — `while (true)` to change to `while (g_running)`
  - `raknet_proxy.cpp:414-437` — shutdown section to extend with client cleanup
  - `<signal.h>` — POSIX signal API

  **Acceptance Criteria**:
  - [ ] `volatile bool g_running = true;` declared
  - [ ] `signal(SIGINT, SignalHandler)` called
  - [ ] `while (g_running)` replaces `while (true)`
  - [ ] Shutdown section iterates clients and destroys child peers
  - [ ] Compiles cleanly

  **QA Scenarios**:

  ```
  Scenario: Verify signal handler compiles
    Tool: Bash
    Preconditions: Task 1, 2 complete
    Steps:
      1. Run: g++ -c -lpthread -g -I RakNet/Source raknet_proxy.cpp -o raknet_proxy.o
      2. Assert exit code 0
    Expected Result: Compile succeeds
    Evidence: .sisyphus/evidence/task-3-compile.txt
  ```

  **Commit**: NO (part of combined commit)

- [x] 4. Refactor ID_NEW_INCOMING_CONNECTION — create child peer + ClientContext

  **What to do**:
  - Remove old IP-based target server detection (`if (senderAddress == expectedTargetAddr)`)
  - On any incoming connection (except NAT server reconnection):
    a. Read `packet->guid` — this is the client's GUID from the connection handshake
    b. Create child peer: `RakNet::RakPeerInterface* childPeer = RakNet::RakPeerInterface::GetInstance()`
    c. Call `childPeer->SetMyGUID(packet->guid)` — impersonate client GUID
    d. Call `childPeer->SetMaximumIncomingConnections(0)` — reject incoming on child
    e. Call `childPeer->Startup(1, &sd, 1)` where `SocketDescriptor sd(0, 0)` (OS port)
    f. Call `childPeer->Connect(args.targetServerIP.c_str(), args.targetServerPort, 0, 0)`
    g. Create `ClientContext ctx{childPeer, packet->guid, senderAddress, RakNet::UNASSIGNED_SYSTEM_ADDRESS}`
    h. Insert into map: `clients[packet->guid] = ctx`
    i. Print: `printf("[客户端] %s GUID:%s 已连接\n", sender, guid)`
  - On `ID_CONNECTION_REQUEST_ACCEPTED` for main peer: only NAT server case remains. Target server detection moves to child peer's `ID_CONNECTION_REQUEST_ACCEPTED`.

  **Must NOT do**:
  - Do NOT create child peer when connecting to NAT server (check `strcmp(ip, natServerIP)`)
  - Do NOT call `AttachPlugin` on child peer
  - Do NOT reuse old `clientAddress` singleton

  **Recommended Agent Profile**:
  > Logic-heavy refactoring of core connection handler. Needs careful RakNet API usage.
  - **Category**: `unspecified-high`
  - **Skills**: []

  **Parallelization**:
  - **Can Run In Parallel**: NO (serial with Tasks 5, 6 but they all work on different case blocks)
  - **Parallel Group**: Wave 2 (with Tasks 5, 6 — different switch cases, can run in parallel)
  - **Blocks**: Task 6, 7
  - **Blocked By**: Task 2 (ClientContext struct)

  **References**:
  - `raknet_proxy.cpp:217-235` — current `ID_NEW_INCOMING_CONNECTION` to refactor
  - `raknet_proxy.cpp:152-171` — existing `SetMyGUID` + `Startup` pattern to follow for child peer
  - `RakNet/Source/RakPeerInterface.h:394` — `GetMyGUID()` API
  - `RakNet/Source/RakPeer.h:958` — `myGuid` member for `SetMyGUID`
  - `RakNet/Source/RakNetTypes.h:154` — `port=0` autoassign note

  **Acceptance Criteria**:
  - [ ] On `ID_NEW_INCOMING_CONNECTION`, new `ClientContext` inserted into `clients` map
  - [ ] Child peer calls `SetMyGUID(packet->guid)` before `Startup()`
  - [ ] Child peer calls `SetMaximumIncomingConnections(0)`
  - [ ] Child peer uses `SocketDescriptor(0, 0)` for auto-assigned port
  - [ ] Child peer calls `Connect(targetServerIP, targetServerPort)`
  - [ ] NAT server connection (`ID_CONNECTION_REQUEST_ACCEPTED`) still works (no child peer created)
  - [ ] Old `expectedTargetAddr` check removed from `ID_NEW_INCOMING_CONNECTION`

  **QA Scenarios**:

  ```
  Scenario: Compile verifies new connection handler
    Tool: Bash
    Preconditions: Task 1, 2 complete
    Steps:
      1. Run: g++ -c -lpthread -g -I RakNet/Source raknet_proxy.cpp -o raknet_proxy.o
      2. Assert exit code 0
    Expected Result: Compile succeeds, no undefined symbols
    Evidence: .sisyphus/evidence/task-4-compile.txt

  Scenario: Verify no stale references to clientAddress singleton
    Tool: Bash (grep)
    Steps:
      1. Run: grep -n "clientAddress\b" raknet_proxy.cpp | grep -v "ctx.clientAddress\|// clientAddress\|ClientContext"
      2. Assert: no matches (all old singleton usages replaced)
    Expected Result: No old singleton references remain
    Evidence: .sisyphus/evidence/task-4-no-singleton.txt
  ```

  **Commit**: NO (part of combined commit)

- [x] 5. Refactor disconnect handlers — cleanup child peer

  **What to do**:
  - In `ID_DISCONNECTION_NOTIFICATION`:
    a. Check if `senderAddress` matches any client's `clientAddress` in `clients` map
    b. If found: `DestroyClient(ctx.childPeer)`, remove from map, print disconnect log
    c. If NOT found in clients map: could be a disconnected child peer (which already removed itself) — ignore or print brief message
  - In `ID_CONNECTION_LOST`:
    a. Same lookup as above
    b. If found: destroy and remove
    c. Also check if `senderAddress == targetServerAddress` (target server lost) — log error but don't take action (save targetServerAddress as "invalid")

  **Must NOT do**:
  - Do not crash if client not found in map (defensive coding)
  - Do not remove the target server address variable

  **Recommended Agent Profile**:
  > Logic for correct cleanup paths. Unspecified-high for careful state management.
  - **Category**: `unspecified-high`
  - **Skills**: []

  **Parallelization**:
  - **Can Run In Parallel**: YES (with Tasks 4, 6 — different switch cases)
  - **Parallel Group**: Wave 2 (with Tasks 4, 6)
  - **Blocks**: Task 7
  - **Blocked By**: Task 2 (ClientContext struct, DestroyClient helper, clients map)

  **References**:
  - `raknet_proxy.cpp:238-253` — current disconnect handlers to refactor
  - `raknet_proxy.cpp` — `DestroyClient()` helper (defined in Task 2)

  **Acceptance Criteria**:
  - [ ] Client disconnect → child peer Shutdown + DestroyInstance called
  - [ ] Client removed from `clients` map after disconnect
  - [ ] Client disconnect printed as single line with GUID
  - [ ] Target server disconnect logged as warning
  - [ ] No crash when unknown address disconnects

  **QA Scenarios**:

  ```
  Scenario: Verify cleanup logic compiles
    Tool: Bash
    Preconditions: Tasks 1-4 complete
    Steps:
      1. Run: g++ -c -lpthread -g -I RakNet/Source raknet_proxy.cpp -o raknet_proxy.o
      2. Assert exit code 0
    Expected Result: Compile succeeds
    Evidence: .sisyphus/evidence/task-5-compile.txt
  ```

  **Commit**: NO (part of combined commit)

- [x] 6. Implement per-client data forwarding logic

  **What to do**:
  - Rewrite `default` case (user data forwarding):
    a. Determine source: iterate `clients` map to find matching `clientAddress` or `targetAddress`
    b. If source is a client (`senderAddress == ctx.clientAddress`):
       - Forward to `ctx.targetAddress` (child peer's connection to target server)
       - Send via `ctx.childPeer` (not main peer!)
       - Strip 1-byte message ID header
    c. If source is target server (`senderAddress == ctx.targetAddress`):
       - Find which client's child peer this came from
       - Forward to `ctx.clientAddress` via main peer
       - Strip 1-byte message ID header
    d. For child peer `ID_CONNECTION_REQUEST_ACCEPTED`: update `ctx.targetAddress = senderAddress`
    e. For child peer receive (in main loop): route to forwarding logic
  - Data path: `client → main peer → child peer → target server` (and reverse)
  - On child peer `Receive()` returning data: that's target → client direction. Forward via main peer to `ctx.clientAddress`

  **Must NOT do**:
  - Do not print per-packet forwarding messages
  - Do not call `PrintPacketData()` (already removed in Task 1)
  - Do not forward non-user-data (check `packetID >= ID_USER_PACKET_ENUM`)

  **Recommended Agent Profile**:
  > Most complex logic — multi-directional forwarding with multiple peer instances.
  - **Category**: `unspecified-high`
  - **Skills**: []

  **Parallelization**:
  - **Can Run In Parallel**: YES (with Tasks 4, 5 — different code paths)
  - **Parallel Group**: Wave 2 (with Tasks 4, 5)
  - **Blocks**: Task 7
  - **Blocked By**: Task 2 (clients map), Task 4 (child peer creation pattern)

  **References**:
  - `raknet_proxy.cpp:347-410` — current `default` case (old user data forwarding) to replace
  - `raknet_proxy.cpp:264-285` — `ID_CONNECTION_REQUEST_ACCEPTED` to update for child peer target detection
  - `RakNet/Source/RakPeerInterface.h:194-198` — `Send()` API for forwarding

  **Acceptance Criteria**:
  - [ ] Client → target forwarding works via child peer `Send()`
  - [ ] Target → client forwarding works via main peer `Send()`
  - [ ] `ctx.targetAddress` updated on child peer's `ID_CONNECTION_REQUEST_ACCEPTED`
  - [ ] Only user-data IDs (>= ID_USER_PACKET_ENUM) forwarded
  - [ ] No per-packet log output in forwarding path
  - [ ] Message ID header stripped before forwarding

  **QA Scenarios**:

  ```
  Scenario: Compile verifies forwarding logic
    Tool: Bash
    Preconditions: Tasks 1-5 complete
    Steps:
      1. Run: g++ -c -lpthread -g -I RakNet/Source raknet_proxy.cpp -o raknet_proxy.o
      2. Assert exit code 0
    Expected Result: Compile succeeds
    Evidence: .sisyphus/evidence/task-6-compile.txt
  ```

  **Commit**: NO (part of combined commit)

- [x] 7. Rewrite main poll loop for N+1 peers

  **What to do**:
  - Replace current single-peer `Receive()` with multi-peer poll:
    ```cpp
    // 1. Poll main peer
    RakNet::Packet* packet = peer->Receive();
    if (packet) { /* handle main peer events (switch) */ }

    // 2. Poll each child peer
    for (auto& pair : clients) {
        ClientContext& ctx = pair.second;
        RakNet::Packet* cp = ctx.childPeer->Receive();
        if (cp) {
            // Determine source: cp came from child peer → forwarded data from target server
            // Forward to client via main peer
            unsigned char id = GetPacketIdentifier(cp);
            if ((unsigned char)id >= ID_USER_PACKET_ENUM) {
                peer->Send((const char*)(cp->data + 1), cp->length - 1,
                           HIGH_PRIORITY, RELIABLE_ORDERED, 0,
                           ctx.clientAddress, false);
            }
            ctx.childPeer->DeallocatePacket(cp);
        }
    }
    ```
  - On child peer `ID_CONNECTION_REQUEST_ACCEPTED` (detected in child peer poll): update `ctx.targetAddress`
  - On child peer `ID_CONNECTION_ATTEMPT_FAILED`: log error, destroy client
  - Delete old single-client forwarding code (the `default` case that checks `isFromClient`/`isFromTarget`)
  - Move main peer `default` case logic into a function call for readability
  - Ensure `RakSleep(30)` still called when no packets on any peer

  **Must NOT do**:
  - Do NOT block on `Receive()` — all calls are non-blocking (return NULL if no packet)
  - Do NOT deallocate packets from main peer in child peer handler
  - Do NOT modify `clients` map during iteration (use post-iteration cleanup)

  **Recommended Agent Profile**:
  > Multi-peer orchestration — the core integration point. Needs careful state management.
  - **Category**: `unspecified-high`
  - **Skills**: []

  **Parallelization**:
  - **Can Run In Parallel**: NO (depends on all previous tasks)
  - **Parallel Group**: Wave 3 (depends on Tasks 1-6)
  - **Blocks**: Tasks 8, 9
  - **Blocked By**: Tasks 1, 2, 3, 4, 5, 6

  **References**:
  - `raknet_proxy.cpp:201-408` — current main loop to replace
  - `raknet_proxy.cpp:110-122` — `GetPacketIdentifier()` for child peer packets
  - `RakNet/Source/RakPeerInterface.h` — `Receive()`, `Send()`, `DeallocatePacket()` APIs

  **Acceptance Criteria**:
  - [ ] Main peer `Receive()` polled first
  - [ ] Each child peer `Receive()` polled in for loop
  - [ ] Child peer packets forwarded to correct client via main peer
  - [ ] `RakSleep(30)` when all peers return NULL
  - [ ] No blocking calls in the loop
  - [ ] Old singleton-based forwarding code removed
  - [ ] Compiles and links cleanly

  **QA Scenarios**:
  ```
  Scenario: Full build compiles
    Tool: Bash
    Preconditions: Tasks 1-6 complete
    Steps:
      1. Run: g++ -c -lpthread -g -I RakNet/Source raknet_proxy.cpp -o raknet_proxy.o
      2. Assert exit code 0
      3. Run: g++ -lpthread -g -fpermissive -I RakNet/Source raknet_proxy.o RakNet/Source/*.cpp -o raknet_proxy
      4. Assert exit code 0
    Expected Result: Full link succeeds
    Evidence: .sisyphus/evidence/task-7-full-build.txt

  Scenario: Binary contains no PrintPacketData
    Tool: Bash
    Steps:
      1. Run: strings raknet_proxy | grep -c "PrintPacketData"
    Expected Result: Returns 0
    Evidence: .sisyphus/evidence/task-7-no-printpacketdata.txt
  ```

  **Commit**: NO (part of combined commit)

- [x] 8. Handle ID_CONNECTION_ATTEMPT_FAILED on child peers

  **What to do**:
  - In child peer poll loop, add handling for `ID_CONNECTION_ATTEMPT_FAILED`:
    - Log error: `printf("[错误] 子Peer连接目标服务器失败: GUID:%s\n", guid)`
    - Call `DestroyClient(ctx.childPeer)`
    - Remove from `clients` map
    - Also disconnect the corresponding client from main peer: `peer->CloseConnection(ctx.clientAddress, true)`
  - Add handling for `ID_ALREADY_CONNECTED` on child peer (ignore, rare but harmless)
  - Add handling for `ID_NO_FREE_INCOMING_CONNECTIONS` on child peer (should not happen with `SetMaximumIncomingConnections(0)`, but log if it does)

  **Must NOT do**:
  - Do not crash if child peer receives unexpected message IDs
  - Do not leave orphaned child peers on connection failure

  **Recommended Agent Profile**:
  > Edge case handling in child peer poll loop. Quick profile.
  - **Category**: `quick`
  - **Skills**: []

  **Parallelization**:
  - **Can Run In Parallel**: YES (with Task 9)
  - **Parallel Group**: Wave 3 (with Task 9)
  - **Blocks**: None
  - **Blocked By**: Task 7 (child peer poll loop)

  **References**:
  - `RakNet/Source/MessageIdentifiers.h:112-113` — `ID_CONNECTION_ATTEMPT_FAILED` docs
  - `raknet_proxy.cpp` — `DestroyClient()` helper from Task 2

  **Acceptance Criteria**:
  - [ ] `ID_CONNECTION_ATTEMPT_FAILED` on child peer → client disconnected + cleaned up
  - [ ] Error message printed with GUID
  - [ ] No memory leak (child peer destroyed)
  - [ ] No crash on unexpected child peer messages

  **QA Scenarios**:
  ```
  Scenario: Compile with error handling
    Tool: Bash
    Steps:
      1. Run: g++ -c -lpthread -g -I RakNet/Source raknet_proxy.cpp -o raknet_proxy.o
      2. Assert exit code 0
    Expected Result: Compile succeeds
    Evidence: .sisyphus/evidence/task-8-compile.txt
  ```

  **Commit**: NO (part of combined commit)

- [x] 9. Final cleanup — dead code removal and printf audit

  **What to do**:
  - Remove any remaining references to old singletons (`clientAddress` except in ClientContext usage)
  - Remove `expectedTargetAddr` variable if no longer used
  - Verify all `printf` calls are at error/key-event level (not per-packet)
  - Verify `#include` list is minimal:
    - Keep: `<stdio.h>`, `<stdlib.h>`, `<string.h>`, `<cstdint>`, `<string>`, `<map>`, `<signal.h>`
    - Keep: `"RakPeerInterface.h"`, `"MessageIdentifiers.h"`, `"BitStream.h"`, `"RakNetTypes.h"`, `"RakSleep.h"`, `"NatPunchthroughClient.h"`
    - Remove: `<iostream>` (if unused)
  - Add a comment header block at top describing multi-client architecture
  - Run final full build

  **Must NOT do**:
  - Do not add new features
  - Do not refactor working code sections
  - Do not change the command-line argument parsing

  **Recommended Agent Profile**:
  > Final polish — cleanup only. Quick profile.
  - **Category**: `quick`
  - **Skills**: []

  **Parallelization**:
  - **Can Run In Parallel**: YES (with Task 8)
  - **Parallel Group**: Wave 3 (with Task 8)
  - **Blocks**: None
  - **Blocked By**: Task 7

  **References**:
  - Entire `raknet_proxy.cpp` — final audit pass

  **Acceptance Criteria**:
  - [ ] No unused variables or singletons
  - [ ] No `PrintPacketData` reference anywhere
  - [ ] All `printf` calls at appropriate level (no per-packet spam)
  - [ ] Header includes are minimal and correct
  - [ ] Full build passes

  **QA Scenarios**:
  ```
  Scenario: Full build with all tasks
    Tool: Bash
    Steps:
      1. Run: g++ -c -lpthread -g -I RakNet/Source raknet_proxy.cpp -o raknet_proxy.o
      2. Run: g++ -lpthread -g -fpermissive -I RakNet/Source raknet_proxy.o RakNet/Source/*.cpp -o raknet_proxy
      3. Assert both exit code 0
      4. Run: strings raknet_proxy | grep -cE "PrintPacketData|转发完成|转发客户端数据|转发目标服务器数据"
      5. Assert: all return 0
    Expected Result: Clean build, no verbose forwarding strings in binary
    Evidence: .sisyphus/evidence/task-9-final-build.txt
  ```

  **Commit**: YES (combined with all tasks)
  - Message: `refactor(proxy): multi-client support with GUID impersonation`
  - Files: `raknet_proxy.cpp`

---

## Final Verification Wave

> ALL 4 agents run in PARALLEL. ALL must APPROVE. Present consolidated results to user and get explicit "okay".

- [x] F1. **Plan Compliance Audit** — `oracle`
  Read the plan end-to-end. For each "Must Have": verify implementation exists. For each "Must NOT Have": search for forbidden patterns. Check evidence files exist in .sisyphus/evidence/.
  Output: `Must Have [N/N] | Must NOT Have [N/N] | Tasks [N/N] | VERDICT: APPROVE/REJECT`

- [x] F2. **Code Quality Review** — `unspecified-high`
  Run `g++ -c -lpthread -g -I RakNet/Source raknet_proxy.cpp -o raknet_proxy.o`. Check for: `as any`/`@ts-ignore`, empty catches, commented-out code, unused variables. Check AI slop: excessive comments, over-abstraction.
  Output: `Build [PASS/FAIL] | Files [N clean/N issues] | VERDICT`

- [x] F3. **Real Manual QA** — `unspecified-high`
  Build full binary. Launch proxy with tmux in background. Verify SIGINT shutdown. Verify no PrintPacketData called.
  Output: `Shutdown [PASS/FAIL] | Silent [PASS/FAIL] | VERDICT`

- [x] F4. **Scope Fidelity Check** — `deep`
  For each task: read "What to do", read actual diff. Verify 1:1 — everything in spec was built, nothing beyond spec. Check "Must NOT do" compliance. Flag unaccounted changes.
  Output: `Tasks [N/N compliant] | Contamination [CLEAN/N issues] | VERDICT`

---

## Commit Strategy

- **All tasks combined**: `refactor(proxy): multi-client support with GUID impersonation` - raknet_proxy.cpp

---

## Success Criteria

### Verification Commands
```bash
# Compile check
g++ -c -lpthread -g -I RakNet/Source raknet_proxy.cpp -o raknet_proxy.o

# Full link
g++ --std=c++11 -pthread -g -I RakNet/Source raknet_proxy.cpp RakNet/Source/*.cpp -o raknet_proxy -fpermissive

# No PrintPacketData in binary
strings raknet_proxy | grep -c PrintPacketData  # Expected: 0

# No "转发完成" in binary
strings raknet_proxy | grep -c "转发完成"       # Expected: 0
```

### Final Checklist
- [x] All "Must Have" present
- [x] All "Must NOT Have" absent
- [x] Build passes
- [x] PrintPacketData absent from binary
