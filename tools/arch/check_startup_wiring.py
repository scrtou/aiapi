#!/usr/bin/env python3
"""启动接线检查（退出码 4）。

动因：R4 依赖倒置后，Manager 不再自取具体实现，改由 main.cc 注入。
注入语句一旦漏写，编译与单元测试全部照常通过（曾真实发生），
但运行期会退化到 Null 实现，静默丢失建表与默认数据。
单元测试无法覆盖此处——它们自己注入 Fake，不经过 main.cc。
故用静态检查守住：每个已倒置的 Manager 必须在 main.cc 中
出现 setStore，且行号早于其 init。
"""
import re
import sys

MAIN = 'src/main.cc'

# (类名, 说明)：完成依赖倒置、要求 main.cc 注入的 Manager
REQUIRED = [
    ('ChannelManager', 'R4 试点 B'),
    ('RetoolWorkspaceManager', 'R4 试点 A'),
]


def line_of(lines, pattern):
    for i, ln in enumerate(lines, 1):
        if re.search(pattern, ln):
            return i
    return None


def main():
    with open(MAIN, encoding='utf-8') as f:
        lines = f.read().splitlines()

    failed = False
    for cls, note in REQUIRED:
        set_ln = line_of(lines, re.escape(cls) + r'::getInstance\(\)\.setStore\(')
        init_ln = line_of(lines, re.escape(cls) + r'::getInstance\(\)\.init\(')
        if set_ln is None:
            print('FAIL %s(%s): main.cc 缺少 setStore 注入，运行期将退化为 Null 实现' % (cls, note))
            failed = True
            continue
        if init_ln is None:
            print('OK   %s(%s): 已注入(第 %d 行)，未见 init 调用' % (cls, note, set_ln))
            continue
        if set_ln >= init_ln:
            print('FAIL %s(%s): setStore 在第 %d 行，晚于 init 的第 %d 行；'
                  'init 期间的建表与默认数据写入会走 Null 实现'
                  % (cls, note, set_ln, init_ln))
            failed = True
        else:
            print('OK   %s(%s): setStore(第 %d 行) 早于 init(第 %d 行)' % (cls, note, set_ln, init_ln))

    return 4 if failed else 0


if __name__ == '__main__':
    sys.exit(main())
