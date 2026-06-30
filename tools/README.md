# LikesProgram Tools

`tools` 目录保存可以随发布包交付的命令行工具。默认由
`LIKESPROGRAM_BUILD_TOOLS` 构建。

## likesprogram-doctor

`likesprogram-doctor` 用于验证源码树或安装后的 LikesProgram 包是否能被消费方正常加载和使用。

当前检查内容：

- Core 版本、字符串格式化和 JSON 转义。
- 已链接 Logging 时，验证包身份和异步 Sink 往返。
- 已链接 Config 时，验证包身份、`key=value` 与 JSON 解析。

常用命令：

```powershell
likesprogram-doctor
likesprogram-doctor --format json
likesprogram-doctor --require core --require logging --require config
likesprogram-doctor --require all
```

退出码：

- `0`：检查通过。
- `1`：命令行参数错误。
- `2`：至少一个诊断项失败。
- `3`：出现未预期运行时错误。

安装 LikesProgram 后，也可以把该工具作为外部消费方单独构建：

```powershell
cmake -S tools/likesprogram-doctor -B build-doctor -DCMAKE_PREFIX_PATH=C:\LikesProgramInstall
cmake --build build-doctor --config Release
```
