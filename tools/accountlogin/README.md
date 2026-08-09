# accountlogin 运维工具

本目录是独立运维/手工调试工具，**不属于 `aiapi` production target**，
也不参与单元测试链接。它从 `src/tools/accountlogin/` 移出，是为了使
`src/` 下的每个 `.cpp/.cc` 都严格归属于一个生产构建 target。

## 内容

- `loginlocal.py` / `loginremote.py`：独立登录自动化脚本；
- `login_client.cpp`：需要 curl 和 nlohmann/json 的独立客户端示例；
- `chayns-login.service`：systemd 部署样例，其用户、路径和启动命令需在部署前按实际环境修改；
- `test.py`：手工调试脚本，不是 `ctest` 测试。

## 边界

- CMake 不构建本目录；
- 架构、覆盖率和 source-ownership 门禁不将这些文件计为生产实现；
- 脚本会访问外部服务，只能由操作者在明确授权的环境中手工运行；
- 它们不是服务启动、请求处理或 Provider 流程的一部分。
