// RakNet 数据转发代理 (NAT Punchthrough Multi-Client Proxy)
// 架构: 主 RakPeer (监听+NAT打洞) + 子 RakPeer (每客户端一个,
// GUID伪装连接目标服务器) 数据流: 客户端 → 主Peer → 子Peer → 目标服务器
// (以及反向)
//
// 改进版本: 完整的MiniWorld Proxy协议支持
// - 完整的消息类型处理
// - 连接状态机管理
// - Proxy成功/失败回调
// - 超时处理机制
// - 事件触发系统
//
#include "BitStream.h"
#include "MessageIdentifiers.h"
#include "NatPunchthroughClient.h"
#include "RakNetTypes.h"
#include "RakPeerInterface.h"
#include "RakSleep.h"
#include <csignal>
#include <cstdint>
#include <ctime>
#include <functional>
#include <map>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <string>
#include <vector>

// MiniWorld custom UDPProxy sub-messages
const unsigned char MW_PROXY_CLIENT_REGISTER =
    0x1E; // client → proxy: 5c 1e [GUID] [extra 8 bytes]
const unsigned char MW_PROXY_CLIENT_REGISTER_RESP =
    0x1F; // proxy  → client: 5c 1f (login success)
const unsigned char MW_PROXY_HOST_REGISTER =
    0x20; // host   → proxy: 5c 20 [GUID1] [GUID2]
const unsigned char MW_PROXY_HOST_REGISTER_RESP = 0x21; // proxy  → host:  5c 21
const unsigned char MW_PROXY_CLIENT_AUTH =
    0x22; // client → proxy: 5c 22 [auth_data]
const unsigned char MW_PROXY_HOST_AUTH =
    0x24;                                 // host   → proxy: 5c 24 [auth_data]
const unsigned char MW_PROXY_DATA = 0x30; // data relay
const unsigned char MW_PROXY_PING = 0x32; // ping
const unsigned char MW_PROXY_PONG = 0x34; // pong
const unsigned char MW_PROXY_DISCONNECT = 0x36; // disconnect notify

#include <miniupnpc/miniupnpc.h>
#include <miniupnpc/upnpcommands.h>
#include <miniupnpc/upnperrors.h>

// 自定义消息ID，避免与RakNet内置ID冲突
enum GameMessages {
  ID_USER_DATA = ID_USER_PACKET_ENUM + 1,
};

// 连接状态枚举 (基于逆向分析)
enum ConnectionState {
  STATE_DISCONNECTED = 0,     // 未连接
  STATE_PUNCH_CONNECTING = 1, // Punch连接中
  STATE_PUNCH_CONNECTED = 2,  // Punch连接成功
  STATE_HOST_CONNECTING = 3,  // 主机连接中
  STATE_PROXY_CONNECTING = 4, // 代理连接中
  STATE_PROXY_HANDSHAKE = 5,  // 代理握手/验证
  STATE_CONNECTED = 6,        // 已连接成功
};

// 事件类型枚举
enum EventType {
  EVENT_PROXY_CONNECT_SUCCESS,    // 代理连接成功
  EVENT_PROXY_CONNECT_FAILED,     // 代理连接失败
  EVENT_HOST_CONNECT_SUCCESS,     // 主机连接成功
  EVENT_HOST_CONNECT_FAILED,      // 主机连接失败
  EVENT_CLIENT_DISCONNECTED,      // 客户端断开连接
  EVENT_NAT_PUNCHTHROUGH_SUCCESS, // NAT穿透成功
  EVENT_NAT_PUNCHTHROUGH_FAILED,  // NAT穿透失败
};

// 事件数据结构
struct EventData {
  EventType type;
  RakNet::RakNetGUID guid;
  RakNet::SystemAddress address;
  std::string reason;
  time_t timestamp;
};

// 事件回调函数类型
using EventCallback = std::function<void(const EventData &)>;

// 命令行参数结构
struct ProgramArgs {
  int localPort;              // 本地服务器监听端口
  int maxClients;             // 本地服务器最大连接数
  std::string natServerIP;    // 远端NAT打洞服务器IP
  int natServerPort;          // 远端NAT打洞服务器端口
  std::string targetServerIP; // 数据转发目标服务器IP
  int targetServerPort;       // 数据转发目标服务器端口
  std::string customGuid;     // 自定义GUID（十六进制字符串，最多16字符）
  std::string coordinatorIP;  // Coordinator服务器IP
  int coordinatorPort;        // Coordinator服务器端口
  bool useUPnP;               // 启用UPnP端口映射
};

// 打印使用说明
void PrintUsage(const char *progName) {
  printf("用法: %s -local_port <port> -max_clients <num> "
         "-nat_ip <ip> -nat_port <port> "
         "-target_ip <ip> -target_port <port> "
         "[-guid <hex16>] [-coordinator_ip <ip> -coordinator_port <port>] "
         "[-upnp]\n",
         progName);
  printf("示例: %s -local_port 60000 -max_clients 10 "
         "-nat_ip 192.168.1.100 -nat_port 61111 "
         "-target_ip 10.0.0.50 -target_port 7000 "
         "-guid 1234567890ABCDEF "
         "-coordinator_ip 192.168.1.200 -coordinator_port 61112\n",
         progName);
}

// 解析命令行参数
bool ParseArgs(int argc, char **argv, ProgramArgs &args) {
  // 设置默认值
  args.localPort = 60000;
  args.maxClients = 10;
  args.natServerIP = "";
  args.natServerPort = 61111;
  args.targetServerIP = "";
  args.targetServerPort = 7000;
  args.customGuid = ""; // 默认为空，表示自动生成
  args.coordinatorIP = "";
  args.coordinatorPort = 61112;
  args.useUPnP = false;

  for (int i = 1; i < argc; i++) {
    std::string arg = argv[i];
    if (arg == "-local_port" && i + 1 < argc) {
      args.localPort = atoi(argv[++i]);
    } else if (arg == "-max_clients" && i + 1 < argc) {
      args.maxClients = atoi(argv[++i]);
    } else if (arg == "-nat_ip" && i + 1 < argc) {
      args.natServerIP = argv[++i];
    } else if (arg == "-nat_port" && i + 1 < argc) {
      args.natServerPort = atoi(argv[++i]);
    } else if (arg == "-target_ip" && i + 1 < argc) {
      args.targetServerIP = argv[++i];
    } else if (arg == "-target_port" && i + 1 < argc) {
      args.targetServerPort = atoi(argv[++i]);
    } else if (arg == "-guid" && i + 1 < argc) {
      args.customGuid = argv[++i];
    } else if (arg == "-coordinator_ip" && i + 1 < argc) {
      args.coordinatorIP = argv[++i];
    } else if (arg == "-coordinator_port" && i + 1 < argc) {
      args.coordinatorPort = atoi(argv[++i]);
    } else if (arg == "-upnp") {
      args.useUPnP = true;
    } else if (arg == "-h" || arg == "--help") {
      PrintUsage(argv[0]);
      return false;
    }
  }

  // 验证必要参数
  if (args.natServerIP.empty() || args.targetServerIP.empty()) {
    printf("错误: NAT服务器IP和目标服务器IP是必填项\n");
    PrintUsage(argv[0]);
    return false;
  }

  // 验证guid格式（十六进制字符串，最多16字符）
  if (!args.customGuid.empty()) {
    if (args.customGuid.length() > 16) {
      return false;
    }
    for (char c : args.customGuid) {
      if (!isxdigit(c)) {
        return false;
      }
    }
  }

  return true;
}

unsigned char GetPacketIdentifier(RakNet::Packet *p) {
  if (p == 0)
    return 255;

  if ((unsigned char)p->data[0] == ID_TIMESTAMP) {
    RakAssert(p->length > sizeof(RakNet::MessageID) + sizeof(RakNet::Time));
    return (unsigned char)
        p->data[sizeof(RakNet::MessageID) + sizeof(RakNet::Time)];
  } else
    return (unsigned char)p->data[0];
}

// 客户端上下文结构 (改进版)
struct ClientContext {
  RakNet::RakPeerInterface *childPeer;
  RakNet::RakNetGUID clientGuid;
  uint32_t uin; // GUID 的低32位，用于路由
  RakNet::SystemAddress clientAddress;
  RakNet::SystemAddress targetAddress;

  // 标志位
  bool isCoordinatorClient; // 通过 coordinator 接入的客户端

  // 状态管理
  ConnectionState state;
  time_t stateStartTime;      // 状态开始时间
  time_t lastActivityTime;    // 最后活动时间
  time_t proxyConnectTimeout; // 代理连接超时时间
  time_t hostConnectTimeout;  // 主机连接超时时间

  // 统计信息
  uint64_t bytesSent;
  uint64_t bytesReceived;
  uint32_t packetsSent;
  uint32_t packetsReceived;
  uint32_t reconnectCount;

  // 认证信息
  bool isAuthenticated;
  std::string authData;

  ClientContext()
      : childPeer(nullptr), uin(0), isCoordinatorClient(false),
        state(STATE_DISCONNECTED), stateStartTime(0),
        lastActivityTime(0), proxyConnectTimeout(0), hostConnectTimeout(0),
        bytesSent(0), bytesReceived(0), packetsSent(0), packetsReceived(0),
        reconnectCount(0), isAuthenticated(false) {}
};

// 事件管理器类
class EventManager {
private:
  std::vector<EventCallback> callbacks;

public:
  void RegisterCallback(EventCallback callback) {
    callbacks.push_back(callback);
  }

  void EmitEvent(const EventData &event) {
    for (auto &callback : callbacks) {
      callback(event);
    }
  }

  void EmitEvent(EventType type, const RakNet::RakNetGUID &guid,
                 const RakNet::SystemAddress &address,
                 const std::string &reason = "") {
    EventData event;
    event.type = type;
    event.guid = guid;
    event.address = address;
    event.reason = reason;
    event.timestamp = time(nullptr);
    EmitEvent(event);
  }
};

// 全局事件管理器
static EventManager g_eventManager;

// 状态管理函数
const char *GetStateName(ConnectionState state) {
  switch (state) {
  case STATE_DISCONNECTED:
    return "DISCONNECTED";
  case STATE_PUNCH_CONNECTING:
    return "PUNCH_CONNECTING";
  case STATE_PUNCH_CONNECTED:
    return "PUNCH_CONNECTED";
  case STATE_HOST_CONNECTING:
    return "HOST_CONNECTING";
  case STATE_PROXY_CONNECTING:
    return "PROXY_CONNECTING";
  case STATE_PROXY_HANDSHAKE:
    return "PROXY_HANDSHAKE";
  case STATE_CONNECTED:
    return "CONNECTED";
  default:
    return "UNKNOWN";
  }
}

void UpdateClientState(ClientContext &ctx, ConnectionState newState) {
  if (ctx.state != newState) {
    ctx.state = newState;
    ctx.stateStartTime = time(nullptr);
  }
}

bool CheckTimeout(ClientContext &ctx, time_t currentTime) {
  // 检查代理连接超时 (15秒)
  if (ctx.state == STATE_PROXY_CONNECTING && ctx.proxyConnectTimeout > 0 &&
      currentTime > ctx.proxyConnectTimeout) {
    printf("[超时] 客户端 %s 代理连接超时\n", ctx.clientGuid.ToString());
    return true;
  }

  // 检查主机连接超时 (20秒)
  if (ctx.state == STATE_HOST_CONNECTING && ctx.hostConnectTimeout > 0 &&
      currentTime > ctx.hostConnectTimeout) {
    printf("[超时] 客户端 %s 主机连接超时\n", ctx.clientGuid.ToString());
    return true;
  }

  return false;
}

void DestroyClient(RakNet::RakPeerInterface *childPeer) {
  if (childPeer) {
    childPeer->Shutdown(300);
    RakNet::RakPeerInterface::DestroyInstance(childPeer);
  }
}

// UPnP state
static struct UPNPUrls g_upnpUrls;
static struct IGDdatas g_upnpData;
static struct UPNPDev *g_upnpDevlist = NULL;
static bool g_upnpInitialized = false;

bool AddUPnPPortMapping(int port) {
  int error = 0;

  g_upnpDevlist = upnpDiscover(2000, NULL, NULL, 0, 0, 2, &error);
  if (!g_upnpDevlist) {
    return false;
  }

  char lanaddr[64] = {0};
  char wanaddr[64] = {0};
  int igd = UPNP_GetValidIGD(g_upnpDevlist, &g_upnpUrls, &g_upnpData, lanaddr,
                             sizeof(lanaddr), wanaddr, sizeof(wanaddr));
  if (igd <= 0) {
    freeUPNPDevlist(g_upnpDevlist);
    g_upnpDevlist = NULL;
    return false;
  }

  char portStr[16];
  snprintf(portStr, sizeof(portStr), "%d", port);

  int result = UPNP_AddPortMapping(
      g_upnpUrls.controlURL, g_upnpData.first.servicetype, portStr, portStr,
      lanaddr, "raknet_proxy", "UDP", NULL, "0");

  if (result == 0) {
    g_upnpInitialized = true;
    return true;
  } else {
    return false;
  }
}

void RemoveUPnPPortMapping(int port) {
  if (!g_upnpInitialized)
    return;

  char portStr[16];
  snprintf(portStr, sizeof(portStr), "%d", port);

  int result = UPNP_DeletePortMapping(g_upnpUrls.controlURL,
                                      g_upnpData.first.servicetype, portStr,
                                      "UDP", NULL);

  if (result == 0) {
  } else {
  }

  FreeUPNPUrls(&g_upnpUrls);
  if (g_upnpDevlist) {
    freeUPNPDevlist(g_upnpDevlist);
    g_upnpDevlist = NULL;
  }
  g_upnpInitialized = false;
}

volatile sig_atomic_t g_running = 1;

void SignalHandler(int) { g_running = 0; }

// 从 RakNetGUID 提取 UIN（低32位）
inline uint32_t GetUIN(const RakNet::RakNetGUID &guid) {
  return (uint32_t)(guid.g & 0xFFFFFFFF);
}

// 发送MiniWorld子消息
void SendMiniWorldMessage(RakNet::RakPeerInterface *peer, unsigned char subID,
                          const RakNet::SystemAddress &target,
                          const void *data = nullptr, size_t dataSize = 0) {
  RakNet::BitStream bs;
  unsigned char header = ID_UDP_PROXY_GENERAL;
  bs.WriteBits(&header, 8, true);
  bs.WriteBits(&subID, 8, true);
  if (data && dataSize > 0) {
    bs.WriteBits((const unsigned char *)data, dataSize * 8, true);
  }
  peer->Send(&bs, HIGH_PRIORITY, RELIABLE_ORDERED, 0, target, false);
}

// 用标准的BitStream::Write写GUID，保证端序正确
void SendMiniWorldMessageGUID(RakNet::RakPeerInterface *peer,
                              unsigned char subID,
                              const RakNet::SystemAddress &target,
                              uint64_t guidValue) {
  unsigned char packet[18];
  packet[0] = ID_UDP_PROXY_GENERAL;
  packet[1] = subID;
  for (int i = 0; i < 8; i++) {
    packet[2 + i] = (guidValue >> (56 - i * 8)) & 0xFF;
  }
  peer->Send((const char *)packet, 10, HIGH_PRIORITY, RELIABLE_ORDERED, 0,
             target, false);
}

// 创建子Peer并连接到目标服务器（提取的公共逻辑）
bool CreateChildPeer(const ProgramArgs &args, ClientContext &ctx,
                     const RakNet::RakNetGUID &clientGuid) {
  ctx.childPeer = RakNet::RakPeerInterface::GetInstance();
  if (!ctx.childPeer) {
    printf("[错误] 无法创建子RakPeer实例\n");
    return false;
  }
  ctx.childPeer->SetMyGUID(clientGuid);
  ctx.childPeer->SetMaximumIncomingConnections(0);
  RakNet::SocketDescriptor sdChild(0, 0);
  if (ctx.childPeer->Startup(1, &sdChild, 1) != RakNet::RAKNET_STARTED) {
    printf("[错误] 子RakPeer启动失败\n");
    DestroyClient(ctx.childPeer);
    ctx.childPeer = nullptr;
    return false;
  }
  if (ctx.childPeer->Connect(args.targetServerIP.c_str(),
                              args.targetServerPort, 0,
                              0) != RakNet::CONNECTION_ATTEMPT_STARTED) {
    printf("[错误] 子RakPeer连接目标服务器失败\n");
    DestroyClient(ctx.childPeer);
    ctx.childPeer = nullptr;
    return false;
  }
  return true;
}

// 处理客户端注册消息
void HandleClientRegister(
    RakNet::RakPeerInterface *peer, RakNet::Packet *packet,
    const ProgramArgs &args,
    std::map<uint32_t, ClientContext> &clients) {
  if (packet->length < 10)
    return;

  RakNet::RakNetGUID guid;
  memcpy(&guid, packet->data + 2, 8);
  uint32_t uin = GetUIN(guid);

  printf("[MiniWorld] 客户端注册 GUID:%s UIN:%u 来自:%s\n", guid.ToString(),
         uin, packet->systemAddress.ToString(true));

  SendMiniWorldMessage(peer, MW_PROXY_CLIENT_REGISTER_RESP,
                       packet->systemAddress);

  // 不管有没有目标GUID，都创建子Peer连接到配置的目标服务器
  ClientContext ctx;
  ctx.clientGuid = guid;
  ctx.uin = uin;
  ctx.clientAddress = packet->systemAddress;
  ctx.targetAddress = RakNet::UNASSIGNED_SYSTEM_ADDRESS;
  ctx.state = STATE_PROXY_CONNECTING;
  ctx.stateStartTime = time(nullptr);
  ctx.lastActivityTime = time(nullptr);
  ctx.isCoordinatorClient = false;

  if (!CreateChildPeer(args, ctx, guid)) {
    return;
  }

  clients[uin] = ctx;
  printf("[MiniWorld] 已分配子Peer, UIN:%u, 状态: PROXY_CONNECTING\n", uin);
  g_eventManager.EmitEvent(EVENT_PROXY_CONNECT_SUCCESS, guid,
                           packet->systemAddress);
}

// 处理主机注册消息
void HandleHostRegister(RakNet::RakPeerInterface *peer, RakNet::Packet *packet,
                        const ProgramArgs &args,
                        std::map<uint32_t, ClientContext> &clients) {
  if (packet->length < 18)
    return;

  RakNet::RakNetGUID guid1, guid2;
  memcpy(&guid1, packet->data + 2, 8);
  memcpy(&guid2, packet->data + 10, 8);
  uint32_t uin1 = GetUIN(guid1);

  SendMiniWorldMessage(peer, MW_PROXY_HOST_REGISTER_RESP,
                       packet->systemAddress);

  auto it = clients.find(uin1);
  if (it != clients.end()) {
    it->second.targetAddress = packet->systemAddress;
    UpdateClientState(it->second, STATE_HOST_CONNECTING);
    it->second.hostConnectTimeout = time(nullptr) + 20;
    printf("[MiniWorld] 客户端 UIN:%u 开始连接主机\n", uin1);
  }
}

// 处理Ping消息
void HandlePingMessage(RakNet::RakPeerInterface *peer, RakNet::Packet *packet,
                       std::map<uint32_t, ClientContext> &clients) {
  for (auto &pair : clients) {
    if (pair.second.clientAddress == packet->systemAddress) {
      pair.second.lastActivityTime = time(nullptr);
      SendMiniWorldMessage(peer, MW_PROXY_PONG, packet->systemAddress);
      break;
    }
  }
}

// 处理断开连接消息
void HandleDisconnectMessage(
    RakNet::RakPeerInterface *peer, RakNet::Packet *packet,
    std::map<uint32_t, ClientContext> &clients) {
  for (auto it = clients.begin(); it != clients.end(); ++it) {
    if (it->second.clientAddress == packet->systemAddress) {
      printf("[MiniWorld] 客户端 UIN:%u 请求断开连接\n", it->first);
      DestroyClient(it->second.childPeer);
      g_eventManager.EmitEvent(EVENT_CLIENT_DISCONNECTED, it->second.clientGuid,
                               packet->systemAddress);
      clients.erase(it);
      break;
    }
  }
}

// 统计信息打印
void PrintStatistics(
    const std::map<uint32_t, ClientContext> &clients) {
  // kept for connection count tracking
}

int main(int argc, char **argv) {
  ProgramArgs args;
  if (!ParseArgs(argc, argv, args)) {
    return 1;
  }

  printf("RakNet proxy starting...\n");

  // 注册事件回调（空，信息已由直接打印覆盖）
  g_eventManager.RegisterCallback([](const EventData &event) {
    (void)event;
  });

  // ----- 1. 创建RakNet实例 -----
  RakNet::RakPeerInterface *peer = RakNet::RakPeerInterface::GetInstance();
  if (peer == NULL) {
    printf("错误: 无法创建RakNet实例\n");
    return 1;
  }

  // ----- 1.5 设置自定义GUID（如果有） -----
  if (!args.customGuid.empty()) {
    uint64_t guidValue = strtoull(args.customGuid.c_str(), NULL, 16);
    RakNet::RakNetGUID customGUID(guidValue);
    peer->SetMyGUID(customGUID);
  }

  // ----- 2. 启动本地服务器 -----
  RakNet::SocketDescriptor sd(args.localPort, 0);
  RakNet::StartupResult result = peer->Startup(args.maxClients, &sd, 1);
  if (result != RakNet::RAKNET_STARTED) {
    printf("错误: 本地服务器启动失败，错误码: %d\n", result);
    RakNet::RakPeerInterface::DestroyInstance(peer);
    return 1;
  }
  peer->SetMaximumIncomingConnections(args.maxClients);
  printf("本地服务器已启动\n");

  // ----- 2.5 附加NAT打洞客户端插件 -----
  RakNet::NatPunchthroughClient *natClient =
      RakNet::NatPunchthroughClient::GetInstance();
  peer->AttachPlugin(natClient);

  if (args.useUPnP) {
    AddUPnPPortMapping(args.localPort);
  }

  RakNet::SystemAddress coordinatorAddress = RakNet::UNASSIGNED_SYSTEM_ADDRESS;
  if (!args.coordinatorIP.empty()) {
    printf("正在连接到Coordinator %s:%d ...\n", args.coordinatorIP.c_str(),
           args.coordinatorPort);
    RakNet::ConnectionAttemptResult coordResult =
        peer->Connect(args.coordinatorIP.c_str(), args.coordinatorPort, 0, 0);
    if (coordResult == RakNet::CONNECTION_ATTEMPT_STARTED) {
      printf("连接Coordinator请求已发送\n");
    }
  }

  // ----- 3. 连接到远端NAT打洞服务器 -----
  printf("正在连接到NAT打洞服务器 %s:%d ...\n", args.natServerIP.c_str(),
         args.natServerPort);
  RakNet::ConnectionAttemptResult connResult =
      peer->Connect(args.natServerIP.c_str(), args.natServerPort,
                    0, // 密码数据（无密码）
                    0  // 密码数据长度
      );
  if (connResult != RakNet::CONNECTION_ATTEMPT_STARTED) {
    printf("错误: 连接NAT打洞服务器失败，错误码: %d\n", connResult);
    // 不退出，继续运行本地服务器
  } else {
    printf("连接NAT打洞服务器的请求已发送\n");
  }

  // 用于追踪与目标服务器的连接地址
  RakNet::SystemAddress targetServerAddress = RakNet::UNASSIGNED_SYSTEM_ADDRESS;
  // 目标服务器的预期地址（IP:端口），用于精确匹配
  RakNet::SystemAddress expectedTargetAddr(args.targetServerIP.c_str(),
                                           args.targetServerPort);
  // 多客户端连接表 (UIN → ClientContext)
  std::map<uint32_t, ClientContext> clients;

  struct sigaction sa;
  sa.sa_handler = SignalHandler;
  sigemptyset(&sa.sa_mask);
  sa.sa_flags = 0;
  sigaction(SIGINT, &sa, NULL);
  sigaction(SIGTERM, &sa, NULL);

  printf("\n服务器运行中，按 Ctrl+C 退出...\n\n");
  // 统计打印计时器
  time_t lastStatsTime = time(nullptr);
  const int STATS_INTERVAL = 60; // 每60秒打印一次统计

  // ----- 4. 主循环：处理网络事件 (N+1 peers) -----
  int packetCount = 0;
  while (g_running) {
    bool hadData = false;
    time_t currentTime = time(nullptr);

    // 检查超时
    for (auto it = clients.begin(); it != clients.end();) {
      if (CheckTimeout(it->second, currentTime)) {
        printf("[超时] 客户端 UIN:%u 连接超时，断开连接\n", it->first);
        DestroyClient(it->second.childPeer);
        g_eventManager.EmitEvent(EVENT_PROXY_CONNECT_FAILED,
                                 it->second.clientGuid,
                                 it->second.clientAddress, "连接超时");
        it = clients.erase(it);
      } else {
        ++it;
      }
    }

    // 定期打印统计信息
    if (currentTime - lastStatsTime >= STATS_INTERVAL) {
      PrintStatistics(clients);
      lastStatsTime = currentTime;
    }

    // 1. Poll main peer
    {
      RakNet::Packet *packet = peer->Receive();
      if (packet) {
        hadData = true;
        packetCount++;
        unsigned char packetID = GetPacketIdentifier(packet);
        RakNet::SystemAddress senderAddress = packet->systemAddress;

        switch (packetID) {
        case ID_NEW_INCOMING_CONNECTION: {
          RakNet::RakNetGUID clientGuid = packet->guid;
          uint32_t uin = GetUIN(clientGuid);
          printf("[连接] %s GUID:%s UIN:%u\n", senderAddress.ToString(true),
                 clientGuid.ToString(), uin);

          // 直接客户端连接（非coordinator/目标服务器）：创建子Peer
          // coordinator/目标服务器已在 ID_CONNECTION_REQUEST_ACCEPTED 中处理
          // 这里只处理 incoming 连接（即游戏客户端直连）
          if (clients.find(uin) != clients.end()) {
            // 已注册，更新targetAddress（子Peer连上目标服务器后触发）
            clients[uin].targetAddress = senderAddress;
            UpdateClientState(clients[uin], STATE_CONNECTED);
            printf("[连接] 目标服务器确认, UIN:%u\n", uin);
            break;
          }

          // 新客户端直连 → 分配子Peer
          ClientContext ctx;
          ctx.clientGuid = clientGuid;
          ctx.uin = uin;
          ctx.clientAddress = senderAddress;
          ctx.targetAddress = RakNet::UNASSIGNED_SYSTEM_ADDRESS;
          ctx.state = STATE_PROXY_CONNECTING;
          ctx.stateStartTime = time(nullptr);
          ctx.lastActivityTime = time(nullptr);
          ctx.isCoordinatorClient = false;

          if (CreateChildPeer(args, ctx, clientGuid)) {
            clients[uin] = ctx;
            printf("[客户端] 直连, 已分配子Peer UIN:%u\n", uin);
            g_eventManager.EmitEvent(EVENT_PROXY_CONNECT_SUCCESS, clientGuid,
                                     senderAddress);
          }
          break;
        }

        case ID_DISCONNECTION_NOTIFICATION: {
          printf("[断开] %s\n", senderAddress.ToString(true));
          for (auto it = clients.begin(); it != clients.end();) {
            if (it->second.clientAddress == senderAddress) {
              printf("[客户端] UIN:%u 断开\n", it->first);
              DestroyClient(it->second.childPeer);
              g_eventManager.EmitEvent(EVENT_CLIENT_DISCONNECTED,
                                       it->second.clientGuid, senderAddress);
              it = clients.erase(it);
            } else {
              ++it;
            }
          }
          // 重连 NAT / Coordinator
          if (coordinatorAddress != RakNet::UNASSIGNED_SYSTEM_ADDRESS &&
              senderAddress == coordinatorAddress) {
            coordinatorAddress = RakNet::UNASSIGNED_SYSTEM_ADDRESS;
            printf("[Coordinator] 断开，正在重连...\n");
            peer->Connect(args.coordinatorIP.c_str(), args.coordinatorPort, 0, 0);
          }
          if (strcmp(senderAddress.ToString(false), args.natServerIP.c_str()) == 0) {
            printf("[NAT] 断开，正在重连...\n");
            peer->Connect(args.natServerIP.c_str(), args.natServerPort, 0, 0);
          }
          break;
        }

        case ID_CONNECTION_LOST: {
          printf("[丢失] %s\n", senderAddress.ToString(true));
          for (auto it = clients.begin(); it != clients.end();) {
            if (it->second.clientAddress == senderAddress ||
                it->second.targetAddress == senderAddress) {
              printf("[客户端] UIN:%u 丢失\n", it->first);
              DestroyClient(it->second.childPeer);
              g_eventManager.EmitEvent(EVENT_CLIENT_DISCONNECTED,
                                       it->second.clientGuid, senderAddress);
              it = clients.erase(it);
            } else {
              ++it;
            }
          }
          // 重连 NAT / Coordinator
          if (coordinatorAddress != RakNet::UNASSIGNED_SYSTEM_ADDRESS &&
              senderAddress == coordinatorAddress) {
            coordinatorAddress = RakNet::UNASSIGNED_SYSTEM_ADDRESS;
            printf("[Coordinator] 断开，正在重连...\n");
            peer->Connect(args.coordinatorIP.c_str(), args.coordinatorPort, 0, 0);
          }
          if (strcmp(senderAddress.ToString(false), args.natServerIP.c_str()) == 0) {
            printf("[NAT] 断开，正在重连...\n");
            peer->Connect(args.natServerIP.c_str(), args.natServerPort, 0, 0);
          }
          break;
        }

        case ID_CONNECTION_REQUEST_ACCEPTED: {
          if (!args.coordinatorIP.empty() &&
              strcmp(senderAddress.ToString(false),
                     args.coordinatorIP.c_str()) == 0) {
            coordinatorAddress = senderAddress;
            printf("[Coordinator] 连接成功\n");

            // 向 Coordinator 注册 (用标准Write保证端序正确)
            RakNet::RakNetGUID myGuid = peer->GetMyGUID();
            SendMiniWorldMessageGUID(peer, MW_PROXY_CLIENT_REGISTER,
                                     coordinatorAddress, myGuid.g);
            printf("[Coordinator] 已发送注册 GUID:%s\n", myGuid.ToString());
          } else if (strcmp(senderAddress.ToString(false),
                            args.natServerIP.c_str()) == 0) {
            printf("[NAT服务器] 连接成功\n");
          } else if (senderAddress == expectedTargetAddr) {
            targetServerAddress = senderAddress;
          }
          break;
        }

        case ID_NO_FREE_INCOMING_CONNECTIONS: {
          printf("[#%d] 警告: 连接已满，无法接受新连接\n", packetCount);
          break;
        }

        case ID_NAT_PUNCHTHROUGH_SUCCEEDED: {
          uint32_t uin = GetUIN(packet->guid);
          printf("[打洞成功] GUID:%s UIN:%u\n", packet->guid.ToString(), uin);
          g_eventManager.EmitEvent(EVENT_NAT_PUNCHTHROUGH_SUCCESS, packet->guid,
                                   senderAddress);

          auto it = clients.find(uin);
          if (it != clients.end()) {
            UpdateClientState(it->second, STATE_PUNCH_CONNECTED);
          }
          break;
        }

        case ID_NAT_PUNCHTHROUGH_FAILED: {
          printf("[打洞失败] GUID: %s\n", packet->guid.ToString());
          g_eventManager.EmitEvent(EVENT_NAT_PUNCHTHROUGH_FAILED, packet->guid,
                                   senderAddress, "NAT穿透失败");
          break;
        }

        case ID_NAT_TARGET_NOT_CONNECTED: {
          printf("[#%d] NAT打洞错误: 目标未连接到打洞服务器\n", packetCount);
          break;
        }

        case ID_NAT_TARGET_UNRESPONSIVE: {
          printf("[#%d] NAT打洞错误: 目标无响应\n", packetCount);
          break;
        }

        case ID_NAT_CONNECTION_TO_TARGET_LOST: {
          printf("[#%d] NAT打洞错误: 与目标的连接丢失\n", packetCount);
          break;
        }

        case ID_NAT_ALREADY_IN_PROGRESS: {
          printf("[#%d] NAT打洞提示: 打洞正在进行中\n", packetCount);
          break;
        }

        case ID_UDP_PROXY_GENERAL: {
          if (packet->length < 2)
            break;
          unsigned char subID = packet->data[1];

          switch (subID) {
          case MW_PROXY_CLIENT_REGISTER:
            HandleClientRegister(peer, packet, args, clients);
            break;

          case MW_PROXY_CLIENT_REGISTER_RESP:
            printf("[Coordinator] 注册成功 来自: %s\n",
                   senderAddress.ToString(true));
            break;

          case MW_PROXY_HOST_REGISTER_RESP:
            printf("[Coordinator] 主机注册成功 来自: %s\n",
                   senderAddress.ToString(true));
            break;

          case MW_PROXY_HOST_REGISTER:
            HandleHostRegister(peer, packet, args, clients);
            break;

          case MW_PROXY_PING:
            HandlePingMessage(peer, packet, clients);
            break;

          case MW_PROXY_DISCONNECT:
            HandleDisconnectMessage(peer, packet, clients);
            break;

          case MW_PROXY_CLIENT_AUTH:
            printf("[MiniWorld] 客户端认证消息来自: %s\n",
                   senderAddress.ToString(true));
            for (auto &pair : clients) {
              if (pair.second.clientAddress == senderAddress) {
                pair.second.isAuthenticated = true;
                pair.second.authData = std::string(
                    (const char *)(packet->data + 2), packet->length - 2);
                printf("[MiniWorld] 客户端 UIN:%u 认证成功\n", pair.first);
                break;
              }
            }
            break;

          case MW_PROXY_HOST_AUTH:
            break;

          case MW_PROXY_DATA: {
            // coordinator → proxy: 5c 30 [UIN:4B] [data...]
            // 根据 UIN 路由到正确的子Peer
            if (packet->length < 6) break;
            uint32_t dataUin = 0;
            memcpy(&dataUin, packet->data + 2, 4);

            auto it = clients.find(dataUin);
            if (it == clients.end()) {
              break;
            }

            ClientContext &ctx = it->second;
            if (ctx.childPeer) {
              RakNet::SystemAddress target =
                  ctx.targetAddress != RakNet::UNASSIGNED_SYSTEM_ADDRESS
                      ? ctx.targetAddress
                      : RakNet::SystemAddress(
                            args.targetServerIP.c_str(),
                            (unsigned short)args.targetServerPort);
              ctx.childPeer->Send((const char *)(packet->data + 6),
                                  packet->length - 6, HIGH_PRIORITY,
                                  RELIABLE_ORDERED, 0, target, false);
              ctx.bytesSent += packet->length - 6;
              ctx.packetsSent++;
            }
            break;
          }

          case 0x23: {
            // coordinator 通知新客户端接入
            // GUID 在网络包中为大端序，需手动组装
            RakNet::RakNetGUID clientGuid;
            uint32_t uin = 0;
            if (packet->length >= 10) {
              uint64_t g = 0;
              for (int i = 0; i < 8; i++)
                g = (g << 8) | (uint8_t)packet->data[2 + i];
              clientGuid = RakNet::RakNetGUID(g);
              uin = GetUIN(clientGuid);
            } else {
              clientGuid = peer->GetMyGUID();
              uin = GetUIN(clientGuid);
            }
            printf("[Coordinator] 客户端上线 GUID:%s UIN:%u\n",
                   clientGuid.ToString(), uin);

            ClientContext ctx;
            ctx.clientGuid = clientGuid;
            ctx.uin = uin;
            ctx.clientAddress = senderAddress;
            ctx.targetAddress = RakNet::UNASSIGNED_SYSTEM_ADDRESS;
            ctx.state = STATE_PROXY_CONNECTING;
            ctx.isCoordinatorClient = true;

            if (CreateChildPeer(args, ctx, clientGuid)) {
              clients[uin] = ctx;
              printf("[Coordinator] 已分配子Peer UIN:%u\n", uin);
            }
            break;
          }

          default:
            break;
          }
          break;
        }

        default: {
          // 根据UIN或地址查找对应的客户端上下文
          ClientContext *ctx = NULL;
          if (packet->length >= 5) {
            uint32_t uin = ((uint32_t)packet->data[1] << 24) |
                           (packet->data[2] << 16) | (packet->data[3] << 8) |
                           packet->data[4];
            auto it = clients.find(uin);
            if (it != clients.end())
              ctx = &it->second;
          }
          if (!ctx) {
            for (auto &pair : clients) {
              if (pair.second.clientAddress == senderAddress ||
                  pair.second.targetAddress == senderAddress) {
                ctx = &pair.second;
                break;
              }
            }
          }
          if (ctx) {
            bool fromTarget = (ctx->targetAddress == senderAddress);
            if (fromTarget) {
              peer->Send((const char *)(packet->data), packet->length,
                         HIGH_PRIORITY, RELIABLE_ORDERED, 0,
                         ctx->clientAddress, false);
              ctx->bytesReceived += packet->length;
              ctx->packetsReceived++;
            } else if (ctx->childPeer) {
              RakNet::SystemAddress target =
                  ctx->targetAddress != RakNet::UNASSIGNED_SYSTEM_ADDRESS
                      ? ctx->targetAddress
                      : RakNet::SystemAddress(
                            args.targetServerIP.c_str(),
                            (unsigned short)args.targetServerPort);
              if (target != RakNet::UNASSIGNED_SYSTEM_ADDRESS) {
                ctx->childPeer->Send((const char *)(packet->data),
                                     packet->length, HIGH_PRIORITY,
                                     RELIABLE_ORDERED, 0, target, false);
                ctx->bytesSent += packet->length;
                ctx->packetsSent++;
              }
            }
          } else if ((unsigned char)packetID > 0x13 &&
                     packetID != ID_UDP_PROXY_GENERAL) {
          }
          break;
        }
        }

        peer->DeallocatePacket(packet);
      }
    }

    // 2. Poll each child peer (target → client/coordinator forwarding)
    for (auto &pair : clients) {
      ClientContext &ctx = pair.second;
      if (!ctx.childPeer)
        continue;

      RakNet::Packet *cp = ctx.childPeer->Receive();
      if (cp) {
        hadData = true;
        unsigned char id = GetPacketIdentifier(cp);

        if (id == ID_CONNECTION_REQUEST_ACCEPTED) {
          ctx.targetAddress = cp->systemAddress;
          UpdateClientState(ctx, STATE_CONNECTED);
          ctx.proxyConnectTimeout = 0;
          ctx.hostConnectTimeout = 0;
          printf("[子Peer] 目标服务器连接成功 UIN:%u, 状态: CONNECTED\n",
                 pair.first);
          g_eventManager.EmitEvent(EVENT_HOST_CONNECT_SUCCESS,
                                   ctx.clientGuid, cp->systemAddress);

          // 透传目标服务器的系统消息给客户端/coordinator
          peer->Send((const char *)(cp->data), cp->length, HIGH_PRIORITY,
                     RELIABLE_ORDERED, 0, ctx.clientAddress, false);
          ctx.bytesReceived += cp->length;
          ctx.packetsReceived++;
        } else if (id == ID_CONNECTION_ATTEMPT_FAILED) {
          printf("[错误] 子Peer连接目标服务器失败 UIN:%u\n", pair.first);
          DestroyClient(ctx.childPeer);
          ctx.childPeer = NULL;
          g_eventManager.EmitEvent(EVENT_HOST_CONNECT_FAILED,
                                   ctx.clientGuid, ctx.clientAddress,
                                   "连接目标服务器失败");
        } else if (id == ID_NEW_INCOMING_CONNECTION) {
          printf("[子Peer] 目标服务器接受连接 UIN:%u\n", pair.first);
          peer->Send((const char *)(cp->data), cp->length, HIGH_PRIORITY,
                     RELIABLE_ORDERED, 0, ctx.clientAddress, false);
          ctx.bytesReceived += cp->length;
          ctx.packetsReceived++;
        } else if (id == ID_DISCONNECTION_NOTIFICATION) {
          printf("[子Peer] 目标服务器主动断开连接 UIN:%u\n", pair.first);
          DestroyClient(ctx.childPeer);
          ctx.childPeer = NULL;
        } else if (id == ID_CONNECTION_LOST) {
          printf("[子Peer] 目标服务器连接丢失 UIN:%u\n", pair.first);
          DestroyClient(ctx.childPeer);
          ctx.childPeer = NULL;
        } else {
          // 所有数据透传，不加包裹
          peer->Send((const char *)(cp->data), cp->length, HIGH_PRIORITY,
                     RELIABLE_ORDERED, 0, ctx.clientAddress, false);
          ctx.bytesReceived += cp->length;
          ctx.packetsReceived++;
        }

        ctx.childPeer->DeallocatePacket(cp);
      }
    }

    if (!hadData) {
      RakSleep(30);
    }
  }

  // 关闭所有子Peer连接
  for (auto &pair : clients) {
    printf("[关闭] 子Peer UIN:%u\n", pair.first);
    DestroyClient(pair.second.childPeer);
  }
  clients.clear();

  if (args.useUPnP) {
    RemoveUPnPPortMapping(args.localPort);
  }

  // ----- 5. 清理资源 -----
  peer->Shutdown(0);
  RakNet::RakPeerInterface::DestroyInstance(peer);
  printf("服务器已关闭\n");

  return 0;
}
