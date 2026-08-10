# orig 运行摘要

原始 TSan 日志（约 245KB）不入库，可用 README 中的命令复现。

## 编译/运行参数
```
-I/home/vps/code/aiapi/src -isystem /home/vps/code/aiapi/.deps/drogon-install/include -isystem /usr/include/jsoncpp -isystem /usr/include/postgresql -std=c++17
TSAN_OPTIONS=halt_on_error=0 history_size=7 second_deadlock_stack=1
```

## 5 次运行结果
- run#1: TSan 告警=0, 完成场景数=5
- run#2: TSan 告警=0, 完成场景数=5
- run#3: TSan 告警=0, 完成场景数=5
- run#4: TSan 告警=0, 完成场景数=5
- run#5: TSan 告警=0, 完成场景数=5

## run#1 场景标记原文
```
[probe] A done: 300 start/stop (only sync pairing observed)
[probe] B done: 4x200 concurrent runOnce
[probe] C done: runOnce vs setEnabled
[probe] D: waiting 13s for loop body to execute at least once...
[probe] D done: loop body executed + concurrent stop/runOnce
[probe] E done: concurrent start(Options) vs runOnce (options_ RW)
```
