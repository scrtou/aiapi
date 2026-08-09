# P0-W1 · 当前真值与基线产物

| 项 | 值 |
|---|---|
| 状态 | DONE |
| 输入 | `src/` 生产头/源文件、`src/test/CMakeLists.txt`、架构门禁脚本 |
| 输出 | `source-audit-2026-08.md`、目标结构/模块/流程文档 |

## 已完成

- 统计 160 个生产头/源文件、模块行数和最大复杂函数；
- 逐模块检查 domain、session、Provider、Controller、DB、账号、队列和 runtime；
- 登记启动、Chat/Responses、账号、thread reaper、停机五条调用链；
- 识别 P0/P1/P2 风险，并确定必须整体重写的组件；
- 新增 ADR-09/10/11，明确 IO、codec 和生产 target 边界；
- `architecture_audit --selftest`、baseline、cycle、layer、223 测试通过。
- 已完成 Chayns 同步 HTTP timeout 修改审查，见
  [`P00-chayns-timeout-review.md`](./P00-chayns-timeout-review.md)。

## 完成证据

1. Chayns timeout 修改已独立提交为 `544bf44`；
2. `audit-baseline.json` 记录 `git.commit=544bf44`、`git.dirty=false`；
3. architecture audit selftest、ratchet、cycle、layer、startup wiring、strict test registration 全部通过；
4. 全量测试 223/223 通过；
5. baseline 检查新增 dirty 元数据负向探针，dirty baseline 返回退出码 2。

## 回滚

本工作包只有文档和审计产物；回滚为恢复文档提交，不触碰业务数据和用户源码修改。
