# Legacy 归档说明

`legacy/` 记录首轮 1.0 版本不纳入新架构的旧模块边界。这里不复制旧实现代码，避免未验收能力被误构建、误安装或误发布；需要恢复时，应从历史工程或版本记录中重新评估后迁入正式包。

## 当前归档模块

| 模块 | 处理结果 | 恢复条件 |
| --- | --- | --- |
| `math/Vector` | 不进入 1.0 组件生态 | 明确未来存在独立数学包定位，并补齐 API、测试、benchmark 与 README |
| `math/Vector3` | 不进入 1.0 组件生态 | 与 `Vector` 一起形成稳定数学能力，不作为 Core 顺手工具迁入 |
| `math/Vector4` | 不进入 1.0 组件生态 | 与 `Vector` 一起形成稳定数学能力，不作为 Core 顺手工具迁入 |
| 宽泛 `Math` 聚合模块 | 不进入 1.0 组件生态 | 拆分出清晰功能域后再按扩展包规则评估 |
| 半成品 TLS Context Manager | 不进入 1.0 组件生态 | 若未来需要安全层封装，应作为用户侧或明确扩展包设计，不能让 `LikesProgramNet` 直接依赖 OpenSSL/SChannel/mbedTLS |

## 已替代或收敛的旧能力

- `PercentileSketch` 已收敛为 `LikesProgramMetrics` 私有实现，仅服务 `Summary` 分位数估算，不作为公共 Math API 安装导出。
- TLS/SSL 不作为独立 Net 子包迁移；`LikesProgramNet` 只提供可继承的 `TcpTransport` / `UdpTransport` 扩展点，由应用侧隔离第三方安全库。

## 恢复流程

1. 先在 `docs/progress/TASKS.md` 增加新的阶段任务，说明模块边界、依赖和验收目标。
2. 再按 `packages/<PackageName>/` 结构建立独立包，不能直接把旧目录放入构建。
3. 公开头只放入 `include/`，私有实现放入 `src/` 或 `src/include/<module>/`。
4. 补齐 tests、examples、benchmark、README、安装规则、doctor 或 consumer-check 验收。
5. 通过 Debug/Release 构建、CTest、安装包外部 consumer、依赖扫描和公开/私有头边界扫描后，才能从 legacy 记录中移除。
