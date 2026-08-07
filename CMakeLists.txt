cmake_minimum_required(VERSION 3.5)
project(aiapi CXX)

# Register tests at the repository build root so `ctest --test-dir <build>`
# discovers tests added by src/test. enable_testing() only affects its directory
# and descendants, so calling it solely from src/ is insufficient here.
enable_testing()

# ##############################################################################
# 唯一的源文件清单维护在 src/CMakeLists.txt。
# 本文件只是一个薄壳,便于在仓库根目录直接 cmake -B build。
# Docker 走 `cd src/build && cmake ..`,同样复用 src/CMakeLists.txt,
# 因此新增源文件只需改一处,不会再出现两边漂移导致的链接失败。
# ##############################################################################

add_subdirectory(src)
