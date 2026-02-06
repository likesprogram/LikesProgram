#pragma once
#include "Buffer.hpp"
#include "Transport.hpp"
#include "Channel.hpp"
#include "EventLoop.hpp"
#include <memory>
#include <functional>
#include <atomic>

namespace LikesProgram {
    namespace Server {
        class Connection {
        public:
            // 构造函数，绑定 fd 和 EventLoop
            Connection(SocketType fd, EventLoop* loop)
                : m_fd(fd),                              // 保存文件描述符
                m_loop(loop),                          // 保存 EventLoop
                m_channel(nullptr),                    // Channel 初始化为空
                m_closed(false) {
            }

            virtual ~Connection() {
                Close();                                  // 析构时关闭
            }

            // 获取文件描述符
            int GetSocket() const { return m_fd; }

            // 设置 Channel
            void SetChannel(Channel* channel) { m_channel = channel; }

            // 读取数据
            void Readable() {
                if (m_closed) return;                     // 已关闭则返回
                Buffer buf;                               // 临时 Buffer
                int n = m_transport->Read(buf);           // 调用 Transport 读数据
                if (n > 0) {
                    OnMessage(buf);                        // 回调用户消息处理
                }
                else if (n == 0) {
                    Close();                               // 对端关闭
                }
                else {
                    // 读错误处理，可扩展
                    Close();
                }
            }

            // 写入数据
            void Write(const Buffer& buf) {
                if (m_closed) return;                     // 已关闭
                m_transport->Write(buf);                  // 调用 Transport 写入
            }

            // 请求升级 TLS（可选）
            void RequestTLSUpgrade() {
                if (m_transport) {
                    m_transport->SSLHandshake();          // 调用 Transport TLS 握手
                }
            }

            // 主动关闭
            void Close() {
                if (m_closed.exchange(true)) return;      // 原子检查是否已关闭
                if (m_channel && m_loop) {
                    m_loop->UnregisterChannel(m_channel); // 注销 Channel
                }
                if (m_transport) m_transport->Close();    // 关闭底层 Transport
                OnClose();                                // 回调用户关闭事件
            }

            // 用户可重写：接收消息回调
            virtual void OnMessage(const Buffer& buf) {
                // 默认实现：日志或直接忽略
            }

            // 用户可重写：关闭事件回调
            virtual void OnClose() {
                // 默认实现：清理资源或通知上层
            }

            // 内部写事件通知
            void Writable() {
                if (m_closed) return;
                // 可实现写缓冲队列发送逻辑
            }

            // 内部读事件通知
            void OnMessageRead() {
                Read();                                   // 调用 Read 处理
            }

        private:
            int m_fd;                                       // 底层 socket fd
            std::unique_ptr<Transport> m_transport;         // TCP/TLS 传输对象
            EventLoop* m_loop;                              // 所属 EventLoop
            Channel* m_channel;                             // 对应 Channel
            std::atomic<bool> m_closed;                     // 是否已关闭
        };

    } // namespace Server
} // namespace LikesProgram
