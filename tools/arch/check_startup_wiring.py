#!/usr/bin/env python3
"""启动接线检查（退出码 4）。

动因：R4 依赖倒置后，Manager 不再自取具体实现，改由 composition root 注入。
注入语句一旦漏写，编译与单元测试全部照常通过（曾真实发生），
但运行期会退化到 Null 实现，静默丢失建表与默认数据。
单元测试无法覆盖此处——它们自己注入 Fake，不经过组装根。
故用静态检查守住：每个已倒置的 Manager 必须在组装根中
出现对应的注入调用，且行号早于其 init。

P04-W2 之后：组装根不再是单个 src/main.cc。初始化流程搬进了
src/runtime/AppWiring.cpp（注册为 AppContext 的步骤序列），main.cc
只保留 config -> build -> run -> shutdown 的编排。

本脚本因此改为在 WIRING_SOURCES 列出的**若干**组装文件中查找，而不是
硬编码单一路径。这不是为了「更灵活」——恰恰相反，是为了让门禁在接线
搬家时**不会静默变成永真或永假**：

  - 搬家前的写法（# 组装根候选文件。顺序即查找优先级；同一条接线只要在其中一个文件里
# 满足「注入早于 init」即判通过。
WIRING_SOURCES = [
    'src/runtime/AppWiring.cpp',
    'src/main.cc',
]）在接线迁出后全部 FAIL，
    是刺眼的假警报，会诱使后来者直接删掉这道门禁；
  - 而若改成「在全仓任意文件里找得到就算数」，则单测里的 Fake 注入
    也会命中，门禁反过来变成永真，比没有更危险。

折中是：候选文件写死成一份短清单，且清单里**一个都不存在**时直接判失败
（见 main() 的前置检查）——路径写错不会伪装成通过。

顺序判据仍是「注入早于 init」，但比较必须发生在**同一文件内**：
跨文件比行号没有意义，两个文件的第 90 行之间不存在先后关系。
"""
import re
import sys

# 组装根候选文件。顺序即查找优先级；同一条接线只要在其中一个文件里
# 满足「注入早于 init」即判通过。
WIRING_SOURCES = [
    'src/runtime/AppWiring.cpp',
    'src/main.cc',
]

# (类名, 注入方法名, 注入正则, init 正则, 说明)：完成依赖倒置、要求 composition
# root 注入的 Manager。已迁移的 Manager 一律以构造参数或局部对象 setter
# 接线；不得靠 getInstance() 补回一个第二构造路径。
# 粒度是「一条接线」而非「一个类」——步骤 101 发现的盲区：
# AccountManager 有两条独立接线(setStore/setChannelStore)，
# 按类名检查时，只要其中一条存在就整体判 OK，另一条被删也不会报警。
REQUIRED = [
    ('ChannelDbManager', 'context-owned construction',
     r'\bauto\s+channelStore\s*=\s*std::make_shared<ChannelDbManager>\s*\(\s*\)',
     r'\bchannels\s*->\s*init\s*\(', 'P5-W3 channel concrete store',
     'ChannelManager 会在未绑定 DB client 的情况下建表，启动会失败或崩溃'),
    ('ChannelManager', 'setStore',
     r'\bchannels\s*->\s*setStore\s*\(',
     r'\bchannels\s*->\s*init\s*\(', 'R4 试点 B',
     '退化为 Null 实现：init 期间的建表与内置渠道写入全部丢失'),
    ('ConfigDbManager', 'context-owned construction',
     r'\bauto\s+configStore\s*=\s*std::make_shared<ConfigDbManager>\s*\(\s*\)',
     r'\baccounts\s*->\s*init\s*\(', 'P5-W3 config concrete store',
     'AccountManager 会在未绑定 DB client 的情况下加载自动化策略，持久化退化为失败'),
    ('AccountDbManager', 'context-owned construction',
     r'\bauto\s+accountStore\s*=\s*std::make_shared<AccountDbManager>\s*\(\s*\)',
     r'\baccounts\s*->\s*init\s*\(', 'P5-W3 account concrete store',
     'AccountManager 会在未绑定 DB client 的情况下建表和加载账号，启动会失败或崩溃'),
    ('AccountBackupDbManager', 'context-owned construction',
     r'\bauto\s+accountBackupStore\s*=\s*std::make_shared<AccountBackupDbManager>\s*\(\s*\)',
     r'\baccounts\s*->\s*init\s*\(', 'P5-W3 account backup concrete store',
     'AccountAdminUseCase 会借用未初始化的备份库，备份/读取路径退化为失败'),
    ('RetoolWorkspaceDbManager', 'context-owned construction',
     r'\bauto\s+workspaceStore\s*=\s*std::make_shared<RetoolWorkspaceDbManager>\s*\(\s*\)',
     r'\bworkspaceManager\s*->\s*init\s*\(', 'P5-W3 workspace concrete store',
     'workspace facade 会在未绑定 DB client 的情况下建表，持久化退化为失败'),
    ('RetoolWorkspaceManager', 'constructor store injection',
     r'\bauto\s+workspaceManager\s*=\s*std::make_shared<RetoolWorkspaceManager>\s*\(\s*workspaceStore\s*\)',
     r'\bworkspaceManager\s*->\s*init\s*\(', 'P5-W3 workspace lifecycle',
     '退化为 Null 实现：建表与默认数据静默丢失'),
    ('AccountManager', 'setStore',
     r'\baccounts\s*->\s*setStore\s*\(',
     r'\baccounts\s*->\s*init\s*\(', 'R4 试点 C',
     '空指针解引用而崩溃（步骤 86 实测）'),
    ('AccountManager', 'setChannelStore',
     r'\baccounts\s*->\s*setChannelStore\s*\(',
     r'\baccounts\s*->\s*init\s*\(', 'R4 试点 C 续·渠道列表',
     '不崩溃：渠道列表恒空、自动补注册静默失效'),
    ('AccountManager', 'setRetoolProvisionClock',
     r'\baccounts\s*->\s*setRetoolProvisionClock\s*\(',
     r'\baccounts\s*->\s*init\s*\(', 'P3-W3·Retool 冷却时钟端口',
     '冷却时钟缺失，Retool 开通节流失效'),
]

# P5-W3：Drogon Controller 不能直接持有 DB/Provider/Account collaborators。
# HealthController 只接收 HealthUseCase；漏接线不会崩溃，却会让 /ready 静默降级，
# 因此仍需在 composition root 静态守住。
REQUIRED_STATIC = [
    ('HealthController', 'setUseCase',
     r'\bHealthController::setUseCase\s*\(\s*healthUseCase\.get\(\)\s*\)',
     'P5-W3 health use case',
     'health use case 未发布，/ready 恒判 not_ready'),
]


def load_sources():
    """读取候选组装文件，返回 [(path, lines)]；不存在的路径跳过。"""
    out = []
    for path in WIRING_SOURCES:
        try:
            with open(path, encoding='utf-8') as f:
                out.append((path, f.read().splitlines()))
        except FileNotFoundError:
            continue
    return out


def line_of(lines, pattern):
    for i, ln in enumerate(lines, 1):
        if re.search(pattern, ln):
            return i
    return None


def locate(sources, inject_pat, init_pat):
    """在候选文件中定位一条接线。

    返回 (path, inject_line, init_line)。init_pat 为 None 表示该接线无 init
    概念（静态 setter）。找不到注入点时返回 (None, None, None)。

    先命中先返回：若同一条接线在多个组装文件中都出现，以 WIRING_SOURCES
    的顺序为准，因为那正是「哪个文件是当前的组装根」的声明。
    """
    for path, lines in sources:
        inject_ln = line_of(lines, inject_pat)
        if inject_ln is None:
            continue
        init_ln = line_of(lines, init_pat) if init_pat else None
        return path, inject_ln, init_ln
    return None, None, None


def main():
    sources = load_sources()
    if not sources:
        print('FAIL 组装根候选文件一个都不存在：%s' % ', '.join(WIRING_SOURCES))
        print('     门禁无法判定。若接线已再次搬家，请同步更新 WIRING_SOURCES。')
        return 4

    print('组装根候选：%s' % ', '.join(path for path, _ in sources))

    failed = False
    for cls, setter, inject_pat, init_pat, note, impact in REQUIRED:
        path, set_ln, init_ln = locate(sources, inject_pat, init_pat)

        if set_ln is None:
            print('FAIL %s.%s(%s): 组装根缺少注入 -> %s' % (cls, setter, note, impact))
            failed = True
            continue
        if init_ln is None:
            print('OK   %s.%s(%s): 已注入(%s:%d)，未见 init 调用' % (cls, setter, note, path, set_ln))
            continue
        if set_ln >= init_ln:
            print('FAIL %s.%s(%s): 注入在 %s:%d，晚于 init 的第 %d 行；'
                  'init 期间的行为 -> %s'
                  % (cls, setter, note, path, set_ln, init_ln, impact))
            failed = True
        else:
            print('OK   %s.%s(%s): 注入(%s:%d) 早于 init(第 %d 行)'
                  % (cls, setter, note, path, set_ln, init_ln))

    for cls, setter, inject_pat, note, impact in REQUIRED_STATIC:
        path, set_ln, _ = locate(sources, inject_pat, None)
        if set_ln is None:
            print('FAIL %s::%s(%s): 组装根缺少注入 -> %s' % (cls, setter, note, impact))
            failed = True
        else:
            print('OK   %s::%s(%s): 已注入(%s:%d)' % (cls, setter, note, path, set_ln))

    return 4 if failed else 0


if __name__ == '__main__':
    sys.exit(main())
