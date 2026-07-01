#pragma once
#include <optional>
#include <LikesProgram/Core/String.hpp>

namespace LikesProgram {
    namespace StringFormat {
        // 格式化规格 : {[index]:[fill][align][sign][#][0][width][.precision][type][typeExpand]}
        class FormatSpec {
        public:
            // 构造默认规格，等价于 "{}"。
            FormatSpec();
            // 构造带参数索引的规格。
            explicit FormatSpec(int idx, bool explicitIdx = false);
            // 复制规格的 PImpl 内容。
            FormatSpec(const FormatSpec& other);
            // 转移规格的 PImpl 所有权。
            FormatSpec(FormatSpec&& other) noexcept;
            ~FormatSpec();

            // 复制赋值保持异常安全的独立状态。
            FormatSpec& operator=(const FormatSpec& other);
            // 移动赋值转移内部状态。
            FormatSpec& operator=(FormatSpec&& other) noexcept;

            // 重置
            void Reset() noexcept;

            // 获取参数索引
            int GetIndex() const noexcept;

            // 设置参数索引和是否显式指定
            void SetIndex(int idx, bool explicitIdx = false);

            // 判断是否显式指定了参数索引
            bool HasExplicitIndex() const noexcept;

            // 获取填充字符
            const String& GetFill() const noexcept;

            // 设置填充字符
            void SetFill(const String& f);

            // 获取对齐方式（<、>、^）
            char32_t GetAlign() const noexcept;

            // 设置对齐方式
            void SetAlign(char32_t a);

            // 获取符号控制（+、-、空格）
            char32_t GetSign() const noexcept;

            // 设置符号控制
            void SetSign(char32_t s);

            // 是否显示进制前缀（#）
            bool GetAlternateForm() const noexcept;

            // 设置是否显示进制前缀
            void SetAlternateForm(bool show);

            // 是否启用零填充（0）
            bool GetZeroPad() const noexcept;

            // 设置是否启用零填充
            void SetZeroPad(bool zero);

            // 获取最小输出宽度
            std::optional<int> GetWidth() const noexcept;

            // 设置最小输出宽度
            void SetWidth(std::optional<int> w);

            // 获取输出精度（用于浮点或字符串截断）
            std::optional<int> GetPrecision() const noexcept;

            // 设置输出精度
            void SetPrecision(std::optional<int> p);

            // 获取类型标识（如 d, f, x, s, t）
            char32_t GetType() const noexcept;

            // 设置类型标识
            void SetType(char32_t t);

            // 获取类型扩展说明（例如时间模板）
            const String& GetTypeExpand() const noexcept;

            // 设置类型扩展说明
            void SetTypeExpand(const String& expand);

            // 判断当前规格是否合法
            bool IsValid() const noexcept;

            // 设置格式是否有效
            void SetValid(bool v);

            // 获取原始格式字符串
            const String& GetRaw() const noexcept;

            // 设置原始格式字符串
            void SetRaw(const String& raw);

        private:
            // PImpl 保存可变格式字段，避免头文件暴露字段布局。
            struct FormatSpecImpl;
            FormatSpecImpl* m_impl = nullptr; // 唯一拥有的格式规格状态
        };
    }
}
