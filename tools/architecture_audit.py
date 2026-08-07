#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""architecture_audit.py — RFC-001 §0-E-1 审计规则  v2.0

v2.0 相对 v1.9 的三处修正（全部因自检发现 v1.9 判定不可信）:
  1. 测试信号识别 drogon_test.h 的 DROGON_TEST / CHECK / REQUIRE，不再假定 gtest。
  2. R2 的「是否被测」改用 **编译期真实依赖闭包**（g++ -MM），不再用文件名子串匹配。
  3. R2 的 fanin 用真实 #include 行解析，不再用 basename 子串 + 排除同名 cpp。

真值来源: src/test/CMakeLists.txt 的 TEST_SOURCES / PROJECT_SOURCES —— 这是
测试二进制实际链接的文件集合，比任何字符串猜测都可靠。
"""
import os, re, sys, json, argparse, subprocess

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
SRC  = os.path.join(ROOT, 'src')
TEST = os.path.join(SRC, 'test')
TEST_CMAKE = os.path.join(TEST, 'CMakeLists.txt')

R3_LIMIT = 200
R2_FANIN = 2

INCDIRS = [SRC,
           os.path.join(SRC, 'apiManager'),
           os.path.join(SRC, 'apipoint'),
           os.path.join(SRC, 'apipoint', 'chaynsapi'),
           os.path.join(SRC, 'sessionManager'),
           os.path.join(SRC, 'controllers'),
           os.path.join(SRC, 'controllers', 'sinks'),
           os.path.join(SRC, 'tools')]
for extra in ('.deps/drogon-install/include', '/usr/include/jsoncpp',
              '/usr/local/include', '/usr/local/include/drogon'):
    p = extra if extra.startswith('/') else os.path.join(ROOT, extra)
    if os.path.isdir(p):
        INCDIRS.append(p)


def walk(base, exts):
    for d, _, fs in os.walk(base):
        if os.sep + '.deps' in d or os.sep + 'build' in d:
            continue
        for f in fs:
            if f.endswith(exts):
                yield os.path.join(d, f)


def read(p):
    with open(p, encoding='utf-8', errors='replace') as fh:
        return fh.read()


def nlines(p):
    return read(p).count('\n') + 1


# ---------------- CMake 真值 ----------------
def cmake_block(name):
    m = re.search(r'set\(\s*' + name + r'\s(.*?)\)', read(TEST_CMAKE), re.S)
    if not m:
        return []
    out = []
    for line in m.group(1).split('\n'):
        line = line.split('#')[0].strip()
        if not line:
            continue
        line = line.replace('${CMAKE_CURRENT_SOURCE_DIR}', TEST)
        out.append(os.path.normpath(line if os.path.isabs(line) else os.path.join(TEST, line)))
    return [p for p in out if p.endswith(('.cpp', '.cc'))]


# ---------------- 依赖闭包 ----------------
INC_RE = re.compile(r'^\s*#\s*include\s*["<]([^">]+)[">]', re.M)


def resolve(inc, origin):
    cands = [os.path.join(os.path.dirname(origin), inc)] + \
            [os.path.join(d, inc) for d in INCDIRS]
    for c in cands:
        c = os.path.normpath(c)
        if os.path.isfile(c) and c.startswith(SRC):
            return c
    return None


def closure_gpp(src):
    """优先用 g++ -MM 拿真实闭包；失败返回 None。"""
    cmd = ['g++', '-std=c++17', '-MM', '-MG'] + ['-I' + d for d in INCDIRS] + [src]
    try:
        r = subprocess.run(cmd, capture_output=True, text=True, timeout=90)
    except Exception:
        return None
    if r.returncode != 0 or not r.stdout.strip():
        return None
    toks = r.stdout.replace('\\\n', ' ').split(':', 1)[-1].split()
    out = set()
    for t in toks:
        p = os.path.normpath(t if os.path.isabs(t) else os.path.join(ROOT, t))
        if p.startswith(SRC) and p.endswith('.h'):
            out.add(p)
    return out


def closure_manual(src):
    """回退：自己做传递闭包。"""
    seen, stack = set(), [src]
    while stack:
        cur = stack.pop()
        for inc in INC_RE.findall(read(cur)):
            p = resolve(inc, cur)
            if p and p not in seen:
                seen.add(p)
                stack.append(p)
    return seen


def closure(src, mode):
    if mode['gpp']:
        c = closure_gpp(src)
        if c is not None:
            return c, 'g++'
        mode['gpp'] = False
    return closure_manual(src), 'manual'


# ---------------- R1 ----------------
FUNC_DEF = re.compile(r'^[A-Za-z_][A-Za-z0-9_:<>,&*~\s]*?\b([a-zA-Z_][A-Za-z0-9_]*)\s*\(', re.M)
MEMBER   = re.compile(r'\bGenerationService::([a-zA-Z_][A-Za-z0-9_]*)\s*\(')


def rule_r1():
    members = set()
    for p in walk(SRC, ('.cpp', '.h')):
        members.update(MEMBER.findall(read(p)))
    hits = []
    for d, _, fs in os.walk(SRC):
        if os.path.basename(d) not in ('tooling', 'actionProtocol'):
            continue
        for f in fs:
            if not f.endswith('.h'):
                continue
            p = os.path.join(d, f)
            for name in set(FUNC_DEF.findall(read(p))):
                if name in members:
                    hits.append({'func': name, 'header': os.path.relpath(p, ROOT)})
    return sorted(hits, key=lambda x: x['func'])


# ---------------- R2 ----------------
DROGON_TEST = re.compile(r'^\s*DROGON_TEST\s*\(', re.M)
GTEST       = re.compile(r'^\s*TEST(_F|_P)?\s*\(', re.M)
ASSERTION   = re.compile(r'\b(CHECK|REQUIRE|ASSERT_[A-Z]+|EXPECT_[A-Z]+)\s*\(')


def rule_r2(diag):
    test_srcs = cmake_block('TEST_SOURCES')
    linked    = set(cmake_block('PROJECT_SOURCES'))
    diag['test_sources'] = len(test_srcs)
    diag['linked_sources'] = len(linked)

    mode = {'gpp': True}
    covered = {}          # header -> 累计断言数
    per_test = []
    for t in test_srcs:
        if not os.path.exists(t):
            continue
        body = read(t)
        cases = len(DROGON_TEST.findall(body)) + len(GTEST.findall(body))
        asserts = len(ASSERTION.findall(body))
        cl, how = closure(t, mode)
        diag['mode'] = how
        per_test.append({'file': os.path.basename(t), 'cases': cases,
                         'asserts': asserts, 'headers': len(cl)})
        if cases > 0 and asserts > 0:
            for h in cl:
                covered[h] = covered.get(h, 0) + asserts
    diag['per_test'] = sorted(per_test, key=lambda x: -x['asserts'])

    # fanin: 真实 include 行解析
    prod_cpp = [p for p in walk(SRC, ('.cpp',)) if not p.startswith(TEST + os.sep)]
    fanin = {}
    for q in prod_cpp:
        for inc in set(INC_RE.findall(read(q))):
            h = resolve(inc, q)
            if h:
                fanin.setdefault(h, set()).add(q)

    hits = []
    for h, users in fanin.items():
        n = len(users)
        if n < R2_FANIN:
            continue
        if covered.get(h, 0) > 0:
            continue
        impl = h[:-2] + '.cpp'
        hits.append({'header': os.path.relpath(h, ROOT), 'fanin': n,
                     'asserts': covered.get(h, 0),
                     'linked': impl in linked,
                     'impl_lines': nlines(impl) if os.path.exists(impl) else 0})
    return sorted(hits, key=lambda x: (-x['fanin'], -x['impl_lines'])), covered, fanin


# ---------------- R3 ----------------
TOP = re.compile(r'^[A-Za-z_~][A-Za-z0-9_:<>,&*~\s]*\(|^(static|inline|namespace)\b')


def rule_r3():
    hits = []
    for p in walk(SRC, ('.cpp',)):
        if p.startswith(TEST + os.sep):
            continue
        lines = read(p).split('\n')
        starts = [i for i, l in enumerate(lines, 1) if TOP.match(l)]
        starts.append(len(lines) + 1)
        for a, b in zip(starts, starts[1:]):
            if b - a > R3_LIMIT:
                hits.append({'file': os.path.relpath(p, ROOT), 'start': a,
                             'end': b - 1, 'lines': b - a})
    return sorted(hits, key=lambda x: -x['lines'])


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument('--json', action='store_true')
    ap.add_argument('--selftest', action='store_true')
    ap.add_argument('--baseline')
    ap.add_argument('--write-baseline')
    a = ap.parse_args()

    diag = {}
    r2, covered, fanin = rule_r2(diag)
    res = {'R1': rule_r1(), 'R2': r2, 'R3': rule_r3()}

    if a.selftest:
        print('=' * 70)
        print('R2 自检 — 已知有测试的文件必须被识别为「已覆盖」')
        print('=' * 70)
        print('  闭包模式: %s   测试源=%d  链接的生产源=%d'
              % (diag.get('mode'), diag['test_sources'], diag['linked_sources']))
        print('\n  每个测试文件:')
        for t in diag['per_test'][:26]:
            print('    %-44s 用例=%-3d 断言=%-4d 闭包头=%d'
                  % (t['file'], t['cases'], t['asserts'], t['headers']))
        print('\n  关键反例（必须 asserts>0，否则 R2 仍是坏的）:')
        ok = True
        for name in ('sessionManager/core/RequestAdapters.h',
                     'sessionManager/core/Session.h',
                     'sessionManager/tooling/XmlTagToolCallCodec.h',
                     'metrics/ErrorStatsService.h',
                     'accountManager/accountManager.h'):
            h = os.path.join(SRC, name)
            c, f = covered.get(h, 0), len(fanin.get(h, ()))
            flag = 'OK  ' if c > 0 else 'ZERO'
            print('    [%s] fanin=%-3d asserts=%-5d %s' % (flag, f, c, name))
            if name.startswith('sessionManager') and c == 0:
                ok = False
        print('\n  自检结论: %s' % ('通过 — R2 可信' if ok else '失败 — R2 仍不可信，勿写入 RFC'))
        return 0 if ok else 2

    if a.write_baseline:
        with open(a.write_baseline, 'w', encoding='utf-8') as fh:
            json.dump({k: len(v) for k, v in res.items()}, fh, indent=2)
        print('baseline written: %s' % a.write_baseline)

    if a.json:
        print(json.dumps(res, indent=2, ensure_ascii=False))
        return 0

    print('=' * 70)
    print('architecture_audit v2.0  (闭包模式: %s)' % diag.get('mode'))
    print('=' * 70)
    print('\n[R1] 同名竞争: %d' % len(res['R1']))
    for h in res['R1']:
        print('  %-32s %s' % (h['func'], h['header']))
    print('\n[R2] 高扇入零断言 (fanin>=%d): %d' % (R2_FANIN, len(res['R2'])))
    for h in res['R2']:
        print('  fanin=%-3d impl=%-5d链接=%-5s %s'
              % (h['fanin'], h['impl_lines'], h['linked'], h['header']))
    print('\n[R3] 单函数 >%d 行: %d' % (R3_LIMIT, len(res['R3'])))
    tot = 0
    for h in res['R3']:
        tot += h['lines']
        print('  %5d行  %s:%d-%d' % (h['lines'], h['file'], h['start'], h['end']))
    print('  ---- 合计 %d 行 ----' % tot)

    if a.baseline and os.path.exists(a.baseline):
        base = json.load(open(a.baseline, encoding='utf-8'))
        bad = [k for k in ('R1', 'R2', 'R3') if len(res[k]) > base.get(k, 0)]
        if bad:
            print('\n!! 回归: %s' % ', '.join(bad))
            return 1
        print('\n基线检查通过')
    return 0


if __name__ == '__main__':
    sys.exit(main())
