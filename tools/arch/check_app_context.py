#!/usr/bin/env python3
"""composition root 结构检查（失败退出码 4）。

动因（P04-W2 / C7）：P4-W2 把启动流程从 main.cc 的双层 lambda 搬进了
AppContext + AppWiring。搬完之后，五条关键性质全部只由「代码现在长这样」
维持，没有任何一条能被编译器或单测抓住：

  A1  build() 的返回值必须被检查。
      这正是 G1 的同构复发点：从前 `return 1` 被 std::function<void()> 吞掉，
      现在若有人写成裸的 `appContext.build();`，进程会带着失败的 Store
      继续 run()。头文件上的 [[nodiscard]] 只在开了对应告警且没被 -Wno- 关掉
      时才有效，且 `(void)appContext.build()` 能合法绕过它。Drogon 的 DB
      client 在 run() 内创建，故 production build() 位于 BeginningAdvice；这
      不改变「返回值必须据以停止启动」这一约束。

  A2  每个 addOwner 都必须落在某个已注册步骤内部，且与其启动动作同处一处。
      AppContext 的回滚正确性依赖「owners_ 里的东西恰好是已经起来的东西」。
      若有人把 addOwner 提到 registerApplicationSteps 的顶层（步骤之外），
      该 owner 会在任何步骤执行前就进入列表，build() 中途失败时会去 stop()
      一个从未 start() 的东西。

  A3  shutdown 的实参必须是绝对时间点（steady_clock::now() + ...），
      不能是裸时长。G5 的整个修法建立在「绝对 deadline 可跨 owner 累减」上，
      改回相对时长不会有任何编译错误，但宽限期会被逐段突破。

  A5  每个「会拉起后台线程的 runtime service」都必须在 AppWiring 里登记为 owner。
      G7 的实质不是「有单例」，而是「后台线程靠隐式析构收尾」。静态析构在
      main 返回之后发生，此时它依赖的 DbManager / drogon DB 客户端可能已先被
      销毁，析构里的兜底 shutdown() 于是变成对已析构对象的调用；而且那一刻
      早已在 shutdown(deadline) 的宽限期之外，停机日志对此只字不提。
      ErrorStatsService 就是这样漏掉的：init() 起了 workerThread_，却没有任何
      addOwner，全靠 ~ErrorStatsService() 兜底——编译、单测、既有四条判据全绿。

  A4  main.cc 不得再直接注入单例 Store（setStore / setSink / setDbProbe 等）。
      接线的唯一真相是 AppWiring；main.cc 里出现第二处注入，
      check_startup_wiring.py 反而会因为「在候选文件里找到了」而通过，
      两道门禁一起变瞎。

  A6  production build() 必须位于 Drogon BeginningAdvice 中。
      db_clients 的配置在 loadConfigFile() 时只被记录，真正的 DbClient 要到
      app().run() 中才创建。把 build() 搬回 run() 前会让所有 DB Store 获得空
      client 并失败；BeginningAdvice 又恰好位于 listener 开放之前，仍是启动屏障。

所有判据都是**结构性**的，不看注释、不看命名，只看语法形状。
盲区必须明说：本脚本是正则级的，不解析 C++。
它能抓住「写法退回旧形状」，抓不住「保持形状但语义错」——
例如 addOwner 登记在步骤内、但登记的 stop 闭包停的是另一个对象。
那类问题归单测（C5/C6 的逆序 teardown 与幂等用例）。
"""
import re
import sys

MAIN = 'src/main.cc'
WIRING = 'src/runtime/AppWiring.cpp'
CONTEXT_H = 'src/runtime/AppContext.h'
FAIL = 4

# main.cc 里禁止出现的注入形状。与 check_startup_wiring.py 的 REQUIRED 表
# 互补：那张表管「AppWiring 里必须有」，这张管「main.cc 里必须没有」。
# A5 的登记册：值是「该 owner 的 stop 闭包里必须出现的调用形状」。
#
# 为什么是白名单而不是自动发现：正则级脚本无法判定 `std::thread` 成员是否
# 真的会被 start，自动发现只会制造噪音。这张表是**人工审过一次**的结论，
# 新增持线程 service 时必须手工进表——进表这个动作本身就是 review 的钩子。
# 表若与代码脱节，A5 会失败而不是静默放行（见 check_thread_owners_registered）。
THREAD_OWNING_SERVICES = [
    # P5-W3: queue is now AppContext-owned and captured by the same owner
    # closure that drains it; a process singleton must not reappear.
    ('BackgroundTaskQueue', r'\bqueue\s*->\s*shutdown\s*\('),
    # P5-W3: AccountManager is context-owned; the owner must retain the same
    # shared object that was published to providers and application services.
    ('AccountManager',      r'\baccounts\s*->\s*stopBackgroundThreads\s*\('),
    # P5-W3: both are normal AppContext-owned objects now.  The capture names
    # make the stop action and the owning object visibly part of one wiring step.
    ('chaynsThreadReaper',  r'\breaper\s*->\s*stop\s*\('),
    ('chatSession',         r'\bsessionStore\s*->\s*stopClearExpiredSession\s*\('),
    # P5-W3: the metrics worker is an AppContext-owned shared object; its
    # deadline-aware owner must stop that exact capture, not rediscover a
    # process singleton.
    ('ErrorStatsService',   r'\berrorStats\s*->\s*shutdown\s*\('),
]

FORBIDDEN_IN_MAIN = [
    (r'\.\s*setStore\s*\(',            'setStore'),
    (r'\.\s*setChannelStore\s*\(',     'setChannelStore'),
    (r'\.\s*setSink\s*\(',             'setSink'),
    (r'::\s*setDbProbe\s*\(',          'setDbProbe'),
    (r'\.\s*setRetoolProvisionClock\s*\(', 'setRetoolProvisionClock'),
]


def read(path):
    with open(path, encoding='utf-8') as f:
        return f.read()


def strip_comments(text):
    """去掉 // 与 /* */ 注释。

    必要而非洁癖：本文件的注释里大量讨论 `appContext.build()`、`setStore` 这些
    正是判据要找的字面量。不去注释，A1/A4 会被自己的解释性注释触发或掩盖。
    """
    text = re.sub(r'/\*.*?\*/', ' ', text, flags=re.S)
    text = re.sub(r'//[^\n]*', ' ', text)
    return text


def lines_of(text):
    return text.splitlines()


def check_build_result_used(src, problems):
    """A1：build() 的返回值必须被绑定到变量。"""
    calls = [(i, ln) for i, ln in enumerate(lines_of(src), 1)
             if re.search(r'\.\s*build\s*\(\s*\)', ln)]
    if not calls:
        problems.append('A1 %s 中找不到 build() 调用；composition root 是否已改写？'
                        '门禁无法判定，按失败处理' % MAIN)
        return
    for ln_no, ln in calls:
        # 必须是「有名字接住」的形式：`auto x = ctx.build()` / `Result x = ...`。
        # 显式丢弃（(void)、std::ignore）同样判失败——它绕过 [[nodiscard]]，
        # 而绕过的后果恰好是 G1。
        if re.search(r'(?:std::ignore\s*=|\(\s*void\s*\))\s*\w+\s*\.\s*build\s*\(', ln):
            problems.append('A1 %s:%d build() 的返回值被显式丢弃，'
                            'G1（失败静默继续）会复发' % (MAIN, ln_no))
            continue
        if not re.search(r'=\s*\w+\s*\.\s*build\s*\(\s*\)', ln):
            problems.append('A1 %s:%d build() 的返回值未被接住：%s'
                            % (MAIN, ln_no, ln.strip()))


def check_nodiscard(src, problems):
    """A1 补强：头文件上的 [[nodiscard]] 不得被摘掉。"""
    if not re.search(r'\[\[nodiscard\]\]\s*StartupResult\s+build\s*\(', src):
        problems.append('A1 %s 中 build() 的 [[nodiscard]] 已消失；'
                        '编译期那层提醒没了' % CONTEXT_H)


def check_owner_inside_step(src, problems):
    """A2：addOwner 必须出现在 addStep 的 lambda 内，或某个 step 函数体内。

    判据用花括号深度而非「离哪个 addStep 近」：后者在嵌套 lambda 下会误判。
    registerApplicationSteps 的顶层语句深度为 1（函数体），步骤 lambda 内
    至少为 2。故「深度 <= 1 的 addOwner」即为逃到步骤之外的那种。
    """
    lines = lines_of(src)
    fn_start = None
    for i, ln in enumerate(lines):
        if re.search(r'void\s+registerApplicationSteps\s*\(', ln):
            fn_start = i
            break
    if fn_start is None:
        problems.append('A2 %s 中找不到 registerApplicationSteps 定义' % WIRING)
        return

    depth = 0
    seen = 0
    for i in range(fn_start, len(lines)):
        ln = lines[i]
        if 'addOwner' in ln and depth <= 1:
            seen += 1
            problems.append('A2 %s:%d addOwner 位于步骤之外（花括号深度 %d）：'
                            'build() 中途失败会去 stop 一个从未 start 的 owner'
                            % (WIRING, i + 1, depth))
        depth += ln.count('{') - ln.count('}')
        if depth <= 0 and i > fn_start:
            break
    return seen


def check_absolute_deadline(src, problems):
    """A3：shutdown 实参必须含 steady_clock::now()。"""
    idx = src.find('.shutdown(')
    if idx < 0:
        problems.append('A3 %s 中找不到 shutdown() 调用' % MAIN)
        return
    # 取调用点之后的一小段（跨行实参），只判是否出现绝对时间基点。
    window = src[idx:idx + 300]
    call = window.split(';')[0]
    if 'steady_clock::now()' not in call:
        problems.append('A3 shutdown() 的实参不含 steady_clock::now()，'
                        '疑似退回相对时长；跨 owner 会逐段累加突破宽限期')


def check_main_has_no_injection(src, problems):
    """A4：main.cc 不得直接注入 Store。"""
    for i, ln in enumerate(lines_of(src), 1):
        for pat, name in FORBIDDEN_IN_MAIN:
            if re.search(pat, ln):
                problems.append('A4 %s:%d 出现 %s：接线的唯一真相应是 %s'
                                % (MAIN, i, name, WIRING))


def check_build_waits_for_drogon_db_clients(src, problems):
    """A6：build() 必须由 BeginningAdvice 包围，而非在 app().run() 之前执行。"""
    marker = 'appContext.build()'
    build_at = src.find(marker)
    if build_at < 0:
        # A1 会给出更详细的缺失诊断；此处不再重复。
        return

    advice_at = src.rfind('registerBeginningAdvice', 0, build_at)
    if advice_at < 0:
        problems.append('A6 %s 中 build() 不在 registerBeginningAdvice 内；'
                        'Drogon DbClient 尚未创建就会访问 Store' % MAIN)
        return
    lambda_open = src.find('{', advice_at, build_at)
    if lambda_open < 0:
        problems.append('A6 %s 中找不到包围 build() 的 BeginningAdvice lambda' % MAIN)
        return

    depth = 0
    for ch in src[lambda_open:build_at]:
        if ch == '{':
            depth += 1
        elif ch == '}':
            depth -= 1
    if depth <= 0:
        problems.append('A6 %s 中 build() 已落在 BeginningAdvice lambda 之外；'
                        'Drogon DbClient 生命周期会被破坏' % MAIN)
    if src.find('drogon::app().run()', build_at) < 0:
        problems.append('A6 %s 中 build() 后找不到 app().run()；'
                        '无法保证 Drogon 的启动顺序' % MAIN)


def check_thread_owners_registered(src, problems):
    """A5：登记册里的每个持线程 service，都要能在 AppWiring 中找到对应的 stop 调用。

    只认「出现在某个 addOwner 之后的同一区段内」过于脆弱（闭包可跨行、可提取成
    具名函数），故判据放宽为：该 stop 形状必须在 AppWiring.cpp 中出现，且文件里
    addOwner 的总数不少于登记册条目数。前者防漏登，后者防「把 stop 写在别处
    （例如某个步骤体里直接调用）却没真正交给 AppContext 编排」。
    """
    for name, pattern in THREAD_OWNING_SERVICES:
        if not re.search(pattern, src):
            problems.append('A5 %s 的停机调用在 %s 中找不到：'
                            '它的后台线程将退回到隐式析构收尾（G7 复发）'
                            % (name, WIRING))
    owner_count = len(re.findall(r'\.\s*addOwner\s*\(', src))
    if owner_count < len(THREAD_OWNING_SERVICES):
        problems.append('A5 %s 中 addOwner 仅 %d 处，少于登记册的 %d 个持线程 service：'
                        '至少有一个的 stop 没有交给 AppContext 编排'
                            % (WIRING, owner_count, len(THREAD_OWNING_SERVICES)))


def main():
    problems = []
    try:
        main_src = strip_comments(read(MAIN))
        wiring_src = strip_comments(read(WIRING))
        context_src = read(CONTEXT_H)
    except FileNotFoundError as exc:
        print('FAIL 待检文件缺失：%s' % exc)
        print('     composition root 若已再次搬家，请同步更新本脚本的路径常量。')
        return FAIL

    check_build_result_used(main_src, problems)
    check_nodiscard(context_src, problems)
    check_owner_inside_step(wiring_src, problems)
    check_absolute_deadline(main_src, problems)
    check_main_has_no_injection(main_src, problems)
    check_build_waits_for_drogon_db_clients(main_src, problems)
    check_thread_owners_registered(wiring_src, problems)

    if problems:
        print('composition root 结构检查未通过：')
        for p in problems:
            print('  FAIL %s' % p)
        return FAIL

    print('OK A1 build() 返回值被接住，且 [[nodiscard]] 仍在')
    print('OK A2 全部 addOwner 均位于步骤内部')
    print('OK A3 shutdown() 使用绝对 deadline')
    print('OK A4 main.cc 未直接注入任何 Store')
    print('OK A6 build() 位于 Drogon BeginningAdvice，DbClient 已创建且 listener 尚未开放')
    print('OK A5 %d 个持线程 runtime service 均已登记为 owner' % len(THREAD_OWNING_SERVICES))
    return 0


if __name__ == '__main__':
    sys.exit(main())
