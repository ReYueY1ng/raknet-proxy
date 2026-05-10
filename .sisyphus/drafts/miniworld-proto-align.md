# 修改 raknet_proxy UDPProxy 逻辑 — 对齐 MiniWorld 协议

## TL;DR

> **目标**: 移除标准 `UDPProxyServer` 插件，改为自定义处理 MiniWorld 的 `ID_UDP_PROXY_GENERAL` (0x5C) 消息
> **关键变更**: 在 `onReceive` 中捕获 `0x5C`，解析子消息 `0x1E`/`0x20`+GUID，注册客户端，保持现有转发逻辑
> **预计影响**: 替换约 30 行，新增约 60 行

---

## Context

### MiniWorld 自定义 UDPProxy 协议

```
客户端注册: 5c 1e [RakNetGUID:8B]
主机注册:   5c 20 [RakNetGUID1:8B] [RakNetGUID2:8B]
```

- 消息在 RakNet 握手完成后立即发送
- Sub-ID `0x1E` (30) 和 `0x20` (32) 硬编码，无枚举名
- GUID 使用 `ReverseBytes` 处理后写入 BitStream（小端序）

### 当前 raknet_proxy 的问题

- 使用标准 `UDPProxyServer` 插件 → `LoginToCoordinator()` 
- MiniWorld 不使用标准 UDPProxy 协议
- 子消息 `0x1E`/`0x20` 不被识别 → 走到 `default` → 被当作未知消息ID

---

## Work Objectives

### 核心目标
移除 `UDPProxyServer` 插件，实现自定义 `0x5C` 消息处理，使 raknet_proxy 能正确响应 MiniWorld 客户端的代理注册。

### 具体变更

1. **`#include "UDPProxyServer.h"` → 删除**
2. **新增 MiniWorld 子消息枚举**
3. **`ID_UDP_PROXY_GENERAL` case** 加入主 switch
4. **解析 `5c 1e` + GUID** → 注册客户端
5. **移除 Coordinator 登录逻辑**（`LoginToCoordinator` 调用）
6. **移除 UDPProxyServer 清理代码**

### 不需要改的

- NAT 打洞逻辑（MiniWorld 仍然使用标准 NAT punch）
- 子 Peer 创建和转发（数据转发机制不变）
- `ClientContext` 结构（已有 GUID 和地址）

---

## TODOs

- [ ] 1. 删除 `#include "UDPProxyServer.h"`，添加 MiniWorld 子消息常量

  **What to do**:
  - 删除 `#include "UDPProxyServer.h"` (第18行)
  - 添加自定义消息常量:
    ```cpp
    // MiniWorld custom UDPProxy sub-messages
    const unsigned char MW_PROXY_CLIENT_REGISTER = 0x1E;  // client → proxy: 5c 1e [GUID]
    const unsigned char MW_PROXY_HOST_REGISTER   = 0x20;  // host   → proxy: 5c 20 [GUID1] [GUID2]
    ```

  **QA Scenarios**:
  - 编译通过: `g++ -std=c++11 -pthread -g -I RakNet/Source raknet_proxy.cpp RakNet/Source/*.cpp -o raknet_proxy -fpermissive`

  **Commit**: `refactor(raknet_proxy): remove UDPProxyServer include, add MiniWorld sub-message constants`

- [ ] 2. 移除 `UDPProxyServer*` 变量和 `LoginToCoordinator` 调用

  **What to do**:
  - 删除 `RakNet::UDPProxyServer* udpProxyServer = ...` 和 `peer->AttachPlugin(udpProxyServer)` (第300-301行)
  - 删除 `ID_CONNECTION_REQUEST_ACCEPTED` 中的 Coordinator 登录逻辑 (第436-441行):
    ```cpp
    // 删除:
    else if (!args.coordinatorIP.empty() && ...) {
        coordinatorAddress = senderAddress;
        printf("[Coordinator] 连接成功\n");
        printf("[Coordinator] 正在登录...\n");
        udpProxyServer->LoginToCoordinator("", coordinatorAddress);
    }
    ```
  - 删除清理代码中的 `RakNet::UDPProxyServer::DestroyInstance(udpProxyServer)` (第453行)

  **QA Scenarios**:
  - 编译通过
  - 代码中不再出现 `udpProxyServer` 或 `UDPProxyServer`

  **Commit**: `refactor(raknet_proxy): remove UDPProxyServer plugin usage`

- [ ] 3. 添加 `ID_UDP_PROXY_GENERAL` (0x5C) 消息处理

  **What to do**:
  - 在 `ID_NAT_ALREADY_IN_PROGRESS` case 之后，`default` case 之前，添加:
    ```cpp
    case ID_UDP_PROXY_GENERAL: {
        if (packet->length < 3) break; // need sub-ID + at least 1 byte data
        unsigned char subID = packet->data[1];
        
        if (subID == MW_PROXY_CLIENT_REGISTER && packet->length >= 10) {
            // 解析 GUID (8 bytes at offset 2)
            uint64_t guidValue;
            memcpy(&guidValue, packet->data + 2, 8);
            // Note: GUID bytes are reversed (little-endian stored)
            // ReverseBytes was used when writing, so we need to reverse back
            // Actually the GUID is 8 bytes in network byte order after reversal
            RakNet::RakNetGUID clientGuid;
            RakNet::BitStream bs(packet->data + 2, 8, false);
            bs.Read(clientGuid);
            
            printf("[MiniWorld] 客户端注册 GUID:%s 来自:%s\n", 
                   clientGuid.ToString(), senderAddress.ToString(true));
            
            // Trigger child peer creation (same as ID_NEW_INCOMING_CONNECTION)
            // ... existing child peer logic ...
        }
        else if (subID == MW_PROXY_HOST_REGISTER && packet->length >= 18) {
            RakNet::BitStream bs(packet->data + 2, 16, false);
            RakNet::RakNetGUID guid1, guid2;
            bs.Read(guid1);
            bs.Read(guid2);
            printf("[MiniWorld] 主机注册 GUID1:%s GUID2:%s 来自:%s\n",
                   guid1.ToString(), guid2.ToString(), senderAddress.ToString(true));
        }
        else {
            printf("[MiniWorld] 未知子消息 0x%02X 来自:%s\n", subID, senderAddress.ToString(true));
        }
        break;
    }
    ```

  **QA Scenarios**:
  - 编译通过
  - 使用 MiniWorld 客户端连接时，应打印 `[MiniWorld] 客户端注册 GUID:xxx 来自:xxx`

  **Commit**: `feat(raknet_proxy): add MiniWorld custom UDPProxy message handling`

- [ ] 4. 将 `ID_UDP_PROXY_GENERAL` 注册触发子 Peer 创建

  **What to do**:
  - 复用现有的子 Peer 创建逻辑 (`ID_NEW_INCOMING_CONNECTION` 中的代码)
  - 提取为独立函数或直接在 `ID_UDP_PROXY_GENERAL` case 中内联:
    ```cpp
    // After parsing clientGuid from 5c 1e message:
    RakNet::RakPeerInterface* childPeer = RakNet::RakPeerInterface::GetInstance();
    childPeer->SetMyGUID(clientGuid);
    childPeer->SetMaximumIncomingConnections(0);
    RakNet::SocketDescriptor sdChild(0, 0);
    childPeer->Startup(1, &sdChild, 1);
    childPeer->Connect(args.targetServerIP.c_str(), args.targetServerPort, 0, 0);
    
    clients[clientGuid] = ClientContext{
        childPeer, clientGuid, senderAddress, RakNet::UNASSIGNED_SYSTEM_ADDRESS
    };
    printf("[MiniWorld] 已分配子Peer, GUID:%s\n", clientGuid.ToString());
    ```

  **QA Scenarios**:
  - MiniWorld 客户端发送 `5c 1e` 后，子 Peer 应连接到目标服务器
  - data 应能双向转发

  **Commit**: `feat(raknet_proxy): create child peer on MiniWorld client registration`

- [ ] 5. 验证编译和基本功能

  **What to do**:
  - 编译: `g++ -std=c++11 -pthread -g -I RakNet/Source raknet_proxy.cpp RakNet/Source/*.cpp -o raknet_proxy -fpermissive`
  - 运行: `./raknet_proxy -local_port 60000 -max_clients 10 -nat_ip <ip> -nat_port 61111 -target_ip <ip> -target_port 7000`
  - 用 MiniWorld 客户端连接验证

  **QA Scenarios**:
  - 编译无错误
  - MiniWorld 客户端连接后打印注册日志
  - 数据转发正常

  **Commit**: `chore(raknet_proxy): verify MiniWorld protocol compatibility`

---

## 变更总结

| 行号 | 变更类型 | 说明 |
|------|----------|------|
| 18 | 删除 | `#include "UDPProxyServer.h"` → 新增常量定义 |
| 300-301 | 删除 | `UDPProxyServer*` + `AttachPlugin` |
| 436-441 | 删除 | `LoginToCoordinator` 调用 |
| 453 | 删除 | `DestroyInstance(udpProxyServer)` |
| 482后 | 新增 | `case ID_UDP_PROXY_GENERAL:` ~60行 |

## 无需删除的

| 保留项 | 原因 |
|--------|------|
| `coordinatorAddress` 变量 | Coordinator 连接本身仍可用 |
| Coordinator `Connect()` 调用 (310行) | 保持连接，自定义协议在数据层 |
| NAT 打洞全部逻辑 | MiniWorld 仍然使用标准 NAT punch |
| `ClientContext` / `clients` map | 现有架构适用于 MiniWorld |
| 子 Peer 转发逻辑 (515-537行) | 不变 |
