#include "../../../include/LikesProgram/system/CoreUtils.hpp"
#if defined(_WIN32)
#ifndef NOMINMAX
#define NOMINMAX
#endif
#ifndef _WINSOCK_DEPRECATED_NO_WARNINGS
#define _WINSOCK_DEPRECATED_NO_WARNINGS
#endif
#include <winsock2.h>
#include <windows.h>
#include <iphlpapi.h>
#include <ws2tcpip.h>
#pragma comment(lib, "iphlpapi.lib")
#pragma comment(lib, "ws2_32.lib")
#elif defined(__APPLE__) || defined(__linux__) || defined(__unix__)
#include <pthread.h>
#include <ifaddrs.h>
#include <net/if.h>
#include <sys/ioctl.h>
#include <unistd.h>
#include <arpa/inet.h>
#include <cstring>
#endif
#include <mutex>
#include <random>
#include <cinttypes>
#include <thread>

namespace LikesProgram {
    namespace CoreUtils {
        void SetCurrentThreadName(const LikesProgram::String& name) {
#if defined(_WIN32)
            // Windows 10 1607+
            using SetThreadDescription_t = HRESULT(WINAPI*)(HANDLE, PCWSTR);
            HMODULE h = GetModuleHandleW(L"Kernel32.dll");
            if (h) {
                auto p = reinterpret_cast<SetThreadDescription_t>(GetProcAddress(h, "SetThreadDescription"));
                if (p) {
                    // 转换成 UTF-16
                    std::wstring wname = name.ToWString();
                    p(GetCurrentThread(), wname.c_str());
                }
            }
#elif defined(__APPLE__) || defined(__linux__) || defined(__unix__)
            // pthread_setname_np 限制16字节（含\0），截断
            std::string trimmed = name.SubString(0, 15).ToStdString();
#if defined(__APPLE__)
            (void)pthread_setname_np(trimmed.c_str());
#elif defined(__linux__)
            (void)pthread_setname_np(pthread_self(), trimmed.c_str());
#else
            (void)trimmed;
#endif
#else
            (void)name;
#endif
        }

        LikesProgram::String GetCurrentThreadName()
        {
#if defined(_WIN32)
            using GetThreadDescription_t = HRESULT(WINAPI*)(HANDLE, PWSTR*);
            HMODULE h = GetModuleHandleW(L"Kernel32.dll");
            if (h) {
                auto p = reinterpret_cast<GetThreadDescription_t>(GetProcAddress(h, "GetThreadDescription"));
                if (p) {
                    PWSTR wstr = nullptr;
                    if (SUCCEEDED(p(GetCurrentThread(), &wstr)) && wstr) {
                        std::wstring ws(wstr);
                        LocalFree(wstr);
                        return LikesProgram::String(ws);
                    }
                }
            }
            return LikesProgram::String();
#elif defined(__linux__)
            char buf[16] = { 0 };
            if (pthread_getname_np(pthread_self(), buf, sizeof(buf)) == 0) {
                return LikesProgram::String(buf);
            }
            return LikesProgram::String();
#elif defined(__APPLE__)
            char buf[64] = { 0 };
            if (pthread_getname_np(pthread_self(), buf, sizeof(buf)) == 0) {
                return LikesProgram::String(buf);
            }
            return LikesProgram::String();
#else
            return LikesProgram::String();
#endif
        }

        LikesProgram::String GetMACAddress()
        {
            static LikesProgram::String cachedMAC;
            static std::once_flag flag;

            std::call_once(flag, []() {
#ifdef _WIN32
                IP_ADAPTER_INFO AdapterInfo[16];
                DWORD dwBufLen = sizeof(AdapterInfo);
                if (GetAdaptersInfo(AdapterInfo, &dwBufLen) == NO_ERROR) {
                    PIP_ADAPTER_INFO pAdapterInfo = AdapterInfo;
                    for (; pAdapterInfo; pAdapterInfo = pAdapterInfo->Next) {
                        if (pAdapterInfo->Type == MIB_IF_TYPE_ETHERNET) { // 物理网卡
                            char mac[18] = { 0 };
                            std::snprintf(mac, sizeof(mac), "%02X:%02X:%02X:%02X:%02X:%02X",
                                pAdapterInfo->Address[0], pAdapterInfo->Address[1],
                                pAdapterInfo->Address[2], pAdapterInfo->Address[3],
                                pAdapterInfo->Address[4], pAdapterInfo->Address[5]);
                            cachedMAC = LikesProgram::String(mac);
                            break;
                        }
                    }
                }
#else
                struct ifaddrs* ifaddr, * ifa;
                if (getifaddrs(&ifaddr) == 0) {
                    for (ifa = ifaddr; ifa; ifa = ifa->ifa_next) {
                        if (!ifa->ifa_addr) continue;
                        if (ifa->ifa_addr->sa_family == AF_PACKET) { // 数据链路层
                            std::string name = ifa->ifa_name;
                            if (name == "lo" || name.find("docker") != std::string::npos ||
                                name.find("veth") != std::string::npos)
                                continue; // 跳过环回和虚拟接口

                            int fd = socket(AF_INET, SOCK_DGRAM, 0);
                            if (fd < 0) continue;

                            struct ifreq ifr;
                            std::strncpy(ifr.ifr_name, ifa->ifa_name, IFNAMSIZ - 1);
                            if (ioctl(fd, SIOCGIFHWADDR, &ifr) == 0) {
                                char mac[18] = { 0 };
                                std::snprintf(mac, sizeof(mac), "%02x:%02x:%02x:%02x:%02x:%02x",
                                    (unsigned char)ifr.ifr_hwaddr.sa_data[0],
                                    (unsigned char)ifr.ifr_hwaddr.sa_data[1],
                                    (unsigned char)ifr.ifr_hwaddr.sa_data[2],
                                    (unsigned char)ifr.ifr_hwaddr.sa_data[3],
                                    (unsigned char)ifr.ifr_hwaddr.sa_data[4],
                                    (unsigned char)ifr.ifr_hwaddr.sa_data[5]);
                                cachedMAC = LikesProgram::String(mac);
                                close(fd);
                                break;
                            }
                            close(fd);
                        }
                    }
                    freeifaddrs(ifaddr);
                }
#endif
                });

            return cachedMAC;
        }

        LikesProgram::String GetLocalIPAddress() {
            static LikesProgram::String cachedIP;
            static std::once_flag flag;

            std::call_once(flag, []() {
#ifdef _WIN32
                WSADATA wsaData;
                if (WSAStartup(MAKEWORD(2, 2), &wsaData) != 0) return;

                char hostname[256] = { 0 };
                if (gethostname(hostname, sizeof(hostname)) == SOCKET_ERROR) {
                    WSACleanup();
                    return;
                }

                addrinfo hints{}, * res = nullptr;
                hints.ai_family = AF_INET; // IPv4
                hints.ai_socktype = SOCK_STREAM;

                if (getaddrinfo(hostname, nullptr, &hints, &res) != 0) {
                    WSACleanup();
                    return;
                }

                std::string ip;
                for (addrinfo* p = res; p != nullptr; p = p->ai_next) {
                    sockaddr_in* sockaddr_ipv4 = reinterpret_cast<sockaddr_in*>(p->ai_addr);
                    ip = inet_ntoa(sockaddr_ipv4->sin_addr);
                    if (ip != "127.0.0.1") break;
                }

                freeaddrinfo(res);
                WSACleanup();
                cachedIP = LikesProgram::String(ip);
#else
                struct ifaddrs* ifaddr, * ifa;
                if (getifaddrs(&ifaddr) == -1) return;

                char ip[INET_ADDRSTRLEN] = { 0 };
                for (ifa = ifaddr; ifa; ifa = ifa->ifa_next) {
                    if (!ifa->ifa_addr) continue;
                    if (ifa->ifa_addr->sa_family == AF_INET) { // IPv4
                        std::string name = ifa->ifa_name;
                        if (name == "lo" || name.find("docker") != std::string::npos ||
                            name.find("veth") != std::string::npos)
                            continue; // 跳过环回和虚拟接口

                        void* addrPtr = &((struct sockaddr_in*)ifa->ifa_addr)->sin_addr;
                        if (inet_ntop(AF_INET, addrPtr, ip, INET_ADDRSTRLEN)) {
                            cachedIP = LikesProgram::String(ip);
                            break;
                        }
                    }
                }
                freeifaddrs(ifaddr);
#endif
                });

            return cachedIP;
        }

        LikesProgram::String GenerateUUID(LikesProgram::String prefix) {
            // 随机数生成器
            static thread_local std::mt19937_64 rng{ std::random_device{}() };
            std::uniform_int_distribution<uint64_t> dist;

            // 线程本地自增序列号
            static thread_local uint16_t seq = 0;

            // 纳秒级时间戳
            uint64_t ts = std::chrono::duration_cast<std::chrono::nanoseconds>(
                std::chrono::steady_clock::now().time_since_epoch()).count();

            // 随机数部分
            uint64_t randPart = dist(rng);

            // 线程 ID 部分
            std::hash<std::thread::id> threadHasher;
            uint64_t threadHash = threadHasher(std::this_thread::get_id());

            // 序号（每线程独立自增，避免同纳秒重复）
            uint16_t mySeq = seq++;

            // 机器 MAC
            LikesProgram::String mac = GetMACAddress(); // 系统调用获取 MAC
            std::hash<std::string> hasher;
            uint16_t macHash = static_cast<uint16_t>(hasher(mac.ToStdString()) & 0xFFFF);
            // 机器 IP
            LikesProgram::String ip = GetLocalIPAddress();
            uint16_t ipHash = static_cast<uint16_t>(hasher(ip.ToStdString()) & 0xFFFF);
            // 生成机器 ID 部分
            uint16_t machineId = (macHash & 0x0FFF) | ((ipHash & 0x000F) << 12);

            char buf[61]; // 16+16+16+4+4+4 hex = 60 + '\0'
            std::snprintf(buf, sizeof(buf), "%016" PRIx64 "%016" PRIx64 "%016" PRIx64 "%04x%04x",
                ts, threadHash, randPart, mySeq, machineId);

            // 拼接前缀
            return prefix.Empty() ? LikesProgram::String(buf) : prefix.Append(LikesProgram::String(buf));
        }
    }
}
