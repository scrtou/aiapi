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
4. 迁移债务白名单只能收紧。ADR-01/02 已明确的目标态基础边（当前为
   `domain -> platform`）单独标识，不作为债务；其余允许边不表示目标架构认可。

## 为什么环检查不够

非法单向依赖不形成环，例如 domain include infrastructure 而 infrastructure 不反向 include domain。Tarjan 会 PASS，但端口方向已经损坏。因此 layer rules 是独立强制门。

## v4 正式层覆盖

P8-W2 后，`layer-rules.json` 只登记正式 `src/` 顶层层：`platform`、`domain`、
`application`、`infrastructure`、`transport`、`runtime` 与 `<root>`。历史业务目录不再作为
模块边界；`check_physical_layout.py` 会拒绝它们复活。

`<root>` 只代表 `main.cc` 进程入口，可以知道启动所需 concrete adapter。其它白名单与正式
CMake target DAG 一致；同层内部的 persistence 直连另由 file-level db-ratchet（兼容名称）冻结。

## 外部依赖边界

当前脚本的模块图只解析仓库自有头文件，不能发现 domain 对 JsonCpp 等外部库的依赖。该债务由 ADR-01 和 migration-plan 阶段 3.4 单独治理；最终 domain target 的外部链接清单和源码禁列共同验收。

## CI 自证

CI 不只运行门禁，还要故意注入非法边并断言脚本以预期退出码失败。无法被反例触发的门禁不具备可信度。
