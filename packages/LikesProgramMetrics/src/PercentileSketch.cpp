#include <metrics/PercentileSketch.hpp>

#include <algorithm>
#include <atomic>
#include <cmath>
#include <cstdint>
#include <limits>
#include <memory>
#include <mutex>
#include <stdexcept>
#include <thread>

namespace LikesProgram {
    namespace Metrics {
        namespace Internal {
            struct PercentileSketch::PercentileSketchImpl {
                std::vector<std::unique_ptr<Shard>> m_shardData; // 分片数组，元素地址稳定
            };

            PercentileSketch::PercentileSketch(size_t compression, size_t shards)
                : m_compression(compression < 20 ? 20 : compression),
                m_shards(shards == 0 ? 1 : shards),
                m_impl(new PercentileSketchImpl{}) {
                // 分片按构造参数一次性创建，Add 路径只做哈希取模。
                m_impl->m_shardData.reserve(m_shards);
                for (size_t i = 0; i < m_shards; ++i) {
                    m_impl->m_shardData.emplace_back(std::make_unique<Shard>());
                }
            }

            PercentileSketch::PercentileSketch(const PercentileSketch& other)
                : m_compression(other.m_compression),
                m_shards(other.m_shards),
                m_impl(new PercentileSketchImpl{}) {
                // 每个分片独立加共享锁，复制时保留缓冲和质心状态。
                m_impl->m_shardData.reserve(m_shards);
                for (const auto& shardPtr : other.m_impl->m_shardData) {
                    auto newShard = std::make_unique<Shard>();
                    std::shared_lock lock(shardPtr->m_mutex);
                    newShard->m_centroids = shardPtr->m_centroids;
                    newShard->m_totalCount = shardPtr->m_totalCount;
                    newShard->m_buffer = shardPtr->m_buffer;
                    m_impl->m_shardData.emplace_back(std::move(newShard));
                }
            }

            PercentileSketch& PercentileSketch::operator=(const PercentileSketch& other) {
                if (this == &other) return *this;

                auto* next = new PercentileSketchImpl{}; // 新实现先构造，保证异常安全
                next->m_shardData.reserve(other.m_shards);
                for (const auto& shardPtr : other.m_impl->m_shardData) {
                    auto newShard = std::make_unique<Shard>();
                    std::shared_lock lock(shardPtr->m_mutex);
                    newShard->m_centroids = shardPtr->m_centroids;
                    newShard->m_totalCount = shardPtr->m_totalCount;
                    newShard->m_buffer = shardPtr->m_buffer;
                    next->m_shardData.emplace_back(std::move(newShard));
                }

                delete m_impl;
                m_compression = other.m_compression;
                m_shards = other.m_shards;
                m_impl = next;
                return *this;
            }

            PercentileSketch::PercentileSketch(PercentileSketch&& other) noexcept
                : m_compression(other.m_compression),
                m_shards(other.m_shards),
                m_impl(other.m_impl) {
                other.m_impl = nullptr;
            }

            PercentileSketch& PercentileSketch::operator=(PercentileSketch&& other) noexcept {
                if (this == &other) return *this;

                delete m_impl;
                m_compression = other.m_compression;
                m_shards = other.m_shards;
                m_impl = other.m_impl;
                other.m_impl = nullptr;
                return *this;
            }

            PercentileSketch::~PercentileSketch() {
                delete m_impl;
                m_impl = nullptr;
            }

            void PercentileSketch::Add(double value) {
                if (!m_impl || !std::isfinite(value)) return;

                const size_t shardId = std::hash<std::thread::id>{}(
                    std::this_thread::get_id()) % m_shards; // 按线程分片降低写竞争
                Shard& shard = *(m_impl->m_shardData[shardId]);

                std::unique_lock lock(shard.m_mutex);
                shard.m_buffer.push_back(value);
                if (shard.m_buffer.size() >= 64) {
                    FlushShard(shard);
                }
            }

            void PercentileSketch::AddBatch(const std::vector<double>& values) {
                for (double value : values) {
                    Add(value);
                }
            }

            void PercentileSketch::FlushShard(Shard& shard) {
                if (shard.m_buffer.empty()) return;

                for (double value : shard.m_buffer) {
                    shard.m_centroids.push_back(Centroid{ value, 1 });
                    if (shard.m_totalCount < std::numeric_limits<int64_t>::max()) {
                        ++shard.m_totalCount;
                    }
                }
                shard.m_buffer.clear();

                if (shard.m_centroids.size() > m_compression * 4) {
                    CompressCentroids(shard.m_centroids, m_compression);
                }
            }

            void PercentileSketch::Compress() {
                if (!m_impl) return;

                for (auto& shard : m_impl->m_shardData) {
                    std::unique_lock lock(shard->m_mutex);
                    FlushShard(*shard);
                    CompressCentroids(shard->m_centroids, m_compression);
                }
            }

            double PercentileSketch::Quantile(double q) const {
                if (q < 0.0 || q > 1.0) {
                    throw std::invalid_argument("Quantile q must be between 0 and 1");
                }
                if (!m_impl) return NAN;

                std::vector<Centroid> merged; // 所有分片质心快照
                int64_t totalCount = 0;       // 全局样本总数
                for (const auto& shard : m_impl->m_shardData) {
                    std::shared_lock lock(shard->m_mutex);
                    totalCount = totalCount > std::numeric_limits<int64_t>::max() - shard->m_totalCount
                        ? std::numeric_limits<int64_t>::max()
                        : totalCount + shard->m_totalCount;
                    merged.insert(merged.end(), shard->m_centroids.begin(), shard->m_centroids.end());
                }

                if (totalCount == 0 || merged.empty()) return NAN;
                CompressCentroids(merged, m_compression);
                std::sort(merged.begin(), merged.end(),
                    [](const Centroid& left, const Centroid& right) {
                        return left.mean < right.mean;
                    });

                const auto targetRank = static_cast<int64_t>(
                    std::ceil(q * static_cast<double>(totalCount))); // 目标全局排名
                int64_t cumulative = 0; // 已覆盖的样本数量
                for (const auto& centroid : merged) {
                    cumulative += centroid.count;
                    if (cumulative >= targetRank) return centroid.mean;
                }

                return merged.back().mean;
            }

            void PercentileSketch::CompressCentroids(std::vector<Centroid>& centroids, size_t compression) {
                if (centroids.empty()) return;

                std::sort(centroids.begin(), centroids.end(),
                    [](const Centroid& left, const Centroid& right) {
                        return left.mean < right.mean;
                    });

                std::vector<Centroid> output; // 压缩后的质心集合
                output.reserve(centroids.size());

                double totalCount = 0.0; // 估算压缩阈值需要的样本总数
                for (const auto& centroid : centroids) {
                    totalCount += centroid.count;
                }

                double cumulative = 0.0; // 当前扫描到的累计样本数
                Centroid current = centroids.front();
                for (size_t i = 1; i < centroids.size(); ++i) {
                    const double quantile = totalCount > 0.0 ? cumulative / totalCount : 0.0;
                    const double limit = 4.0 * totalCount * quantile * (1.0 - quantile)
                        / static_cast<double>(compression);

                    if (current.count + centroids[i].count <= std::max(1.0, limit)) {
                        const int64_t combinedCount = current.count > std::numeric_limits<int64_t>::max()
                            - centroids[i].count
                            ? std::numeric_limits<int64_t>::max()
                            : current.count + centroids[i].count; // 合并后的样本覆盖数
                        const long double weighted =
                            (static_cast<long double>(current.mean) * static_cast<long double>(current.count)
                                + static_cast<long double>(centroids[i].mean)
                                * static_cast<long double>(centroids[i].count))
                            / static_cast<long double>(combinedCount); // 使用更宽中间值降低极端权重溢出风险
                        if (weighted > static_cast<long double>(std::numeric_limits<double>::max())) {
                            current.mean = std::numeric_limits<double>::max();
                        }
                        else if (weighted < static_cast<long double>(std::numeric_limits<double>::lowest())) {
                            current.mean = std::numeric_limits<double>::lowest();
                        }
                        else {
                            current.mean = static_cast<double>(weighted);
                        }
                        current.count = combinedCount;
                    }
                    else {
                        output.push_back(current);
                        current = centroids[i];
                    }
                    cumulative += centroids[i].count;
                }

                output.push_back(current);
                centroids.swap(output);
            }

            void PercentileSketch::Merge(const PercentileSketch& other) {
                if (!m_impl || !other.m_impl) return;

                std::vector<Centroid> snapshot; // 先取源快照，避免跨对象长时间持锁
                snapshot.reserve(other.m_compression);

                for (const auto& shard : other.m_impl->m_shardData) {
                    std::shared_lock lock(shard->m_mutex);
                    snapshot.insert(snapshot.end(), shard->m_centroids.begin(), shard->m_centroids.end());
                    for (double value : shard->m_buffer) {
                        if (std::isfinite(value)) {
                            snapshot.push_back(Centroid{ value, 1 });
                        }
                    }
                }

                const size_t shardId = std::hash<std::thread::id>{}(
                    std::this_thread::get_id()) % m_shards; // 合并写入当前线程所属分片
                Shard& targetShard = *(m_impl->m_shardData[shardId]);

                std::unique_lock lock(targetShard.m_mutex);
                targetShard.m_centroids.reserve(targetShard.m_centroids.size() + snapshot.size());
                for (const auto& centroid : snapshot) {
                    if (!std::isfinite(centroid.mean) || centroid.count <= 0) continue;

                    targetShard.m_centroids.push_back(centroid);
                    targetShard.m_totalCount =
                        targetShard.m_totalCount > std::numeric_limits<int64_t>::max() - centroid.count
                        ? std::numeric_limits<int64_t>::max()
                        : targetShard.m_totalCount + centroid.count; // 保留合并样本权重
                }
                CompressCentroids(targetShard.m_centroids, m_compression);
            }

            void PercentileSketch::Serialize(std::ostream& os) const {
                static constexpr uint64_t magic = 0x5444494745534BULL; // "TDIGESK"
                os.write(reinterpret_cast<const char*>(&magic), sizeof(magic));
                os.write(reinterpret_cast<const char*>(&m_compression), sizeof(m_compression));

                const uint64_t shardCount = static_cast<uint64_t>(m_shards); // 文件中的分片数量
                os.write(reinterpret_cast<const char*>(&shardCount), sizeof(shardCount));

                for (const auto& shard : m_impl->m_shardData) {
                    std::shared_lock lock(shard->m_mutex);
                    const uint64_t count = static_cast<uint64_t>(shard->m_centroids.size());
                    os.write(reinterpret_cast<const char*>(&count), sizeof(count));
                    for (const auto& centroid : shard->m_centroids) {
                        os.write(reinterpret_cast<const char*>(&centroid.mean), sizeof(centroid.mean));
                        os.write(reinterpret_cast<const char*>(&centroid.count), sizeof(centroid.count));
                    }
                }
            }

            PercentileSketch PercentileSketch::Deserialize(std::istream& is) {
                uint64_t magic = 0; // 文件魔数
                is.read(reinterpret_cast<char*>(&magic), sizeof(magic));
                if (magic != 0x5444494745534BULL) {
                    throw std::runtime_error("Invalid sketch format");
                }

                size_t compression = 0;       // 文件中的压缩参数
                uint64_t shardCountRaw = 0;   // 文件中的分片数量
                is.read(reinterpret_cast<char*>(&compression), sizeof(compression));
                is.read(reinterpret_cast<char*>(&shardCountRaw), sizeof(shardCountRaw));
                if (shardCountRaw == 0 || shardCountRaw > 1024) {
                    throw std::runtime_error("Invalid sketch shard count");
                }

                PercentileSketch sketch(compression, static_cast<size_t>(shardCountRaw));
                for (size_t i = 0; i < sketch.m_shards; ++i) {
                    uint64_t count = 0; // 当前分片质心数量
                    is.read(reinterpret_cast<char*>(&count), sizeof(count));
                    if (count > static_cast<uint64_t>(std::numeric_limits<int32_t>::max())) {
                        throw std::runtime_error("Invalid sketch centroid count");
                    }

                    auto& shard = *sketch.m_impl->m_shardData[i];
                    shard.m_centroids.resize(static_cast<size_t>(count));
                    for (size_t j = 0; j < static_cast<size_t>(count); ++j) {
                        is.read(reinterpret_cast<char*>(&shard.m_centroids[j].mean), sizeof(double));
                        is.read(reinterpret_cast<char*>(&shard.m_centroids[j].count), sizeof(int64_t));
                        if (!std::isfinite(shard.m_centroids[j].mean)
                            || shard.m_centroids[j].count <= 0) {
                            throw std::runtime_error("Invalid sketch centroid value");
                        }
                        shard.m_totalCount =
                            shard.m_totalCount > std::numeric_limits<int64_t>::max()
                            - shard.m_centroids[j].count
                            ? std::numeric_limits<int64_t>::max()
                            : shard.m_totalCount + shard.m_centroids[j].count;
                    }
                }

                return sketch;
            }

            std::vector<std::pair<double, int64_t>> PercentileSketch::GetCentroids() const {
                std::vector<std::pair<double, int64_t>> result; // 质心快照
                if (!m_impl) return result;

                for (const auto& shard : m_impl->m_shardData) {
                    std::shared_lock lock(shard->m_mutex);
                    for (const auto& centroid : shard->m_centroids) {
                        result.emplace_back(centroid.mean, centroid.count);
                    }
                }
                return result;
            }

            void PercentileSketch::Reset() {
                if (!m_impl) return;

                for (auto& shard : m_impl->m_shardData) {
                    std::unique_lock lock(shard->m_mutex);
                    shard->m_buffer.clear();
                    shard->m_centroids.clear();
                    shard->m_totalCount = 0;
                }
            }
        }
    }
}
