#if defined(__linux__) && !defined(_GNU_SOURCE)
#define _GNU_SOURCE
#endif

#include "net/platform/UdpBatchOps.hpp"
#include "net/platform/SocketOps.hpp"

#if defined(__linux__)

#include <algorithm>
#include <sys/uio.h>
#include <vector>

namespace LikesProgram {
    namespace Net {
        namespace Internal {
            namespace {
                constexpr std::size_t kUdpMaxDatagramBytes = 65536; // UDP payload 接收容量上限
                constexpr std::size_t kUdpNativeBatchLimit = 64;    // 单次系统调用批量上限

                IoResult MakeBatchOk(std::int64_t nbytes) {
                    return IoResult{ IoStatus::Ok, nbytes, 0 };
                }

                IoResult MakeBatchWouldBlock() {
                    return IoResult{ IoStatus::WouldBlock, 0, 0 };
                }

                IoResult MakeBatchError(int error) {
                    return IoResult{ IoStatus::Error, 0, error };
                }
            }

            bool TryReceiveUdpBatch(
                SocketType fd,
                UdpReceiveDatagram* datagrams,
                std::size_t count,
                sockaddr_storage* lastPeer,
                SocketLength* lastPeerLength,
                IoResult* result) {
                if (result == nullptr) return true;
                if (datagrams == nullptr || count == 0) {
                    *result = MakeBatchOk(0);
                    return true;
                }

                const std::size_t batchCount = (std::min)(count, kUdpNativeBatchLimit); // 控制临时元数据容量
                std::vector<mmsghdr> messages(batchCount); // 内核批量消息描述符
                std::vector<iovec> iovecs(batchCount); // 每条数据报的单段接收缓冲
                std::vector<sockaddr_storage> peers(batchCount); // 每条数据报的发送方地址

                for (std::size_t index = 0; index < batchCount; ++index) {
                    if (datagrams[index].buffer == nullptr) {
                        datagrams[index].result = MakeBatchError(InvalidSocketArgumentError());
                        *result = datagrams[index].result;
                        return true;
                    }

                    std::uint8_t* target =
                        datagrams[index].buffer->PrepareWrite(kUdpMaxDatagramBytes); // 直接写入调用方 Buffer
                    iovecs[index].iov_base = target;
                    iovecs[index].iov_len = kUdpMaxDatagramBytes;
                    messages[index].msg_hdr.msg_name = &peers[index];
                    messages[index].msg_hdr.msg_namelen =
                        static_cast<SocketLength>(sizeof(peers[index]));
                    messages[index].msg_hdr.msg_iov = &iovecs[index];
                    messages[index].msg_hdr.msg_iovlen = 1;
                }

                for (;;) {
                    const int received = ::recvmmsg(
                        fd,
                        messages.data(),
                        static_cast<unsigned int>(batchCount),
                        0,
                        nullptr); // 非阻塞 socket 不在批处理层等待
                    if (received > 0) {
                        std::int64_t totalBytes = 0; // 当前批次总 payload 字节数
                        for (int item = 0; item < received; ++item) {
                            const std::size_t index = static_cast<std::size_t>(item); // 当前完成槽位
                            const std::size_t nread =
                                static_cast<std::size_t>(messages[index].msg_len); // 内核返回长度
                            datagrams[index].buffer->HasWritten(nread);
                            datagrams[index].result = MakeBatchOk(static_cast<std::int64_t>(nread));
                            totalBytes += static_cast<std::int64_t>(nread);

                            const SocketLength peerLength =
                                messages[index].msg_hdr.msg_namelen; // 当前 peer 实际长度
                            if (datagrams[index].peer != nullptr) {
                                *datagrams[index].peer = Address(peers[index], peerLength);
                            }
                        }

                        const std::size_t lastIndex =
                            static_cast<std::size_t>(received - 1); // 最近完成的数据报槽位
                        if (lastPeer != nullptr) *lastPeer = peers[lastIndex];
                        if (lastPeerLength != nullptr) {
                            *lastPeerLength = messages[lastIndex].msg_hdr.msg_namelen;
                        }
                        *result = MakeBatchOk(totalBytes);
                        return true;
                    }

                    if (received == 0) {
                        *result = MakeBatchWouldBlock();
                        return true;
                    }

                    const int error = GetLastSocketError(); // 保存 errno 后再判断重试策略
                    if (IsInterrupted(error)) continue;
                    *result = IsWouldBlock(error) ? MakeBatchWouldBlock() : MakeBatchError(error);
                    return true;
                }
            }

            bool TrySendUdpBatch(
                SocketType fd,
                UdpSendDatagram* datagrams,
                std::size_t count,
                IoResult* result) {
                if (result == nullptr) return true;
                if (datagrams == nullptr || count == 0) {
                    *result = MakeBatchOk(0);
                    return true;
                }

                const std::size_t batchCount = (std::min)(count, kUdpNativeBatchLimit); // 控制单次提交规模
                for (std::size_t index = 0; index < batchCount; ++index) {
                    if (datagrams[index].data == nullptr || datagrams[index].len == 0) {
                        return false;
                    }
                    if (datagrams[index].peer == nullptr
                        || !datagrams[index].peer->IsValid()
                        || datagrams[index].peer->SockAddr() == nullptr
                        || datagrams[index].peer->Length() == 0) {
                        datagrams[index].result = MakeBatchError(InvalidSocketArgumentError());
                        *result = datagrams[index].result;
                        return true;
                    }
                }

                std::vector<mmsghdr> messages(batchCount); // 内核批量消息描述符
                std::vector<iovec> iovecs(batchCount); // 每条数据报的单段发送缓冲
                for (std::size_t index = 0; index < batchCount; ++index) {
                    iovecs[index].iov_base = const_cast<std::uint8_t*>(datagrams[index].data);
                    iovecs[index].iov_len = datagrams[index].len;
                    messages[index].msg_hdr.msg_name =
                        const_cast<sockaddr*>(datagrams[index].peer->SockAddr());
                    messages[index].msg_hdr.msg_namelen = datagrams[index].peer->Length();
                    messages[index].msg_hdr.msg_iov = &iovecs[index];
                    messages[index].msg_hdr.msg_iovlen = 1;
                }

                for (;;) {
                    const int sent = ::sendmmsg(
                        fd,
                        messages.data(),
                        static_cast<unsigned int>(batchCount),
                        0); // 非阻塞 socket 只提交当前可写批次
                    if (sent > 0) {
                        std::int64_t totalBytes = 0; // 当前批次总 payload 字节数
                        for (int item = 0; item < sent; ++item) {
                            const std::size_t index = static_cast<std::size_t>(item); // 当前完成槽位
                            const std::int64_t nwrite =
                                static_cast<std::int64_t>(messages[index].msg_len); // 内核返回长度
                            datagrams[index].result = MakeBatchOk(nwrite);
                            totalBytes += nwrite;
                        }
                        if (static_cast<std::size_t>(sent) < batchCount) {
                            datagrams[static_cast<std::size_t>(sent)].result = MakeBatchWouldBlock();
                        }
                        *result = MakeBatchOk(totalBytes);
                        return true;
                    }

                    if (sent == 0) {
                        *result = MakeBatchWouldBlock();
                        return true;
                    }

                    const int error = GetLastSocketError(); // 保存 errno 后再判断重试策略
                    if (IsInterrupted(error)) continue;
                    *result = IsWouldBlock(error) ? MakeBatchWouldBlock() : MakeBatchError(error);
                    return true;
                }
            }
        }
    }
}

#endif
