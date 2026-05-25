# 微信文件重复清理监控脚本使用说明

> 快速开始：将`wechat_duplicate_monitor`文件夹放置在微信默认文件接收目录（C:\Users\User-Name\Documents\xwechat_files\wxid_xxxxxxxxxx\msg\file\）下 ，然后运行`启动微信重复文件监控.bat`即可。

本目录中的 `wechat_duplicate_monitor.py` 用于监测微信接收文件目录下的月份子文件夹，查找内容完全相同的重复文件，并只保留创建时间最早的一个文件。

脚本的设计目标是安全清理微信反复转发、发送文件时产生的副本，避免因为文件名相似而误删内容不同的文件。

## 适用目录结构

脚本默认放在 `wechat_duplicate_monitor` 工具文件夹内，并扫描这个工具文件夹的上一级目录中名称符合 `YYYY-MM` 格式的子文件夹，例如：

```text
file/
├── 2026-03/
├── 2026-04/
├── 2026-05/
├── desktop.ini
└── wechat_duplicate_monitor/
    ├── README.md
    ├── wechat_duplicate_monitor.py
    ├── duplicate_monitor.log
    ├── 启动微信重复文件监控.bat
    └── 关闭微信重复文件监控.bat
```

不会扫描根目录下的普通文件，例如 `desktop.ini`、单独放在根目录的表格文件等，也不会扫描 `wechat_duplicate_monitor` 工具文件夹本身。

## 判重规则

脚本不会根据文件名判断重复。

判重流程如下：

1. 扫描所有 `YYYY-MM` 月份目录中的文件。
2. 跳过正在使用、正在编辑、无法访问或仍在变化的文件。
3. 先按文件大小分组，大小不同的文件一定不会互相判重。
4. 对大小相同的文件计算内容哈希，默认使用 `sha256`。
5. 只有大小相同且 `sha256` 完全相同的文件，才会被视为重复文件。
6. 在真正移动或删除前，会再次读取文件并重新校验大小和哈希。
7. 每组重复文件中保留创建时间最早的一个。

因此，下面这种情况不会被误删：

```text
材料.docx
材料(1).docx
```

只要两个文件内容不同，即使名字很像，也不会被当作重复文件。

## 对已打开文件的处理

脚本会跳过正在使用的文件，例如：

- 正在 Word 中打开的 `.docx` 文件；
- 正在 WPS、Excel、PowerPoint 中编辑的文件；
- Office/WPS 产生的 `~$` 开头的临时锁文件；
- 正在被微信、同步软件、杀毒软件等占用的文件。

这些文件不会参与本轮判重，也不会被移动或删除。等文件关闭后，下一轮扫描会再次检查。

## 安装要求

需要 Python 3。

本机已验证可用版本：

```bash
python --version
```

如果能看到类似下面的输出即可：

```text
Python 3.11.3
```

脚本只使用 Python 标准库，不需要额外安装第三方依赖。

## 推荐使用流程

强烈建议按下面顺序使用：

1. 先演练，只看结果，不移动、不删除。
2. 确认输出无误后，使用移动模式，把重复文件移到 `.duplicate_trash`。
3. 观察一段时间，确认没有问题后，再考虑是否使用直接删除模式。

## 常用命令

下面的命令默认在 `wechat_duplicate_monitor` 工具文件夹中运行。

### 1. 查看帮助

```bash
python wechat_duplicate_monitor.py --help
```

### 2. 演练扫描一次，不做任何修改

```bash
python wechat_duplicate_monitor.py --once --stable-seconds 0
```

说明：

- `--once` 表示只扫描一次后退出。
- `--stable-seconds 0` 表示测试时不等待文件稳定，方便立即看到结果。
- 默认 `--action dry-run`，所以不会移动或删除任何文件。

### 3. 持续演练监控

```bash
python wechat_duplicate_monitor.py
```

默认行为：

- 每 30 秒扫描一次；
- 只打印会处理哪些重复文件；
- 不移动、不删除；
- 按 `Ctrl+C` 停止。

### 4. 推荐的安全清理方式：移动重复文件

可以直接双击：

```text
启动微信重复文件监控.bat
```

或在工具文件夹中运行：

```bash
python wechat_duplicate_monitor.py --action move
```

重复文件会被移动到微信文件根目录下的：

```text
.duplicate_trash/
```

例如：

```text
2026-05/材料(1).docx
```

会被移动到：

```text
.duplicate_trash/2026-05/材料(1).docx
```

原来的月份目录中只保留创建时间最早的那一份。

### 5. 直接删除重复文件

确认移动模式长期运行没有问题后，才建议使用：

```bash
python wechat_duplicate_monitor.py --action delete
```

该模式会直接删除重复文件，不经过 `.duplicate_trash`。

如果不确定，请继续使用 `--action move`。

## 命令参数说明

### `--root ROOT`

指定微信文件根目录。

默认值：脚本所在工具文件夹的上一级目录，也就是微信文件月份目录所在的根目录。

示例：

```bash
python wechat_duplicate_monitor.py --root "/c/Users/Administrator/Documents/xwechat_files/wxid_8x3ei7lfrodu22_7fca/msg/file"
```

如果脚本就放在微信文件根目录下的 `wechat_duplicate_monitor` 文件夹中，通常不需要传这个参数。

### `--interval SECONDS`

持续监控时，每隔多少秒扫描一次。

默认值：`30`

示例：

```bash
python wechat_duplicate_monitor.py --action move --interval 60
```

表示每 60 秒扫描一次。

### `--stable-seconds SECONDS`

文件大小和修改时间保持不变多少秒后，才参与判重。

默认值：`3`

用途：避免微信正在接收、Office 正在保存、同步软件正在写入时，脚本误处理尚未稳定的文件。

示例：

```bash
python wechat_duplicate_monitor.py --action move --stable-seconds 30
```

表示文件稳定 30 秒后才参与判重。

### `--action dry-run|move|delete`

指定发现重复文件后的处理方式。

默认值：`dry-run`

可选值：

| 值         | 含义                         | 是否修改文件 |
| --------- | -------------------------- | ------ |
| `dry-run` | 只打印将要处理的重复文件               | 否      |
| `move`    | 移动重复文件到 `.duplicate_trash` | 是      |
| `delete`  | 直接删除重复文件                   | 是      |

推荐长期使用：

```bash
python wechat_duplicate_monitor.py --action move
```

### `--trash-dir PATH`

指定 `--action move` 时重复文件移动到哪里。

默认值：根目录下的 `.duplicate_trash`。

示例：

```bash
python wechat_duplicate_monitor.py --action move --trash-dir "/c/Users/Administrator/Desktop/微信重复文件备份"
```

### `--algorithm ALGORITHM`

指定哈希算法。

默认值：`sha256`

一般不需要修改。

示例：

```bash
python wechat_duplicate_monitor.py --algorithm sha256
```

### `--chunk-size BYTES`

读取文件时的分块大小。

默认值：`1048576`，也就是 1MB。

一般不需要修改。

### `--once`

只扫描一次后退出。

适合测试或手动清理。

示例：

```bash
python wechat_duplicate_monitor.py --once --action move
```

### `--log-file PATH`

指定日志文件路径。

默认值：工具文件夹下的 `duplicate_monitor.log`。

示例：

```bash
python wechat_duplicate_monitor.py --action move --log-file "duplicate_monitor.log"
```

## 日志说明

脚本会同时向终端和日志文件输出信息。

默认日志文件位于工具文件夹内：

```text
wechat_duplicate_monitor/duplicate_monitor.log
```

日志文件会自动整理：

- 始终保留最近一次检查的完整日志；
- 历史日志中只保留真正移动、删除、移动失败、删除失败的记录；
- 更早的“未发现内容完全相同的重复文件”这类空检查记录会自动清掉，避免日志每 30 秒膨胀。

常见日志含义：

```text
未发现内容完全相同的重复文件。
```

表示本轮没有找到重复文件。

```text
保留：2026-05/材料.docx（创建时间 ...，大小 ...，sha256 ...）
```

表示这一组重复文件中保留的文件。

```text
演练：将删除重复文件：2026-05/材料(1).docx
```

表示当前是 `dry-run` 模式，只是提示，不会真的删除。

```text
已移动重复文件：2026-05/材料(1).docx -> .duplicate_trash/2026-05/材料(1).docx
```

表示 `move` 模式下，重复文件已经被移动到回收目录。

```text
跳过正在使用的文件：2026-05/材料.docx
```

表示文件当前被打开或占用，本轮不会处理。

## 安全建议

### 首次运行一定先用演练模式

```bash
python wechat_duplicate_monitor.py --once
```

确认输出符合预期后，再使用 `--action move`。

### 长期运行建议使用移动模式

推荐：

```bash
python wechat_duplicate_monitor.py --action move
```

不推荐一开始就使用：

```bash
python wechat_duplicate_monitor.py --action delete
```

### 定期检查 `.duplicate_trash`

如果使用 `--action move`，重复文件不会立即消失，而是被集中移动到 `.duplicate_trash`。

确认里面的文件确实不需要后，再手动清空该目录。

### 不要在正在大量接收文件时直接删除

如果微信正在批量接收文件、网盘正在同步、Office 正在保存文件，建议继续使用默认的 `--stable-seconds 3` 或调大到 `30`。

示例：

```bash
python wechat_duplicate_monitor.py --action move --stable-seconds 30
```

## 典型使用场景

### 场景一：手动清理一次

```bash
python wechat_duplicate_monitor.py --once --action move
```

适合偶尔运行一次，把当前已有重复文件移动到 `.duplicate_trash`。

### 场景二：双击启动并在后台持续监控

双击：

```text
启动微信重复文件监控.bat
```

脚本会以 `--action move` 模式运行，每 30 秒扫描一次。

停止方式：双击：

```text
关闭微信重复文件监控.bat
```

### 场景三：降低扫描频率

```bash
python wechat_duplicate_monitor.py --action move --interval 300
```

表示每 5 分钟扫描一次。

### 场景四：只看会删除什么

```bash
python wechat_duplicate_monitor.py --once --action dry-run
```

不会移动或删除任何文件。

## 常见问题

### 文件名一样或相似就会被删除吗？

不会。

脚本不按文件名判重，只按文件内容哈希判重。

### `材料.docx` 和 `材料(1).docx` 内容不同，会删除吗？

不会。

只要内容不同，哈希就不同，不会被视为重复文件。

### 如果两个文件内容一样，但文件名完全不同，会处理吗？

会。

因为脚本判断的是内容是否完全一致，不依赖文件名。

### 会删除正在打开的 Word 文件吗？

不会主动处理正在使用的文件。

脚本会跳过被占用的文件，等文件关闭后再参与下一轮扫描。

### 保留哪一个文件？

保留创建时间最早的文件。

如果创建时间完全相同，则按路径名称排序后保留排在前面的那个。

### `.duplicate_trash` 里的文件会再次被扫描吗？

默认不会。

脚本只扫描 `YYYY-MM` 格式的月份目录，`.duplicate_trash` 不符合这个格式。

### 空文件会被处理吗？

会。

如果多个空文件内容完全一致，脚本也会只保留创建时间最早的一个。

### 脚本运行时可以继续使用微信吗？

可以。

脚本会跳过正在变化或正在使用的文件。

## 故障排查

### 提示找不到月份目录

如果出现类似提示：

```text
未找到 YYYY-MM 格式的月份子文件夹
```

说明当前运行脚本的位置不对，或者需要手动指定 `--root`。

请先确认你所在目录下有类似 `2026-05` 的子文件夹。

### 终端中文显示乱码

日志文件 `duplicate_monitor.log` 使用 UTF-8 编码保存。

如果终端显示乱码，可以优先查看日志文件，或者使用支持 UTF-8 的终端。

### 移动失败或删除失败

常见原因：

- 文件正在被其他程序占用；
- 权限不足；
- 文件被同步软件或杀毒软件短暂锁定；
- 文件路径过长或目标磁盘不可写。

通常可以稍后再运行一次。

## 推荐命令总结

首次检查：

```bash
python wechat_duplicate_monitor.py --once
```

安全清理一次：

```bash
python wechat_duplicate_monitor.py --once --action move
```

长期监控：

```bash
python wechat_duplicate_monitor.py --action move
```

直接删除，不推荐首次使用：

```bash
python wechat_duplicate_monitor.py --action delete
```
