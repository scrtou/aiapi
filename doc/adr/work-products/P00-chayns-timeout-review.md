# P0-W1 · Chayns 同步 HTTP 超时修改审查

| 项 | 内容 |
|---|---|
| 状态 | DONE |
| 范围 | `ChaynsPollingPolicy.h`、`chaynsapi.cpp`、`test_chayns_model_catalog.cpp` |
| 性质 | P0 基线收口前的已有工作区修改审查，不是 Provider 架构重写 |

## 1. 问题

`kRequestPollingDeadline` 只能约束轮询循环。如果单次同步 `sendRequest` 没有 timeout，调用卡住后
循环无法重新检查五分钟 deadline，worker 仍可能无限占用。

## 2. 修改

- 普通 Chayns 请求统一显式使用 `kUpstreamRequestTimeoutSeconds = 30s`；
- 图片上传使用独立 `kUpstreamUploadTimeoutSeconds = 300s`；
- 补齐最初修改遗漏的两个新 thread 创建/413 降级重试调用；
- 检查 `chaynsapi.cpp` 中全部 9 个 `sendRequest`，均已有显式 timeout；
- 单测约束 timeout 为正、上传 timeout 不小于普通请求、普通请求 timeout 小于轮询总 deadline。

## 3. 调用影响

```text
ChaynsProvider(old chaynsapi)
  -> Drogon HttpClient::sendRequest(request, timeout)
  -> timeout/network result
  -> 既有 ProviderError/session error 分支
```

不改变 HTTP body、headers、账号选择、thread correlation 或重试判据。

## 4. 验证

```text
cmake --build build -j2                                      PASS
ctest -R ChaynsModelCatalog|ChaynsPollingPolicy             7/7 PASS
ctest --test-dir build --output-on-failure                  223/223 PASS
git diff --check                                             PASS
```

当前测试只能锁定 policy 常量和编译调用签名；真实超时/取消行为将在 P1 假上游 contract 中验证。

## 5. 回滚

代码回滚只恢复 `sendRequest(request)` 旧签名和 policy 常量；不涉及数据库、配置或远端数据迁移。
