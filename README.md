# 微信重复文件监控

一个用于自动清理微信接收文件重复副本的 Windows 小工具。

程序会监控微信文件目录下的 `YYYY-MM` 月份文件夹，按文件内容计算 `sha256` 哈希，找出内容完全一致的重复文件，并保留创建时间最早的一份。当前推荐使用低内存 C 语言版本；原 Python 版本保留在单独文件夹中，方便需要时备用。

## 功能特点

- 只扫描 `YYYY-MM` 格式的月份目录，不扫描工具目录本身。
- 不按文件名判重，只按文件大小和内容哈希判重。
- 默认保留创建时间最早的文件。
- 跳过正在使用、正在编辑、无法访问或仍在变化的文件。
- 支持演练、移动到回收目录、直接删除三种模式。
- 支持双击 BAT 脚本后台启动和停止。
- 日志会自动整理，避免长期运行后日志无限增长。
- C 版无需 Python 环境，适合低内存常驻运行。

## 目录结构

建议把 `wechat_duplicate_monitor` 文件夹放在微信文件接收目录下，例如：

```text
file/
├── 2026-03/
├── 2026-04/
├── 2026-05/
├── desktop.ini
└── wechat_duplicate_monitor/
    ├── README.md
    ├── .gitignore
    ├── wechat_duplicate_monitor.c
    ├── wechat_duplicate_monitor.exe
    ├── 启动微信重复文件监控_C版.bat
    ├── 关闭微信重复文件监控_C版.bat
    └── PythonVersion-运行Python版需将PythonVersion文件夹内所有文件放到wechat_duplicate_monitor目录下运行/
        ├── wechat_duplicate_monitor.py
        ├── 启动微信重复文件监控.bat
        └── 关闭微信重复文件监控.bat
```

C 版程序默认扫描工具文件夹的上一级目录，也就是上面示例中的 `file/`。

不会扫描根目录下的普通文件，也不会扫描 `.duplicate_trash` 或 `wechat_duplicate_monitor` 工具目录。

## 快速开始：推荐 C 版

### 双击后台启动

1. 把 `wechat_duplicate_monitor` 文件夹放到微信文件接收目录下。
2. 确认目录里存在 `wechat_duplicate_monitor.exe`。
3. 双击运行：

```text
启动微信重复文件监控_C版.bat
```

C 版 BAT 会以 `move` 模式后台启动。发现重复文件后，会把重复文件移动到微信文件根目录下的：

```text
.duplicate_trash/
```

停止后台监控时双击：

```text
关闭微信重复文件监控_C版.bat
```

### 命令行运行

先演练扫描一次，不移动、不删除：

```bash
wechat_duplicate_monitor.exe --once --stable-seconds 0
```

确认输出无误后，移动重复文件到 `.duplicate_trash`：

```bash
wechat_duplicate_monitor.exe --once --action move
```

长期监控：

```bash
wechat_duplicate_monitor.exe --action move
```

## 编译 C 版

本项目提供 C 源码 `wechat_duplicate_monitor.c`。在 Windows 上可使用 MinGW GCC 编译：

```bash
C:\lib\mingw64\bin\gcc.exe -O2 -o wechat_duplicate_monitor.exe wechat_duplicate_monitor.c -s
```

编译完成后会生成：

```text
wechat_duplicate_monitor.exe
```

## C 版参数说明

```bash
wechat_duplicate_monitor.exe [选项]
```

| 参数                               | 默认值                            | 说明                   |
| -------------------------------- | ------------------------------ | -------------------- |
| `--root PATH`                    | 程序所在文件夹的上一级目录                  | 指定微信文件根目录            |
| `--interval SECONDS`             | `3`                            | 持续监控时的扫描间隔           |
| `--stable-seconds SECONDS`       | `3`                            | 文件大小和修改时间保持不变多久后参与判重 |
| `--action dry-run\|move\|delete` | `dry-run`                      | 处理方式                 |
| `--trash-dir PATH`               | 根目录下的 `.duplicate_trash`       | `move` 模式的移动目标目录     |
| `--algorithm sha256`             | `sha256`                       | C 版当前支持 `sha256`     |
| `--chunk-size BYTES`             | `1048576`                      | 读取文件时的分块大小           |
| `--once`                         | 关闭                             | 只扫描一次后退出             |
| `--log-file PATH`                | 程序目录下的 `duplicate_monitor.log` | 指定日志文件               |
| `--help`                         | -                              | 查看帮助                 |

### 处理方式

| 模式        | 含义                          | 是否修改文件 |
| --------- | --------------------------- | ------ |
| `dry-run` | 只打印将要处理的重复文件                | 否      |
| `move`    | 把重复文件移动到 `.duplicate_trash` | 是      |
| `delete`  | 直接删除重复文件                    | 是      |

建议长期使用 `move`，确认 `.duplicate_trash` 中的文件确实不需要后，再手动清空。

## Python 版

Python 版现在放在：

```text
PythonVersion-运行Python版需将PythonVersion文件夹内所有文件放到wechat_duplicate_monitor目录下运行/
```

如果要运行 Python 版，请先把这个文件夹内的全部文件复制到 `wechat_duplicate_monitor` 根目录，也就是和 `README.md`、`wechat_duplicate_monitor.exe` 同一级的位置。

复制后可以运行：

```bash
python wechat_duplicate_monitor.py --once
```

移动重复文件：

```bash
python wechat_duplicate_monitor.py --action move
```

后台启动和停止：

```text
启动微信重复文件监控.bat
关闭微信重复文件监控.bat
```

Python 版只使用标准库，不需要额外安装第三方依赖，但需要本机已安装 Python 3。

## BAT 脚本说明

| 文件                                 | 版本       | 用途                                        |
| ---------------------------------- | -------- | ----------------------------------------- |
| `启动微信重复文件监控_C版.bat`                | C 版      | 后台启动 `wechat_duplicate_monitor.exe`       |
| `关闭微信重复文件监控_C版.bat`                | C 版      | 停止 C 版后台监控                                |
| `PythonVersion-.../启动微信重复文件监控.bat` | Python 版 | 复制到根目录后后台启动 `wechat_duplicate_monitor.py` |
| `PythonVersion-.../关闭微信重复文件监控.bat` | Python 版 | 复制到根目录后停止 Python 版后台监控                    |

C 版 BAT 使用独立的运行文件，避免和 Python 版互相影响：

| 文件                        | 用途         |
| ------------------------- | ---------- |
| `duplicate_monitor_c.pid` | C 版后台进程 ID |
| `duplicate_monitor_c.log` | C 版监控日志    |

Python 版对应文件为：

| 文件                    | 用途              |
| --------------------- | --------------- |
| `duplicate_monitor.pid` | Python 版后台进程 ID |
| `duplicate_monitor.log` | Python 版监控日志    |

## 判重规则

程序不会根据文件名判断重复。

判重流程：

1. 扫描所有 `YYYY-MM` 月份目录中的文件。
2. 跳过正在使用、无法访问或仍在变化的文件。
3. 先按文件大小分组，大小不同的文件不会互相判重。
4. 对大小相同的文件计算 `sha256`。
5. 只有大小相同且 `sha256` 完全相同的文件，才会被视为重复文件。
6. 在真正移动或删除前，会再次读取文件并复核大小和哈希。
7. 每组重复文件中保留创建时间最早的一份。

例如下面两个文件即使文件名相似，也不会因为名字相似而被误删：

```text
材料.docx
材料(1).docx
```

只有它们的内容完全一致时，才会被视为重复文件。

## 对正在使用文件的处理

程序会跳过正在使用的文件，例如：

- 正在 Word、Excel、PowerPoint、WPS 中打开的文件；
- Office/WPS 产生的 `~$` 开头临时锁文件；
- 正在被微信、同步软件、杀毒软件占用的文件；
- 大小或修改时间仍在变化的文件。

这些文件不会参与本轮判重。等文件关闭或稳定后，下一轮扫描会再次检查。

## 日志说明

双击 BAT 后只会写入一个主日志文件；命令行直接运行时也会输出到终端。日志会自动整理，避免默认每 3 秒扫描时持续膨胀。

常见日志：

```text
未发现内容完全相同的重复文件。
```

表示本轮没有找到重复文件。

```text
保留：2026-05\材料.docx（创建时间 ...，大小 ...，sha256 ...）
```

表示这一组重复文件中保留的文件。

```text
演练：将删除重复文件：2026-05\材料(1).docx
```

表示当前是 `dry-run` 模式，不会真的移动或删除。

```text
已移动重复文件：2026-05\材料(1).docx -> .duplicate_trash\2026-05\材料(1).docx
```

表示 `move` 模式下重复文件已经被移动到回收目录。

```text
跳过正在使用的文件：2026-05\材料.docx
```

表示文件当前被打开或占用，本轮不会处理。

## 安全建议

首次使用建议先运行演练模式：

```bash
wechat_duplicate_monitor.exe --once
```

确认输出符合预期后，再使用移动模式：

```bash
wechat_duplicate_monitor.exe --once --action move
```

不建议一开始就使用直接删除模式：

```bash
wechat_duplicate_monitor.exe --action delete
```

如果微信正在批量接收文件、网盘正在同步、Office 正在保存文件，可以调大稳定等待时间：

```bash
wechat_duplicate_monitor.exe --action move --stable-seconds 30
```

## 常见问题

### 文件名一样或相似就会被处理吗？

不会。程序只按内容哈希判重，不按文件名判重。

### 内容一样但文件名完全不同，会处理吗？

会。只要文件大小相同且内容哈希完全一致，就会被视为重复文件。

### `.duplicate_trash` 里的文件会再次被扫描吗？

默认不会。程序只扫描 `YYYY-MM` 格式的月份目录。

### 空文件会被处理吗？

会。多个空文件内容完全一致，也会只保留创建时间最早的一份。

### C 版和 Python 版可以同时运行吗？

不建议同时运行。虽然两者使用不同的 PID 和日志文件，但它们扫描的是同一批微信文件，同时运行没有必要。

### 提示找不到月份目录怎么办？

请确认工具文件夹的上一级目录中存在类似 `2026-05` 的月份文件夹。也可以手动指定根目录：

```bash
wechat_duplicate_monitor.exe --root "C:\Users\你的用户名\Documents\xwechat_files\wxid_xxx\msg\file" --once
```

