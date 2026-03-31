# ZKVault

ZKVault 是一个基于 C++ 和 OpenSSL 的单用户密码保险箱 CLI 原型。

当前版本聚焦最小可运行闭环：
- 初始化保险库
- 使用主密码通过 `scrypt` 派生 KEK
- 使用 KEK 包裹 DEK
- 使用 DEK 通过 `AES-256-GCM` 加密密码条目
- 通过 CLI 完成增删查改和列表查看

## 技术概览

- 运行环境：Linux
- 构建工具：CMake
- 密码学库：OpenSSL
- KDF：`scrypt`
- 对称加密：`AES-256-GCM`
- 当前条目类型：`PasswordEntry`

数据模型：
- `PasswordEntry`：`name`、`password`、`note`
- `MasterKeyFile`：`version`、`kdf`、`salt`、`wrap_iv`、`encrypted_dek`、`auth_tag`
- `EncryptedEntryFile`：`version`、`data_iv`、`ciphertext`、`auth_tag`

## 目录结构

```text
src/
├── crypto/   # 随机数、KDF、AES-GCM、hex 编解码
├── model/    # PasswordEntry、MasterKeyFile、EncryptedEntryFile
├── storage/  # .zkv_master 和条目文件读写
└── main.cpp  # CLI 入口
```

## 快速开始

构建项目：

```bash
cmake -S . -B build
cmake --build build
```

初始化保险库：

```bash
./build/zkvault init
```

程序会交互式提示输入两次主密码；两次一致后才会生成 `.zkv_master`。

## 测试

列出当前已注册的测试：

```bash
ctest --test-dir build -N
```

运行全部测试：

```bash
ctest --test-dir build
```

如果测试失败并希望直接看到失败输出：

```bash
ctest --test-dir build --output-on-failure
```

当前仓库已包含一条 CLI 冒烟测试 `zkvault_cli_smoke`，会在临时目录中验证以下主流程：
- `init`
- `add`
- `get`
- `update`
- `list`
- `change-master-password`
- 非法条目名拒绝
- `delete`

也可以直接运行测试脚本：

```bash
bash tests/cli_smoke_test.sh ./build/zkvault
```

测试脚本会在 `/tmp` 下创建临时目录并自动清理，不会污染当前仓库中的 `.zkv_master` 或 `data/`。

## 命令说明

初始化保险库：

```bash
./build/zkvault init
```

更换主密码：

```bash
./build/zkvault change-master-password
```

程序会交互式提示输入当前主密码，并要求输入两次新主密码，然后使用新密码重新包裹 DEK 并覆盖 `.zkv_master`。

新增条目：

```bash
./build/zkvault add <name>
```

程序会交互式提示输入主密码、条目密码和备注。

读取条目：

```bash
./build/zkvault get <name>
```

更新条目：

```bash
./build/zkvault update <name>
```

程序会交互式提示输入主密码、条目密码和备注。

删除条目：

```bash
./build/zkvault delete <name>
```

列出条目：

```bash
./build/zkvault list
```

说明：
- 除 `delete` 和 `list` 外，命令都会交互式提示输入主密码
- `add` 和 `update` 还会继续交互式输入条目密码和备注，避免敏感内容出现在 shell 历史或进程参数里

条目名限制：
- 只允许字母、数字、`.`、`-`、`_`
- 不允许空串、`.`、`..`
- 不允许路径分隔符、空格和其他特殊字符

## 存储格式

主密钥文件：
- `.zkv_master`

加密条目文件：
- `data/<name>.zkv`

当前版本中，`.zkv_master` 和 `*.zkv` 的外层容器都使用 JSON；真正的业务内容不会以明文写入条目文件。

## 当前边界

已经完成：
- CLI 原型
- 主密码不落盘
- `scrypt` 派生 KEK
- DEK 包裹与解包
- 条目加密存储
- 基础 CLI 冒烟测试

尚未完成：
- 日记条目
- HTTP API / 守护进程形态
- 更完整的错误分类
- 更严格的敏感内存生命周期管理
- 更全面的自动化测试覆盖
- 存储格式升级机制

## 后续方向

- 补全日记模型
- 将 `kdf` 参数显式写入主密钥文件
- 增加单元测试和更多负面场景测试
- 从 CLI 原型演进到服务端架构
