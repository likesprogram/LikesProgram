#include <stringFormat/FormatInternal.hpp>
#include <LikesProgram/Core/time/Time.hpp>

#include <algorithm>
#include <atomic>
#include <cstdint>
#include <iomanip>
#include <limits>
#include <memory>
#include <mutex>
#include <shared_mutex>
#include <sstream>
#include <string_view>
#include <unordered_map>

namespace LikesProgram {
    namespace StringFormat {
        namespace {
            constexpr size_t kParseCacheMaxEntries = 256; // 解析缓存达到上限后整表清理

            // 判断类型字符是否属于 v1.0 规范内建类型。
            bool IsKnownFormatType(char32_t type) {
                static const char32_t types[] = { // 当前格式器支持的内建类型字符表
                    U's', U'S', U'd', U'i', U'o', U'O', U'u', U'x', U'X',
                    U'b', U'B', U'f', U'F', U'e', U'E', U'g', U'G',
                    U'c', U'p', U'P', U't', U'T', U'%'
                };
                for (auto t : types) {
                    if (type == t) return true;
                }
                return false;
            }

            // 判断类型字符是否可以走整数格式化路径。
            bool IsIntegerType(char32_t type) {
                return type == U's' || type == U'd' || type == U'i' ||
                    type == U'x' || type == U'X' || type == U'b' ||
                    type == U'B' || type == U'o' || type == U'O' ||
                    type == U'c';
            }

            // 判断类型字符是否可以走浮点格式化路径。
            bool IsFloatingType(char32_t type) {
                return type == U's' || type == U'f' || type == U'F' ||
                    type == U'e' || type == U'E' || type == U'g' ||
                    type == U'G' || type == U'%';
            }

            // 将 FormatSpec 拍平成值类型，减少热路径虚调用和 PImpl 间接访问。
            struct FormatSpecSnapshot {
                int index = -1;                    // 参数索引，-1 表示自动索引
                bool explicitIndex = false;        // 是否显式写了索引
                char32_t type = U's';              // 格式类型字符
                char32_t sign = 0;                 // 符号控制字符
                char32_t align = U'>';             // 对齐方式
                bool alternate = false;            // 是否输出进制前缀
                bool zeroPad = false;              // 是否启用 0 填充
                std::optional<int> width;          // 最小输出宽度
                std::optional<int> precision;      // 精度或字符串截断长度
                std::wstring fill = L" ";          // 填充字符串
                String typeExpand;                 // 类型扩展内容

                FormatSpecSnapshot() = default;
                explicit FormatSpecSnapshot(const FormatSpec& spec)
                    : index(spec.GetIndex()),
                    explicitIndex(spec.HasExplicitIndex()),
                    type(spec.GetType()),
                    sign(spec.GetSign()),
                    align(spec.GetAlign()),
                    alternate(spec.GetAlternateForm()),
                    zeroPad(spec.GetZeroPad()),
                    width(spec.GetWidth()),
                    precision(spec.GetPrecision()),
                    fill(spec.GetFill().ToWString()),
                    typeExpand(spec.GetTypeExpand()) {
                    if (fill.empty()) fill = L" ";
                }
            };

            // 将无符号整数转换为指定进制文本。
            std::wstring UnsignedToBase(unsigned long long value, unsigned base, bool upper) {
                const wchar_t* digits = upper ? L"0123456789ABCDEF" : L"0123456789abcdef"; // 当前进制字符表
                std::wstring out; // 反向写入后再翻转的数字文本
                do {
                    out.push_back(digits[value % base]);
                    value /= base;
                } while (value != 0);
                // 临时缓冲按低位到高位写入，返回前翻转为正常顺序。
                std::reverse(out.begin(), out.end());
                return out;
            }

            // 根据格式类型和 # 标志生成整数前缀。
            std::wstring IntegerPrefix(char32_t type, bool alternate) {
                if (!alternate) return std::wstring();
                // # 标志只对二/八/十六进制产生前缀。
                switch (type) {
                case U'x': return L"0x";
                case U'X': return L"0X";
                case U'b': return L"0b";
                case U'B': return L"0B";
                case U'o': return L"0o";
                case U'O': return L"0O";
                default: return std::wstring();
                }
            }

            // 根据正负号和 sign 规格生成符号前缀。
            std::wstring SignPrefix(bool negative, const FormatSpec& spec) {
                if (negative) return L"-";
                // 非负数只在显式 + 或空格符号策略下输出前缀。
                if (spec.GetSign() == U'+') return L"+";
                if (spec.GetSign() == U' ') return L" ";
                return std::wstring();
            }

            // 安全取得有符号整数绝对值，避免 LLONG_MIN 溢出。
            unsigned long long SignedMagnitude(long long value) {
                if (value >= 0) return static_cast<unsigned long long>(value);
                return static_cast<unsigned long long>(-(value + 1)) + 1ULL;
            }

            std::wstring FormatSignedInteger(long long value, const FormatSpec& spec) {
                const char32_t type = spec.GetType();
                if (!IsIntegerType(type)) return std::wstring();

                if (type == U'c') {
                    return String(static_cast<char32_t>(value)).ToWString();
                }

                const bool negative = value < 0; // 有符号整数是否为负
                if (type == U'x' || type == U'X') {
                    return IntegerPrefix(type, spec.GetAlternateForm()) +
                        UnsignedToBase(static_cast<unsigned long long>(value), 16, type == U'X');
                }
                if (type == U'b' || type == U'B') {
                    return IntegerPrefix(type, spec.GetAlternateForm()) +
                        UnsignedToBase(static_cast<unsigned long long>(value), 2, false);
                }
                if (type == U'o' || type == U'O') {
                    return IntegerPrefix(type, spec.GetAlternateForm()) +
                        UnsignedToBase(static_cast<unsigned long long>(value), 8, false);
                }

                return SignPrefix(negative, spec) + UnsignedToBase(SignedMagnitude(value), 10, false);
            }

            std::wstring FormatUnsignedInteger(unsigned long long value, const FormatSpec& spec) {
                const char32_t type = spec.GetType();
                if (!IsIntegerType(type)) return std::wstring();

                if (type == U'c') {
                    return String(static_cast<char32_t>(value)).ToWString();
                }

                if (type == U'x' || type == U'X') {
                    return IntegerPrefix(type, spec.GetAlternateForm()) +
                        UnsignedToBase(value, 16, type == U'X');
                }
                if (type == U'b' || type == U'B') {
                    return IntegerPrefix(type, spec.GetAlternateForm()) +
                        UnsignedToBase(value, 2, false);
                }
                if (type == U'o' || type == U'O') {
                    return IntegerPrefix(type, spec.GetAlternateForm()) +
                        UnsignedToBase(value, 8, false);
                }

                return SignPrefix(false, spec) + UnsignedToBase(value, 10, false);
            }

            std::optional<std::wstring> FormatIntegerFast(unsigned long long magnitude, bool negative, const FormatSpecSnapshot& spec) {
                if (!IsIntegerType(spec.type) || spec.type == U'c') return std::nullopt;

                unsigned base = 10;          // 当前整数输出进制
                bool upper = false;          // 十六进制字母是否大写
                std::wstring_view prefix;    // # 标志产生的进制前缀
                if (spec.type == U'x' || spec.type == U'X') {
                    base = 16;
                    upper = spec.type == U'X';
                    if (spec.alternate) prefix = upper ? std::wstring_view(L"0X", 2) : std::wstring_view(L"0x", 2);
                }
                else if (spec.type == U'b' || spec.type == U'B') {
                    base = 2;
                    if (spec.alternate) prefix = spec.type == U'B' ? std::wstring_view(L"0B", 2) : std::wstring_view(L"0b", 2);
                }
                else if (spec.type == U'o' || spec.type == U'O') {
                    base = 8;
                    if (spec.alternate) prefix = spec.type == U'O' ? std::wstring_view(L"0O", 2) : std::wstring_view(L"0o", 2);
                }

                wchar_t digitsBuffer[65]; // 64-bit 二进制最多 64 位，额外一位留安全余量
                size_t digitCount = 0;    // digitsBuffer 中已写入的数字数
                const wchar_t* digits = upper ? L"0123456789ABCDEF" : L"0123456789abcdef"; // 当前进制字符表
                do {
                    digitsBuffer[digitCount++] = digits[magnitude % base];
                    magnitude /= base;
                } while (magnitude != 0);

                wchar_t signChar = 0; // 需要输出的符号字符，0 表示无符号前缀
                if (negative) signChar = L'-';
                else if (spec.sign == U'+') signChar = L'+';
                else if (spec.sign == U' ') signChar = L' ';

                const size_t prefixLen = prefix.size(); // 进制前缀宽字符数
                const size_t rawLen = (signChar ? 1 : 0) + prefixLen + digitCount; // 不含填充的输出长度
                const size_t width = spec.width && *spec.width > 0 ? static_cast<size_t>(*spec.width) : 0; // 目标最小宽度
                const size_t pad = width > rawLen ? width - rawLen : 0; // 需要补齐的填充宽度
                const bool zeroInternal = spec.align == U'=' || spec.zeroPad; // 是否在符号/前缀后内部补零
                const bool simpleFill = spec.fill.size() == 1; // 快路径只处理单 code unit 填充
                const wchar_t padChar = zeroInternal && spec.zeroPad ? L'0' : (simpleFill ? spec.fill[0] : L' '); // 实际填充字符

                std::wstring out; // 当前整数格式化输出缓冲
                if (!simpleFill && pad != 0 && !zeroInternal) return std::nullopt;
                out.reserve(rawLen + (simpleFill ? pad : 0));

                // 右/居中默认先写外部填充，'=' 对齐则延后到符号和前缀之后。
                if (!zeroInternal && spec.align != U'<') {
                    out.append(pad, padChar);
                }
                if (signChar) out.push_back(signChar);
                out.append(prefix);
                if (zeroInternal) out.append(pad, padChar);
                for (size_t i = 0; i < digitCount; ++i) {
                    out.push_back(digitsBuffer[digitCount - 1 - i]);
                }
                // 左对齐填充放到数字主体之后。
                if (!zeroInternal && spec.align == U'<') {
                    out.append(pad, padChar);
                }
                return out;
            }

            std::wstring ApplyStringPrecision(const String& value, const FormatSpec& spec) {
                if (!spec.GetPrecision()) return value.ToWString();
                const int precision = *spec.GetPrecision();
                if (precision <= 0) return std::wstring();
                return value.Left(static_cast<size_t>(precision)).ToWString();
            }

            size_t NumericPrefixLength(const std::wstring& src) {
                size_t pos = 0; // 数值前缀扫描位置
                if (pos < src.size() && (src[pos] == L'+' || src[pos] == L'-' || src[pos] == L' ')) {
                    ++pos;
                }
                if (pos + 1 < src.size() && src[pos] == L'0') {
                    const wchar_t marker = src[pos + 1]; // 进制前缀标记字符
                    if (marker == L'x' || marker == L'X' ||
                        marker == L'b' || marker == L'B' ||
                        marker == L'o' || marker == L'O') {
                        pos += 2;
                    }
                }
                return pos;
            }

            size_t WStringCodePointCount(const std::wstring& src) {
#if WCHAR_MAX == 0xFFFF
                size_t count = 0; // 已统计的 Unicode code point 数
                for (size_t i = 0; i < src.size();) {
                    const wchar_t c = src[i]; // 当前 UTF-16 单元
                    if (c >= 0xD800 && c <= 0xDBFF &&
                        i + 1 < src.size() && src[i + 1] >= 0xDC00 && src[i + 1] <= 0xDFFF) {
                        i += 2;
                    }
                    else {
                        ++i;
                    }
                    ++count;
                }
                return count;
#else
                return src.size();
#endif
            }

            std::wstring ApplyWStringPrecision(const std::wstring& value, const FormatSpec& spec) {
                if (!spec.GetPrecision()) return value;
                const int precision = *spec.GetPrecision();
                if (precision <= 0) return std::wstring();

#if WCHAR_MAX == 0xFFFF
                size_t offset = 0; // 精度截断对应的 UTF-16 偏移
                size_t count = 0; // 已保留的 Unicode code point 数
                while (offset < value.size() && count < static_cast<size_t>(precision)) {
                    if (value[offset] >= 0xD800 && value[offset] <= 0xDBFF &&
                        offset + 1 < value.size() && value[offset + 1] >= 0xDC00 && value[offset + 1] <= 0xDFFF) {
                        offset += 2;
                    }
                    else {
                        ++offset;
                    }
                    ++count;
                }
                return value.substr(0, offset);
#else
                return value.substr(0, std::min(value.size(), static_cast<size_t>(precision)));
#endif
            }

            std::wstring U16ViewToWString(std::u16string_view value) {
                if (value.empty()) return std::wstring();
#if WCHAR_MAX == 0xFFFF
                return std::wstring(reinterpret_cast<const wchar_t*>(value.data()), value.size());
#else
                return String(value).ToWString();
#endif
            }

            std::wstring U32ViewToWString(std::u32string_view value) {
                if (value.empty()) return std::wstring();
#if WCHAR_MAX == 0xFFFF
                std::wstring out; // UTF-32 视图转出的 UTF-16 宽字符串
                out.reserve(value.size());
                for (char32_t cp : value) {
                    if (cp <= 0xFFFF) {
                        if (cp >= 0xD800 && cp <= 0xDFFF) throw std::runtime_error("Invalid UTF-32 surrogate codepoint");
                        out.push_back(static_cast<wchar_t>(cp));
                    }
                    else {
                        if (cp > 0x10FFFF) throw std::runtime_error("Invalid UTF-32 codepoint");
                        cp -= 0x10000;
                        out.push_back(static_cast<wchar_t>((cp >> 10) + 0xD800));
                        out.push_back(static_cast<wchar_t>((cp & 0x3FF) + 0xDC00));
                    }
                }
                return out;
#else
                return std::wstring(reinterpret_cast<const wchar_t*>(value.data()), value.size());
#endif
            }

            void AppendStringToWString(std::wstring& out, const String& value) {
#if WCHAR_MAX == 0xFFFF
                out.append(reinterpret_cast<const wchar_t*>(value.data()), value.Length());
#else
                out += value.ToWString();
#endif
            }

            struct CompiledFormatToken {
                bool isPlaceholder = false; // 当前 token 是否为占位符
                std::wstring literal; // 非占位符 token 的字面量文本
                FormatSpecSnapshot spec; // 占位符 token 的拍平格式规格
            };

            struct CompiledFormatPlan {
                bool hasFatalError = false; // 解析阶段是否出现致命错误
                bool hasExplicitIndex = false; // 格式串是否使用显式参数索引
                std::vector<CompiledFormatToken> tokens; // 编译后的 token 列表
            };

            std::shared_ptr<const CompiledFormatPlan> CompilePlan(const FormatParser::Result& res) {
                auto plan = std::make_shared<CompiledFormatPlan>(); // 线程本地复用的编译计划
                plan->hasFatalError = res.hasFatalError;
                plan->tokens.reserve(res.tokens.size());
                for (const auto& tok : res.tokens) {
                    CompiledFormatToken out; // 当前解析 token 的编译结果
                    out.isPlaceholder = tok.isPlaceholder;
                    if (tok.isPlaceholder) {
                        out.spec = FormatSpecSnapshot(tok.spec);
                        if (out.spec.explicitIndex) plan->hasExplicitIndex = true;
                    }
                    else {
                        out.literal.reserve(tok.literal.Length());
                        AppendStringToWString(out.literal, tok.literal);
                    }
                    plan->tokens.push_back(std::move(out));
                }
                return plan;
            }

            std::vector<Any> MaterializeArgs(const FormatArgView* args, size_t argCount) {
                std::vector<Any> out; // 由轻量视图物化出的 std::any 参数列表
                out.reserve(argCount);
                for (size_t i = 0; i < argCount; ++i) {
                    out.emplace_back(args[i].makeAny ? args[i].makeAny(args[i].value) : Any());
                }
                return out;
            }
        }

        // 格式化核心私有状态，只在实现文件中暴露布局。
        struct FormatInternal::FormatInternalImpl {
            // 支持 std::u16string_view 透明查找的 FNV-1a 哈希。
            struct U16Hash {
                using is_transparent = void;
                size_t operator()(std::u16string_view s) const noexcept {
                    size_t h = 1469598103934665603ull; // FNV-1a 64 位偏移基准值
                    for (char16_t c : s) {
                        h ^= static_cast<size_t>(c);
                        h *= 1099511628211ull;
                    }
                    return h;
                }
                size_t operator()(const std::u16string& s) const noexcept {
                    return (*this)(std::u16string_view(s));
                }
            };

            // 支持 std::u16string_view 透明查找的等值比较。
            struct U16Equal {
                using is_transparent = void;
                bool operator()(std::u16string_view lhs, std::u16string_view rhs) const noexcept {
                    return lhs == rhs;
                }
                bool operator()(const std::u16string& lhs, const std::u16string& rhs) const noexcept {
                    return lhs == rhs;
                }
            };

            mutable std::shared_mutex m_mutex; // 保护 formatter 注册表
            std::unordered_map<std::string, UserFormatter> m_nameFormatters; // {:uName} 注册表
            std::unordered_map<std::type_index, UserFormatter> m_typeFormatters; // typeid 注册表
            std::atomic<size_t> m_nameFormatterCount{ 0 }; // 名称注册表快路径判空计数
            std::atomic<size_t> m_typeFormatterCount{ 0 }; // 类型注册表快路径判空计数
            mutable std::shared_mutex m_parseCacheMutex; // 保护格式串解析缓存
            std::unordered_map<std::u16string, std::shared_ptr<const FormatParser::Result>, U16Hash, U16Equal> m_parseCache; // 格式串到解析结果的缓存
        };

        FormatInternal::FormatInternal() : m_impl(new FormatInternalImpl()) {
        }

        FormatInternal::~FormatInternal() {
            delete m_impl;
            m_impl = nullptr;
        }

        FormatInternal::FormatInternal(const FormatInternal& other)
            : m_impl(new FormatInternalImpl()) {
            if (other.m_impl) {
                std::shared_lock lock(other.m_impl->m_mutex);
                // formatter 注册表可复制，解析缓存与实例生命周期绑定不复制。
                m_impl->m_nameFormatters = other.m_impl->m_nameFormatters;
                m_impl->m_typeFormatters = other.m_impl->m_typeFormatters;
                m_impl->m_nameFormatterCount.store(m_impl->m_nameFormatters.size(), std::memory_order_release);
                // 原子计数与容器大小同步，快路径据此判断是否需要回退。
                m_impl->m_typeFormatterCount.store(m_impl->m_typeFormatters.size(), std::memory_order_release);
                m_impl->m_parseCache.clear();
            }
        }

        FormatInternal& FormatInternal::operator=(const FormatInternal& other) {
            if (this == &other) return *this;
            if (!m_impl) m_impl = new FormatInternalImpl();
            if (!other.m_impl) {
                std::unique_lock lock(m_impl->m_mutex);
                // 赋值自空实现时清空注册表和解析缓存。
                m_impl->m_nameFormatters.clear();
                m_impl->m_typeFormatters.clear();
                m_impl->m_nameFormatterCount.store(0, std::memory_order_release);
                // 空实现没有任何 formatter，两个计数都归零。
                m_impl->m_typeFormatterCount.store(0, std::memory_order_release);
                {
                    std::unique_lock cacheLock(m_impl->m_parseCacheMutex);
                    m_impl->m_parseCache.clear();
                }
                return *this;
            }

            std::unique_lock lockThis(m_impl->m_mutex, std::defer_lock);
            std::shared_lock lockOther(other.m_impl->m_mutex, std::defer_lock);
            std::lock(lockThis, lockOther);
            // 同时锁住两边注册表，避免复制过程中用户注册 formatter。
            m_impl->m_nameFormatters = other.m_impl->m_nameFormatters;
            m_impl->m_typeFormatters = other.m_impl->m_typeFormatters;
            m_impl->m_nameFormatterCount.store(m_impl->m_nameFormatters.size(), std::memory_order_release);
            // 原子计数与容器大小同步，避免读者看见过期计数。
            m_impl->m_typeFormatterCount.store(m_impl->m_typeFormatters.size(), std::memory_order_release);
            {
                std::unique_lock cacheLock(m_impl->m_parseCacheMutex);
                m_impl->m_parseCache.clear();
            }
            return *this;
        }

        FormatInternal::FormatInternal(FormatInternal&& other) noexcept
            : m_impl(other.m_impl) {
            other.m_impl = nullptr;
        }

        FormatInternal& FormatInternal::operator=(FormatInternal&& other) noexcept {
            if (this == &other) return *this;
            delete m_impl;
            m_impl = other.m_impl;
            other.m_impl = nullptr;
            // 移动后源对象只保留空 PImpl 指针，析构安全。
            return *this;
        }

        FormatInternal& FormatInternal::Instance() {
            static FormatInternal instance; // 全局共享格式化器注册表实例
            return instance;
        }

        void FormatInternal::RegisterFormatter(const std::string& name, UserFormatter func) {
            if (name.empty()) return;
            std::unique_lock lock(m_impl->m_mutex);
            // 注册表写入后同步原子计数，快路径可先看计数决定是否回退。
            m_impl->m_nameFormatters[name] = std::move(func);
            m_impl->m_nameFormatterCount.store(m_impl->m_nameFormatters.size(), std::memory_order_release);
        }

        void FormatInternal::UnregisterFormatter(const std::string& name) {
            if (name.empty()) return;
            std::unique_lock lock(m_impl->m_mutex);
            // 删除后更新计数，让无 formatter 场景保持零成本检查。
            m_impl->m_nameFormatters.erase(name);
            m_impl->m_nameFormatterCount.store(m_impl->m_nameFormatters.size(), std::memory_order_release);
        }

        bool FormatInternal::HasFormatter(const std::string& name) const {
            if (name.empty()) return false;
            std::shared_lock lock(m_impl->m_mutex);
            // 只读查询使用共享锁，允许多个线程并发检查。
            return m_impl->m_nameFormatters.find(name) != m_impl->m_nameFormatters.end();
        }

        const FormatParser::Result* FormatInternal::GetParsedFormat(const String& fmt) {
            const std::u16string_view keyView(fmt.data(), fmt.Length()); // 以 UTF-16 内容作为解析缓存键
            struct LocalParseCache {
                const FormatInternal* owner = nullptr; // 缓存所属 FormatInternal 实例
                std::u16string key;                    // 最近一次格式串键
                std::shared_ptr<const FormatParser::Result> value; // 最近一次解析结果
            };
            thread_local LocalParseCache localCache; // 每线程最近一次命中缓存

            if (localCache.owner == this && localCache.value &&
                std::u16string_view(localCache.key) == keyView) {
                return localCache.value.get();
            }

            {
                std::shared_lock lock(m_impl->m_parseCacheMutex); // 共享读取全局解析缓存
                auto it = m_impl->m_parseCache.find(keyView);     // 全局缓存候选项
                if (it != m_impl->m_parseCache.end()) {
                    localCache.owner = this;
                    localCache.key.assign(keyView);
                    localCache.value = it->second;
                    return localCache.value.get();
                }
            }

            FormatParser parser; // 未命中缓存时使用的格式串解析器
            auto parsed = std::make_shared<FormatParser::Result>(parser.Parse(fmt)); // 新解析结果
            std::u16string key(keyView); // 将 view 固化为 unordered_map 拥有的 key
            {
                std::unique_lock lock(m_impl->m_parseCacheMutex); // 写入全局解析缓存
                auto it = m_impl->m_parseCache.find(std::u16string_view(key)); // 并发写入后的二次检查
                if (it != m_impl->m_parseCache.end()) {
                    localCache.owner = this;
                    localCache.key.assign(keyView);
                    localCache.value = it->second;
                    return localCache.value.get();
                }

                if (m_impl->m_parseCache.size() >= kParseCacheMaxEntries) {
                    m_impl->m_parseCache.clear();
                }
                // 全局缓存拥有 key 字符串，线程本地缓存只保存最近一次命中。
                m_impl->m_parseCache.emplace(std::move(key), parsed);
            }
            localCache.owner = this;
            localCache.key.assign(keyView);
            localCache.value = parsed;
            // 返回裸指针但所有权由 shared_ptr 缓存保持。
            return localCache.value.get();
        }

        String FormatInternal::FormatAny(const String& fmt, const std::vector<Any>& args) {
            const auto* resPtr = GetParsedFormat(fmt); // 解析缓存返回的格式 token 列表
            const auto& res = *resPtr;                 // 本次格式化使用的解析结果
            if (res.hasFatalError) return String(U"{!}");

            const size_t argCount = args.size(); // 可用参数数量
            bool hasExplicitIndex = false;       // 是否存在显式索引占位符
            for (const auto& tok : res.tokens) {
                if (tok.isPlaceholder && tok.spec.HasExplicitIndex()) {
                    hasExplicitIndex = true;
                    break;
                }
            }

            std::vector<bool> used; // 显式索引模式下记录已被占用的参数
            if (hasExplicitIndex) {
                used.assign(argCount, false);
                for (const auto& tok : res.tokens) {
                    if (!tok.isPlaceholder || !tok.spec.HasExplicitIndex()) continue;
                    const int idx = tok.spec.GetIndex(); // 当前显式指定的参数索引
                    if (idx >= 0 && static_cast<size_t>(idx) < argCount) {
                        used[static_cast<size_t>(idx)] = true;
                    }
                }
            }

            size_t nextAuto = 0; // 自动索引下一候选参数
            std::wstring output; // 宽字符输出缓冲，最后一次性构造 String
            output.reserve(fmt.Length() + args.size() * 8);

            for (const auto& tok : res.tokens) {
                if (!tok.isPlaceholder) {
                    AppendStringToWString(output, tok.literal);
                    continue;
                }

                const FormatSpec& spec = tok.spec; // 当前占位符规格
                int chosenIndex = -1;              // 当前占位符最终使用的参数索引

                if (spec.HasExplicitIndex()) {
                    chosenIndex = spec.GetIndex();
                }
                else {
                    while (hasExplicitIndex && nextAuto < argCount && used[nextAuto]) ++nextAuto;
                    if (nextAuto < argCount) {
                        chosenIndex = static_cast<int>(nextAuto);
                        if (hasExplicitIndex) used[nextAuto] = true;
                        ++nextAuto;
                    }
                }

                if (chosenIndex < 0 || static_cast<size_t>(chosenIndex) >= argCount) {
                    output += L"{?}";
                    continue;
                }

                std::wstring formattedText = FormatArgumentToWString(&args[static_cast<size_t>(chosenIndex)], spec); // 参数格式化结果
                if (!spec.GetWidth()) {
                    output += formattedText;
                }
                else {
                    output += ApplyAlignmentAndFill(formattedText, spec);
                }
            }

            return String(output);
        }

        String FormatInternal::FormatViews(const String& fmt, const FormatArgView* args, size_t argCount) {
            if (m_impl->m_nameFormatterCount.load(std::memory_order_acquire) != 0 ||
                m_impl->m_typeFormatterCount.load(std::memory_order_acquire) != 0) {
                return FormatAny(fmt, MaterializeArgs(args, argCount));
            }

            const auto* resPtr = GetParsedFormat(fmt); // 当前格式串解析结果缓存指针
            struct LocalPlanCache {
                const FormatParser::Result* source = nullptr; // 当前线程缓存对应的解析结果
                std::shared_ptr<const CompiledFormatPlan> plan; // 当前线程缓存的编译计划
            };
            thread_local LocalPlanCache planCache; // 避免重复编译同一格式串的线程本地缓存
            if (planCache.source != resPtr || !planCache.plan) {
                planCache.source = resPtr;
                planCache.plan = CompilePlan(*resPtr);
            }
            const auto& plan = *planCache.plan; // 本次格式化使用的编译计划
            if (plan.hasFatalError) return String(U"{!}");

            auto formatSnapshot = [&](const FormatArgView& arg, const FormatSpecSnapshot& spec, bool& handled) -> std::wstring {
                handled = true;
                if (!IsKnownFormatType(spec.type)) return L"{!type}";
                if (spec.type == U'u') {
                    handled = false;
                    return {};
                }

                const auto& ti = arg.type; // 当前参数视图携带的静态类型
                auto formatSigned = [&](long long value) -> std::optional<std::wstring> { // 有符号整数快路径格式化器
                    return FormatIntegerFast(SignedMagnitude(value), value < 0, spec);
                };
                auto formatUnsigned = [&](unsigned long long value) -> std::optional<std::wstring> { // 无符号整数快路径格式化器
                    return FormatIntegerFast(value, false, spec);
                };

                std::optional<std::wstring> formatted; // 快路径格式化结果，空值表示回退
                if (ti == typeid(int)) formatted = formatSigned(*static_cast<const int*>(arg.value));
                else if (ti == typeid(short)) formatted = formatSigned(*static_cast<const short*>(arg.value));
                else if (ti == typeid(long)) formatted = formatSigned(*static_cast<const long*>(arg.value));
                // 整数族不物化 std::any，直接读取 FormatArgView 中的原值。
                else if (ti == typeid(long long)) formatted = formatSigned(*static_cast<const long long*>(arg.value));
                else if (ti == typeid(unsigned int)) formatted = formatUnsigned(*static_cast<const unsigned int*>(arg.value));
                else if (ti == typeid(unsigned short)) formatted = formatUnsigned(*static_cast<const unsigned short*>(arg.value));
                // 无符号整数同样保持 view 快路径，避免临时 Any。
                else if (ti == typeid(unsigned long)) formatted = formatUnsigned(*static_cast<const unsigned long*>(arg.value));
                else if (ti == typeid(unsigned long long)) formatted = formatUnsigned(*static_cast<const unsigned long long*>(arg.value));
                else if ((spec.type == U's' || spec.type == U'S') && ti == typeid(const char32_t*)) {
                    if (spec.width || spec.precision) {
                        handled = false;
                        return {};
                    }
                    // 指针字符串视图快路径只处理无宽度/精度的纯文本输出。
                    const char32_t* ptr = static_cast<const char32_t*>(arg.value);
                    formatted = ptr ? U32ViewToWString(std::u32string_view(ptr)) : std::wstring();
                }
                else if ((spec.type == U's' || spec.type == U'S') && ti == typeid(std::u32string_view)) {
                    if (spec.width || spec.precision) {
                        handled = false;
                        return {};
                    }
                    formatted = U32ViewToWString(*static_cast<const std::u32string_view*>(arg.value));
                }
                else if ((spec.type == U's' || spec.type == U'S') && ti == typeid(std::u16string_view)) {
                    if (spec.width || spec.precision) {
                        handled = false;
                        return {};
                    }
                    formatted = U16ViewToWString(*static_cast<const std::u16string_view*>(arg.value));
                }

                if (!formatted) {
                    handled = false;
                    return {};
                }

                return *formatted;
            };

            std::vector<bool> used; // 显式索引模式下记录已被占用的参数
            if (plan.hasExplicitIndex) {
                used.assign(argCount, false);
                for (const auto& tok : plan.tokens) {
                    if (!tok.isPlaceholder || !tok.spec.explicitIndex) continue;
                    const int idx = tok.spec.index; // 当前显式指定的参数索引
                    if (idx >= 0 && static_cast<size_t>(idx) < argCount) {
                        used[static_cast<size_t>(idx)] = true;
                    }
                }
            }

            size_t nextAuto = 0; // 自动索引下一候选参数
            std::wstring output; // 宽字符输出缓冲，最后一次性构造 String
            output.reserve(fmt.Length() + argCount * 8);

            for (const auto& tok : plan.tokens) {
                if (!tok.isPlaceholder) {
                    output += tok.literal;
                    continue;
                }

                const FormatSpecSnapshot& spec = tok.spec; // 拍平后的占位符规格
                int chosenIndex = -1;                      // 当前占位符最终使用的参数索引

                if (spec.explicitIndex) {
                    chosenIndex = spec.index;
                }
                else {
                    while (plan.hasExplicitIndex && nextAuto < argCount && used[nextAuto]) ++nextAuto;
                    if (nextAuto < argCount) {
                        chosenIndex = static_cast<int>(nextAuto);
                        if (plan.hasExplicitIndex) used[nextAuto] = true;
                        ++nextAuto;
                    }
                }

                if (chosenIndex < 0 || static_cast<size_t>(chosenIndex) >= argCount) {
                    output += L"{?}";
                    continue;
                }

                bool handled = false; // 是否命中 FormatArgView 快路径
                std::wstring formattedText = formatSnapshot(args[static_cast<size_t>(chosenIndex)], spec, handled); // 快路径格式化结果
                if (!handled) {
                    return FormatAny(fmt, MaterializeArgs(args, argCount));
                }

                output += formattedText;
            }

            return String(output);
        }

        String FormatInternal::FormatArgument(const Any* argPtr, const FormatSpec& spec) const {
            return String(FormatArgumentToWString(argPtr, spec));
        }

        std::wstring FormatInternal::FormatArgumentToWString(const Any* argPtr, const FormatSpec& spec) const {
            if (!argPtr) return L"{?}";
            if (!IsKnownFormatType(spec.GetType())) return L"{!type}";

            const Any& a = *argPtr; // 当前待格式化的 std::any 参数

            if (spec.GetType() == U'u') {
                std::string name = spec.GetTypeExpand().ToStdString(); // 自定义命名 formatter 名称
                if (name.empty()) return L"{!type}";
                if (auto out = TryInvokeNamedFormatter(name, a, spec)) return out->ToWString();
                return L"{!type}";
            }

            std::type_index tid = std::type_index(a.type()); // 用户类型 formatter 的查找键
            if (auto out = TryInvokeTypeFormatter(tid, a, spec)) return out->ToWString();

            if (auto bs = FormatBuiltInToStdString(a, spec)) {
                return *bs;
            }

            String str; // std::any 转 String 的回退承载对象
            if (String::FromAny(a, str)) {
                if (spec.GetType() != U's' && spec.GetType() != U'S') return L"{!type}";
                return ApplyStringPrecision(str, spec);
            }

            return L"{!type}";
        }

        std::wstring FormatInternal::FormatViewArgumentToWString(const FormatArgView& arg, const FormatSpec& spec, bool& handled) const {
            handled = true;
            if (!IsKnownFormatType(spec.GetType())) return L"{!type}";
            if (spec.GetType() == U'u') return L"{!type}";

            if (auto bs = FormatBuiltInViewToStdString(arg, spec)) {
                return *bs;
            }

            handled = false;
            return std::wstring();
        }

        std::optional<std::wstring> FormatInternal::FormatBuiltInViewToStdString(const FormatArgView& arg, const FormatSpec& spec) const {
            try {
                const FormatSpecSnapshot snap(spec); // 拍平规格，减少重复 PImpl 访问
                const char32_t type = snap.type; // 当前格式类型字符
                const auto& ti = arg.type; // 当前参数视图携带的静态类型

                auto formatSigned = [&](long long value) -> std::optional<std::wstring> { // 有符号整数快路径格式化器
                    return FormatIntegerFast(SignedMagnitude(value), value < 0, snap);
                };
                auto formatUnsigned = [&](unsigned long long value) -> std::optional<std::wstring> { // 无符号整数快路径格式化器
                    return FormatIntegerFast(value, false, snap);
                };

                if (ti == typeid(int)) return formatSigned(*static_cast<const int*>(arg.value));
                if (ti == typeid(short)) return formatSigned(*static_cast<const short*>(arg.value));
                if (ti == typeid(long)) return formatSigned(*static_cast<const long*>(arg.value));
                // 视图路径避免 std::any 分配，直接从指针读取整数值。
                if (ti == typeid(long long)) return formatSigned(*static_cast<const long long*>(arg.value));
                if (ti == typeid(unsigned int)) return formatUnsigned(*static_cast<const unsigned int*>(arg.value));
                if (ti == typeid(unsigned short)) return formatUnsigned(*static_cast<const unsigned short*>(arg.value));
                // 无符号整数直接读取原值，保持零分配。
                if (ti == typeid(unsigned long)) return formatUnsigned(*static_cast<const unsigned long*>(arg.value));
                if (ti == typeid(unsigned long long)) return formatUnsigned(*static_cast<const unsigned long long*>(arg.value));

                if (ti == typeid(float) || ti == typeid(double) || ti == typeid(long double)) {
                    if (!IsFloatingType(type)) return std::nullopt;
                    long double v = 0.0; // 提升后的浮点值
                    if (ti == typeid(float)) v = *static_cast<const float*>(arg.value);
                    else if (ti == typeid(double)) v = *static_cast<const double*>(arg.value);
                    else v = *static_cast<const long double*>(arg.value);

                    std::wostringstream oss; // 浮点格式化输出缓冲
                    const int prec = spec.GetPrecision().value_or(6); // 浮点输出精度
                    if (type == U'e' || type == U'E') oss << std::scientific << std::setprecision(prec);
                    else if (type == U'g' || type == U'G') oss << std::defaultfloat << std::setprecision(prec);
                    else oss << std::fixed << std::setprecision(prec);
                    // 大写格式类型要求指数和特殊值使用大写形式。
                    if (type == U'E' || type == U'F' || type == U'G') oss << std::uppercase;

                    const bool percent = type == U'%'; // 是否按百分比格式输出
                    const long double formattedValue = percent ? v * 100.0L : v;
                    if (formattedValue >= 0) oss << SignPrefix(false, spec);
                    oss << static_cast<long double>(formattedValue);
                    // 百分号格式在数值放大后追加字面百分号。
                    if (percent) oss << L"%";
                    return oss.str();
                }

                if (ti == typeid(bool)) {
                    const bool v = *static_cast<const bool*>(arg.value); // bool 参数值
                    if (type == U's' || type == U'S') return v ? L"true" : L"false";
                    if (type == U'd' || type == U'i') return v ? L"1" : L"0";
                    return std::nullopt;
                }

                if (ti == typeid(char)) {
                    const char v = *static_cast<const char*>(arg.value); // char 参数值
                    if (type == U'c' || type == U's' || type == U'S') return String(v).ToWString();
                    return FormatSignedInteger(static_cast<unsigned char>(v), spec);
                }
                if (ti == typeid(char8_t)) {
                    const char8_t v = *static_cast<const char8_t*>(arg.value); // char8_t 参数值
                    if (type == U'c' || type == U's' || type == U'S') return String(v).ToWString();
                    return FormatUnsignedInteger(static_cast<unsigned char>(v), spec);
                }
                if (ti == typeid(char16_t)) {
                    const char16_t v = *static_cast<const char16_t*>(arg.value); // char16_t 参数值
                    if (type == U'c' || type == U's' || type == U'S') return U16ViewToWString(std::u16string_view(&v, 1));
                    return FormatUnsignedInteger(static_cast<unsigned long long>(v), spec);
                }
                if (ti == typeid(char32_t)) {
                    const char32_t v = *static_cast<const char32_t*>(arg.value); // char32_t 参数值
                    if (type == U'c' || type == U's' || type == U'S') return U32ViewToWString(std::u32string_view(&v, 1));
                    return FormatUnsignedInteger(static_cast<unsigned long long>(v), spec);
                }
                if (ti == typeid(wchar_t)) {
                    const wchar_t v = *static_cast<const wchar_t*>(arg.value); // wchar_t 参数值
                    if (type == U'c' || type == U's' || type == U'S') return std::wstring(1, v);
                    return FormatUnsignedInteger(static_cast<unsigned long long>(v), spec);
                }

                if (type == U'p' || type == U'P') {
                    const void* ptr = nullptr; // 待格式化的指针值
                    if (ti == typeid(const void*)) ptr = arg.value;
                    else if (ti == typeid(void*)) ptr = const_cast<void*>(arg.value);
                    else return std::nullopt;

                    std::wostringstream oss; // 指针十六进制输出缓冲
                    oss << L"0x"
                        << std::uppercase << std::hex
                        << std::setw(sizeof(void*) * 2) << std::setfill(L'0')
                        << reinterpret_cast<std::uintptr_t>(ptr);
                    // 指针统一输出固定宽度十六进制，便于跨平台比较。
                    return oss.str();
                }

                if (ti == typeid(LikesProgram::Time::TimePoint)) {
                    if (type != U's' && type != U't' && type != U'T') return std::nullopt;
                    const auto& tp = *static_cast<const LikesProgram::Time::TimePoint*>(arg.value);
                    // 无扩展类型时使用默认时间格式，有扩展时透传给 Time 模块。
                    if (spec.GetTypeExpand().Empty()) return LikesProgram::Time::FormatTime(tp).ToWString();
                    return LikesProgram::Time::FormatTime(tp, spec.GetTypeExpand()).ToWString();
                }

                if (type != U's' && type != U'S') return std::nullopt;
                // 字符串族只允许 s/S 类型，其余类型交给调用方回退或报错。
                if (ti == typeid(String)) return ApplyStringPrecision(*static_cast<const String*>(arg.value), spec);
                if (ti == typeid(std::wstring)) return ApplyWStringPrecision(*static_cast<const std::wstring*>(arg.value), spec);
                if (ti == typeid(std::wstring_view)) return ApplyWStringPrecision(std::wstring(*static_cast<const std::wstring_view*>(arg.value)), spec);
                // std::string 家族按 UTF-8 构造 String 后再套用精度。
                if (ti == typeid(std::string)) return ApplyStringPrecision(String(*static_cast<const std::string*>(arg.value), String::Encoding::UTF8), spec);
                if (ti == typeid(std::string_view)) return ApplyStringPrecision(String(*static_cast<const std::string_view*>(arg.value), String::Encoding::UTF8), spec);
                if (ti == typeid(std::u8string)) return ApplyStringPrecision(String(*static_cast<const std::u8string*>(arg.value)), spec);
                // view 类型按值构造临时 String/WString 后再应用精度。
                if (ti == typeid(std::u8string_view)) return ApplyStringPrecision(String(*static_cast<const std::u8string_view*>(arg.value)), spec);
                if (ti == typeid(std::u16string)) return ApplyWStringPrecision(U16ViewToWString(*static_cast<const std::u16string*>(arg.value)), spec);
                if (ti == typeid(std::u16string_view)) return ApplyWStringPrecision(U16ViewToWString(*static_cast<const std::u16string_view*>(arg.value)), spec);
                // UTF-32 家族先转换到 wstring，再执行 code point 精度裁剪。
                if (ti == typeid(std::u32string)) return ApplyWStringPrecision(U32ViewToWString(*static_cast<const std::u32string*>(arg.value)), spec);
                if (ti == typeid(std::u32string_view)) return ApplyWStringPrecision(U32ViewToWString(*static_cast<const std::u32string_view*>(arg.value)), spec);
                if (ti == typeid(const char*)) {
                    const char* ptr = static_cast<const char*>(arg.value);
                    return ApplyStringPrecision(String(ptr ? ptr : "", String::Encoding::UTF8), spec);
                }
                if (ti == typeid(char*)) {
                    char* ptr = static_cast<char*>(const_cast<void*>(arg.value));
                    return ApplyStringPrecision(String(ptr ? ptr : "", String::Encoding::UTF8), spec);
                }
                if (ti == typeid(const char8_t*)) {
                    const char8_t* ptr = static_cast<const char8_t*>(arg.value);
                    return ApplyStringPrecision(String(ptr ? ptr : u8""), spec);
                }
                if (ti == typeid(char8_t*)) {
                    char8_t* ptr = static_cast<char8_t*>(const_cast<void*>(arg.value));
                    return ApplyStringPrecision(String(ptr ? ptr : u8""), spec);
                }
                if (ti == typeid(const wchar_t*)) {
                    const wchar_t* ptr = static_cast<const wchar_t*>(arg.value);
                    return ApplyWStringPrecision(ptr ? std::wstring(ptr) : std::wstring(), spec);
                }
                if (ti == typeid(wchar_t*)) {
                    wchar_t* ptr = static_cast<wchar_t*>(const_cast<void*>(arg.value));
                    return ApplyWStringPrecision(ptr ? std::wstring(ptr) : std::wstring(), spec);
                }
                if (ti == typeid(const char16_t*)) {
                    const char16_t* ptr = static_cast<const char16_t*>(arg.value);
                    return ApplyWStringPrecision(ptr ? U16ViewToWString(std::u16string_view(ptr)) : std::wstring(), spec);
                }
                if (ti == typeid(char16_t*)) {
                    char16_t* ptr = static_cast<char16_t*>(const_cast<void*>(arg.value));
                    return ApplyWStringPrecision(ptr ? U16ViewToWString(std::u16string_view(ptr)) : std::wstring(), spec);
                }
                if (ti == typeid(const char32_t*)) {
                    const char32_t* ptr = static_cast<const char32_t*>(arg.value);
                    return ApplyWStringPrecision(ptr ? U32ViewToWString(std::u32string_view(ptr)) : std::wstring(), spec);
                }
                if (ti == typeid(char32_t*)) {
                    char32_t* ptr = static_cast<char32_t*>(const_cast<void*>(arg.value));
                    return ApplyWStringPrecision(ptr ? U32ViewToWString(std::u32string_view(ptr)) : std::wstring(), spec);
                }
            }
            catch (...) {
                return std::nullopt;
            }

            return std::nullopt;
        }

        std::optional<std::wstring> FormatInternal::FormatBuiltInToStdString(const Any& a, const FormatSpec& spec) const {
            try {
                const char32_t type = spec.GetType(); // 当前格式类型字符

                if (a.type() == typeid(int) || a.type() == typeid(short) ||
                    a.type() == typeid(long) || a.type() == typeid(long long)) {
                    long long v = 0; // 提升后的有符号整数值
                    if (a.type() == typeid(int)) v = std::any_cast<int>(a);
                    else if (a.type() == typeid(short)) v = std::any_cast<short>(a);
                    else if (a.type() == typeid(long)) v = std::any_cast<long>(a);
                    // 最大有符号类型直接进入统一格式化函数。
                    else v = std::any_cast<long long>(a);

                    auto out = FormatSignedInteger(v, spec); // 有符号整数格式化结果
                    if (out.empty() && v != 0) return std::nullopt;
                    // 空结果只允许数值 0 的空字符串格式，其它情况回退。
                    return out;
                }

                if (a.type() == typeid(unsigned int) ||
                    a.type() == typeid(unsigned short) ||
                    a.type() == typeid(unsigned long) ||
                    a.type() == typeid(unsigned long long)) {
                    unsigned long long v = 0; // 提升后的无符号整数值
                    if (a.type() == typeid(unsigned int)) v = std::any_cast<unsigned int>(a);
                    else if (a.type() == typeid(unsigned short)) v = std::any_cast<unsigned short>(a);
                    else if (a.type() == typeid(unsigned long)) v = std::any_cast<unsigned long>(a);
                    // 最大无符号类型直接进入统一格式化函数。
                    else v = std::any_cast<unsigned long long>(a);

                    auto out = FormatUnsignedInteger(v, spec); // 无符号整数格式化结果
                    if (out.empty() && v != 0) return std::nullopt;
                    // 空结果只允许数值 0 的空字符串格式，其它情况回退。
                    return out;
                }

                if (a.type() == typeid(float) || a.type() == typeid(double) || a.type() == typeid(long double)) {
                    if (!IsFloatingType(type)) return std::nullopt;

                    long double v = 0.0; // 提升后的浮点值
                    if (a.type() == typeid(float)) v = std::any_cast<float>(a);
                    else if (a.type() == typeid(double)) v = std::any_cast<double>(a);
                    // long double 保留最高精度，再交给流格式化。
                    else v = std::any_cast<long double>(a);

                    std::wostringstream oss; // 浮点格式化输出缓冲
                    const int prec = spec.GetPrecision().value_or(6); // 浮点输出精度

                    if (type == U'e' || type == U'E') {
                        oss << std::scientific << std::setprecision(prec);
                    }
                    else if (type == U'g' || type == U'G') {
                        oss << std::defaultfloat << std::setprecision(prec);
                    }
                    else {
                        oss << std::fixed << std::setprecision(prec);
                    }

                    if (type == U'E' || type == U'F' || type == U'G') {
                        oss << std::uppercase;
                    }

                    const bool percent = type == U'%'; // 是否按百分比格式输出
                    const long double formattedValue = percent ? v * 100.0L : v;
                    if (formattedValue >= 0) oss << SignPrefix(false, spec);
                    oss << static_cast<long double>(formattedValue);
                    // 百分号格式在数值放大后追加字面百分号。
                    if (percent) oss << L"%";
                    return oss.str();
                }

                if (a.type() == typeid(bool)) {
                    if (type == U's' || type == U'S') return std::any_cast<bool>(a) ? L"true" : L"false";
                    if (type == U'd' || type == U'i') return std::any_cast<bool>(a) ? L"1" : L"0";
                    return std::nullopt;
                }

                if (a.type() == typeid(char)) {
                    if (type == U'c' || type == U's' || type == U'S') {
                        return String(std::any_cast<char>(a)).ToWString();
                    }
                    return FormatSignedInteger(static_cast<unsigned char>(std::any_cast<char>(a)), spec);
                }

                if (a.type() == typeid(char8_t)) {
                    if (type == U'c' || type == U's' || type == U'S') {
                        return String(std::any_cast<char8_t>(a)).ToWString();
                    }
                    return FormatUnsignedInteger(static_cast<unsigned char>(std::any_cast<char8_t>(a)), spec);
                }

                if (a.type() == typeid(char16_t)) {
                    if (type == U'c' || type == U's' || type == U'S') {
                        return String(std::any_cast<char16_t>(a)).ToWString();
                    }
                    return FormatUnsignedInteger(static_cast<unsigned long long>(std::any_cast<char16_t>(a)), spec);
                }

                if (a.type() == typeid(char32_t)) {
                    if (type == U'c' || type == U's' || type == U'S') {
                        return String(std::any_cast<char32_t>(a)).ToWString();
                    }
                    return FormatUnsignedInteger(static_cast<unsigned long long>(std::any_cast<char32_t>(a)), spec);
                }

                if (a.type() == typeid(wchar_t)) {
                    if (type == U'c' || type == U's' || type == U'S') {
                        return std::wstring(1, std::any_cast<wchar_t>(a));
                    }
                    return FormatUnsignedInteger(static_cast<unsigned long long>(std::any_cast<wchar_t>(a)), spec);
                }

                if (type == U'p' || type == U'P') {
                    const void* ptr = nullptr; // 待格式化的指针值
                    if (a.type() == typeid(const void*)) ptr = std::any_cast<const void*>(a);
                    else if (a.type() == typeid(void*)) ptr = std::any_cast<void*>(a);
                    else return std::nullopt;

                    std::wostringstream oss; // 指针十六进制输出缓冲
                    oss << L"0x"
                        << std::uppercase << std::hex
                        << std::setw(sizeof(void*) * 2) << std::setfill(L'0')
                        << reinterpret_cast<std::uintptr_t>(ptr);
                    return oss.str();
                }

                if (a.type() == typeid(String)) {
                    if (type != U's' && type != U'S') return std::nullopt;
                    return ApplyStringPrecision(std::any_cast<String>(a), spec);
                }
                if (a.type() == typeid(std::wstring)) {
                    if (type != U's' && type != U'S') return std::nullopt;
                    return ApplyWStringPrecision(std::any_cast<std::wstring>(a), spec);
                }
                if (a.type() == typeid(std::wstring_view)) {
                    if (type != U's' && type != U'S') return std::nullopt;
                    return ApplyWStringPrecision(std::wstring(std::any_cast<std::wstring_view>(a)), spec);
                }
                if (a.type() == typeid(const wchar_t*)) {
                    if (type != U's' && type != U'S') return std::nullopt;
                    const wchar_t* ptr = std::any_cast<const wchar_t*>(a);
                    return ApplyWStringPrecision(ptr ? std::wstring(ptr) : std::wstring(), spec);
                }
                if (a.type() == typeid(wchar_t*)) {
                    if (type != U's' && type != U'S') return std::nullopt;
                    wchar_t* ptr = std::any_cast<wchar_t*>(a);
                    return ApplyWStringPrecision(ptr ? std::wstring(ptr) : std::wstring(), spec);
                }
                if (a.type() == typeid(std::string)) {
                    if (type != U's' && type != U'S') return std::nullopt;
                    return ApplyStringPrecision(String(std::any_cast<std::string>(a), String::Encoding::UTF8), spec);
                }
                if (a.type() == typeid(std::string_view)) {
                    if (type != U's' && type != U'S') return std::nullopt;
                    return ApplyStringPrecision(String(std::any_cast<std::string_view>(a), String::Encoding::UTF8), spec);
                }
                if (a.type() == typeid(std::u8string)) {
                    if (type != U's' && type != U'S') return std::nullopt;
                    return ApplyStringPrecision(String(std::any_cast<std::u8string>(a)), spec);
                }
                // std::any 路径需要从 any 中取值，再转换到统一字符串表示。
                if (a.type() == typeid(std::u8string_view)) {
                    if (type != U's' && type != U'S') return std::nullopt;
                    return ApplyStringPrecision(String(std::any_cast<std::u8string_view>(a)), spec);
                }
                if (a.type() == typeid(std::u16string)) {
                    if (type != U's' && type != U'S') return std::nullopt;
                    return ApplyWStringPrecision(U16ViewToWString(std::any_cast<std::u16string>(a)), spec);
                }
                if (a.type() == typeid(std::u16string_view)) {
                    if (type != U's' && type != U'S') return std::nullopt;
                    return ApplyWStringPrecision(U16ViewToWString(std::any_cast<std::u16string_view>(a)), spec);
                }
                if (a.type() == typeid(const char16_t*)) {
                    if (type != U's' && type != U'S') return std::nullopt;
                    const char16_t* ptr = std::any_cast<const char16_t*>(a);
                    return ApplyWStringPrecision(ptr ? U16ViewToWString(std::u16string_view(ptr)) : std::wstring(), spec);
                }
                if (a.type() == typeid(char16_t*)) {
                    if (type != U's' && type != U'S') return std::nullopt;
                    char16_t* ptr = std::any_cast<char16_t*>(a);
                    return ApplyWStringPrecision(ptr ? U16ViewToWString(std::u16string_view(ptr)) : std::wstring(), spec);
                }
                if (a.type() == typeid(std::u32string)) {
                    if (type != U's' && type != U'S') return std::nullopt;
                    return ApplyWStringPrecision(U32ViewToWString(std::any_cast<std::u32string>(a)), spec);
                }
                if (a.type() == typeid(std::u32string_view)) {
                    if (type != U's' && type != U'S') return std::nullopt;
                    return ApplyWStringPrecision(U32ViewToWString(std::any_cast<std::u32string_view>(a)), spec);
                }
                if (a.type() == typeid(const char32_t*)) {
                    if (type != U's' && type != U'S') return std::nullopt;
                    const char32_t* ptr = std::any_cast<const char32_t*>(a);
                    return ApplyWStringPrecision(ptr ? U32ViewToWString(std::u32string_view(ptr)) : std::wstring(), spec);
                }
                if (a.type() == typeid(char32_t*)) {
                    if (type != U's' && type != U'S') return std::nullopt;
                    char32_t* ptr = std::any_cast<char32_t*>(a);
                    return ApplyWStringPrecision(ptr ? U32ViewToWString(std::u32string_view(ptr)) : std::wstring(), spec);
                }
                // 窄字符指针按 UTF-8 解释，空指针格式化为空字符串。
                if (a.type() == typeid(const char*)) {
                    if (type != U's' && type != U'S') return std::nullopt;
                    const char* ptr = std::any_cast<const char*>(a);
                    return ApplyStringPrecision(String(ptr ? ptr : "", String::Encoding::UTF8), spec);
                }
                if (a.type() == typeid(char*)) {
                    if (type != U's' && type != U'S') return std::nullopt;
                    char* ptr = std::any_cast<char*>(a);
                    return ApplyStringPrecision(String(ptr ? ptr : "", String::Encoding::UTF8), spec);
                }
                if (a.type() == typeid(const char8_t*)) {
                    if (type != U's' && type != U'S') return std::nullopt;
                    const char8_t* ptr = std::any_cast<const char8_t*>(a);
                    return ApplyStringPrecision(String(ptr ? ptr : u8""), spec);
                }
                // char8_t 指针已经明确是 UTF-8，仍需处理空指针。
                if (a.type() == typeid(char8_t*)) {
                    if (type != U's' && type != U'S') return std::nullopt;
                    char8_t* ptr = std::any_cast<char8_t*>(a);
                    return ApplyStringPrecision(String(ptr ? ptr : u8""), spec);
                }

                if (a.type() == typeid(LikesProgram::Time::TimePoint)) {
                    if (type != U's' && type != U't' && type != U'T') return std::nullopt;
                    auto tp = std::any_cast<LikesProgram::Time::TimePoint>(a); // 待格式化的时间点
                    if (spec.GetTypeExpand().Empty()) return LikesProgram::Time::FormatTime(tp).ToWString();
                    return LikesProgram::Time::FormatTime(tp, spec.GetTypeExpand()).ToWString();
                }
            }
            catch (...) {
                return std::nullopt;
            }

            return std::nullopt;
        }

        std::optional<String> FormatInternal::TryInvokeNamedFormatter(const std::string& name, const Any& a, const FormatSpec& spec) const {
            if (m_impl->m_nameFormatterCount.load(std::memory_order_acquire) == 0) return std::nullopt;
            UserFormatter formatter; // 命名 formatter 的可调用副本
            {
                std::shared_lock lock(m_impl->m_mutex);
                auto it = m_impl->m_nameFormatters.find(name); // 命名 formatter 查找结果
                if (it == m_impl->m_nameFormatters.end()) return std::nullopt;
                formatter = it->second;
            }

            try {
                return formatter(a, spec);
            }
            catch (...) {
                return std::nullopt;
            }
        }

        std::optional<String> FormatInternal::TryInvokeTypeFormatter(const std::type_index& ti, const Any& a, const FormatSpec& spec) const {
            if (m_impl->m_typeFormatterCount.load(std::memory_order_acquire) == 0) return std::nullopt;
            UserFormatter formatter; // 类型 formatter 的可调用副本
            {
                std::shared_lock lock(m_impl->m_mutex);
                auto it = m_impl->m_typeFormatters.find(ti); // 类型 formatter 查找结果
                if (it == m_impl->m_typeFormatters.end()) return std::nullopt;
                formatter = it->second;
            }

            try {
                return formatter(a, spec);
            }
            catch (...) {
                return std::nullopt;
            }
        }

        std::wstring FormatInternal::ApplyAlignmentAndFill(const std::wstring& src, const FormatSpec& spec) const {
            const int width = spec.GetWidth().value_or(0); // 目标最小 code point 宽度
            if (width <= 0) return src;

            const size_t srcLen = WStringCodePointCount(src); // 源文本 code point 数
            if (srcLen >= static_cast<size_t>(width)) return src;

            const size_t padUnits = static_cast<size_t>(width) - srcLen; // 需要补齐的 code point 数
            std::wstring fill = spec.GetFill().ToWString();             // 填充文本，可为多 code point
            if (fill.empty()) fill = L" ";

            auto appendFill = [&](std::wstring& out, size_t count) { // 按 code point 数追加填充文本
                if (count == 0) return;
                if (fill.size() == 1) {
                    out.append(count, fill[0]);
                }
                else {
                    out += RepeatFillToLen(fill, count);
                }
            };

            if (spec.GetAlign() == U'=') {
                const size_t prefixLen = NumericPrefixLength(src); // 数字符号和进制前缀长度
                std::wstring out; // 符号/前缀后填充的输出缓冲
                out.reserve(src.size() + padUnits * fill.size());
                out.append(src, 0, prefixLen);
                appendFill(out, padUnits);
                // '=' 对齐把填充放在符号/前缀之后、主体之前。
                out.append(src, prefixLen, std::wstring::npos);
                return out;
            }

            switch (spec.GetAlign()) {
            case U'<': {
                std::wstring out; // 左对齐输出缓冲
                out.reserve(src.size() + padUnits * fill.size());
                out += src;
                appendFill(out, padUnits);
                // 左对齐先写原文，再补右侧填充。
                return out;
            }
            case U'^': {
                const size_t leftPad = padUnits / 2;       // 居中对齐左侧填充数
                const size_t rightPad = padUnits - leftPad; // 居中对齐右侧填充数
                std::wstring out; // 居中对齐输出缓冲
                out.reserve(src.size() + padUnits * fill.size());
                appendFill(out, leftPad);
                out += src;
                // 居中对齐将奇数填充放到右侧。
                appendFill(out, rightPad);
                return out;
            }
            case U'>':
            default: {
                std::wstring out; // 右对齐输出缓冲
                out.reserve(src.size() + padUnits * fill.size());
                appendFill(out, padUnits);
                out += src;
                // 右对齐先写填充，再写原文。
                return out;
            }
            }
        }

        std::wstring FormatInternal::RepeatFillToLen(const std::wstring& fill, size_t count) {
            if (count == 0) return std::wstring();
            if (fill.empty()) return std::wstring(count, L' ');

            std::wstring out; // 重复填充生成的宽字符串
            out.reserve(count);
            while (out.size() < count) {
                out += fill;
            }
            if (out.size() > count) {
                out.resize(count);
#if WCHAR_MAX == 0xFFFF
                if (!out.empty() && out.back() >= 0xD800 && out.back() <= 0xDBFF) {
                    out.pop_back();
                }
#endif
            }
            return out;
        }
    }
}
