#!/usr/bin/env python3
"""启动接线检查（退出码 4）。

动因：R4 依赖倒置后，Manager 不再自取具体实现，改由 main.cc 注入。
注入语句一旦漏写，编译与单元测试全部照常通过（曾真实发生），
但运行期会退化到 Null 实现，静默丢失建表与默认数据。
单元测试无法覆盖此处——它们自己注入 Fake，不经过 main.cc。
故用静态检查守住：每个已倒置的 Manager 必须在 main.cc 中
出现对应的注入调用，且行号早于其 init。
"""
import re
import sys

MAIN = 'src/main.cc'

# (类名, 说明)：完成依赖倒置、要求 main.cc 注入的 Manager
# (类名, 注入方法名, 说明)：完成依赖倒置、要求 main.cc 注入的接线。
# 粒度是「一条接线」而非「一个类」——步骤 101 发现的盲区：
# AccountManager 有两条独立接线(setStore/setChannelStore)，
# 按类名检查时，只要其中一条存在就整体判 OK，另一条被删也不会报警。
REQUIRED = [
    ('ChannelManager', 'setStore', 'R4 试点 B',
     '退化为 Null 实现：init 期间的建表与内置渠道写入全部丢失'),
    ('RetoolWorkspaceManager', 'setStore', 'R4 试点 A',
     '退化为 Null 实现：建表与默认数据静默丢失'),
    ('AccountManager', 'setStore', 'R4 试点 C',
     '空指针解引用而崩溃（步骤 86 实测）'),
    ('AccountManager', 'setChannelStore', 'R4 试点 C 续·渠道列表',
     '不崩溃：渠道列表恒空、自动补注册静默失效'),
    ('AccountManager', 'setRetoolProvisionClock', 'P3-W3·Retool 冷却时钟端口',
     '冷却时钟缺失，Retool 开通节流失效'),
    # P04：ErrorStatsService::init() 内有 if(!dbManager_) 回退分支，
    # 故漏注入既不崩溃也不是 Null，而是静默绕开端口回落到具体单例。
    ('metrics::ErrorStatsService', 'setSink', 'P04·错误统计落库端口',
     '不崩溃：静默绕开端口，回落到 ErrorStatsDbManager 具体单例'),
]

# 步骤 176：静态 setter 形式的接线（无单例、无 init）。
# HealthController 复用 IAccountStore 端口做 /ready 的库探针；漏注入不崩溃，
# 只会让 /ready 恒报 not_ready —— 正因为静默，更需要门禁守。
REQUIRED_STATIC = [
    ('HealthController', 'setDbProbe', 'R4·/ready 库探针',
     '探针为空，/ready 恒判 not_ready'),
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
    for cls, setter, note, impact in REQUIRED:
        set_ln = line_of(lines, re.escape(cls) + r'::getInstance\(\)\.' + re.escape(setter) + r'\(')
        init_ln = line_of(lines, re.escape(cls) + r'::getInstance\(\)\.init\(')
        if set_ln is None:
            print('FAIL %s.%s(%s): main.cc 缺少注入 -> %s' % (cls, setter, note, impact))
            failed = True
            continue
        if init_ln is None:
            print('OK   %s.%s(%s): 已注入(第 %d 行)，未见 init 调用' % (cls, setter, note, set_ln))
            continue
        if set_ln >= init_ln:
            print('FAIL %s.%s(%s): 注入在第 %d 行，晚于 init 的第 %d 行；'
                  'init 期间的行为 -> %s'
                  % (cls, setter, note, set_ln, init_ln, impact))
            failed = True
        else:
            print('OK   %s.%s(%s): 注入(第 %d 行) 早于 init(第 %d 行)' % (cls, setter, note, set_ln, init_ln))

    for cls, setter, note, impact in REQUIRED_STATIC:
        set_ln = line_of(lines, re.escape(cls) + r'::' + re.escape(setter) + r'\(')
        if set_ln is None:
            print('FAIL %s::%s(%s): main.cc 缺少注入 -> %s' % (cls, setter, note, impact))
            failed = True
        else:
            print('OK   %s::%s(%s): 已注入(第 %d 行)' % (cls, setter, note, set_ln))

    return 4 if failed else 0


if __name__ == '__main__':
    sys.exit(main())
