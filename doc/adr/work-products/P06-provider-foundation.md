# P6-W1 Result / Error / ProviderBase 基础（完成记录）

## 输入与范围

- P5 已收口 composition root、业务 locator 和 Controller use-case 边界；
- 阶段 6 的首项要求先建立统一的 `Result/Error/Deadline/CancellationToken` 与薄
  `ProviderBase`，再按 chayns、retool 两条真实调用链迁移；
- ADR-05 要求 `Result<T>` 支持 move-only、`T == Error`、`void`、错误态显式失败和
  `[[nodiscard]]`；ADR-07 要求生产 Provider 必须经 final NVI 边界。

本工作项**只交付基础契约**。现有 `IProviderRegistry`、`APIinterface`、chayns/retool 的
`generate(session_st&)` 没有伪装成已迁移的 adapter；它们将在 P6-W2/P6-W3 按真实垂直切片替换，
避免形成不能删除的双轨。

## 设计与实现

### 平台错误与结果

- 新增 `platform/result/{ErrorCode,Error,Result}.h`：`Error` 将安全消息、上游原始
  `providerCode`、上游实际 HTTP status 和只供诊断的 `detail` 分开；`ErrorEvent` 仍是独立
  观测模型。
- `Result<T>` 以 `std::variant<Value, Error>` 保存成功包装，从而同时支持 move-only `T` 与
  `T == Error`；`Result<void>` 有对应特化。对错误态取 `value()` 或对成功态取 `error()` 会抛
  `std::logic_error`，不会返回伪默认值。
- 旧 `error::ErrorCode/AppError` 和 `generation::ErrorCode` 改为 platform alias，HTTP/string
  映射只委托给 `platform::defaultHttpStatus/errorCodeName`。legacy `ProviderErrorCode` 保留为
  Provider 内部细分类，通过 `toPlatformError()` 投影到跨层 `Error`。
- 新增绝对 `platform::Deadline` helper；`CancellationSource::token()` 返回只能观察、不能
  `request()` 的共享状态 `CancellationToken`。token 可在 source 析构后安全读取。

### 新 Provider 契约与 NVI

```text
caller
  -> IChatProvider::generate(ProviderRequest, ProviderCallContext)
     -> ProviderBase::generate(...) final
        -> cancelled / deadline preflight -> Result<ProviderResponse>::failure(Error)
        -> doGenerate(...)                 -> success Result 原样返回
        -> failed Result                   -> 校验 Error、单次 FailureObserver 上报
        -> infrastructure exception        -> Internal Error、单次上报
```

- 新增 JSON/Drogon-free 的 `ProviderRequest`、`ProviderResponse`、`ProviderCapabilities`、
  `ProviderCallContext` 与 `IChatProvider`。请求不含 `session_st`，失败不嵌入 response。
- `ProviderCallContext` 持有只读 cancellation view 与共享绝对 deadline；sink/event 接入留给
  P6-W2/P7 的真实 generation 流程，避免本项制造只会转发的空接口。
- `ProviderBase::generate()` 是 `final` NVI，只做 cancellation/deadline 前置检查、异常转换、
  失败结果合法性、单次失败上报；它不保存请求状态，也不规定 HTTP、重试、轮询或 SSE 流程。
- `makeProductionProvider<T>()` 在 C++17 用 `static_assert(std::is_base_of_v<ProviderBase, T>)`
  约束未来生产构造；测试 fake 可以直接实现 `IChatProvider`。

为允许 domain 的新契约使用 platform 基础值对象，`layer-rules.json` 显式允许 ADR-01/02 已批准的
目标态 `domain -> platform`；这不是对具体 IO/codec 的豁免，后者仍会被 layer gate 拒绝。

## 测试与门禁

实际执行结果：

```text
cmake -S . -B build -DCMAKE_BUILD_TYPE=Debug    PASS
cmake --build build -j1                         PASS
ctest --test-dir build --output-on-failure      PASS 385/385
build/src/test/aiapi_test                       PASS 385 cases / 1995 assertions
```

新增的回归覆盖：

- `Result` 成功/失败载荷、move-only 值、`Result<Error>`、错误状态访问和 `Result<void>`；
- ErrorCode/HTTP 映射及 legacy `ProviderError` 的跨层投影；
- token 只读性与 source 析构后的状态安全；
- ProviderBase 成功 NVI、取消/过期前置拒绝、失败一次上报和异常转换；
- `tools/arch/check_provider_foundation.py` 检查契约、无 JSON/Drogon/legacy session 泄漏、final NVI、
  生产继承约束及测试注册。它还以 C++17 `-fsyntax-only -Werror=unused-result` 正/反编译 probe 验证
  `Result` 未被丢弃。

全量架构门禁（architecture audit、cycle/layer/db、严格 test registration、source ownership、
include、target DAG、enqueue、AppContext、shutdown deadline、全部 P5 门禁和 P6 foundation gate）均通过。
P6 selftest 临时移除泛型 `Result` 的 `[[nodiscard]]` 后，foundation gate 如预期返回 rc=4；文件随后
逐字节恢复。

## 遗留与下一步

- 运行期 registry 仍返回 `APIinterface`；chayns/retool 仍有 `session_st` 副作用。这些是 P6-W2 和
  P6-W3 的显式验收对象，不得因本基础包存在而误标完成。
- ProviderCallContext 的 event sink、HTTP timeout/polling cancellation 以及唯一 transport Error 出口
  必须随真实 provider/application/transport slice 接通；本项不复制 legacy 流程。
- `ProviderErrorCode` 的具体分类只允许留在 legacy/provider 内部，新的 port 出口必须使用
  `platform::Error`。

## 回滚

代码回滚必须成组撤回平台契约、legacy alias、ProviderBase/CMake、测试和 P6 gate，并把
`domain -> platform` layer rule 一并恢复；不得只删 ProviderBase 或只恢复旧 ErrorCode，否则会留下
同名错误模型或未受保护的 provider 构造路径。没有数据 schema、配置或上游状态变更，因此无数据
恢复步骤。
