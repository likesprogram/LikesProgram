#include <LikesProgram/Core/String.hpp>
#include <LikesProgram/Core/Version.hpp>

#ifndef LIKESPROGRAM_DOCTOR_HAS_CONFIG
#define LIKESPROGRAM_DOCTOR_HAS_CONFIG 0
#endif

#ifndef LIKESPROGRAM_DOCTOR_HAS_LOGGING
#define LIKESPROGRAM_DOCTOR_HAS_LOGGING 0
#endif

#if LIKESPROGRAM_DOCTOR_HAS_CONFIG
#include <LikesProgram/Config/Config.hpp>
#endif

#if LIKESPROGRAM_DOCTOR_HAS_LOGGING
#include <LikesProgram/Logging/Logging.hpp>
#include <chrono>
#include <memory>
#include <mutex>
#include <source_location>
#endif

#include <cctype>
#include <cstddef>
#include <exception>
#include <iostream>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace {
    // 命令输出格式，保持文本给人工读、JSON 给 CI 或发布流水线读。
    enum class OutputFormat {
        Text,
        Json
    };

    // 单项诊断状态，Skipped 只用于未链接的可选包。
    enum class CheckState {
        Passed,
        Failed,
        Skipped
    };

    // 稳定退出码，便于发布脚本做明确分支。
    enum class ExitCode {
        Success = 0,
        UsageError = 1,
        CheckFailed = 2,
        UnexpectedError = 3
    };

    // 命令行选项，记录用户要求的格式和必需组件集合。
    struct Options {
        OutputFormat format = OutputFormat::Text;              // 输出格式，默认便于人工读取
        bool showHelp = false;                                 // 是否只打印帮助
        bool showVersion = false;                              // 是否只打印版本
        std::vector<std::string> requiredComponents;           // 用户显式要求必须存在的组件
    };

    // 参数解析结果，失败时保留稳定错误文本。
    struct ParseResult {
        bool ok = true;                                        // 参数是否通过解析
        Options options;                                       // 成功解析后的选项对象
        std::string error;                                     // 参数错误时的人类可读说明
    };

    // 单项检查结果，既服务文本输出，也服务 JSON 输出。
    struct CheckResult {
        std::string component;                                 // 组件短名，例如 core/logging/config
        CheckState state = CheckState::Failed;                 // 当前检查状态
        std::string detail;                                    // 诊断细节或失败原因
        bool required = false;                                 // 失败时是否影响整体退出码
    };

    // 完整诊断报告，聚合所有检查项和最终结论。
    struct Report {
        std::string version;                                   // LikesProgram 统一版本号
        bool ok = false;                                       // 所有必需检查是否通过
        std::vector<CheckResult> checks;                       // 按固定顺序输出的检查项
    };

    // 返回当前统一版本文本。
    std::string VersionText() {
        return std::string(LikesProgram::Version::CurrentString());
    }

    // 判断 value 是否以 prefix 开头，用于解析 --key=value 形式。
    bool StartsWith(std::string_view value, std::string_view prefix) {
        return value.size() >= prefix.size() && value.substr(0, prefix.size()) == prefix;
    }

    // 复制并去除首尾空白，允许 --require=core, logging 这类输入。
    std::string TrimCopy(std::string_view value) {
        std::size_t begin = 0;                                 // 首个非空白字符位置
        while (begin < value.size()
            && std::isspace(static_cast<unsigned char>(value[begin])) != 0) {
            ++begin;
        }

        std::size_t end = value.size();                        // 尾后一位，便于构造子串
        while (end > begin
            && std::isspace(static_cast<unsigned char>(value[end - 1])) != 0) {
            --end;
        }

        return std::string(value.substr(begin, end - begin));
    }

    // ASCII 小写化，命令行组件名只接受 ASCII 稳定标识。
    std::string ToLowerCopy(std::string_view value) {
        std::string result;                                    // 小写后的输出副本
        result.reserve(value.size());

        for (char ch : value) {
            result.push_back(static_cast<char>(
                std::tolower(static_cast<unsigned char>(ch))));
        }

        return result;
    }

    // 在线性小集合中查找组件名，避免为三个组件引入额外容器。
    bool Contains(const std::vector<std::string>& values, std::string_view value) {
        for (const auto& item : values) {
            if (item == value) return true;
        }

        return false;
    }

    // 添加去重后的必需组件，保证输出和失败判断稳定。
    void AddUnique(std::vector<std::string>& values, std::string_view value) {
        if (!Contains(values, value)) values.emplace_back(value);
    }

    // 判断组件名是否属于当前发布诊断工具理解的稳定集合。
    bool IsKnownComponent(std::string_view component) {
        return component == "core" || component == "logging" || component == "config";
    }

    // 解析单个 --require 组件，all/full 会展开为全部当前稳定组件。
    bool AddRequiredComponent(Options& options, std::string_view rawComponent, std::string& error) {
        const std::string component = ToLowerCopy(TrimCopy(rawComponent)); // 归一化后的组件名
        if (component.empty()) {
            error = "empty component in --require";
            return false;
        }

        if (component == "all" || component == "full") {
            AddUnique(options.requiredComponents, "core");
            AddUnique(options.requiredComponents, "logging");
            AddUnique(options.requiredComponents, "config");
            return true;
        }

        if (!IsKnownComponent(component)) {
            error = "unknown component '" + component + "'";
            return false;
        }

        AddUnique(options.requiredComponents, component);
        return true;
    }

    // 拆分逗号分隔的组件列表，返回 view 以避免临时复制。
    std::vector<std::string_view> SplitComponentList(std::string_view value) {
        std::vector<std::string_view> result;                  // 拆分后的组件片段
        std::size_t begin = 0;                                 // 当前片段起点

        while (begin <= value.size()) {
            const std::size_t end = value.find(',', begin);    // 当前片段终点或 npos
            if (end == std::string_view::npos) {
                result.push_back(value.substr(begin));
                break;
            }

            result.push_back(value.substr(begin, end - begin));
            begin = end + 1;
        }

        return result;
    }

    // 解析一个 --require 参数值，支持逗号分隔的多个组件。
    bool AddRequiredComponents(Options& options, std::string_view rawValue, std::string& error) {
        for (std::string_view component : SplitComponentList(rawValue)) {
            if (!AddRequiredComponent(options, component, error)) return false;
        }

        return true;
    }

    // 解析命令行参数，错误路径不抛异常，统一交给 main 输出。
    ParseResult ParseArguments(int argc, char** argv) {
        ParseResult result;                                    // 累积解析状态和选项

        for (int i = 1; i < argc; ++i) {
            const std::string_view arg(argv[i] == nullptr ? "" : argv[i]); // 当前参数视图

            if (arg == "--help" || arg == "-h") {
                result.options.showHelp = true;
            }
            else if (arg == "--version") {
                result.options.showVersion = true;
            }
            else if (arg == "--json") {
                result.options.format = OutputFormat::Json;
            }
            else if (arg == "--text") {
                result.options.format = OutputFormat::Text;
            }
            else if (arg == "--format") {
                if (i + 1 >= argc) {
                    result.ok = false;
                    result.error = "--format requires 'text' or 'json'";
                    return result;
                }

                const std::string format = ToLowerCopy(argv[++i]); // --format 的显式值
                if (format == "text") {
                    result.options.format = OutputFormat::Text;
                }
                else if (format == "json") {
                    result.options.format = OutputFormat::Json;
                }
                else {
                    result.ok = false;
                    result.error = "unknown format '" + format + "'";
                    return result;
                }
            }
            else if (StartsWith(arg, "--format=")) {
                const std::string format = ToLowerCopy(arg.substr(9)); // 等号后的格式名
                if (format == "text") {
                    result.options.format = OutputFormat::Text;
                }
                else if (format == "json") {
                    result.options.format = OutputFormat::Json;
                }
                else {
                    result.ok = false;
                    result.error = "unknown format '" + format + "'";
                    return result;
                }
            }
            else if (arg == "--require") {
                if (i + 1 >= argc) {
                    result.ok = false;
                    result.error = "--require requires a component name";
                    return result;
                }

                if (!AddRequiredComponents(result.options, argv[++i], result.error)) {
                    result.ok = false;
                    return result;
                }
            }
            else if (StartsWith(arg, "--require=")) {
                if (!AddRequiredComponents(result.options, arg.substr(10), result.error)) {
                    result.ok = false;
                    return result;
                }
            }
            else {
                result.ok = false;
                result.error = "unknown argument '" + std::string(arg) + "'";
                return result;
            }
        }

        return result;
    }

    // Core 是必选基础包，其他组件按用户 require 列表决定是否必需。
    bool IsRequired(const Options& options, std::string_view component) {
        return component == "core" || Contains(options.requiredComponents, component);
    }

    // 构造通过结果，集中保持状态字段一致。
    CheckResult Passed(std::string component, std::string detail, bool required) {
        return CheckResult{ std::move(component), CheckState::Passed, std::move(detail), required };
    }

    // 构造失败结果，失败是否影响总结果由 required 决定。
    CheckResult Failed(std::string component, std::string detail, bool required) {
        return CheckResult{ std::move(component), CheckState::Failed, std::move(detail), required };
    }

    // 构造跳过结果，用于未链接的可选扩展包。
    CheckResult Skipped(std::string component, std::string detail, bool required) {
        return CheckResult{ std::move(component), CheckState::Skipped, std::move(detail), required };
    }

    // 检查 Core 的版本、格式化和 JSON 转义基础能力。
    CheckResult RunCoreCheck(bool required) {
        const std::string expected = std::string(LikesProgram::Version::Name)
            + " "
            + VersionText();                                  // 期望格式化输出

        const LikesProgram::String name{ std::string(LikesProgram::Version::Name) }; // Core 名称参数
        const LikesProgram::String version{ VersionText() };  // Core 版本参数
        const std::string formatted = LikesProgram::String::Format(
            u"{} {}", name, version).ToStdString();            // 实际格式化结果

        if (formatted != expected) {
            return Failed("core", "String formatting returned '" + formatted
                + "', expected '" + expected + "'", required);
        }

        const std::string escaped = LikesProgram::String::EscapeJson(
            u"doctor \"ok\"").ToStdString();                  // JSON 内容转义结果
        if (escaped != "doctor \\\"ok\\\"") {
            return Failed("core", "JSON escaping returned '" + escaped + "'", required);
        }

        return Passed("core", "version " + VersionText()
            + "; string and escaping checks passed", required);
    }

    // 检查 Config 包身份和核心解析能力。
    CheckResult RunConfigCheck(bool required) {
#if LIKESPROGRAM_DOCTOR_HAS_CONFIG
        if (!LikesProgram::Config::PackageAvailable()) {
            return Failed("config", "PackageAvailable returned false", required);
        }

        const char* packageVersion = LikesProgram::Config::PackageVersion(); // Config 包版本指针
        if (packageVersion == nullptr) {
            return Failed("config", "PackageVersion returned null", required);
        }

        if (std::string(packageVersion) != VersionText()) {
            return Failed("config", "package version " + std::string(packageVersion)
                + " does not match core version " + VersionText(), required);
        }

        const auto keyValue = LikesProgram::Config::Configuration::FromKeyValueLines(
            u"service.name=doctor\n"
            u"service.port=7001\n"
            u"feature.logging=true\n");                       // key=value 基础解析样本

        if (keyValue.GetString(u"service.name") != u"doctor") {
            return Failed("config", "key-value parser returned wrong service.name", required);
        }
        if (keyValue.GetInt64(u"service.port") != 7001) {
            return Failed("config", "key-value parser returned wrong service.port", required);
        }
        if (!keyValue.GetBool(u"feature.logging")) {
            return Failed("config", "key-value parser returned wrong feature.logging", required);
        }

        const auto json = LikesProgram::Config::Configuration::TryFromJson(
            u"{\"release\":{\"channel\":\"stable\",\"build\":1}}"); // JSON 基础解析样本
        if (!json.IsOk()) {
            return Failed("config", json.GetStatus().ToString().ToStdString(), required);
        }
        if (json.Value().GetString(u"release.channel") != u"stable") {
            return Failed("config", "JSON parser returned wrong release.channel", required);
        }

        return Passed("config", "package version " + std::string(packageVersion)
            + "; key-value and JSON checks passed", required);
#else
        const std::string detail =
            "component is not linked into this tool; enable LIKESPROGRAM_BUILD_CONFIG=ON"; // 未链接说明
        return required ? Failed("config", detail, required) : Skipped("config", detail, required);
#endif
    }

#if LIKESPROGRAM_DOCTOR_HAS_LOGGING
    // 诊断用 Sink，只计数不输出，避免工具污染控制台。
    class CountingSink final : public LikesProgram::Log::Sink {
    public:
        // 创建带固定名称的诊断 Sink。
        CountingSink()
            : Sink(u"doctor-counting-sink") {
        }

        // 记录收到的日志消息数量。
        void Write(const LikesProgram::Log::Message&) override {
            std::lock_guard<std::mutex> lock(m_mutex);         // 保护跨线程写入计数
            ++m_count;
        }

        // 返回当前累计消息数。
        std::size_t Count() const {
            std::lock_guard<std::mutex> lock(m_mutex);         // 与后台日志线程同步
            return m_count;
        }

    private:
        mutable std::mutex m_mutex;                            // 保护 m_count 的互斥锁
        std::size_t m_count = 0;                               // 已接收日志消息数
    };
#endif

    // 检查 Logging 包身份、启动、Flush、诊断和 Sink 分发路径。
    CheckResult RunLoggingCheck(bool required) {
#if LIKESPROGRAM_DOCTOR_HAS_LOGGING
        if (!LikesProgram::Logging::PackageAvailable()) {
            return Failed("logging", "PackageAvailable returned false", required);
        }

        const char* packageVersion = LikesProgram::Logging::PackageVersion(); // Logging 包版本指针
        if (packageVersion == nullptr) {
            return Failed("logging", "PackageVersion returned null", required);
        }

        if (std::string(packageVersion) != VersionText()) {
            return Failed("logging", "package version " + std::string(packageVersion)
                + " does not match core version " + VersionText(), required);
        }

        auto& logger = LikesProgram::Log::Logger::Instance(false, false); // 全局 Logger 诊断对象
        bool started = false;                                // 是否已经成功启动后台线程
        auto cleanup = [&logger, &started]() {
            if (started) {
                (void)logger.Shutdown(std::chrono::seconds(5), true);
            }
            else {
                logger.Shutdown(true);
            }
        };

        logger.Shutdown(true);
        logger.SetLevel(LikesProgram::Log::Level::Trace);
        logger.SetEncoding(LikesProgram::String::Encoding::UTF8);
        logger.SetLoggerName(u"likesprogram-doctor");

        LikesProgram::Log::LoggerOptions options;             // 诊断专用轻量队列配置
        options.maxQueueSize = 128;
        options.enqueueTimeout = std::chrono::milliseconds(250);
        options.outputFormat = LikesProgram::Log::LogOutputFormat::Text;
        logger.Configure(options);

        const auto sink = std::make_shared<CountingSink>();   // 接收一条诊断日志的 Sink
        logger.AddSink(sink);

        if (!logger.Start()) {
            cleanup();
            return Failed("logging", "logger did not start", required);
        }
        started = true;

        logger.Log(LikesProgram::Log::Level::Info, std::source_location::current(),
            u"doctor logging check");

        if (!logger.Flush(std::chrono::seconds(5))) {
            cleanup();
            return Failed("logging", "logger flush timed out", required);
        }

        const auto diagnostics = logger.Diagnostics();        // Shutdown 前的运行态快照
        cleanup();

        if (sink->Count() != 1) {
            return Failed("logging", "counting sink received "
                + std::to_string(sink->Count()) + " messages, expected 1", required);
        }
        if (!diagnostics.stats.running) {
            return Failed("logging", "diagnostics did not report running logger before shutdown",
                required);
        }
        if (diagnostics.stats.sinkCount != 1) {
            return Failed("logging", "diagnostics reported "
                + std::to_string(diagnostics.stats.sinkCount) + " sinks, expected 1", required);
        }
        if (diagnostics.stats.acceptedMessages < 1 || diagnostics.stats.processedMessages < 1) {
            return Failed("logging", "logger statistics did not include the emitted message",
                required);
        }

        return Passed("logging", "package version " + std::string(packageVersion)
            + "; async sink round trip passed", required);
#else
        const std::string detail =
            "component is not linked into this tool; enable LIKESPROGRAM_BUILD_LOGGING=ON"; // 未链接说明
        return required ? Failed("logging", detail, required)
            : Skipped("logging", detail, required);
#endif
    }

    // 执行全部诊断项，固定顺序便于发布脚本比较输出。
    Report RunDiagnostics(const Options& options) {
        Report report;                                        // 本次诊断报告
        report.version = VersionText();
        report.checks.push_back(RunCoreCheck(IsRequired(options, "core")));
        report.checks.push_back(RunLoggingCheck(IsRequired(options, "logging")));
        report.checks.push_back(RunConfigCheck(IsRequired(options, "config")));

        report.ok = true;
        for (const auto& check : report.checks) {
            if (check.state == CheckState::Failed) {
                report.ok = false;
                break;
            }
        }

        return report;
    }

    // 返回检查状态的稳定英文标识，JSON 和文本共用。
    const char* StateName(CheckState state) {
        switch (state) {
        case CheckState::Passed: return "passed";
        case CheckState::Failed: return "failed";
        case CheckState::Skipped: return "skipped";
        }

        return "unknown";
    }

    // 转义普通 UTF-8 文本为 JSON 字符串内容，不包含外层引号。
    std::string JsonEscape(std::string_view value) {
        std::string result;                                    // JSON 转义输出缓存
        result.reserve(value.size() + 8);

        static constexpr char hex[] = "0123456789abcdef";      // 控制字符十六进制表
        for (unsigned char ch : value) {
            switch (ch) {
            case '\\': result += "\\\\"; break;
            case '"': result += "\\\""; break;
            case '\b': result += "\\b"; break;
            case '\f': result += "\\f"; break;
            case '\n': result += "\\n"; break;
            case '\r': result += "\\r"; break;
            case '\t': result += "\\t"; break;
            default:
                if (ch < 0x20) {
                    result += "\\u00";
                    result += hex[(ch >> 4) & 0x0F];
                    result += hex[ch & 0x0F];
                }
                else {
                    result.push_back(static_cast<char>(ch));
                }
                break;
            }
        }

        return result;
    }

    // 输出面向人工阅读的诊断结果。
    void PrintText(const Report& report, std::ostream& out) {
        out << "LikesProgram Doctor " << report.version << '\n';
        out << "Result: " << (report.ok ? "passed" : "failed") << "\n\n";

        for (const auto& check : report.checks) {
            out << check.component << ": " << StateName(check.state)
                << (check.required ? " required" : " optional") << '\n';
            out << "  " << check.detail << '\n';
        }
    }

    // 输出面向 CI 和发布流水线读取的稳定 JSON。
    void PrintJson(const Report& report, std::ostream& out) {
        out << "{\n";
        out << "  \"tool\": \"likesprogram-doctor\",\n";
        out << "  \"likesprogram_version\": \"" << JsonEscape(report.version) << "\",\n";
        out << "  \"ok\": " << (report.ok ? "true" : "false") << ",\n";
        out << "  \"checks\": [\n";

        for (std::size_t i = 0; i < report.checks.size(); ++i) {
            const auto& check = report.checks[i];              // 当前输出的检查项
            out << "    {\n";
            out << "      \"component\": \"" << JsonEscape(check.component) << "\",\n";
            out << "      \"state\": \"" << StateName(check.state) << "\",\n";
            out << "      \"required\": " << (check.required ? "true" : "false") << ",\n";
            out << "      \"detail\": \"" << JsonEscape(check.detail) << "\"\n";
            out << "    }" << (i + 1 == report.checks.size() ? "" : ",") << '\n';
        }

        out << "  ]\n";
        out << "}\n";
    }

    // 输出命令行帮助，保持参数和退出码说明集中维护。
    void PrintUsage(std::ostream& out) {
        out << "Usage:\n";
        out << "  likesprogram-doctor [options]\n\n";
        out << "Options:\n";
        out << "  --help, -h                 Show this help text.\n";
        out << "  --version                  Show tool and package version.\n";
        out << "  --format text|json         Select output format.\n";
        out << "  --text                     Shortcut for --format text.\n";
        out << "  --json                     Shortcut for --format json.\n";
        out << "  --require NAME             Require core, logging, config, or all.\n";
        out << "  --require=NAME[,NAME...]   Require one or more components.\n\n";
        out << "Exit codes:\n";
        out << "  0  diagnostics passed\n";
        out << "  1  invalid command line\n";
        out << "  2  diagnostics failed\n";
        out << "  3  unexpected runtime error\n";
    }
}

// 工具主入口：解析参数、执行诊断并转换为稳定退出码。
int main(int argc, char** argv) {
    try {
        ParseResult parsed = ParseArguments(argc, argv);       // 命令行解析结果
        if (!parsed.ok) {
            std::cerr << "likesprogram-doctor: " << parsed.error << "\n\n";
            PrintUsage(std::cerr);
            return static_cast<int>(ExitCode::UsageError);
        }

        if (parsed.options.showHelp) {
            PrintUsage(std::cout);
            return static_cast<int>(ExitCode::Success);
        }

        if (parsed.options.showVersion) {
            std::cout << "likesprogram-doctor " << VersionText()
                << " (LikesProgram " << VersionText() << ")\n";
            return static_cast<int>(ExitCode::Success);
        }

        const Report report = RunDiagnostics(parsed.options);  // 本次完整诊断报告
        if (parsed.options.format == OutputFormat::Json) {
            PrintJson(report, std::cout);
        }
        else {
            PrintText(report, std::cout);
        }

        return static_cast<int>(report.ok ? ExitCode::Success : ExitCode::CheckFailed);
    }
    catch (const std::exception& ex) {
        std::cerr << "likesprogram-doctor: unexpected error: " << ex.what() << '\n';
        return static_cast<int>(ExitCode::UnexpectedError);
    }
    catch (...) {
        std::cerr << "likesprogram-doctor: unexpected non-standard error\n";
        return static_cast<int>(ExitCode::UnexpectedError);
    }
}
