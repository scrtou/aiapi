# P1-W2 · Chayns 脱敏 fixture 与假上游

| 项 | 值 |
|---|---|
| 状态 | DONE |
| 前置 | P1-W1 运行时 coverage 基线已完成 |
| 原始输入 | 本地、git ignored 的 `har/*.har`；禁止提交 |

## 1. 本工作项边界

本工作项锁定 Chayns 当前 wire 行为，不改 Provider 抽象，不提前进行 P3 target 拆分：

1. 从本地 HAR 提取 thread create、follow-up message、poll 204/200、read、delete、model；
2. 所有认证头、cookie、真实 person/thread/message/site/workspace ID、姓名、文本和时间戳必须脱敏；
3. fixture 只保存语义 JSON；动态 ID、时间戳和 query 参数归一化；
4. 建立脚本级泄密门禁和 C++ contract 测试；
5. 再引入最窄的 HTTP/clock 测试缝，使用本地假上游覆盖 Chayns 生产权威入口；
6. 最后从 Chat/Responses 流式/非流式四入口运行离线冒烟。

## 2. 已确认的 HAR 流程

只对本地文件做离线解析，没有向任何真实域名发起请求。

| 契约 | 录制行为 |
|---|---|
| 新建 free thread | `POST /intercom-backend/v2/thread?forceCreate=true` → 201 |
| 新建 pro thread | 同一路径，body 额外包含 `workspaceUacId`、`typeId=9` → 201 |
| follow-up message | `POST /intercom-backend/v2/thread/{threadId}/message` → 201 |
| 轮询未完成 | `GET .../{threadId}/message` → 204，无 body |
| 轮询有消息 | `GET .../{threadId}/message` → 200，消息数组 |
| 标为已读 | `PATCH /intercom-backend/v2/thread/read` → 200 |
| 删除成员/thread | `DELETE /intercom-backend/v2/thread/member/delete` → 200 |
| model catalog | `GET /chayns-ai-chatbot/nativeModelChatbot` → 200，模型数组 |

## 3. 计划产物

- `tools/fixtures/extract_chayns_har.py`：显式 allowlist 提取器；
- `src/test/fixtures/chayns/*.json`：可提交的合成标识 fixture；
- fixture 安全检查：禁止 secret header/字段和非占位身份值；
- C++ wire contract tests；
- fake upstream/clock 与生产入口运行证据；
- coverage 报告中 `chaynsapi::generate/postChatMessage` 的执行次数和分支。

## 4. 已完成切片

### 4.1 脱敏 fixture

- `tools/fixtures/extract_chayns_har.py` 使用显式字段 allowlist，完全丢弃原始 header；
- 已生成 8 份 fixture：free/pro create、message、poll 204/200、read、delete、model；
- 身份、thread/message ID、workspace、正文和时间统一换成合成值；
- `--check` 会重新离线提取、执行 JWT/email/secret fragment 扫描并检查漂移；
- `ChaynsFixture_*` 5 个 C++ contract 测试验证 wire 差异、状态码、model 解析和
  reasoning/final 消息关联。

### 4.2 生产路径假上游

引入两个只负责外部边界、不持有业务策略的瘦接口：

```text
chaynsapi
  ├── IChaynsHttpTransport::send(baseUrl, request, timeout)
  └── IChaynsClock::now/sleepFor
```

- production 默认实现仍调用 Drogon `HttpClient` 和 `steady_clock/sleep_for`；
- 测试注入 `FixtureChaynsTransport`，只消费 fixture 队列，任何意外请求立即失败；
- fake transport 不开放 socket，不访问真实域名；
- fake clock 在一次 sleep 后推进到 deadline，不等待 5 分钟墙钟时间；
- ledger DB 只是被测 Provider 的协作者桩，`chaynsapi.cpp`、model、correlation、
  AccountManager 选择逻辑均为真实实现。

`ChaynsProvider_*` 4 个测试已覆盖：

1. free create → 204 → reasoning/final；
2. pro create 的 `typeId=9/workspaceUacId`；
3. continuation 复用 thread 并走 message POST；
4. fake clock 到达 polling deadline 并映射为 504。

另有 4 个离线冒烟从生产 `GenerationService::runGuarded` 进入同一个 Chayns fake
transport，并分别使用 `ChatJsonSink`、`ChatSseSink`、`ResponsesJsonSink`、
`ResponsesSseSink`。这证明四种协议/输出模式不是只测 serializer helper；HTTP Controller
的队列、event-loop 和断连边界仍由 P1-W5 单独验证。

### 4.3 当前运行证据

| 项 | 结果 |
|---|---:|
| 脱敏 fixture | 8 |
| fixture contract | 5/5 PASS |
| 生产 Provider 假上游测试 | 4/4 PASS |
| coverage 全量测试 | 236/236 PASS |
| `chaynsapi::generate` | 9 次；23/27 行；24/48 分支 |
| `chaynsapi::postChatMessage` | 9 次；404/634 行；752/2186 分支 |
| `chaynsapi.cpp` | 543/1003 行；905/3232 分支 |
| `GenerationService::runGuarded` | 四种模式均执行 |
| `GenerationService::emitResultEvents` | 4 次；真实成员实现已执行 |

实际命令：

```bash
python3 tools/fixtures/extract_chayns_har.py --check
ctest --test-dir build -R '^(ChaynsFixture_|ChaynsProvider_)' --output-on-failure
ctest --test-dir build-coverage --output-on-failure
python3 tools/coverage/generate_report.py
```

## 5. 遗留风险与后续归属

- HTTP Controller 本身尚未 instrument；队列/event-loop/断连属于 P1-W5；
- delete/read fixture 已做 wire contract，Provider delete 权威入口的执行留在回收/SIGTERM harness；
- 413、明确拒绝和账号切换的完整生产分支覆盖仍可在 P1-W4 增强，但不影响本工作项的
  HAR wire、clock、success/timeout 和四输出模式门禁；
- 原 HAR 仍可能含真实凭据，只能保留在 git ignored 的本地目录。

P1-W2 所需的脱敏、假时钟、真实 Provider 执行和四输出模式已完成。

## 6. 回滚

fixture 和测试缝将作为独立切片提交。回滚时可删除提取器、合成 fixture 和假上游测试；
不得删除原有 timeout、账号单飞、轮询 deadline 或消息关联保护。
