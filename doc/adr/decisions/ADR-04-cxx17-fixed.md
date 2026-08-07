# ADR-04 固定 C++17，移除标准探测

| 项 | 值 |
|---|---|
| 状态 | 已落地（commit `efb4003`） |
| 来源 | RFC-001 v2.5 §2（原行 230~271），P4 拆分外移 |
| 迁移落点 | 见 [`migration-plan.md`](../migration-plan.md) |
| 数字真值源 | [`architecture-baseline.md`](../architecture-baseline.md) |

---

## 决策与理由

**决策**：明确采用 C++17，删除 `check_include_file_cxx` 探测与 20/17/14 三级降级逻辑。

```cmake
set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)
```

同时**必须**修正 `src/test/CMakeLists.txt` 中的 `set(CMAKE_CXX_STANDARD 20)` 兜底，
改为与主程序一致的 17，或直接删除该兜底、由顶层统一提供。

**理由**：
1. 项目实测已在 C++17（约 146 处 C++17 特性），与现状一致，**零迁移成本**。
2. Drogon 依赖本身要求 `cxx_std_17`。
3. 探测式降级会导致不同机器编译出语义不同的产物（见 §0.4），必须消除。
4. 主程序 17 / 测试 20 的不一致会让单测无法真实反映产物行为，必须统一。

##### 允许使用的特性（明确白名单）

本方案的设计直接建立在 C++17 之上，可放心使用：

| 特性 | 在本方案中的用途 |
|------|------------------|
| `std::optional` | 可空返回值；`ProviderResult::usage` 保持现状 |
| `std::variant` | `Result<T, Error>` 的判别式实现（见 §4.5） |
| `std::string_view` | Provider 分片解析零拷贝（`parseChunk`） |
| `if constexpr` | Pipeline / 编解码的编译期分支 |
| 结构化绑定 | 遍历 map、多返回值解包 |
| 内联变量、折叠表达式、`std::string_view` 字面量 | 通用 |
| `[[nodiscard]]` | **强制**用于 `Result<T>`，防止错误被静默忽略 |

##### 明确禁止（C++20 及以上）

`concepts`、`ranges`、`std::expected`、`std::span`、`coroutine`、designated initializers、
`std::format`、三路比较运算符。

> CI 门禁：编译参数必须包含 `-std=c++17`，且不得出现 `-std=c++20` / `gnu++`。

---
