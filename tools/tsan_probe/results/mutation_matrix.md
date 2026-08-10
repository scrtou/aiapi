| 版本 | 变异内容 | TSan 告警 | SUMMARY 指向 |
|---|---|---|---|
| orig | 无（现网代码） | 0（5/5 次） | — |
| M1 | 摘除 optionsMutex_ 的 2 处 lock_guard | 2 | getOptions() |
| M2 | runOnce() 内加无保护 static 计数器 | 1 | runOnce() |
