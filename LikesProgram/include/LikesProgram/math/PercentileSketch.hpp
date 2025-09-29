#pragma once
#include "../LikesProgramLibExport.hpp"
#include <vector>
#include <shared_mutex>

namespace LikesProgram {
    namespace Math {
        class LIKESPROGRAM_API PercentileSketch {
        public:
            explicit PercentileSketch(size_t compression = 400, size_t shards = 8);
            // 拷贝构造 & 拷贝赋值
            PercentileSketch(const PercentileSketch& other);
            PercentileSketch& operator=(const PercentileSketch& other);
            // 移动构造 & 移动赋值
            PercentileSketch(PercentileSketch&& other) noexcept;
            PercentileSketch& operator=(PercentileSketch&& other) noexcept;
            ~PercentileSketch();

            // 基本操作
            void Add(double x);
            void AddBatch(const std::vector<double>& xs);
            double Quantile(double q) const;
            void Compress();

            // 分布式/并行支持
            void Merge(const PercentileSketch& other);

            // 序列化（小端二进制）
            void Serialize(std::ostream& os) const;
            static PercentileSketch Deserialize(std::istream& is);

            // 调试/监控
            std::vector<std::pair<double, int>> GetCentroids() const;

            void Reset();
        private:
            struct Centroid {
                double mean;
                int count;
            };

            // 每个分片都有独立的锁 + buffer，减少竞争
            struct Shard {
                mutable std::shared_mutex mtx;
                std::vector<Centroid> centroids;
                int totalCount = 0;
                std::vector<double> buffer; // 无锁批量写入前的缓存
            };

            size_t m_compression;
            size_t m_shards;

            struct PercentileSketchImpl;
            PercentileSketchImpl* m_impl;

            void flushShard(Shard& shard);
            static void compressCentroids(std::vector<Centroid>& centroids, size_t compression);
        };
    }
}
