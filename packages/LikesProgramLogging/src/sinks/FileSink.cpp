#include <LikesProgram/Logging/sinks/FileSink.hpp>
#include <LikesProgram/Core/time/Time.hpp>
#include <algorithm>
#include <chrono>
#include <cctype>
#include <filesystem>
#include <fstream>
#include <iomanip>
#include <memory>
#include <mutex>
#include <sstream>
#include <stdexcept>
#include <string>
#include <thread>
#include <vector>

#ifdef _WIN32
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#else
#include <cerrno>
#include <fcntl.h>
#include <sys/file.h>
#include <unistd.h>
#endif

namespace LikesProgram {
    namespace Log {
        namespace {
            // 使用 FNV-1a 将文件路径折叠为稳定短后缀，避免系统锁名过长。
            std::string HexHash(const std::string& value) {
                uint64_t hash = 14695981039346656037ull; // FNV-1a 固定种子，保证跨进程锁名可预测
                for (unsigned char ch : value) {
                    hash ^= static_cast<uint64_t>(ch);
                    hash *= 1099511628211ull;
                }

                std::ostringstream out; // 稳定生成锁名后缀，避免路径字符进入系统锁名
                out << std::hex << std::setw(16) << std::setfill('0') << hash;
                return out.str();
            }

            // 将用户提供的锁名规整为系统互斥体/lock file 可接受的安全字符。
            std::string SanitizeLockName(const std::string& value) {
                std::string result; // 规整后的锁名主体
                result.reserve(value.size());
                for (unsigned char ch : value) {
                    result.push_back(std::isalnum(ch) ? static_cast<char>(ch) : '_');
                }
                return result.empty() ? std::string("default") : result;
            }

            // 未显式配置锁名时，根据日志文件路径生成跨进程一致的默认锁名。
            std::string BuildDefaultLockName(const std::string& source) {
                return std::string("LikesProgramLoggingFile_") + HexHash(source);
            }

            // 将锁等待时间转换为毫秒，非正值表示立即超时。
            uint64_t LockTimeoutMilliseconds(std::chrono::milliseconds timeout) {
                if (timeout.count() <= 0) return 0;
                return static_cast<uint64_t>(timeout.count());
            }

            // RAII 跨进程文件锁，保护 FileSink 的轮转、写入和保留策略临界区。
            class ProcessFileLockGuard {
            public:
                // 配置未启用时构造为空操作对象，启用时立即尝试获取跨进程锁。
                ProcessFileLockGuard(const MultiProcessFileConfig& config, const std::string& source)
                    : m_enabled(config.enabled) {
                    if (!m_enabled) return;
                    Lock(config, source);
                }

                // 析构时释放已持有的系统锁，保证异常路径不会遗留锁。
                ~ProcessFileLockGuard() {
                    Unlock();
                }

                ProcessFileLockGuard(const ProcessFileLockGuard&) = delete;
                ProcessFileLockGuard& operator=(const ProcessFileLockGuard&) = delete;

            private:
                // 根据平台使用命名互斥体或 lock file 获取跨进程独占锁。
                void Lock(const MultiProcessFileConfig& config, const std::string& source) {
                    const std::string rawName = config.lockName.Empty() // 用户锁名或基于路径生成的默认锁名
                        ? BuildDefaultLockName(source)
                        : config.lockName.ToStdString();

#ifdef _WIN32
                    std::string safeName = rawName; // Windows 命名互斥体名称，必要时补 Local 前缀
                    if (safeName.find('\\') == std::string::npos) {
                        safeName = std::string("Local\\") + SanitizeLockName(safeName);
                    }

                    std::wstring mutexName = String(safeName).ToWString(); // CreateMutexW 使用的宽字符锁名
                    m_handle = CreateMutexW(nullptr, FALSE, mutexName.c_str());
                    if (!m_handle) throw std::runtime_error("Failed to create log file process mutex");

                    DWORD waitMs = config.lockTimeout == std::chrono::milliseconds::max() // Windows API 等待毫秒数
                        ? INFINITE
                        : static_cast<DWORD>((std::min<uint64_t>)(LockTimeoutMilliseconds(config.lockTimeout),
                            static_cast<uint64_t>(INFINITE - 1)));
                    DWORD result = WaitForSingleObject(m_handle, waitMs); // 获取互斥体后的等待结果
                    if (result != WAIT_OBJECT_0 && result != WAIT_ABANDONED) {
                        CloseHandle(m_handle);
                        m_handle = nullptr;
                        throw std::runtime_error("Timed out waiting for log file process mutex");
                    }
                    m_locked = true;
#else
                    std::filesystem::path lockPath = std::filesystem::temp_directory_path() / // POSIX lock file 路径
                        (SanitizeLockName(rawName) + ".lock");
                    m_fd = open(lockPath.string().c_str(), O_CREAT | O_RDWR, 0666);
                    if (m_fd < 0) throw std::runtime_error("Failed to open log file process lock");

                    const auto begin = std::chrono::steady_clock::now(); // 超时计算起点
                    const uint64_t timeoutMs = LockTimeoutMilliseconds(config.lockTimeout); // 本次等待上限
                    while (flock(m_fd, LOCK_EX | LOCK_NB) != 0) {
                        if (errno != EWOULDBLOCK && errno != EAGAIN) {
                            close(m_fd);
                            m_fd = -1;
                            throw std::runtime_error("Failed to lock log file process lock");
                        }

                        if (config.lockTimeout != std::chrono::milliseconds::max()) {
                            const auto elapsed = std::chrono::duration_cast<std::chrono::milliseconds>(
                                std::chrono::steady_clock::now() - begin).count();
                            if (timeoutMs == 0 || static_cast<uint64_t>(elapsed) >= timeoutMs) {
                                close(m_fd);
                                m_fd = -1;
                                throw std::runtime_error("Timed out waiting for log file process lock");
                            }
                        }

                        std::this_thread::sleep_for(std::chrono::milliseconds(1));
                    }
                    m_locked = true;
#endif
                }

                // 释放当前平台对应的系统锁，供析构和失败清理路径复用。
                void Unlock() noexcept {
                    if (!m_enabled || !m_locked) return;

#ifdef _WIN32
                    if (m_handle) {
                        ReleaseMutex(m_handle);
                        CloseHandle(m_handle);
                        m_handle = nullptr;
                    }
#else
                    if (m_fd >= 0) {
                        flock(m_fd, LOCK_UN);
                        close(m_fd);
                        m_fd = -1;
                    }
#endif
                    m_locked = false;
                }

                bool m_enabled = false; // false 时该 guard 为无操作对象
                bool m_locked = false;  // 是否已经拿到跨进程锁
#ifdef _WIN32
                HANDLE m_handle = nullptr; // Windows 命名互斥体句柄
#else
                int m_fd = -1;             // POSIX lock file 描述符
#endif
            };
        }

        class FileSink::FileSinkImpl {
        public:
            // 兼容旧构造参数：只指定最大 MB 时转为 FileSinkOptions。
            explicit FileSinkImpl(const LikesProgram::String& path,
                const LikesProgram::String& filename, size_t maxFileSizeMB = 30)
                : FileSinkImpl(path, filename, FileSinkOptions{ maxFileSizeMB }) {
            }

            // 构造单进程 FileSink，实现层仍复用完整配置构造函数。
            explicit FileSinkImpl(const LikesProgram::String& path,
                const LikesProgram::String& filename, const FileSinkOptions& options)
                : FileSinkImpl(path, filename, options, MultiProcessFileConfig()) {
            }

            // 构造文件 Sink 并立即打开当前日志文件，失败直接抛出给调用方。
            explicit FileSinkImpl(const LikesProgram::String& path,
                const LikesProgram::String& filename, const FileSinkOptions& options,
                const MultiProcessFileConfig& multiProcess)
                : m_path(path), m_filename(filename), m_options(NormalizeOptions(options)),
                m_multiProcess(multiProcess) {
                if (m_path.Empty()) m_path = u"./logs";
                if (m_filename.Empty()) m_filename = u"Logger.log";
                ProcessFileLockGuard processLock(m_multiProcess, BuildLockSource()); // 打开/轮转前的跨进程互斥保护
                OpenNewFileLocked(std::chrono::system_clock::now());
            }

            // 析构时关闭文件流，Flush 失败只能由显式 Flush 路径暴露。
            ~FileSinkImpl() {
                if (m_file.is_open()) m_file.close(); // 明确释放 Windows 文件句柄，避免依赖成员析构顺序
            }

            // 写入一条已格式化日志；必要时根据大小或日期触发轮转。
            void Write(const String& formatted, String::Encoding encoding,
                std::chrono::system_clock::time_point timestamp) {
                std::lock_guard<std::mutex> lock(m_mutex);
                ProcessFileLockGuard processLock(m_multiProcess, BuildLockSource()); // 覆盖跨进程尺寸检查、轮转和写入
                if (m_multiProcess.enabled && !m_currentFilePath.empty()) {
                    std::error_code ec; // 读取 peer 进程写入后的实际文件大小错误码
                    auto size = std::filesystem::file_size(m_currentFilePath, ec); // 当前文件真实字节数
                    if (!ec) m_currentSize = static_cast<size_t>(size);
                }
                if (NeedRotate(timestamp)) OpenNewFileLocked(timestamp);
                if (!m_file.is_open()) throw std::runtime_error("Log file stream is not open");

                std::string encoded = formatted.ToStdString(encoding); // 目标编码下的单条日志字节串
                m_file << encoded << '\n';
                if (!m_file) throw std::runtime_error("Failed to write log file");

                m_currentSize += encoded.size() + 1;
            }

            // 刷新当前文件流；跨进程模式下刷新后重开文件以观察 peer 写入。
            void Flush() {
                std::lock_guard<std::mutex> lock(m_mutex);
                if (!m_file.is_open()) return;

                ProcessFileLockGuard processLock(m_multiProcess, BuildLockSource()); // 保护 flush 与可选重开窗口
                if (m_multiProcess.enabled && !m_currentFilePath.empty()) {
                    std::error_code ec; // 读取当前文件大小的错误码
                    auto size = std::filesystem::file_size(m_currentFilePath, ec); // flush 前同步后的文件大小
                    if (!ec) m_currentSize = static_cast<size_t>(size);
                }
                m_file.flush();
                if (!m_file) throw std::runtime_error("Failed to flush log file");

                if (m_multiProcess.enabled && !m_currentFilePath.empty()) {
                    ReopenCurrentFileLocked();
                }
            }

            // 热更新文件轮转和保留策略，并立即执行一次保留清理。
            void Configure(const FileSinkOptions& options) {
                std::lock_guard<std::mutex> lock(m_mutex);
                ProcessFileLockGuard processLock(m_multiProcess, BuildLockSource()); // 防止配置期间 peer 同时轮转/清理
                m_options = NormalizeOptions(options);
                EnforceRetention();
            }

            // 返回当前归一化后的文件策略快照。
            FileSinkOptions Options() const {
                std::lock_guard<std::mutex> lock(m_mutex);
                return m_options;
            }

        private:
            struct ManagedFile {
                std::filesystem::path path;                         // 被当前 FileSink 命名规则托管的日志文件
                std::filesystem::file_time_type writeTime;           // 最近修改时间，用于保留天数和淘汰排序
                uintmax_t size = 0;                                  // 当前文件大小，参与总大小约束
            };

            // 将旧 MB 字段转换为字节字段，保持调用方传入对象不变。
            static FileSinkOptions NormalizeOptions(const FileSinkOptions& options) {
                FileSinkOptions normalized = options; // 复制后归一化，保持调用方对象不变
                if (normalized.maxFileSizeBytes == 0 && normalized.maxFileSizeMB > 0) {
                    normalized.maxFileSizeBytes = normalized.maxFileSizeMB * 1024 * 1024;
                }
                return normalized;
            }

            // 构造参与默认锁名哈希的绝对文件标识。
            std::string BuildLockSource() const {
                std::filesystem::path source = std::filesystem::absolute( // 日志文件基准路径
                    std::filesystem::path(m_path.ToStdString()) / m_filename.ToStdString());
                return source.string();
            }

            // 打开当天可写日志文件；若现有文件达到大小上限则推进轮转索引。
            void OpenNewFileLocked(std::chrono::system_clock::time_point timestamp) {
                if (m_file.is_open()) m_file.close();

                m_currentSize = 0;
                m_lastRotateTime = timestamp;
                LikesProgram::String timeDir = LikesProgram::Time::FormatTime(m_lastRotateTime, u"%Y-%m-%d"); // 日期目录名

                std::filesystem::path dir = std::filesystem::path(m_path.ToStdString()) / timeDir.ToStdString(); // 当天日志目录
                std::filesystem::create_directories(dir);

                std::string filePath; // 本轮尝试打开的日志文件路径
                while (true) {
                    filePath = BuildLogFileName(dir, m_filename.ToStdString(), m_fileIndex);

                    if (!std::filesystem::exists(filePath)) break;

                    auto size = std::filesystem::file_size(filePath); // 已存在文件大小，用于判断是否续写
                    if (m_options.maxFileSizeBytes == 0 || size < m_options.maxFileSizeBytes) {
                        m_currentSize = static_cast<size_t>(size);
                        break;
                    }

                    ++m_fileIndex;
                }

                m_file.open(filePath, std::ios::out | std::ios::app);
                if (!m_file) throw std::runtime_error("Failed to open log file: " + filePath);

                m_currentFilePath = std::filesystem::path(filePath);
                EnforceRetention();
            }

            // 跨进程模式下重新打开当前文件，避免长时间持有陈旧文件状态。
            void ReopenCurrentFileLocked() {
                if (m_file.is_open()) m_file.close();

                m_file.open(m_currentFilePath, std::ios::out | std::ios::app);
                if (!m_file) {
                    throw std::runtime_error("Failed to reopen log file after flush: " +
                        m_currentFilePath.string());
                }

                std::error_code ec; // 文件大小读取错误码
                auto size = std::filesystem::file_size(m_currentFilePath, ec); // 重开后真实文件大小
                if (!ec) m_currentSize = static_cast<size_t>(size);
            }

            // 判断是否需要按大小或日期轮转，必要时更新下一文件索引。
            bool NeedRotate(std::chrono::system_clock::time_point time) {
                if (!m_file.is_open()) return false;

                if (m_options.maxFileSizeBytes > 0 && m_currentSize >= m_options.maxFileSizeBytes) {
                    ++m_fileIndex;
                    return true;
                }

                std::time_t nowTime = std::chrono::system_clock::to_time_t(time); // 当前消息时间
                std::time_t lastTime = std::chrono::system_clock::to_time_t(m_lastRotateTime); // 上次轮转时间
                std::tm nowTm = LikesProgram::Time::ToLocalTime(nowTime); // 当前本地日期
                std::tm lastTm = LikesProgram::Time::ToLocalTime(lastTime); // 上次轮转本地日期

                if (nowTm.tm_year != lastTm.tm_year || nowTm.tm_mon != lastTm.tm_mon || nowTm.tm_mday != lastTm.tm_mday) {
                    m_fileIndex = 0;
                    return true;
                }

                return false;
            }

            // 根据目录、基础文件名和轮转索引生成最终文件名。
            std::string BuildLogFileName(const std::filesystem::path& dir,
                const std::string& base, size_t index) {
                if (index == 0) return (dir / base).string();
                return (dir / (std::to_string(index) + "_" + base)).string();
            }

            // 判断文件名是否由当前 FileSink 的基础文件名和数字轮转前缀生成。
            bool IsManagedFileName(const std::filesystem::path& path) const {
                std::string name = path.filename().string(); // 待检查文件名
                std::string base = m_filename.ToStdString(); // 当前 Sink 的基础文件名
                if (name == base) return true;
                if (name.size() <= base.size() + 1) return false;
                if (name.substr(name.size() - base.size()) != base) return false;
                if (name[name.size() - base.size() - 1] != '_') return false;

                const size_t prefixSize = name.size() - base.size() - 1; // 轮转索引前缀长度
                return std::all_of(name.begin(), name.begin() + static_cast<std::ptrdiff_t>(prefixSize),
                    [](unsigned char ch) { return ch >= '0' && ch <= '9'; });
            }

            // 递归收集当前日志根目录下由本 Sink 管理的文件。
            std::vector<ManagedFile> CollectManagedFiles() const {
                std::vector<ManagedFile> files; // 只收集当前 FileSink 命名规则下的普通文件
                std::filesystem::path root = std::filesystem::path(m_path.ToStdString()); // 日志根目录
                if (!std::filesystem::exists(root)) return files;

                for (const auto& entry : std::filesystem::recursive_directory_iterator(root)) {
                    if (!entry.is_regular_file()) continue;
                    if (!IsManagedFileName(entry.path())) continue;

                    ManagedFile file; // 当前候选日志文件元数据
                    file.path = entry.path();
                    file.writeTime = entry.last_write_time();
                    file.size = entry.file_size();
                    files.push_back(std::move(file));
                }

                return files;
            }

            // 判断目标路径是否等于当前打开文件，清理策略不得删除该文件。
            bool IsCurrentFile(const std::filesystem::path& path) const {
                if (m_currentFilePath.empty()) return false;
                std::error_code ec; // equivalent 失败时的非抛出错误码
                return std::filesystem::equivalent(path, m_currentFilePath, ec) && !ec;
            }

            // 删除非当前文件，错误静默吞掉，避免清理失败影响写日志主路径。
            void RemoveFileIfNotCurrent(const std::filesystem::path& path) {
                if (IsCurrentFile(path)) return;
                std::error_code ec; // remove 的非抛出错误码
                std::filesystem::remove(path, ec);
            }

            // 按天数、文件数和总大小依次执行保留策略，始终保留当前文件。
            void EnforceRetention() {
                if (m_options.retentionDays == 0 &&
                    m_options.maxRetainedFiles == 0 &&
                    m_options.maxTotalSizeBytes == 0) {
                    return;
                }

                auto files = CollectManagedFiles(); // 当前策略可管理的文件快照
                const auto now = std::filesystem::file_time_type::clock::now(); // 文件时间钟的当前时间
                if (m_options.retentionDays > 0) {
                    const auto maxAge = std::chrono::hours(24 * static_cast<int64_t>(m_options.retentionDays)); // 最大保留时长
                    for (const auto& file : files) {
                        if (now - file.writeTime > maxAge) RemoveFileIfNotCurrent(file.path);
                    }
                    files = CollectManagedFiles();
                }

                std::sort(files.begin(), files.end(), [](const ManagedFile& lhs, const ManagedFile& rhs) {
                    return lhs.writeTime < rhs.writeTime;
                });

                if (m_options.maxRetainedFiles > 0) {
                    while (files.size() > m_options.maxRetainedFiles) {
                        RemoveFileIfNotCurrent(files.front().path);
                        files.erase(files.begin());
                    }
                    files = CollectManagedFiles();
                    std::sort(files.begin(), files.end(), [](const ManagedFile& lhs, const ManagedFile& rhs) {
                        return lhs.writeTime < rhs.writeTime;
                    });
                }

                if (m_options.maxTotalSizeBytes > 0) {
                    uintmax_t totalSize = 0; // 当前托管文件总字节数
                    for (const auto& file : files) totalSize += file.size;

                    while (totalSize > m_options.maxTotalSizeBytes && !files.empty()) {
                        const auto removedSize = files.front().size; // 本轮淘汰文件大小
                        RemoveFileIfNotCurrent(files.front().path);
                        files.erase(files.begin());
                        totalSize = totalSize > removedSize ? totalSize - removedSize : 0;
                    }
                }
            }

        private:
            std::ofstream m_file;                                      // 当前持有的日志文件流
            LikesProgram::String m_path;                               // 日志根目录
            size_t m_fileIndex = 0;                                    // 同一天内的轮转文件索引
            LikesProgram::String m_filename;                           // 基础文件名
            size_t m_currentSize = 0;                                  // 当前文件已写入字节数近似值
            FileSinkOptions m_options;                                  // 当前轮转和保留策略
            MultiProcessFileConfig m_multiProcess;                       // 可选跨进程文件锁配置
            std::filesystem::path m_currentFilePath;                    // 当前打开文件路径，清理时避免删除
            std::chrono::system_clock::time_point m_lastRotateTime;    // 上次打开文件的时间
            mutable std::mutex m_mutex;                                // 保护文件流、策略和当前文件路径
        };

        FileSink::FileSink(const LikesProgram::String& path,
            const LikesProgram::String& filename, size_t maxFileSizeMB)
            : Sink(u"FileSink"), m_impl(new FileSinkImpl(path, filename, maxFileSizeMB)) {
        }

        FileSink::FileSink(const LikesProgram::String& path,
            const LikesProgram::String& filename, const FileSinkOptions& options)
            : Sink(u"FileSink"), m_impl(new FileSinkImpl(path, filename, options)) {
        }

        FileSink::FileSink(const LikesProgram::String& path,
            const LikesProgram::String& filename, const FileSinkOptions& options,
            const MultiProcessFileConfig& multiProcess)
            : Sink(u"FileSink"), m_impl(new FileSinkImpl(path, filename, options, multiProcess)) {
        }

        FileSink::~FileSink() {
            if (m_impl) {
                try {
                    m_impl->Flush();
                }
                catch (...) {
                    // 析构不抛出，Flush 显式调用路径负责暴露失败。
                }
                delete m_impl;
            }
            m_impl = nullptr;
        }

        void FileSink::Write(const Message& message) {
            if (!m_impl) return;

            LikesProgram::String formatted = FormatLogMessage(message);
            m_impl->Write(formatted, message.encoding, message.timestamp);
        }

        void FileSink::Flush() {
            if (!m_impl) return;

            m_impl->Flush();
        }

        void FileSink::Configure(const FileSinkOptions& options) {
            if (!m_impl) return;

            m_impl->Configure(options);
        }

        FileSinkOptions FileSink::Options() const {
            if (!m_impl) return FileSinkOptions();

            return m_impl->Options();
        }

        std::shared_ptr<Sink> FileSink::CreateSink(const LikesProgram::String& path,
            const LikesProgram::String& filename, size_t maxFileSizeMB) {
            return std::make_shared<FileSink>(path, filename, maxFileSizeMB);
        }

        std::shared_ptr<Sink> FileSink::CreateSink(const LikesProgram::String& path,
            const LikesProgram::String& filename, const FileSinkOptions& options) {
            return std::make_shared<FileSink>(path, filename, options);
        }

        std::shared_ptr<Sink> FileSink::CreateSink(const LikesProgram::String& path,
            const LikesProgram::String& filename, const FileSinkOptions& options,
            const MultiProcessFileConfig& multiProcess) {
            return std::make_shared<FileSink>(path, filename, options, multiProcess);
        }
    }
}
