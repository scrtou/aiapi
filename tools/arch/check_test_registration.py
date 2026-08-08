#!/usr/bin/env python3
"""测试注册一致性检查（失败退出码 4）。

动因（步骤 98-116 实际踩中）：src/test/CMakeLists.txt 用 ParseAndAddDrogonTests，
它在 configure 期扫描源文件里的 DROGON_TEST 宏来生成 add_test 条目。
只跑 cmake --build 而不重新 configure 时，新写的用例会被编译进二进制，
但不会出现在 ctest 名单里 —— ctest 照样报全部通过，而新用例一次都没执行。
绿色因此完全失去意义。（实测：用例数长期停在 188，重新 configure 后才变成 190。）

两种模式：

  STRICT（本地开发；存在已 configure 的 build/）
      比对「源码中 DROGON_TEST 名字集合」vs「ctest -N 注册的名字集合」。
      这是唯一能抓住上述事故的判据。

  DEGRADED（CI；干净检出，无 build/）
      CI job 不做 cmake configure（.deps 里的 Drogon 44M 且不入库，
      构建代价远超本门禁价值），因此拿不到 ctest 名单。
      退而比对「src/test/ 下实际存在的测试源文件」vs「CMakeLists 的 TEST_SOURCES」。

      必须明说盲区：降级判据是文件级的，只能抓住
      「新增测试文件却忘了加进 TEST_SOURCES」；
      对「文件早已在列表、只是新增了 DROGON_TEST 而未重新 configure」
      —— 也就是本轮真正踩到的那一种 —— 完全无感。
      所以 CI 通过不代表该类事故已被拦截，它只拦住了同类问题的另一半。

  --require-strict
      拒绝降级：拿不到 ctest 名单即失败。供本地钩子 / 发布前检查使用。
"""
import argparse
import glob
import os
import re
import subprocess
import sys

TEST_DIR = 'src/test'
BUILD_DIR = 'build'
CMAKE_FILE = os.path.join(TEST_DIR, 'CMakeLists.txt')
FAIL = 4


def read_text(path):
    with open(path, encoding='utf-8') as f:
        return f.read()


def test_files():
    return sorted(glob.glob(os.path.join(TEST_DIR, '*.cpp')) +
                  glob.glob(os.path.join(TEST_DIR, '*.cc')))


def declared_tests():
    names = set()
    for path in test_files():
        for m in re.finditer(r'DROGON_TEST\s*\(\s*([A-Za-z_][A-Za-z0-9_]*)\s*\)',
                             read_text(path)):
            names.add(m.group(1))
    return names


def registered_tests():
    """ctest -N 注册的用例名；拿不到则返回 None（区别于「拿到了但为空」）。"""
    if not os.path.isdir(BUILD_DIR):
        return None
    try:
        r = subprocess.run(['ctest', '--test-dir', BUILD_DIR, '-N'],
                           capture_output=True, text=True)
    except OSError:
        return None
    names = set()
    for ln in r.stdout.splitlines():
        m = re.match(r'\s*Test\s+#\d+:\s+(\S+)', ln)
        if m:
            names.add(m.group(1))
    return names or None


def listed_sources():
    if not os.path.exists(CMAKE_FILE):
        return None
    m = re.search(r'set\s*\(\s*TEST_SOURCES(.*?)\)', read_text(CMAKE_FILE), re.S)
    if not m:
        return None
    return set(re.findall(r'([A-Za-z0-9_]+\.(?:cpp|cc))', m.group(1)))


def run_strict(reg):
    decl = declared_tests()
    missing = sorted(decl - reg)
    stale = sorted(reg - decl)
    print('[STRICT] 源码声明 %d 个 DROGON_TEST，ctest 注册 %d 个' % (len(decl), len(reg)))
    if not missing and not stale:
        print('OK   声明与注册完全一致')
        return 0
    if missing:
        print('FAIL 以下用例已写入源码但未被 ctest 注册，等于从未执行：')
        for n in missing:
            print('       - ' + n)
    if stale:
        print('FAIL 以下用例在 ctest 名单中但源码里已不存在（名单陈旧）：')
        for n in stale:
            print('       - ' + n)
    print('     修复：cmake -S . -B build && cmake --build build')
    return FAIL


def run_degraded():
    print('[DEGRADED] 无法读取 ctest 名单（build/ 不存在或未 configure）。')
    print('           改用文件级静态判据。盲区：无法发现')
    print('           「文件已在 TEST_SOURCES、新增 DROGON_TEST 却未重新 configure」')
    print('           这一类事故；本步通过不等于该类事故已被拦截。')
    listed = listed_sources()
    if listed is None:
        print('FAIL 无法从 %s 解析出 TEST_SOURCES' % CMAKE_FILE)
        return FAIL
    ondisk = set(os.path.basename(x) for x in test_files())
    unlisted = sorted(ondisk - listed)
    ghost = sorted(listed - ondisk)
    print('           磁盘 %d 个测试源文件，TEST_SOURCES 列出 %d 个'
          % (len(ondisk), len(listed)))
    if not unlisted and not ghost:
        print('OK   文件级一致（注意上述盲区仍然存在）')
        return 0
    if unlisted:
        print('FAIL 以下文件存在于 %s 但不在 TEST_SOURCES，永远不会被编译：' % TEST_DIR)
        for n in unlisted:
            print('       - ' + n)
        print('     处置：加入 TEST_SOURCES，或确认其为死文件后删除。')
    if ghost:
        print('FAIL 以下文件在 TEST_SOURCES 中但磁盘上不存在：')
        for n in ghost:
            print('       - ' + n)
    return FAIL


def main():
    ap = argparse.ArgumentParser(description='测试注册一致性检查')
    ap.add_argument('--require-strict', action='store_true',
                    help='拒绝降级：拿不到 ctest 名单即失败')
    args = ap.parse_args()

    reg = registered_tests()
    if reg is not None:
        return run_strict(reg)
    if args.require_strict:
        print('FAIL --require-strict 已指定，但无法从 ctest -N 读到任何用例。')
        print('     修复：cmake -S . -B build && cmake --build build')
        return FAIL
    return run_degraded()


if __name__ == '__main__':
    sys.exit(main())
