# chaynsThreadReaper TSan 窄范围探针

## 目的
对 `chaynsThreadReaper` 的启停与回收路径做 ThreadSanitizer 竞态检测。
不走完整 aiapi_test（TSan 全量链接过重），只链接 reaper 单文件 + 桩。

## 关键前提（必须知道，否则会得出假阴性结论）
`start()` 会把参数钳到安全下界：`scanIntervalSeconds >= 10`、`idleSeconds >= 60`。
`loop()` 是「先睡后干」：进循环先 `wait_for(scanIntervalSeconds)`，再执行循环体。
=> 任何短于 10s 的场景都**只能**观测到 start/stop 的同步配对，
   `loop()` 的循环体（`runOnce()`）一次都不会执行。
   早期版本的探针正因此得到「0 告警」的假干净结果。

## 场景设计与各自覆盖的目标
| 场景 | 内容 | 覆盖的共享状态 |
|---|---|---|
| A | 300 次 start/stop | `stopRequested_` / `wakeCv_` / `worker_` 同步配对 |
| B | 4 线程 × 200 次并发 `runOnce()` | `options_` 读读、DB 桩 |
| C | `runOnce()` 与 `setEnabled()` 并发 | `chaynsThreadDbManager::enabled_` |
| D | start 后等 13s（跨过 10s 钳制）再并发 stop + runOnce | `loop()` 循环体真正执行 |
| E | `start(Options)` 与 `runOnce()` 并发 | `options_` **读写**并发（A~D 均不覆盖） |

## 探针有效性验证（变异测试）
只有「注入已知 race 能被报出」才能让 orig 的 0 告警具备意义。
见 `results/mutation_matrix.md`：M1 摘除 `optionsMutex_` → 报 `getOptions()`；
M2 在 `runOnce()` 内加无保护 static 计数器 → 报 `runOnce()`。
变异只作用于 `/tmp` 副本，`src/` 全程 md5 校验未变。

## 复现
见 `results/compile_flags.txt`；链接库取自 `build/test_root/src/test/CMakeFiles/aiapi_test.dir/link.txt`。
orig 连跑 5 次均 0 告警（`history_size=7`）。

## 构建纪律（本轮踩坑后固化）

本轮曾三次得出假结论，都源于同一个毛病：结论文案先写好，再让数据往里填。
具体表现与对策：

1. **不要用 `&&` 串联构建与运行，再 `tail` 输出。**
   `cmake --build` 的 configure 阶段失败时，若目录里残留旧二进制，
   直接运行它会打印 `All tests passed`，看起来全绿，实际测的是几天前的代码。
   对策：构建与运行分两条命令，各自单独取 `$?` 并打印。

2. **跑测试前校验二进制时间戳晚于源码改动时间。**
   ```
   stat -c '%y %n' build/test_root/src/test/aiapi_test
   stat -c '%y %n' src/apipoint/chaynsapi/chaynsThreadReaper.cpp
   ```
   前者必须更新。本轮出现过二进制比源码旧两天的情况。

3. **不要在 grep 之后打印写死的断言提示语。**
   形如 `echo '(无输出 = 无依赖)'` 会在 grep 实际有命中时依然打印，
   直接盖过真实结果。对策：让判断依赖实际返回值，或只打印结果本身。

4. **注意 `start()` 对配置的钳制。**
   `scanIntervalSeconds` 被钳到 >=10s、`idleSeconds` 钳到 >=60s，且 `loop()`
   是先睡后干。任何短于 10s 的探针场景都跑不到循环体，只能观测 start/stop
   的同步配对 —— 这正是 M1 最初假阴性的原因。

5. **不要用抽样代替全称判断。**
   「二进制比源码新」是对全部改动文件的全称命题，只查一个文件不能证明它。
   正确做法是遍历 `git status --porcelain` 的全部条目逐一比对 mtime：
   ```
   BIN=build/test_root/src/test/aiapi_test
   BIN_EPOCH=$(stat -c %Y "$BIN")
   for f in $(git status --porcelain | awk '{print $2}'); do
     [ -f "$f" ] || continue
     [ "$(stat -c %Y "$f")" -gt "$BIN_EPOCH" ] && echo "[NEWER] $f"
   done
   ```
   输出为空才算通过。若有命中，不要用「那个改动不影响产物」搪塞——
   `touch` 一个真实源文件强制重编重链，拿到时间戳绝对最新的二进制再跑。
