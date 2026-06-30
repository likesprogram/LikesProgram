#pragma once
#include <LikesProgram/Core/system/LikesProgramCoreExport.hpp>
#include <LikesProgram/Core/String.hpp>
#include <stringFormat/FormatParser.hpp>
#include <stringFormat/FormatSpec.hpp>
#include <any>
#include <functional>
#include <optional>
#include <string>
#include <typeindex>
#include <vector>

namespace LikesProgram {
    namespace StringFormat {
        using UserFormatter = std::function<String(const Any&, const FormatSpec&)>;
        // 格式化运行时核心，维护注册表、解析缓存和内建类型格式化路径。
        class LIKESPROGRAM_CORE_API FormatInternal {
        public:
            // 公开构造函数，以同时支持 单例模式 和 多例模式
            FormatInternal();
            ~FormatInternal();
            // 拷贝会复制注册表和解析缓存状态。
            FormatInternal(const FormatInternal&);
            // 拷贝赋值会替换当前注册表和解析缓存。
            FormatInternal& operator=(const FormatInternal&);
            // 移动会转移内部 PImpl 所有权。
            FormatInternal(FormatInternal&&) noexcept;
            // 移动赋值会接管另一个实例的 PImpl。
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

            // 格式化
            String FormatAny(const String& fmt, const std::vector<Any>& args);
            // 使用参数视图格式化，优先走无 Any 快路径。
            String FormatViews(const String& fmt, const FormatArgView* args, size_t argCount);

        private: // 内部实现函数

            // 获取或构建格式串解析结果，结果由解析缓存持有。
            const FormatParser::Result* GetParsedFormat(const String& fmt);

            // 将一组参数（any）和一个 FormatSpec 渲染为 String
            String FormatArgument(const Any* argPtr, const FormatSpec& spec) const;
            // 将 Any 参数直接格式化为宽字符串，减少中间 String 拼接。
            std::wstring FormatArgumentToWString(const Any* argPtr, const FormatSpec& spec) const;
            // 将参数视图直接格式化为宽字符串，handled 表示是否命中快路径。
            std::wstring FormatViewArgumentToWString(const FormatArgView& arg, const FormatSpec& spec, bool& handled) const;

            // 内建类型格式化（返回 std::nullopt 表示不处理）
            std::optional<std::wstring> FormatBuiltInToStdString(const Any& a, const FormatSpec& spec) const;
            // 内建类型视图快路径，避免为基础类型构造 Any。
            std::optional<std::wstring> FormatBuiltInViewToStdString(const FormatArgView& arg, const FormatSpec& spec) const;

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
            // 私有状态定义在 FormatInternal.cpp，避免头文件暴露容器布局。
            struct FormatInternalImpl;
            FormatInternalImpl* m_impl = nullptr; // 唯一拥有的实现状态
        };

    }
}
