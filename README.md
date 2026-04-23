# ZKVault

ZKVault 是一个面向本地终端环境的安全保险库项目，当前以 CLI、会话式 shell 和 TUI 原型三种入口提供敏感数据管理能力。项目目标是沉淀一套可演进的终端保险库基础设施，而不是服务端系统、HTTP API 或多租户平台。

## 项目概览

- 产品形态：本地终端安全保险库
- 主要界面：CLI、会话式 shell、TUI 原型
- 部署方式：单机本地运行
- 技术栈：C++20、CMake、OpenSSL
- 非目标：HTTP API、守护进程、多租户服务端架构

当前重点包括：

- 主密码管理与密钥派生
- 条目加密、解密与本地持久化
- 终端安全输入与敏感信息隔离
- 面向 shell/TUI 的前端契约与状态组织

## 已实现能力

### CLI

`zkvault` 当前支持以下命令：

```text
zkvault init
zkvault shell
zkvault tui
zkvault change-master-password
zkvault add <name>
zkvault get <name>
zkvault update <name>
zkvault delete <name>
zkvault list
```

说明：

- `init`：初始化保险库并生成 `.zkv_master`
- `shell`：启动一次解锁、多步操作的会话式终端原型
- `tui`：启动当前的全屏终端界面原型
- `change-master-password`：更新主密码并重新包裹 DEK
- `add <name>`：创建条目
- `get <name>`：读取条目
- `update <name>`：更新条目
- `delete <name>`：删除条目
- `list`：列出条目标识

### 会话式 shell 原型

`zkvault shell` 用于验证未来 TUI 的核心交互模型，当前已支持：

- 启动时自动检测保险库是否存在，不存在时可直接初始化
- 一次解锁后连续执行 `list`、`find`、`next`、`prev`、`show`、`add`、`update`、`delete`
- 在同一会话内执行主密码轮换
- 在同一会话内显式 `lock` / `unlock`
- 失败或取消后恢复最近一次稳定视图
- 保持与 CLI 一致的高风险确认语义

Shell 帮助命令如下：

```text
help
list
find <term>
next
prev
show [name]
add <name>
update <name>
delete <name>
change-master-password
lock
unlock
quit
```

### TUI 原型

`zkvault tui` 在复用同一套前端契约与 shell runtime 的基础上，增加了备用屏幕和全屏重绘骨架，当前已覆盖：

- 浏览区、状态区、视图区的基础布局
- 帮助、列表、详情、锁定视图
- 条目新增、更新表单
- 条目删除与主密码轮换确认流程
- 主密码轮换表单
- 锁定、解锁、退出

当前按键如下：

- `Up` / `Down` 或 `j` / `k`：移动焦点
- `Enter`：查看当前选中条目
- `a`：新增条目
- `e`：更新当前选中条目
- `d`：删除当前选中条目
- `m`：轮换主密码
- `?`：打开帮助
- `Esc`：返回浏览区或取消当前交互
- `l`：锁定保险库
- `u`：解锁保险库
- `q`：退出 TUI

说明：

- TUI 当前已实现新增、更新、删除和主密码轮换

## 安全与交互约束

- 主密码始终通过终端交互输入，不通过命令行参数传递
- 密码输入显示 `*` 掩码，并支持退格删除
- 普通文本输入同样支持退格，不会回显 `^H` 或 `^?`
- `shell` 会话在开始时解锁一次，后续可连续执行多步操作
- `add` 仅允许创建新条目；若条目已存在则失败
- `update` 仅允许更新已有条目；若条目不存在则失败
- 条目名仅允许字母、数字、`.`、`-`、`_`
- 条目名不允许为空、不允许为 `.` 或 `..`，也不允许包含路径分隔符、空格或其他特殊字符

高风险操作需要显式确认：

- 覆盖条目：再次输入目标条目名
- 删除条目：再次输入目标条目名
- 轮换主密码：输入 `CHANGE`

若确认值不匹配，操作会立即失败，不会继续写入。

若设置环境变量 `ZKVAULT_SHELL_IDLE_TIMEOUT_SECONDS=<正整数>`，`shell` 和已解锁状态下的 `tui` 会在空闲超时后自动锁定并清屏。

## 架构说明

当前代码结构如下：

```text
src/
├── app/      # 保险库动作、会话状态与前端契约
├── crypto/   # 随机数、KDF、AES-GCM、hex 编解码
├── model/    # PasswordEntry、MasterKeyFile、EncryptedEntryFile
├── shell/    # 会话式终端原型与 runtime
├── storage/  # .zkv_master 和条目文件读写
├── terminal/ # 终端输入与显示抽象
├── tui/      # 全屏 TUI 原型
└── main.cpp  # CLI 入口与命令分发
```

建议分层如下：

- `Vault Core`：密钥派生、数据加解密、文件存储、条目校验
- `Terminal Integration Layer`：命令封装、输入采集、错误映射、前端调用组织
- `TUI Application Layer`：界面布局、焦点管理、快捷键、状态切换

核心原则是：界面能力可以演进，但安全核心边界不应被界面实现反向侵入。

## 构建与运行

### 环境要求

- Linux
- CMake 3.16+
- 支持 C++20 的编译器
- OpenSSL 开发库

### 构建

```bash
cmake -S . -B build
cmake --build build
```

### 快速开始

初始化保险库：

```bash
./build/zkvault init
```

列出条目：

```bash
./build/zkvault list
```

读取条目：

```bash
./build/zkvault get <name>
```

启动会话式 shell：

```bash
./build/zkvault shell
```

启动 TUI 原型：

```bash
./build/zkvault tui
```

若直接执行：

```bash
./build/zkvault
```

当前行为是输出命令用法并等待子命令，不会自动进入 `shell` 或 `tui`。

## 测试

查看测试列表：

```bash
ctest --test-dir build -N
```

运行全部测试：

```bash
ctest --test-dir build
```

## 当前定位

ZKVault 现在更适合被理解为“终端保险库交互模型与安全边界的实现工程”，而不是最终完成态产品。当前仓库已经把 CLI、会话式 shell 和 TUI 原型统一到了同一套动作语义、状态机和错误映射上，为后续继续演进 TUI 提供了稳定基础。

## 长期目标

在保持当前本地优先、终端优先架构的前提下，ZKVault 的长期目标可以扩展为支持云端部署能力，但该方向应建立在 zero-knowledge 安全模型之上，而不是把解密能力直接迁移到服务端。

更具体地说，长期演进方向可以包括：

- 提供可选的云端密文同步、备份与恢复能力
- 支持多设备之间的保险库数据同步
- 由客户端完成主密码处理、密钥派生与数据加解密
- 服务端仅存储密文、必要元数据和同步状态，不持有可用于解密用户数据的主密钥或等价物

若进入云端场景，安全边界应明确区分：

- 传输层安全：通过 TLS 保护网络传输
- 应用层 zero-knowledge：通过客户端加密确保服务端无法获取明文

这意味着未来若增加云能力，目标应是“zero-knowledge 的云端密文存储与同步平台”，而不是“可在服务端直接解密和操作明文保险库的托管应用”。
