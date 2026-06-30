# LikesProgramConfig 使用手册

`LikesProgramConfig` 是 LikesProgram 的配置扩展包。它只依赖 `LikesProgramCore`，提供一棵拥有型配置值树，并支持轻量 `key=value`、JSON、YAML、TOML 解析与序列化，以及 `ConfigSchema` 校验和默认值注入。

当前实现不引入第三方 JSON/YAML/TOML 库，适合这些场景：

- 读取服务启动配置。
- 在模块之间传递配置快照。
- 使用 JSON/YAML/TOML 表达嵌套对象、数组和标量。
- 用 dotted path 快速读取常见嵌套字段。
- 在启动阶段校验必填字段、类型约束和默认值。

## 构建和链接

Config 默认不构建。使用前需要在配置 LikesProgram 时显式开启：

```powershell
cd C:\Users\TX2\Desktop\LikesProgramProjects\LikesProgram
cmake -S . -B build-config -DLIKESPROGRAM_BUILD_CONFIG=ON
cmake --build build-config --config Debug
ctest --test-dir build-config -R LikesProgramConfigTests --output-on-failure -C Debug
```

在外部项目中链接：

```cmake
find_package(LikesProgram CONFIG REQUIRED)

add_executable(MyApp main.cpp)
target_link_libraries(MyApp PRIVATE LikesProgram::Config)
```

`LikesProgram::Config` 会传递依赖 `LikesProgram::Core`。

## 最小可运行示例

```cpp
#include <LikesProgram/Config/Config.hpp>

#include <iostream>

int main() {
    auto config = LikesProgram::Config::Configuration::FromJson(
        u"{"
        u"\"service\":{\"name\":\"orders\",\"port\":8080},"
        u"\"feature\":{\"enabled\":true}"
        u"}");

    auto name = config.GetString(u"service.name", u"unknown");
    auto port = config.GetInt64(u"service.port", 80);
    auto enabled = config.GetBool(u"feature.enabled", false);

    std::cout << LikesProgram::String::Format(u"{}:{} enabled={}",
        name, port, enabled).ToStdString() << std::endl;

    return 0;
}
```

## 公开头文件

优先使用聚合头：

```cpp
#include <LikesProgram/Config/Config.hpp>
```

也可以按需包含叶子头：

```cpp
#include <LikesProgram/Config/ConfigValue.hpp>
#include <LikesProgram/Config/ConfigSchema.hpp>
#include <LikesProgram/Config/Configuration.hpp>
```

## 包信息 API

```cpp
#include <LikesProgram/Config/Config.hpp>

const char* name = LikesProgram::Config::PackageName();       // "LikesProgramConfig"
const char* version = LikesProgram::Config::PackageVersion(); // 跟随 LikesProgram 版本
bool ok = LikesProgram::Config::PackageAvailable();           // true 表示已链接到当前进程
```

返回值不需要调用方释放。

## ConfigValue 值树

`ConfigValue` 是通用配置值树节点，支持：

```cpp
LikesProgram::Config::ConfigValueType::Null;
LikesProgram::Config::ConfigValueType::String;
LikesProgram::Config::ConfigValueType::Int64;
LikesProgram::Config::ConfigValueType::Double;
LikesProgram::Config::ConfigValueType::Bool;
LikesProgram::Config::ConfigValueType::Array;
LikesProgram::Config::ConfigValueType::Object;
```

构造标量：

```cpp
LikesProgram::Config::ConfigValue s(u"orders");
LikesProgram::Config::ConfigValue i(8080);
LikesProgram::Config::ConfigValue d(0.75);
LikesProgram::Config::ConfigValue b(true);
```

构造对象和数组：

```cpp
auto service = LikesProgram::Config::ConfigValue::Object();
service.Set(u"name", u"orders");
service.Set(u"port", 8080);

auto labels = LikesProgram::Config::ConfigValue::Array();
labels.PushBack(u"api");
labels.PushBack(u"worker");

service.Set(u"labels", std::move(labels));
```

读取类型：

```cpp
auto type = service.Type();
bool isObject = service.IsObject();
size_t count = service.Size();
auto keys = service.Keys();
```

读取值：

```cpp
auto name = service.Get(u"name").AsString(u"unknown");
auto port = service.Get(u"port").AsInt64(80);
auto firstLabel = service.Get(u"labels").At(0).AsString();
```

`Get(key)` 支持 dotted path：

```cpp
auto root = LikesProgram::Config::ConfigValue::Object();
root.Set(u"service", service);

root.Get(u"service.name").AsString(); // "orders"
```

## Configuration 文档门面

`Configuration` 是面向用户的拥有型配置文档。默认根节点是对象，也可以使用对象、数组或标量作为根节点。

```cpp
LikesProgram::Config::Configuration config;

config.Set(u"service.name", u"orders");
config.Set(u"service.port", u"8080");
config.SetValue(u"feature.enabled", LikesProgram::Config::ConfigValue(true));
```

读取：

```cpp
LikesProgram::String name = config.GetString(u"service.name", u"unknown");
int64_t port = config.GetInt64(u"service.port", 80);
double ratio = config.GetDouble(u"service.ratio", 1.0);
bool enabled = config.GetBool(u"feature.enabled", false);
```

检查、删除、清空：

```cpp
bool hasName = config.Contains(u"service.name");
bool removed = config.Remove(u"service.port");
size_t count = config.Size();
config.Clear();
```

访问或替换根节点：

```cpp
auto root = config.Root();
config.SetRoot(LikesProgram::Config::ConfigValue::Object());
```

兼容别名：

```cpp
LikesProgram::Configuration config; // 等价 LikesProgram::Config::Configuration
```

## key 规则

`Configuration::Set`、`Get`、`Contains`、`Remove` 会修剪 key 前后的 ASCII 空白：

```cpp
config.Set(u" service.port ", u"8080");
config.Contains(u"service.port"); // true
```

规则：

- 空 key 会被忽略。
- key 前后的 `space`、`\t`、`\r`、`\n` 会被去掉。
- key 中间的空白会保留。
- key 匹配区分大小写。
- 重复设置同一个 key 会覆盖旧值。
- dotted path 会逐层读取对象字段。

需要保留带点号的字面 key 时，不要通过 `Configuration::Get("a.b")` 读取；可以先取得对象节点，再对该对象调用 `Get`：

```cpp
auto metadata = config.Root().Get(u"service.metadata");
auto ownerName = metadata.Get(u"owner.name").AsString();
```

## 标量转换

`ConfigValue` 可以按目标类型读取，类型不匹配或解析失败时返回默认值：

```cpp
LikesProgram::Config::ConfigValue(u"16").AsInt64(-1);    // 16
LikesProgram::Config::ConfigValue(u"12x").AsInt64(42);   // 42
LikesProgram::Config::ConfigValue(u"0.75").AsDouble(0.0);// 0.75
LikesProgram::Config::ConfigValue(u"maybe").AsBool(true);// true
```

布尔解析支持：

```text
true false 1 0 yes no on off
```

布尔文本不区分大小写，前后 ASCII 空白会被修剪。

## key=value 文本

解析：

```cpp
LikesProgram::String text =
    u"# service settings\n"
    u"service.name = orders\n"
    u"service.port = 8080\n"
    u"feature.enabled = true\n";

auto config = LikesProgram::Config::Configuration::FromKeyValueLines(text);
```

规则：

- 每行只查找第一个 `=`。
- `=` 左边是 key，右边是 value。
- key 和 value 前后的 ASCII 空白会被去掉。
- 空行会被忽略。
- 修剪后以 `#` 开头的行是注释，会被忽略。
- 没有 `=` 的行会被忽略。
- 空 key 会被忽略。
- 重复 key 以后出现的值为准。
- Unicode 内容会保留。
- 空 value 会保存为空字符串，但 `GetString(key, default)` 会返回默认值。

导出：

```cpp
LikesProgram::String out = config.ToKeyValueLines();
```

导出规则：

- 每个配置项一行，格式为 `key=value\n`。
- 嵌套对象会以 dotted path 展平。
- 不做 JSON/YAML/TOML 转义。
- 不自动排序，通常按写入顺序输出。

## JSON

解析 JSON：

```cpp
auto config = LikesProgram::Config::Configuration::FromJson(
    u"{\"service\":{\"name\":\"orders\",\"port\":8080},\"items\":[\"api\",2,false]}");
```

不抛异常的解析：

```cpp
auto result = LikesProgram::Config::Configuration::TryFromJson(u"{\"a\": [1,}");
if (!result.IsOk()) {
    auto message = result.GetStatus().ToString();
}
```

序列化：

```cpp
LikesProgram::String pretty = config.ToJson(2);
LikesProgram::String compact = config.ToJson(-1);
```

支持对象、数组、字符串、整数、浮点、布尔、null、常见转义和 Unicode surrogate pair。解析错误会携带行列诊断。

## YAML

解析 YAML：

```cpp
auto config = LikesProgram::Config::Configuration::FromYaml(
    u"service:\n"
    u"  name: orders\n"
    u"  port: 8080\n"
    u"items:\n"
    u"  - api\n"
    u"  - worker\n");
```

序列化：

```cpp
LikesProgram::String yaml = config.ToYaml();
```

当前 YAML 支持缩进对象、数组、基础标量和块字符串等服务配置常用形态。它不是完整 YAML 生态实现，不支持 anchors、aliases、tags 等高级特性。缩进不能使用 tab。

## TOML

解析 TOML：

```cpp
auto config = LikesProgram::Config::Configuration::FromToml(
    u"title = \"orders\"\n"
    u"service.port = 8080\n"
    u"[feature]\n"
    u"enabled = true\n"
    u"labels = [\"api\", \"worker\"]\n"
    u"limits = { cpu = 2, memory = \"512Mi\" }\n");
```

读取：

```cpp
config.GetString(u"title");
config.GetInt64(u"service.port");
config.Root().Get(u"feature.labels").At(0).AsString();
config.GetString(u"feature.limits.memory");
```

序列化：

```cpp
LikesProgram::String toml = config.ToToml();
```

当前 TOML 支持 table、dotted key、quoted key、array、inline table、array table 和基础类型。重复 key、标量/table 冲突、裸字符串和非法 Unicode scalar 会返回解析错误。

## 格式往返

`Configuration` 可以在支持的格式之间转换：

```cpp
auto config = LikesProgram::Config::Configuration::FromToml(tomlText);

LikesProgram::String json = config.ToJson(-1);
LikesProgram::String yaml = config.ToYaml();
LikesProgram::String toml = config.ToToml();
```

注意：不同格式的注释、空白、字段排列和某些语法糖不会被保留；往返目标是保留值树语义。

## Schema 校验和默认值

`ConfigSchema` 支持必填字段、可选字段、数组元素约束、类型约束、未知字段控制和默认值注入。

```cpp
auto config = LikesProgram::Config::Configuration::FromJson(
    u"{\"service\":{\"name\":\"orders\",\"port\":8080},\"feature\":{}}\n");

auto serviceSchema = LikesProgram::Config::ConfigSchema::ObjectType()
    .Required(u"name", LikesProgram::Config::ConfigSchema::StringType())
    .Required(u"port", LikesProgram::Config::ConfigSchema::Int64Type())
    .AllowUnknownKeys(false);

auto schema = LikesProgram::Config::ConfigSchema::ObjectType()
    .Required(u"service", serviceSchema)
    .Optional(u"feature", LikesProgram::Config::ConfigSchema::ObjectType())
    .Optional(u"workers", LikesProgram::Config::ConfigSchema::Int64Type(),
        LikesProgram::Config::ConfigValue(4))
    .AllowUnknownKeys(false);

auto validation = config.Validate(schema);
if (!validation.IsOk()) {
    auto report = validation.Report();
}

config.ApplyDefaults(schema);
auto workers = config.GetInt64(u"workers"); // 4
```

Schema 类型：

```cpp
ConfigSchema::Any();
ConfigSchema::NullType();
ConfigSchema::StringType();
ConfigSchema::Int64Type();
ConfigSchema::DoubleType();
ConfigSchema::BoolType();
ConfigSchema::NumberType();
ConfigSchema::ArrayType();
ConfigSchema::ArrayType(itemSchema);
ConfigSchema::ObjectType();
```

`Validate` 会聚合错误，不会遇到第一处错误就停止。`Report()` 返回换行拼接的完整诊断。

## 拷贝、移动和快照

`Configuration` 和 `ConfigValue` 都是值语义：

```cpp
LikesProgram::Config::Configuration a;
a.Set(u"name", u"A");

auto b = a;             // 深拷贝
b.Set(u"name", u"B");   // 不影响 a

auto c = std::move(b);  // 移动
b.Set(u"reuse", u"ok"); // moved-from 对象仍可重新使用
```

适合把配置对象作为快照传给模块：

```cpp
void InitModule(LikesProgram::Config::Configuration config) {
    auto name = config.GetString(u"module.name", u"default");
}
```

## 与 Core String 的关系

所有 key 和文本值都使用 `LikesProgram::String`：

```cpp
LikesProgram::String key(u"service.name");
LikesProgram::String value(u"orders");

config.Set(key, value);
```

这意味着：

- 可以保存 Unicode 文本。
- 读取 `std::string` 前请调用 `ToStdString()`。
- 外部字节流进入配置系统前，应明确按 UTF-8、GBK 等编码构造 `LikesProgram::String`。

## 推荐配置加载模板

如果你从文件读取 UTF-8 文本，可以这样组织：

```cpp
#include <LikesProgram/Config/Config.hpp>

#include <fstream>
#include <sstream>

LikesProgram::Config::Configuration LoadJsonConfigFile(const std::string& path) {
    std::ifstream file(path, std::ios::binary);
    std::ostringstream buffer;
    buffer << file.rdbuf();

    LikesProgram::String text(buffer.str(), LikesProgram::String::Encoding::UTF8);
    return LikesProgram::Config::Configuration::FromJson(text);
}
```

需要错误返回而不是异常时：

```cpp
auto result = LikesProgram::Config::Configuration::TryFromToml(text);
if (!result.IsOk()) {
    return result.GetStatus();
}

auto config = result.MoveValue();
```

## 使用注意事项

`GetString(key, default)` 遇到空字符串会返回默认值。如果你需要区分“存在但为空”和“不存在”，先用 `Contains(key)` 或 `Get(key).Raw()`。

`FromKeyValueLines` 会修剪 value 前后 ASCII 空白。需要保留首尾空格时，建议使用 JSON/YAML/TOML 字符串。

`Configuration::ToToml()` 只对对象根生成 TOML 文档；非对象根会返回空文档。

解析器面向服务配置场景，不处理 include/import、环境变量展开、远程加载或并发读写协调。

`Configuration` 本身不承诺多线程并发写安全。跨线程共享时建议先构造快照，再按值传递或由调用方加锁。

## 更多可运行样例

- `examples/ConfigExample.cpp`：包身份和 key=value 解析基础用法。
- `tests/ConfigPackageTests.cpp`：完整行为测试，覆盖 key=value、JSON、YAML、TOML、Schema、格式往返、错误输入和兼容别名。
- `benchmarks/ConfigBenchmark.cpp`：Release 场景下的配置构建、解析和序列化开销观察样例。
