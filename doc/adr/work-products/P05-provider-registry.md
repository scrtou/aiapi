# P5-W1 ProviderRegistry / Router 注入

## 输入

- 阶段 5 要求先替换 `ApiFactory/ApiManager`，再进入 Session/ResponseIndex/Gate 注入；
- P1 characterization 已锁定生成、模型列表、readiness 与 chayns reaper 路径；
- P4 `AppContext/AppWiring` 已成为唯一 composition root。

## 设计与实现

```text
AppContext::build
  -> AppWiring::stepProviderRegistry
     -> construct chaynsapi / retoolapi
     -> ProviderRegistry::registerProvider
     -> ProviderRegistry::freeze
     -> inject IProviderRegistry into Controllers / GenerationService /
        chatSession / chaynsThreadReaper
```

- `IProviderRegistry` 是消费者唯一可见的只读查找端口；
- `infrastructure/provider/ProviderRegistry` 是 runtime 持有的实现，构造期可注册，发布后冻结；
- 两个生产 Provider 由 composition root 显式构造，不再依赖静态初始化、`void*` 工厂或
  whole-archive 恰好保留注册对象；
- `chatSession` 的 transfer/erase 四处调用改走注入端口；Controller、生成服务和 reaper
  不再访问 provider 全局管理器；
- 删除 `ApiFactory.{h,cpp}`、`ApiManager.{h,cpp}` 及 Provider 注册宏/`createApi()` 壳；
- legacy source ceiling 从 39 降至 38。

## 测试与门禁

- ProviderRegistry 查找、缺失、冻结后拒绝晚注册；
- 注入 Registry 的模型列表、readiness、生成 fixture、reaper shutdown 路径；
- `tools/arch/check_provider_registry.py` 禁止恢复旧四个文件、旧符号或双轨接线；
- Debug clean reconfigure/build 通过；`ctest` 338/338 通过；
- architecture audit ratchet、cycle/layer/db、strict test registration、source ownership、
  include、target DAG、enqueue、AppContext、shutdown deadline 与 ProviderRegistry 门禁通过。

## 遗留问题

- `IProviderRegistry` 暂时返回宽 `APIinterface`；按计划在 P6 由 `IChatProvider/IModelCatalog`
  拆分替换，不在 P5 预先复制 Provider 接口；
- `chatSession`、reaper 与部分 Controller 自身仍是 singleton/static 注入形状，分别由
  P5-W2/P5-W3 的 owner/use-case 注入继续收口。

## 回滚

代码回滚应整体恢复旧 Factory/Manager、Provider 注册宏和所有消费者查询路径；不得只恢复
`ApiManager::getInstance()` 形成新旧双轨。数据 schema 未改变，无数据回滚步骤。
