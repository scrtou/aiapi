# C4 / R4: 模块依赖环检测门禁（阶段 0.7 第一项）
#
# 判据：#include 的头文件**基名** -> 该头文件在 src/ 下的顶层目录。
# 之所以用基名而非 include 路径前缀：v2.3 曾用路径前缀判定模块归属，测出 0 个环 —— 那是错的。
# 仓库里 200 处 include 只写文件名，路径前缀判据完全看不见它们。
# 基名判据成立的前提是「头文件名全库唯一」，本脚本先自检该前提，不成立直接失败。
#
# 退出码：0 = 通过；1 = 存在超出基线的环；2 = 前提被破坏（存在跨目录同名头文件）；
#         3 = 分层边界被破坏（--layer-rules）；
#         4 = 新增了业务层对 dbManager 的直接 include（--db-ratchet）。
import argparse
import io
import json
import os
import sys
import re
from collections import defaultdict

INCLUDE_RE = re.compile(r'^\s*#\s*include\s*[<"]([^>"]+)[>"]')
HEADER_EXT = ('.h', '.hpp', '.hh', '.hxx')
SOURCE_EXT = HEADER_EXT + ('.cpp', '.cc', '.cxx', '.c')


def walk_files(root, skip_dirs):
    for dirpath, dirnames, filenames in os.walk(root):
        dirnames[:] = [d for d in dirnames if d not in skip_dirs]
        for name in filenames:
            if name.endswith(SOURCE_EXT):
                yield os.path.join(dirpath, name)


def top_module(path, root):
    parts = os.path.relpath(path, root).split(os.sep)
    return parts[0] if len(parts) > 1 else '<root>'


def build_header_index(root, skip_dirs):
    # 返回 (基名 -> 模块, 重名清单)
    index = {}
    dups = defaultdict(set)
    for path in walk_files(root, skip_dirs):
        if not path.endswith(HEADER_EXT):
            continue
        base = os.path.basename(path)
        mod = top_module(path, root)
        if base in index and index[base] != mod:
            dups[base].update({index[base], mod})
        index[base] = mod
    return index, dups


def build_graph(root, skip_dirs, header_index):
    # 返回 edges: {src_mod: {dst_mod: [file:line 证据, ...]}}
    edges = defaultdict(lambda: defaultdict(list))
    for path in walk_files(root, skip_dirs):
        src = top_module(path, root)
        try:
            with io.open(path, encoding='utf-8', errors='ignore') as fh:
                lines = fh.readlines()
        except OSError:
            continue
        for lineno, line in enumerate(lines, 1):
            m = INCLUDE_RE.match(line)
            if not m:
                continue
            dst = header_index.get(os.path.basename(m.group(1)))
            if dst and dst != src:
                edges[src][dst].append('%s:%d -> %s' % (
                    os.path.relpath(path, root), lineno, m.group(1)))
    return edges


def tarjan(edges):
    # 迭代版 Tarjan，返回节点数 > 1 的强连通分量
    index, low, on_stack = {}, {}, {}
    stack, counter, result = [], [0], []
    nodes = set(edges) | {d for s in edges for d in edges[s]}
    for start in sorted(nodes):
        if start in index:
            continue
        index[start] = low[start] = counter[0]
        counter[0] += 1
        stack.append(start)
        on_stack[start] = True
        work = [(start, iter(sorted(edges.get(start, ()))))]
        while work:
            node, it = work[-1]
            advanced = False
            for nxt in it:
                if nxt not in index:
                    index[nxt] = low[nxt] = counter[0]
                    counter[0] += 1
                    stack.append(nxt)
                    on_stack[nxt] = True
                    work.append((nxt, iter(sorted(edges.get(nxt, ())))))
                    advanced = True
                    break
                if on_stack.get(nxt):
                    low[node] = min(low[node], index[nxt])
            if advanced:
                continue
            work.pop()
            if work:
                parent = work[-1][0]
                low[parent] = min(low[parent], low[node])
            if low[node] == index[node]:
                comp = []
                while True:
                    w = stack.pop()
                    on_stack[w] = False
                    comp.append(w)
                    if w == node:
                        break
                if len(comp) > 1:
                    result.append(sorted(comp))
    return sorted(result, key=lambda c: (-len(c), c))


def bidirectional(edges):
    pairs = set()
    for a in edges:
        for b in edges[a]:
            if a in edges.get(b, {}) and (b, a) not in pairs:
                pairs.add((a, b))
    return sorted(pairs)


def show_evidence(edges, a, b, indent):
    for ev in edges[a][b][:3]:
        print('%s%s' % (indent, ev))
    for ev in edges[b][a][:3]:
        print('%s%s' % (indent, ev))


def scan_db_includes(root, skip_dirs, header_index, target='dbManager'):
    """扫描 target 模块之外、直接 include target 头文件的源文件。

    粒度是**文件**不是模块：模块粒度下，一个已在白名单的模块可以无限追加
    新的 include 而不被发现，棘轮就形同虚设。
    返回 {相对路径: [证据行, ...]}。
    """
    hits = {}
    for path in walk_files(root, skip_dirs):
        src = top_module(path, root)
        if src == target:
            continue
        try:
            with io.open(path, encoding='utf-8', errors='ignore') as fh:
                lines = fh.readlines()
        except OSError:
            continue
        for lineno, line in enumerate(lines, 1):
            m = INCLUDE_RE.match(line)
            if not m:
                continue
            if header_index.get(os.path.basename(m.group(1))) == target:
                rel = os.path.relpath(path, root).replace(os.sep, '/')
                hits.setdefault(rel, []).append('%s:%d -> %s' % (rel, lineno, m.group(1)))
    return hits


def check_db_ratchet(path, hits, root):
    """棘轮判定。返回退出码（0 通过 / 4 新增违规）。"""
    with io.open(path, encoding='utf-8') as fh:
        spec = json.load(fh)
    allowed = set(spec.get('allowed_files', []))
    must_clean = sorted(spec.get('must_stay_clean', []))

    actual = set(hits)
    added = sorted(actual - allowed)
    removed = sorted(allowed - actual)

    print('')
    print('== dbManager 直接依赖棘轮（%s）==' % path)
    print('  冻结 %d 个文件，当前实际 %d 个' % (len(allowed), len(actual)))

    failed = False
    for rel in added:
        failed = True
        print('  FAIL 新增直连 dbManager: %s' % rel)
        for ev in hits[rel][:3]:
            print('         %s' % ev)

    # 已倒置的模块必须保持零直连，否则等于把缝隙又焊死了。
    for mod in must_clean:
        dirty = sorted(r for r in actual if r.split('/')[0] == mod)
        if dirty:
            failed = True
            print('  FAIL %s 应保持零直连（已完成依赖倒置），却出现: %s'
                  % (mod, ', '.join(dirty)))
        else:
            print('  OK   %s 保持零直连' % mod)

    if failed:
        print('')
        print('  业务层要访问数据，请经 domain/port 下的接口注入，'
              '照 IRetoolWorkspaceStore 的做法。')
        print('  确需放宽，请显式改 %s 并在提交信息里说明理由。' % path)
        return 4

    print('  PASS 无新增直连')
    if removed:
        print('  改善 %d 个文件已解除直连: %s' % (len(removed), ', '.join(removed)))
        print('  记得收紧棘轮：--write-db-ratchet')
    return 0


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument('--root', default='src')
    ap.add_argument('--skip', nargs='*', default=['test', 'build', 'third_party'])
    ap.add_argument('--baseline', help='基线 JSON；实际环必须是基线的子集')
    ap.add_argument('--write-baseline', action='store_true')
    ap.add_argument('--layer-rules',
                    help='分层边界规则 JSON；模块出边必须是 allow_out 的子集')
    ap.add_argument('--evidence', action='store_true', help='打印每条边的 file:line 证据')
    ap.add_argument('--db-ratchet',
                    help='dbManager 直接依赖棘轮 JSON；新增直连即 FAIL(4)')
    ap.add_argument('--write-db-ratchet', action='store_true',
                    help='按当前现状写入/收紧 --db-ratchet 清单')
    args = ap.parse_args()

    if not os.path.isdir(args.root):
        print('ERROR: root 不存在: %s' % args.root)
        return 2
    skip = set(args.skip)

    header_index, dups = build_header_index(args.root, skip)
    print('== 前提自检：头文件名全库唯一 ==')
    if dups:
        print('  FAIL 跨目录同名头文件 %d 个，基名判据失效：' % len(dups))
        for base in sorted(dups):
            print('    %s  出现于 %s' % (base, ', '.join(sorted(dups[base]))))
        print('  ADR-03 的机械改写脚本在此状态下会静默改错目标，必须先改名。')
        return 2
    print('  OK 重名数 = 0，共 %d 个头文件' % len(header_index))

    edges = build_graph(args.root, skip, header_index)
    sccs = tarjan(edges)
    bidir = bidirectional(edges)

    print('')
    print('== 双向边 %d 条 ==' % len(bidir))
    for a, b in bidir:
        print('  %s <--> %s' % (a, b))
        if args.evidence:
            show_evidence(edges, a, b, '      ')

    print('')
    print('== 强连通分量（真正的环）%d 个 ==' % len(sccs))
    for comp in sccs:
        print('  {%s}  n=%d' % (', '.join(comp), len(comp)))

    # 分层边界检查：环检测看不见「中立层被污染」——那不构成环。
    # ADR-01 允许 domain -> platform；任何超出 layer-rules 的出边仍会破坏
    # 内侧不依赖具体适配器的方向约束，必须单独断言。
    if args.layer_rules:
        with io.open(args.layer_rules, encoding='utf-8') as fh:
            rules = json.load(fh)
        layer_bad = []
        print('')
        print('== 分层边界（%s）==' % args.layer_rules)
        for mod, spec in sorted(rules.get('modules', {}).items()):
            allow = set(spec.get('allow_out', []))
            actual = set(edges.get(mod, {}))
            bad = sorted(actual - allow)
            if bad:
                layer_bad.append(mod)
                print('  FAIL %s 越界出边 -> %s（允许: %s）'
                      % (mod, ', '.join(bad), sorted(allow) or '无'))
                for d in bad:
                    for ev in edges[mod][d][:3]:
                        print('         %s' % ev)
            else:
                print('  OK   %s 出边 %s，未越界'
                      % (mod, sorted(actual) or '(空)'))
        if layer_bad:
            print('')
            print('  中立层被污染：%d 个模块越界。这不构成环，环检测永远发现不了。'
                  % len(layer_bad))
            return 3

    # dbManager 直接依赖棘轮：环检测与分层检查都看不见它。
    # accountManager -> dbManager 是单向边，不成环；模块也已在 layer-rules 白名单里。
    # 但"白名单模块内部无限追加 include"正是倒置成果被悄悄侵蚀的路径。
    if args.db_ratchet or args.write_db_ratchet:
        hits = scan_db_includes(args.root, skip, header_index)
        target = args.db_ratchet or 'tools/arch/db-include-ratchet.json'
        if args.write_db_ratchet:
            payload = {
                '_comment': ('业务层直接 include dbManager 的文件白名单（棘轮）。'
                             '新增即 FAIL(4)。冻结现状，不主张现状即目标态。'),
                'allowed_files': sorted(hits),
                'must_stay_clean': ['retoolWorkspace'],
                '_note': ('must_stay_clean 列出已完成依赖倒置的模块，必须保持零直连。'
                          '每完成一个模块的倒置，把它从 allowed_files 移除并加入此列表。'),
            }
            parent = os.path.dirname(target)
            if parent:
                os.makedirs(parent, exist_ok=True)
            with io.open(target, 'w', encoding='utf-8') as fh:
                fh.write(json.dumps(payload, indent=2, ensure_ascii=False))
                fh.write('\n')
            print('')
            print('已写入 dbManager 棘轮: %s（%d 个文件）' % (target, len(hits)))
        else:
            rc = check_db_ratchet(target, hits, args.root)
            if rc:
                return rc

    current = {'sccs': [sorted(c) for c in sccs],
               'bidirectional': [list(p) for p in bidir]}

    if args.write_baseline:
        target = args.baseline or 'tools/arch/cycles-baseline.json'
        parent = os.path.dirname(target)
        if parent:
            os.makedirs(parent, exist_ok=True)
        with io.open(target, 'w', encoding='utf-8') as fh:
            fh.write(json.dumps(current, indent=2, ensure_ascii=False))
            fh.write('\n')
        print('')
        print('已写入基线: %s' % target)
        return 0

    if not args.baseline:
        print('')
        print('未指定 --baseline，仅报告，不做门禁判定。')
        return 0

    with io.open(args.baseline, encoding='utf-8') as fh:
        base = json.load(fh)
    allowed_sccs = {tuple(sorted(c)) for c in base.get('sccs', [])}
    allowed_bidir = {tuple(p) for p in base.get('bidirectional', [])}

    # 判据：SCC 只要被某个基线 SCC 覆盖（子集）就是改善，放行。
    # 用等值比较会把「环缩小」误判为「新增环」——环从 n=9 缩到 n=4 时
    # 元组不再相等，假阳性 FAIL。真正的回归特征是出现基线覆盖不到的成员。
    allowed_sets = [set(c) for c in allowed_sccs]
    new_sccs = [c for c in sccs
                if not any(set(c) <= a for a in allowed_sets)]
    new_bidir = [p for p in bidir if p not in allowed_bidir]

    print('')
    print('== 门禁判定（基线: %s）==' % args.baseline)
    if new_sccs or new_bidir:
        for c in new_sccs:
            print('  FAIL 新增环: {%s}' % ', '.join(c))
        for a, b in new_bidir:
            print('  FAIL 新增双向边: %s <--> %s' % (a, b))
            show_evidence(edges, a, b, '        ')
        print('  依赖环不会自己消失，只会长回来 —— 请修复后再提交。')
        return 1

    kept_sccs = len([c for c in sccs if tuple(c) in allowed_sccs])
    shrunk = [c for c in sccs
              if tuple(c) not in allowed_sccs
              and any(set(c) < a for a in allowed_sets)]
    for c in shrunk:
        print('  改善 环已缩小: {%s}  n=%d（基线中它属于更大的环）' % (', '.join(c), len(c)))
    kept_bidir = len([p for p in bidir if p in allowed_bidir])
    print('  PASS 无新增环')
    if len(allowed_sccs) > kept_sccs or len(allowed_bidir) > kept_bidir:
        print('  已修复 %d 个环 / %d 条双向边 —— 记得收紧基线：--write-baseline' % (
            len(allowed_sccs) - kept_sccs, len(allowed_bidir) - kept_bidir))
    return 0


if __name__ == '__main__':
    sys.exit(main())
