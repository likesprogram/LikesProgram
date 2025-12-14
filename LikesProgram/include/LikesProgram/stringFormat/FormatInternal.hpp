#pragma once
#include "../system/LikesProgramLibExport.hpp"
#include "../String.hpp"
#include "FormatParser.hpp"
#include "FormatSpec.hpp"
#include <any>
#include <functional>
#include <optional>
#include <shared_mutex>
#include <string>
#include <typeindex>
#include <unordered_map>
#include <vector>

namespace LikesProgram {
    namespace StringFormat {
        using UserFormatter = std::function<String(const Any&, const FormatSpec&)>;
        class LIKESPROGRAM_API FormatInternal {
        public:
            // 公开构造函数，以同时支持 单例模式 和 多例模式
            FormatInternal();
            ~FormatInternal();
            // 拷贝构造 & 拷贝赋值
            FormatInternal(const FormatInternal&);
            FormatInternal& operator=(const FormatInternal&);
            // 移动构造 & 移动赋值
            FormatInternal(FormatInternal&&) noexcept;
            FormatInternal& operator=(FormatInternal&&) noexcept;

            // 获取单例实例
            static FormatInternal& Instance();

            // 注册按名称的格式化器 (用于 {:uName} 语法)
            // 注册格式化函数
            void RegisterFormatter(const std::string& name, UserFormatter func);
            // 移除按名称的格式化器
            void UnregisterFormatter(const std::string& name);
            // 查询按名称的格式化器是否存在
            bool HasFormatter(const std::string& name) const;

            // 注册按类型的格式化器（当参数类型匹配时自动调用）
            template<typename T, typename F>
            void RegisterFormatterForType(F&& func) {
                UserFormatter wrapper = [fn = std::forward<F>(func)](const Any& a, const FormatSpec& spec) -> String {
                    try {
                        // 尝试转换为T（const T&）。如果失败，返回回退字符串
                        return fn(std::any_cast<const T&>(a), spec);
                    }
                    catch (...) {
                        // 类型转换失败则返回错误占位符
                        return String(U"{!type}");
                    }
                    };
                std::unique_lock lock(m_impl->m_mutex);
                m_impl->m_typeFormatters[std::type_index(typeid(T))] = std::move(wrapper);
            }

            // 移除按类型的格式化器
            template<typename T>
            void UnregisterFormatterForType() {
                std::unique_lock lock(m_impl->m_mutex);
                m_impl->m_typeFormatters.erase(std::type_index(typeid(T)));
            }

            // 格式化
            String FormatAny(const String& fmt, const std::vector<Any>& args);

        private: // 内部实现函数

            // 将一组参数（any）和一个 FormatSpec 渲染为 String
            String FormatArgument(const Any* argPtr, const FormatSpec& spec) const;

            // 内建类型格式化（返回 std::nullopt 表示不处理）
            std::optional<std::wstring> FormatBuiltInToStdString(const Any& a, const FormatSpec& spec) const;

            // 调用 name 注册表
            std::optional<String> TryInvokeNamedFormatter(const std::string& name, const Any& a, const FormatSpec& spec) const;

            // 调用 typeid 注册表
            std::optional<String> TryInvokeTypeFormatter(const std::type_index& ti, const Any& a, const FormatSpec& spec) const;

            // 对结果应用对齐/填充/宽度（result is UTF-8 std::wstring）
            std::wstring ApplyAlignmentAndFill(const std::wstring& src, const FormatSpec& spec) const;

            // 尝试通过构造函数将 any 转换为 String
            template<typename T>
            String TryStringConstructor(const Any& a) {
                try {
                    return String(std::any_cast<T>(a));
                }
                catch (...) {}
                return String(); // 失败返回空 String 或占位符
            }

            // 对齐与填充处理
            static std::wstring RepeatFillToLen(const std::wstring& fill, size_t count);

        private:
            struct FormatInternalImpl {
                mutable std::shared_mutex m_mutex;
                std::unordered_map<std::string, UserFormatter> m_nameFormatters;
                std::unordered_map<std::type_index, UserFormatter> m_typeFormatters;
            };
            FormatInternalImpl* m_impl = nullptr;
        };

    }
}
