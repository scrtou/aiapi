# R4 · 依赖环与模块方向棘轮

## 规则

```bash
python3 tools/arch/check_cycles.py \
  --baseline tools/arch/cycles-baseline.json \
  --layer-rules tools/arch/layer-rules.json
```

同时满足：

1. 不得新增 SCC 或双向边；
2. 每个已登记模块的实际出边必须是 `layer-rules.json` 白名单的子集；
3. 自有头文件 basename 在机械 include 改写前必须唯一；
4. 白名单只能收紧。当前允许边是迁移债务上限，不表示目标架构认可这些依赖。

## 为什么环检查不够

非法单向依赖不形成环，例如 domain include infrastructure 而 infrastructure 不反向 include domain。Tarjan 会 PASS，但端口方向已经损坏。因此 layer rules 是独立强制门。

## v3 模块覆盖

`layer-rules.json` 已登记当前所有有意义的顶层模块，包括高出度的 controllers/apipoint/sessionManager/accountManager；旧版只冻结低出度模块的盲区已关闭。

`<root>` 是 composition root，可以知道具体模块。其它当前白名单随阶段 3～7 迁移逐条收紧，最终由 platform/domain/application/infrastructure/transport target 规则取代。

## 外部依赖边界

当前脚本的模块图只解析仓库自有头文件，不能发现 domain 对 JsonCpp 等外部库的依赖。该债务由 ADR-01 和 migration-plan 阶段 3.4 单独治理；最终 domain target 的外部链接清单和源码禁列共同验收。

## CI 自证

CI 不只运行门禁，还要故意注入非法边并断言脚本以预期退出码失败。无法被反例触发的门禁不具备可信度。
