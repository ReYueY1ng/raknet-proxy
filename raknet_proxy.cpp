// RakNet 数据转发代理 (NAT Punchthrough Multi-Client Proxy)
// 架构: 主 RakPeer (监听+NAT打洞) + 子 RakPeer (每客户端一个, GUID伪装连接目标服务器)
// 数据流: 客户端 → 主Peer → 子Peer → 目标服务器 (以及反向)
//
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <string>
#include <cstdint>
#include <map>
#include <signal.h>
#include "RakPeerInterface.h"
#include "MessageIdentifiers.h"
#include "BitStream.h"
#include "RakNetTypes.h"
#include "RakSleep.h"
#include "NatPunchthroughClient.h"

// 自定义消息ID，避免与RakNet内置ID冲突
enum GameMessages {
    ID_USER_DATA = ID_USER_PACKET_ENUM + 1,
};

// 命令行参数结构
struct ProgramArgs {
    int localPort;              // 本地服务器监听端口
    int maxClients;             // 本地服务器最大连接数
    std::string natServerIP;    // 远端NAT打洞服务器IP
    int natServerPort;          // 远端NAT打洞服务器端口
    std::string targetServerIP; // 数据转发目标服务器IP
    int targetServerPort;       // 数据转发目标服务器端口
    std::string customGuid;     // 自定义GUID（十六进制字符串，最多16字符）
};

// 打印使用说明
void PrintUsage(const char* progName) {
    printf("用法: %s -local_port <port> -max_clients <num> "
           "-nat_ip <ip> -nat_port <port> "
           "-target_ip <ip> -target_port <port> "
           "[-guid <hex16>]\n", progName);
    printf("示例: %s -local_port 60000 -max_clients 10 "
           "-nat_ip 192.168.1.100 -nat_port 61111 "
           "-target_ip 10.0.0.50 -target_port 7000 "
           "-guid 1234567890ABCDEF\n", progName);
}

// 解析命令行参数
bool ParseArgs(int argc, char** argv, ProgramArgs& args) {
    // 设置默认值
    args.localPort = 60000;
    args.maxClients = 10;
    args.natServerIP = "";
    args.natServerPort = 61111;
    args.targetServerIP = "";
    args.targetServerPort = 7000;
    args.customGuid = "";  // 默认为空，表示自动生成

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
            printf("错误: GUID字符串长度不能超过16个字符\n");
            return false;
        }
        for (char c : args.customGuid) {
            if (!isxdigit(c)) {
                printf("错误: GUID只能包含十六进制字符 (0-9, A-F, a-f)\n");
                return false;
            }
        }
    }

    return true;
}

unsigned char GetPacketIdentifier(RakNet::Packet *p)
{
    if (p==0)
        return 255;

    if ((unsigned char)p->data[0] == ID_TIMESTAMP)
    {
        RakAssert(p->length > sizeof(RakNet::MessageID) + sizeof(RakNet::Time));
        return (unsigned char) p->data[sizeof(RakNet::MessageID) + sizeof(RakNet::Time)];
    }
    else
        return (unsigned char) p->data[0];
}


struct ClientContext {
    RakNet::RakPeerInterface* childPeer;
    RakNet::RakNetGUID clientGuid;
    RakNet::SystemAddress clientAddress;
    RakNet::SystemAddress targetAddress;
};

void DestroyClient(RakNet::RakPeerInterface* childPeer) {
    if (childPeer) {
        childPeer->Shutdown(300);
        RakNet::RakPeerInterface::DestroyInstance(childPeer);
    }
}

volatile bool g_running = true;

void SignalHandler(int) {
    g_running = false;
}

int main(int argc, char** argv) {
    ProgramArgs args;
    if (!ParseArgs(argc, argv, args)) {
        return 1;
    }

    printf("========================================\n");
    printf("RakNet 数据转发服务器\n");
    printf("========================================\n");
    printf("本地监听端口: %d\n", args.localPort);
    printf("最大客户端数: %d\n", args.maxClients);
    printf("NAT打洞服务器: %s:%d\n", args.natServerIP.c_str(), args.natServerPort);
    printf("目标转发服务器: %s:%d\n", args.targetServerIP.c_str(), args.targetServerPort);
    if (!args.customGuid.empty()) {
        printf("自定义GUID: %s\n", args.customGuid.c_str());
    } else {
        printf("使用系统自动生成的GUID\n");
    }
    printf("========================================\n");

    // ----- 1. 创建RakNet实例 -----
    RakNet::RakPeerInterface* peer = RakNet::RakPeerInterface::GetInstance();
    if (peer == NULL) {
        printf("错误: 无法创建RakNet实例\n");
        return 1;
    }

    // ----- 1.5 设置自定义GUID（如果有） -----
    if (!args.customGuid.empty()) {
        uint64_t guidValue = strtoull(args.customGuid.c_str(), NULL, 16);
        RakNet::RakNetGUID customGUID(guidValue);
        peer->SetMyGUID(customGUID);
        printf("已设置自定义GUID: %s\n", peer->GetMyGUID().ToString());
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
    printf("本地服务器已在端口 %d 上启动，当前GUID: %s\n", 
           args.localPort, peer->GetMyGUID().ToString());

    // ----- 2.5 附加NAT打洞客户端插件 -----
    RakNet::NatPunchthroughClient* natClient = RakNet::NatPunchthroughClient::GetInstance();
    peer->AttachPlugin(natClient);
    printf("NAT打洞客户端插件已附加\n");

    // ----- 3. 连接到远端NAT打洞服务器 -----
    printf("正在连接到NAT打洞服务器 %s:%d ...\n",
           args.natServerIP.c_str(), args.natServerPort);
    RakNet::ConnectionAttemptResult connResult = peer->Connect(
        args.natServerIP.c_str(),
        args.natServerPort,
        0,     // 密码数据（无密码）
        0      // 密码数据长度
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
    RakNet::SystemAddress expectedTargetAddr(args.targetServerIP.c_str(), args.targetServerPort);
    // 多客户端连接表 (GUID → ClientContext)
    std::map<RakNet::RakNetGUID, ClientContext> clients;

    signal(SIGINT, SignalHandler);

    printf("\n服务器运行中，按 Ctrl+C 退出...\n\n");

    // ----- 4. 主循环：处理网络事件 (N+1 peers) -----
    int packetCount = 0;
    while (g_running) {
        bool hadData = false;

        // 1. Poll main peer
        {
        RakNet::Packet* packet = peer->Receive();
        if (packet) {
            hadData = true;
            packetCount++;
            unsigned char packetID = GetPacketIdentifier(packet);
            RakNet::SystemAddress senderAddress = packet->systemAddress;

            switch (packetID) {
            case ID_NEW_INCOMING_CONNECTION: {
                RakNet::RakNetGUID clientGuid = packet->guid;
                printf("[连接] %s GUID:%s\n", senderAddress.ToString(true), clientGuid.ToString());

                // Create child peer with client's GUID to impersonate
                RakNet::RakPeerInterface* childPeer = RakNet::RakPeerInterface::GetInstance();
                if (childPeer == NULL) {
                    printf("[错误] 无法创建子RakPeer实例\n");
                    break;
                }
                childPeer->SetMyGUID(clientGuid);
                childPeer->SetMaximumIncomingConnections(0);
                RakNet::SocketDescriptor sdChild(0, 0);
                RakNet::StartupResult sr = childPeer->Startup(1, &sdChild, 1);
                if (sr != RakNet::RAKNET_STARTED) {
                    printf("[错误] 子RakPeer启动失败: %d\n", sr);
                    RakNet::RakPeerInterface::DestroyInstance(childPeer);
                    break;
                }
                RakNet::ConnectionAttemptResult cr = childPeer->Connect(
                    args.targetServerIP.c_str(), args.targetServerPort, 0, 0);
                if (cr != RakNet::CONNECTION_ATTEMPT_STARTED) {
                    printf("[错误] 子RakPeer连接目标服务器失败: %d\n", cr);
                    DestroyClient(childPeer);
                    break;
                }

                clients[clientGuid] = ClientContext{
                    childPeer, clientGuid, senderAddress,
                    RakNet::UNASSIGNED_SYSTEM_ADDRESS
                };
                printf("[客户端] 已分配子Peer, GUID:%s\n", clientGuid.ToString());
                break;
            }

            case ID_DISCONNECTION_NOTIFICATION: {
                printf("[断开] %s\n", senderAddress.ToString(true));
                for (auto it = clients.begin(); it != clients.end(); ) {
                    if (it->second.clientAddress == senderAddress) {
                        printf("[客户端] GUID:%s 断开\n", it->first.ToString());
                        DestroyClient(it->second.childPeer);
                        it = clients.erase(it);
                    } else {
                        ++it;
                    }
                }
                break;
            }

            case ID_CONNECTION_LOST: {
                printf("[丢失] %s\n", senderAddress.ToString(true));
                if (senderAddress == targetServerAddress) {
                    targetServerAddress = RakNet::UNASSIGNED_SYSTEM_ADDRESS;
                    printf("[目标服务器] 连接丢失\n");
                }
                for (auto it = clients.begin(); it != clients.end(); ) {
                    if (it->second.clientAddress == senderAddress ||
                        it->second.targetAddress == senderAddress) {
                        printf("[客户端] GUID:%s 丢失\n", it->first.ToString());
                        DestroyClient(it->second.childPeer);
                        it = clients.erase(it);
                    } else {
                        ++it;
                    }
                }
                break;
            }

            case ID_CONNECTION_REQUEST_ACCEPTED: {
                if (strcmp(senderAddress.ToString(false), args.natServerIP.c_str()) == 0) {
                    printf("[NAT服务器] 连接成功\n");
                }
                else if (senderAddress == expectedTargetAddr) {
                    targetServerAddress = senderAddress;
                    printf("[目标服务器] 连接成功\n");
                }
                break;
            }

            case ID_NO_FREE_INCOMING_CONNECTIONS: {
                printf("[#%d] 警告: 连接已满，无法接受新连接\n", packetCount);
                break;
            }

            case ID_NAT_PUNCHTHROUGH_SUCCEEDED: {
                printf("[打洞成功] GUID:%s\n", packet->guid.ToString());
                break;
            }

            case ID_NAT_PUNCHTHROUGH_FAILED: {
                printf("[#%d] NAT打洞失败: 目标GUID: %s\n",
                       packetCount, packet->guid.ToString());
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

            default: {
                if ((unsigned char)packetID >= ID_USER_PACKET_ENUM) {
                    // Lookup client by address in clients map
                    ClientContext* ctx = NULL;
                    for (auto& pair : clients) {
                        if (pair.second.clientAddress == senderAddress) {
                            ctx = &pair.second;
                            break;
                        }
                    }
                    if (ctx && ctx->childPeer) {
                        ctx->childPeer->Send((const char*)(packet->data + 1), packet->length - 1,
                                             HIGH_PRIORITY, RELIABLE_ORDERED, 0,
                                             targetServerAddress != RakNet::UNASSIGNED_SYSTEM_ADDRESS
                                                 ? targetServerAddress
                                                 : ctx->targetAddress,
                                             false);
                    }
                } else {
                    printf("[未知消息ID] %d 来自: %s\n",
                           (int)packetID, senderAddress.ToString(true));
                }
                break;
            }
            }

            peer->DeallocatePacket(packet);
        }
        }

        // 2. Poll each child peer (target → client forwarding)
        for (auto& pair : clients) {
            ClientContext& ctx = pair.second;
            if (!ctx.childPeer) continue;

            RakNet::Packet* cp = ctx.childPeer->Receive();
            if (cp) {
                hadData = true;
                unsigned char id = GetPacketIdentifier(cp);

                if (id == ID_CONNECTION_REQUEST_ACCEPTED) {
                    ctx.targetAddress = cp->systemAddress;
                    printf("[子Peer] 目标服务器连接成功 GUID:%s\n", pair.first.ToString());
                } else if (id == ID_CONNECTION_ATTEMPT_FAILED) {
                    printf("[错误] 子Peer连接目标服务器失败 GUID:%s\n", pair.first.ToString());
                    DestroyClient(ctx.childPeer);
                    ctx.childPeer = NULL;
                } else if ((unsigned char)id >= ID_USER_PACKET_ENUM) {
                    // Target server → client: forward via main peer
                    peer->Send((const char*)(cp->data + 1), cp->length - 1,
                               HIGH_PRIORITY, RELIABLE_ORDERED, 0,
                               ctx.clientAddress, false);
                }
                // Ignore other child peer messages silently

                ctx.childPeer->DeallocatePacket(cp);
            }
        }

        if (!hadData) {
            RakSleep(30);
        }
    }

    // 关闭所有子Peer连接
    for (auto& pair : clients) {
        DestroyClient(pair.second.childPeer);
    }
    clients.clear();

    // ----- 5. 清理资源 -----
    printf("\n正在关闭服务器...\n");
    peer->Shutdown(300);
    RakNet::NatPunchthroughClient::DestroyInstance(natClient);
    RakNet::RakPeerInterface::DestroyInstance(peer);
    printf("服务器已关闭\n");

    return 0;
}
