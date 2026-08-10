#!/usr/bin/env python3
"""BackgroundTaskQueue 入队结果处理门禁（退出码 4）。

动因（P4-W1 / C7）：`enqueue()` 已改为返回 `[[nodiscard]] EnqueueResult`。
但 `[[nodiscard]]` 只能拦住「完全丢弃返回值」这一种写法，拦不住
`const auto r = ...enqueue(...);` 之后再也不读 `r` —— 编译器至多给
一条 -Wunused-variable，而项目未开 -Werror，绿色照旧。

退化路径是真实的：C4~C6 逐层把 23 处调用点从 bool shim 改成显式处理，
只要有人图省事写 `const auto ignored = ...enqueue(...)` 就地退回静默丢弃，
队列满 / 停机中的失败重新变成「请求已受理」的假象，且无任何测试会红。

判据（两条，都必须过）：
  A. 声明处仍带 `[[nodiscard]]`——防止有人为了消警告把属性删掉。
  B. 每个生产调用点绑定的变量，在其后必须被真正读取：
     出现在 `if` / `switch` / 比较 / 传参 / `!=` / `== EnqueueResult::` 等位置。
     只写不读即判 FAIL。

已知局限：判据是函数体内的文本作用域近似，不做真正的数据流分析。
把结果存进成员变量再跨函数读取的写法会被误报 FAIL —— 当前 23 处均为
局部变量就地判断，写法一旦分化必须改判据，不要靠加白名单绕过。
"""
import os
import re
import sys

HEADER = 'src/utils/BackgroundTaskQueue.h'
SRC_DIR = 'src'
EXCLUDE_DIRS = {'test'}
FAIL = 4

ENQ_RE = re.compile(r'\.enqueue\s*\(')
BIND_RE = re.compile(r'(?:const\s+)?(?:auto|EnqueueResult)\s+(\w+)\s*=\s*$')
INLINE_RE = re.compile(r'(?:if|while|switch|return|==|!=|&&|\|\||\breturn\b)')
STMT_BOUNDARY = ';{}'


def read(path):
    with open(path, encoding='utf-8') as f:
        return f.read()


def production_sources():
    out = []
    for root, dirs, files in os.walk(SRC_DIR):
        dirs[:] = [d for d in dirs if d not in EXCLUDE_DIRS]
        for fn in files:
            if fn.endswith(('.cc', '.cpp')):
                out.append(os.path.join(root, fn))
    return sorted(out)


def check_nodiscard(errors):
    text = read(HEADER)
    if not re.search(r'\[\[nodiscard\]\]\s*EnqueueResult\s+enqueue\s*\(', text):
        errors.append(
            HEADER + ': enqueue() 缺少 [[nodiscard]] EnqueueResult 声明')


def statement_prefix(text, pos):
    """取 `.enqueue(` 之前、到最近语句边界为止的片段。"""
    start = 0
    for i in range(pos - 1, -1, -1):
        if text[i] in STMT_BOUNDARY:
            start = i + 1
            break
    return text[start:pos]


def function_tail(text, pos):
    """取调用点之后、到本函数结束（列 0 的 `}`）为止的片段。

    不放宽到全文：同名局部变量（23 处里多个都叫 `r`）会让
    「后文出现过该名字」这种判据在任意位置误判为已读取。"""
    m = re.search(r'\n\}', text[pos:])
    return text[pos:pos + m.start()] if m else text[pos:]


def check_call_sites(errors):
    total = 0
    for path in production_sources():
        text = read(path)
        for m in ENQ_RE.finditer(text):
            total += 1
            line = text[:m.start()].count('\n') + 1
            prefix = statement_prefix(text, m.start())
            bind = BIND_RE.search(prefix.replace('\n', ' ').rstrip()
                                  [:len(prefix)] if False else
                                  re.sub(r'[^\S\n]+', ' ', prefix).strip()
                                  .rsplit('=', 1)[0] + '=')
            if bind:
                var = bind.group(1)
                tail = function_tail(text, m.end())
                used = re.search(r'\b' + re.escape(var) +
                                 r'\b\s*(?:!=|==|\)|,|\.|;)', tail) or \
                    re.search(r'(?:if|switch|while|return|!)\s*\(?[^;\n]*\b' +
                              re.escape(var) + r'\b', tail)
                if not used:
                    errors.append(
                        '%s:%d: enqueue() 结果 `%s` 绑定后从未被读取'
                        % (path, line, var))
            elif INLINE_RE.search(prefix):
                continue  # 就地判断，如 `if (...enqueue(...) != Accepted)`
            else:
                errors.append(
                    '%s:%d: enqueue() 返回值被整体丢弃' % (path, line))
    return total


def main():
    errors = []
    check_nodiscard(errors)
    total = check_call_sites(errors)
    if errors:
        print('FAIL 入队结果处理门禁')
        for e in errors:
            print('  ' + e)
        return FAIL
    print(f'OK   {total}/{total} 生产调用点显式处理 EnqueueResult；'
          '声明保留 [[nodiscard]]')
    return 0


if __name__ == '__main__':
    sys.exit(main())
