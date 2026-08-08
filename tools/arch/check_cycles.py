# C4 / R4: 模块依赖环检测门禁（阶段 0.7 第一项）
#
# 判据：#include 的头文件**基名** -> 该头文件在 src/ 下的顶层目录。
# 之所以用基名而非 include 路径前缀：v2.3 曾用路径前缀判定模块归属，测出 0 个环 —— 那是错的。
# 仓库里 200 处 include 只写文件名，路径前缀判据完全看不见它们。
# 基名判据成立的前提是「头文件名全库唯一」，本脚本先自检该前提，不成立直接失败。
#
# 退出码：0 = 通过；1 = 存在超出基线的环；2 = 前提被破坏（存在跨目录同名头文件）。
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


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument('--root', default='src')
    ap.add_argument('--skip', nargs='*', default=['test', 'build', 'third_party'])
    ap.add_argument('--baseline', help='基线 JSON；实际环必须是基线的子集')
    ap.add_argument('--write-baseline', action='store_true')
    ap.add_argument('--evidence', action='store_true', help='打印每条边的 file:line 证据')
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
