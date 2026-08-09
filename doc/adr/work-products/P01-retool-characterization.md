# P1-W3 · Retool workflow/agent characterization

| 项 | 值 |
|---|---|
| 状态 | DOING |
| 前置 | P1-W2 Chayns 离线假上游已完成 |

## 1. 目标

锁定 `retoolapi::requestWorkflow` 与 `retoolapi::requestAgent` 两种生产模式的当前行为：

- URL/path、认证头存在性和 request body 语义；
- workspace 亲和与 usage 计数 acquire/release；
- workflow/agent 成功响应到 `ProviderResult` 的映射；
- 401/429/5xx、超时和无效 JSON 的错误映射；
- 所有 fixture 脱敏、离线、无真实凭据和真实时间依赖。

## 2. 实施顺序

1. 审计 `retoolapi.cpp` 两条模式的真实请求/响应字段和共享副作用；
2. 搜索本地可用录制输入；没有权威录制时只建立合成 characterization，明确标注来源；
3. 引入只覆盖外部 I/O/clock 的窄测试缝，不移动业务策略；
4. 让 `aiapi_test` 编译并执行真实 `retoolapi.cpp`；
5. 生成两种模式的语义 fixture、正常/错误测试和 coverage 证据；
6. 更新本产物后再进入 P1-W4。

## 3. 禁止事项

- 不向真实上游发请求；
- 不提交 HAR、token、cookie、workspace URL 或用户输入；
- 不以 `RetoolWorkspaceManager` port 测试代替 Provider 权威实现；
- 不在本工作项提前改 ProviderBase 或 production target 结构。

## 4. 回滚

测试缝和 fixture 独立提交；默认 production transport 行为必须保持不变。
