#include "../../../include/LikesProgram/math/PercentileSketch.hpp"
#include <algorithm>
#include <stdexcept>
#include <cmath>
#include <cstring>
#include <mutex>
#include <atomic>
#include <ostream>
#include <istream>
#include <cstdint>
#include <utility>
#include <memory>

namespace LikesProgram {
    namespace Math {
        struct PercentileSketch::PercentileSketchImpl {
            std::vector<std::unique_ptr<Shard>> m_shardData;
        };

        PercentileSketch::PercentileSketch(size_t compression, size_t shards)
            : m_compression(compression), m_shards(shards), m_impl(new PercentileSketchImpl{}) {
            if (compression < 20) compression = 20;
            if (shards == 0) shards = 1;
            m_impl->m_shardData.reserve(shards);
            for (size_t i = 0; i < shards; ++i) {
                m_impl->m_shardData.emplace_back(std::make_unique<Shard>());
            }
        }
        PercentileSketch::PercentileSketch(const PercentileSketch& other)
            : m_compression(other.m_compression), m_shards(other.m_shards) {
            m_impl = new PercentileSketchImpl;
            m_impl->m_shardData.reserve(m_shards);
            for (const auto& shardPtr : other.m_impl->m_shardData) {
                auto newShard = std::make_unique<Shard>();
                std::shared_lock lock(shardPtr->mtx); // 共享锁
                newShard->centroids = shardPtr->centroids;
                newShard->totalCount = shardPtr->totalCount;
                newShard->buffer = shardPtr->buffer;
                m_impl->m_shardData.emplace_back(std::move(newShard));
            }
        }
        PercentileSketch& PercentileSketch::operator=(const PercentileSketch& other) {
            if (this == &other) return *this;

            m_compression = other.m_compression;
            m_shards = other.m_shards;

            PercentileSketchImpl* newImpl = new PercentileSketchImpl;
            newImpl->m_shardData.reserve(m_shards);
            for (const auto& shardPtr : other.m_impl->m_shardData) {
                auto newShard = std::make_unique<Shard>();
                std::shared_lock lock(shardPtr->mtx); // 共享锁
                newShard->centroids = shardPtr->centroids;
                newShard->totalCount = shardPtr->totalCount;
                newShard->buffer = shardPtr->buffer;
                newImpl->m_shardData.emplace_back(std::move(newShard));
            }

            delete m_impl;
            m_impl = newImpl;

            return *this;
        }

        PercentileSketch::PercentileSketch(PercentileSketch&& other) noexcept
            : m_compression(other.m_compression), m_shards(other.m_shards), m_impl(other.m_impl) {
            other.m_impl = nullptr;
        }
        PercentileSketch& PercentileSketch::operator=(PercentileSketch&& other) noexcept {
            if (this == &other) return *this;

            m_compression = other.m_compression;
            m_shards = other.m_shards;

            delete m_impl;
            m_impl = other.m_impl;
            other.m_impl = nullptr;

            return *this;
        }

        PercentileSketch::~PercentileSketch() {
            if (m_impl) delete m_impl;
            m_impl = nullptr;
        }

        void PercentileSketch::Add(double x) {
            size_t shardId = std::hash<std::thread::id>{}(std::this_thread::get_id()) % m_shards;
            Shard& shard = *(m_impl->m_shardData[shardId]);
            {
                std::unique_lock lock(shard.mtx); // 独占锁
                shard.buffer.push_back(x);
                if (shard.buffer.size() >= 64) {
                    flushShard(shard);
                }
            }
        }

        void PercentileSketch::AddBatch(const std::vector<double>& xs) {
            for (double v : xs) Add(v);
        }

        void PercentileSketch::flushShard(Shard& shard) {
            if (shard.buffer.empty()) return;
            std::sort(shard.buffer.begin(), shard.buffer.end());

            // 简单地把 buffer 的点合并为单点质心
            for (double v : shard.buffer) {
                shard.centroids.push_back({ v, 1 });
                shard.totalCount++;
            }
            shard.buffer.clear();

            // 压缩质心
            if (shard.centroids.size() > m_compression * 4) {
                compressCentroids(shard.centroids, m_compression);
            }
        }

        void PercentileSketch::Compress() {
            for (auto& shard : m_impl->m_shardData) {
                std::unique_lock lock(shard->mtx); // 独占锁
                flushShard(*shard);
                compressCentroids(shard->centroids, m_compression);
            }
        }

        double PercentileSketch::Quantile(double q) const {
            if (q < 0.0 || q > 1.0) throw std::invalid_argument("Quantile q must be between 0 and 1");

            // 收集所有分片
            std::vector<Centroid> merged;
            int totalCount = 0;
            for (auto& shard : m_impl->m_shardData) {
                std::shared_lock lock(shard->mtx); // 共享锁
                totalCount += shard->totalCount;
                merged.insert(merged.end(), shard->centroids.begin(), shard->centroids.end());
            }
            if (totalCount == 0) return NAN;

            compressCentroids(merged, m_compression);
            std::sort(merged.begin(), merged.end(),
                [](auto& a, auto& b) { return a.mean < b.mean; });

            int targetRank = static_cast<int>(q * totalCount);
            int cum = 0;
            for (auto& c : merged) {
                cum += c.count;
                if (cum >= targetRank) return c.mean;
            }
            return merged.back().mean;
        }

        void PercentileSketch::compressCentroids(std::vector<Centroid>& centroids, size_t compression) {
            if (centroids.empty()) return;
            std::sort(centroids.begin(), centroids.end(),
                [](auto& a, auto& b) { return a.mean < b.mean; });

            std::vector<Centroid> out;
            out.reserve(centroids.size());

            double totalCount = 0;
            for (auto& c : centroids) totalCount += c.count;

            double cumCount = 0;
            Centroid cur = centroids[0];
            for (size_t i = 1; i < centroids.size(); i++) {
                double q = cumCount / totalCount;
                double k = 4 * totalCount * q * (1 - q) / compression; // 论文近似公式

                if (cur.count + centroids[i].count <= k) {
                    double newCount = cur.count + centroids[i].count;
                    cur.mean = (cur.mean * cur.count + centroids[i].mean * centroids[i].count) / newCount;
                    cur.count = static_cast<int>(newCount);
                }
                else {
                    out.push_back(cur);
                    cur = centroids[i];
                }
                cumCount += centroids[i].count;
            }
            out.push_back(cur);
            centroids.swap(out);
        }

        void PercentileSketch::Merge(const PercentileSketch& other) {
            for (size_t i = 0; i < other.m_shards; ++i) {
                const Shard& src = *other.m_impl->m_shardData[i];
                std::shared_lock lock(src.mtx); // 共享锁
                for (auto& c : src.centroids) {
                    Add(c.mean); // 简化：逐点合并
                }
            }
        }

        void PercentileSketch::Serialize(std::ostream& os) const {
            uint64_t magic = 0x5444494745534B; // "TDIGESK"
            os.write(reinterpret_cast<const char*>(&magic), sizeof(magic));
            os.write(reinterpret_cast<const char*>(&m_compression), sizeof(m_compression));
            uint64_t shardCount = m_shards;
            os.write(reinterpret_cast<const char*>(&shardCount), sizeof(shardCount));

            for (auto& shard : m_impl->m_shardData) {
                std::shared_lock lock(shard->mtx); // 共享锁
                uint64_t n = shard->centroids.size();
                os.write(reinterpret_cast<const char*>(&n), sizeof(n));
                for (auto& c : shard->centroids) {
                    os.write(reinterpret_cast<const char*>(&c.mean), sizeof(c.mean));
                    os.write(reinterpret_cast<const char*>(&c.count), sizeof(c.count));
                }
            }
        }

        PercentileSketch PercentileSketch::Deserialize(std::istream& is) {
            uint64_t magic;
            is.read(reinterpret_cast<char*>(&magic), sizeof(magic));
            if (magic != 0x5444494745534B) throw std::runtime_error("Invalid sketch format");

            size_t compression, shardCount;
            is.read(reinterpret_cast<char*>(&compression), sizeof(compression));
            is.read(reinterpret_cast<char*>(&shardCount), sizeof(shardCount));

            PercentileSketch sketch(compression, shardCount);
            for (size_t i = 0; i < shardCount; i++) {
                uint64_t n;
                is.read(reinterpret_cast<char*>(&n), sizeof(n));
                auto& shard = *sketch.m_impl->m_shardData[i];
                shard.centroids.resize(n);
                for (size_t j = 0; j < n; j++) {
                    is.read(reinterpret_cast<char*>(&shard.centroids[j].mean), sizeof(double));
                    is.read(reinterpret_cast<char*>(&shard.centroids[j].count), sizeof(int));
                    shard.totalCount += shard.centroids[j].count;
                }
            }
            return sketch;
        }

        std::vector<std::pair<double, int>> PercentileSketch::GetCentroids() const {
            std::vector<std::pair<double, int>> res;
            for (auto& shard : m_impl->m_shardData) {
                std::shared_lock lock(shard->mtx); // 共享锁
                for (auto& c : shard->centroids) {
                    res.emplace_back(c.mean, c.count);
                }
            }
            return res;
        }
    }
}