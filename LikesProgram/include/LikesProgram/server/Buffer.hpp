#pragma once
#include <vector>
#include <cstdint>
#include <cstring>
#include <algorithm>

namespace LikesProgram {
    namespace Server {

        // 高效动态缓冲区
        class Buffer {
        public:
            // 构造函数
            Buffer() = default;

            // 构造函数，指定初始容量
            explicit Buffer(size_t capacity) {
                m_bytes.reserve(capacity); // 预分配空间，减少动态扩容
            }

            // 清空缓冲区
            void Clear() {
                m_bytes.clear();           // 清除所有字节
            }

            // 获取缓冲区当前大小
            size_t Size() const {
                return m_bytes.size();     // 返回当前数据长度
            }

            // 获取底层指针
            const uint8_t* Data() const {
                return m_bytes.data();     // 返回数据指针，方便 Transport 使用
            }

            // 追加数据
            void Append(const uint8_t* data, size_t len) {
                if (!data || len == 0) return;
                m_bytes.insert(m_bytes.end(), data, data + len); // 在末尾追加数据
            }

            // 追加 Buffer
            void Append(const Buffer& buf) {
                Append(buf.Data(), buf.Size());                  // 调用上面的 Append
            }

            // 移除前 len 个字节
            void Consume(size_t len) {
                if (len >= m_bytes.size()) {
                    Clear();                                     // 超出大小则清空
                }
                else {
                    m_bytes.erase(m_bytes.begin(), m_bytes.begin() + len); // 删除前 len 个字节
                }
            }

        private:
            std::vector<uint8_t> m_bytes; // 底层字节数组，动态增长
        };

    } // namespace Server
} // namespace LikesProgram
