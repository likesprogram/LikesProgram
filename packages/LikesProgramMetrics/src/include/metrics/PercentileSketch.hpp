#pragma once

#include <cstddef>
#include <cstdint>
#include <istream>
#include <ostream>
#include <shared_mutex>
#include <utility>
#include <vector>

namespace LikesProgram {
    namespace Metrics {
        namespace Internal {
            // Summary 私有百分位估算器，不作为公共 Math/Stats 能力导出。
            class PercentileSketch {
            public:
                // 创建分片 sketch，compression 越大尾部精度越高。
                explicit PercentileSketch(size_t compression = 400, size_t shards = 8);
                // 深拷贝所有分片、质心和缓冲区。
                PercentileSketch(const PercentileSketch& other);
                // 深拷贝赋值所有分片、质心和缓冲区。
                PercentileSketch& operator=(const PercentileSketch& other);
                // 移动接管 sketch 实现。
                PercentileSketch(PercentileSketch&& other) noexcept;
                // 移动赋值 sketch 实现。
                PercentileSketch& operator=(PercentileSketch&& other) noexcept;
                // 释放 sketch 实现。
                ~PercentileSketch();

                // 添加单个样本。
                void Add(double value);
                // 批量添加样本。
                void AddBatch(const std::vector<double>& values);
                // 查询分位数，q 必须位于 [0, 1]。
                double Quantile(double q) const;
                // 将分片缓冲合并到质心，降低查询成本。
                void Compress();
                // 合并另一个 sketch 的质心。
                void Merge(const PercentileSketch& other);

                // 序列化为小端二进制格式，仅服务包内调试和测试。
                void Serialize(std::ostream& os) const;
                // 从小端二进制格式反序列化 sketch。
                static PercentileSketch Deserialize(std::istream& is);
                // 返回质心快照，便于测试估算器状态。
                std::vector<std::pair<double, int64_t>> GetCentroids() const;
                // 清空所有分片。
                void Reset();
            private:
                struct Centroid {
                    double mean = 0.0;  // 质心均值
                    int64_t count = 0;  // 质心覆盖样本数
                };

                struct Shard {
                    mutable std::shared_mutex m_mutex; // 保护当前分片质心和缓冲区
                    std::vector<Centroid> m_centroids; // 已压缩质心集合
                    int64_t m_totalCount = 0;          // 当前分片总样本数
                    std::vector<double> m_buffer;      // 写入缓冲，满阈值后压缩
                };

                size_t m_compression = 400; // 压缩目标规模下限
                size_t m_shards = 8;        // 分片数量，降低多线程写入竞争

                struct PercentileSketchImpl;
                PercentileSketchImpl* m_impl = nullptr; // 分片数组 PImpl

                // 将单个分片的缓冲区刷入质心集合。
                void FlushShard(Shard& shard);
                // 对质心集合执行一次近似压缩。
                static void CompressCentroids(std::vector<Centroid>& centroids, size_t compression);
            };
        }
    }
}
