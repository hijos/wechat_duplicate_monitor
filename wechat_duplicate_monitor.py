#!/usr/bin/env python3
import argparse
import ctypes
import ctypes.wintypes
import hashlib
import logging
import os
import shutil
import sys
import time
from collections import defaultdict
from pathlib import Path

APP_DIR = Path(__file__).resolve().parent
DEFAULT_ROOT = APP_DIR.parent
MONTH_DIR_LENGTH = 7
HASH_ALGORITHMS = tuple(sorted(name for name in hashlib.algorithms_guaranteed if not name.startswith("shake_")))
WINDOWS_SHARING_VIOLATION_ERRORS = {32, 33}
SCAN_START_MESSAGE = "===== 本轮检查开始 ====="
SCAN_END_MESSAGE = "===== 本轮检查结束 ====="
PERSISTENT_LOG_KEYWORDS = ("已移动重复文件：", "已删除重复文件：", "移动失败：", "删除失败：")


def is_month_dir(path: Path) -> bool:
    name = path.name
    return (
        path.is_dir()
        and len(name) == MONTH_DIR_LENGTH
        and name[:4].isdigit()
        and name[4] == "-"
        and name[5:7].isdigit()
        and 1 <= int(name[5:7]) <= 12
    )


def file_identity(path: Path, algorithm: str, chunk_size: int) -> str:
    digest = hashlib.new(algorithm)
    with path.open("rb") as file:
        for chunk in iter(lambda: file.read(chunk_size), b""):
            digest.update(chunk)
    return digest.hexdigest()


def created_time(stat_result: os.stat_result) -> float:
    return stat_result.st_ctime


def is_file_in_use(path: Path) -> bool:
    if path.name.startswith("~$"):
        return True
    if sys.platform != "win32":
        return False

    kernel32 = ctypes.WinDLL("kernel32", use_last_error=True)
    kernel32.CreateFileW.argtypes = [
        ctypes.wintypes.LPCWSTR,
        ctypes.wintypes.DWORD,
        ctypes.wintypes.DWORD,
        ctypes.wintypes.LPVOID,
        ctypes.wintypes.DWORD,
        ctypes.wintypes.DWORD,
        ctypes.wintypes.HANDLE,
    ]
    kernel32.CreateFileW.restype = ctypes.wintypes.HANDLE
    kernel32.CloseHandle.argtypes = [ctypes.wintypes.HANDLE]
    kernel32.CloseHandle.restype = ctypes.wintypes.BOOL

    delete_access = 0x00010000
    file_read_attributes = 0x00000080
    open_existing = 3
    file_attribute_normal = 0x00000080
    invalid_handle = ctypes.c_void_p(-1).value

    handle = kernel32.CreateFileW(
        str(path),
        delete_access | file_read_attributes,
        0,
        None,
        open_existing,
        file_attribute_normal,
        None,
    )
    if handle == invalid_handle:
        return ctypes.get_last_error() in WINDOWS_SHARING_VIOLATION_ERRORS

    kernel32.CloseHandle(handle)
    return False


def iter_files(root: Path):
    for month_dir in sorted((item for item in root.iterdir() if is_month_dir(item)), key=lambda item: item.name):
        for dirpath, dirnames, filenames in os.walk(month_dir):
            dirnames[:] = [name for name in dirnames if not name.startswith(".")]
            for filename in filenames:
                path = Path(dirpath) / filename
                try:
                    if path.is_file() and not path.is_symlink():
                        yield path
                except OSError as error:
                    logging.warning("跳过无法访问的文件：%s（%s）", path, error)


def update_stable_files(root: Path, seen: dict[str, tuple[int, int, float]], stable_seconds: float):
    now = time.time()
    stable_files = []
    current_paths = set()

    for path in iter_files(root):
        path_key = str(path)
        current_paths.add(path_key)
        try:
            stat_result = path.stat()
        except OSError as error:
            logging.warning("跳过无法读取状态的文件：%s（%s）", path, error)
            continue

        if is_file_in_use(path):
            seen.pop(path_key, None)
            logging.info("跳过正在使用的文件：%s", relative(path, root))
            continue

        signature = (stat_result.st_size, stat_result.st_mtime_ns)
        previous = seen.get(path_key)
        if previous is None or previous[:2] != signature:
            seen[path_key] = (signature[0], signature[1], now)
            if stable_seconds <= 0:
                stable_files.append((path, stat_result))
            continue

        first_stable_at = previous[2]
        if now - first_stable_at >= stable_seconds:
            stable_files.append((path, stat_result))

    for path_key in list(seen):
        if path_key not in current_paths:
            seen.pop(path_key, None)

    return stable_files


def find_duplicate_groups(files, algorithm: str, chunk_size: int):
    by_size = defaultdict(list)
    for path, stat_result in files:
        if stat_result.st_size == 0:
            by_size[0].append((path, stat_result))
        elif stat_result.st_size > 0:
            by_size[stat_result.st_size].append((path, stat_result))

    duplicate_groups = []
    for size, same_size_files in by_size.items():
        if len(same_size_files) < 2:
            continue

        by_hash = defaultdict(list)
        for path, stat_result in same_size_files:
            try:
                digest = file_identity(path, algorithm, chunk_size)
            except OSError as error:
                logging.warning("跳过无法读取内容的文件：%s（%s）", path, error)
                continue
            by_hash[digest].append((path, stat_result))

        for digest, same_hash_files in by_hash.items():
            if len(same_hash_files) > 1:
                duplicate_groups.append((size, digest, same_hash_files))

    return duplicate_groups


def choose_keeper(files):
    return min(files, key=lambda item: (created_time(item[1]), str(item[0]).lower()))


def relative(path: Path, root: Path) -> str:
    try:
        return str(path.relative_to(root))
    except ValueError:
        return str(path)


def move_duplicate(path: Path, root: Path, trash_dir: Path):
    destination = trash_dir / path.relative_to(root)
    destination.parent.mkdir(parents=True, exist_ok=True)

    if destination.exists():
        stem = destination.stem
        suffix = destination.suffix
        counter = 1
        while True:
            candidate = destination.with_name(f"{stem}.{counter}{suffix}")
            if not candidate.exists():
                destination = candidate
                break
            counter += 1

    shutil.move(str(path), str(destination))
    return destination


def remove_duplicates(root: Path, duplicate_groups, algorithm: str, chunk_size: int, action: str, trash_dir: Path):
    removed_count = 0
    saved_bytes = 0

    for size, digest, files in duplicate_groups:
        current_files = []
        for path, _stat_result in files:
            if is_file_in_use(path):
                logging.info("跳过删除前仍在使用的文件：%s", relative(path, root))
                continue

            try:
                current_stat = path.stat()
                current_digest = file_identity(path, algorithm, chunk_size)
            except OSError as error:
                logging.warning("跳过删除前无法复核的文件：%s（%s）", path, error)
                continue

            if current_stat.st_size == size and current_digest == digest:
                current_files.append((path, current_stat))
            else:
                logging.warning("跳过已变化的文件：%s", relative(path, root))

        if len(current_files) < 2:
            continue

        keeper, keeper_stat = choose_keeper(current_files)
        logging.info("保留：%s（创建时间 %.0f，大小 %d，%s %s）", relative(keeper, root), created_time(keeper_stat), size, algorithm, digest)

        for path, _stat_result in current_files:
            if path == keeper:
                continue

            if action == "dry-run":
                logging.info("演练：将删除重复文件：%s", relative(path, root))
            elif action == "move":
                try:
                    destination = move_duplicate(path, root, trash_dir)
                except OSError as error:
                    logging.error("移动失败：%s（%s）", path, error)
                    continue
                logging.info("已移动重复文件：%s -> %s", relative(path, root), relative(destination, root))
            else:
                try:
                    path.unlink()
                except OSError as error:
                    logging.error("删除失败：%s（%s）", path, error)
                    continue
                logging.info("已删除重复文件：%s", relative(path, root))

            removed_count += 1
            saved_bytes += size

    return removed_count, saved_bytes


def scan_once(root: Path, seen: dict[str, tuple[int, int, float]], args):
    stable_files = update_stable_files(root, seen, args.stable_seconds)
    duplicate_groups = find_duplicate_groups(stable_files, args.algorithm, args.chunk_size)

    if not duplicate_groups:
        logging.info("未发现内容完全相同的重复文件。")
        return

    removed_count, saved_bytes = remove_duplicates(
        root,
        duplicate_groups,
        args.algorithm,
        args.chunk_size,
        args.action,
        args.trash_dir,
    )
    logging.info("本轮处理重复文件 %d 个，涉及 %.2f MB。", removed_count, saved_bytes / 1024 / 1024)


def is_persistent_history_log(line: str) -> bool:
    return any(keyword in line for keyword in PERSISTENT_LOG_KEYWORDS)


def compact_log_file(log_file: Path):
    file_handlers = [handler for handler in logging.getLogger().handlers if isinstance(handler, logging.FileHandler)]
    acquired_handlers = []
    try:
        for handler in file_handlers:
            handler.acquire()
            acquired_handlers.append(handler)
            handler.flush()

        try:
            lines = log_file.read_text(encoding="utf-8").splitlines(keepends=True)
        except OSError:
            return

        latest_scan_index = None
        for index in range(len(lines) - 1, -1, -1):
            if SCAN_START_MESSAGE in lines[index]:
                latest_scan_index = index
                break

        if latest_scan_index is None:
            return

        compacted_lines = [line for line in lines[:latest_scan_index] if is_persistent_history_log(line)]
        compacted_lines.extend(lines[latest_scan_index:])
        log_file.write_text("".join(compacted_lines), encoding="utf-8")

        resolved_log_file = log_file.resolve()
        for handler in file_handlers:
            if Path(handler.baseFilename).resolve() == resolved_log_file and handler.stream is not None:
                handler.stream.seek(0, os.SEEK_END)
    finally:
        for handler in reversed(acquired_handlers):
            handler.release()


def parse_args():
    parser = argparse.ArgumentParser(description="监测微信文件月份目录，按内容哈希删除重复文件，保留创建时间最早的一个。")
    parser.add_argument("--root", type=Path, default=DEFAULT_ROOT, help="微信文件根目录，默认是脚本所在文件夹的上一级目录。")
    parser.add_argument("--interval", type=float, default=30, help="监测扫描间隔秒数，默认 30。")
    parser.add_argument("--stable-seconds", type=float, default=3, help="文件大小和修改时间保持不变多少秒后才参与判重，默认 3。")
    parser.add_argument("--action", choices=("dry-run", "move", "delete"), default="dry-run", help="处理方式：dry-run 只打印；move 移到回收目录；delete 直接删除。默认 dry-run。")
    parser.add_argument("--trash-dir", type=Path, default=None, help="action=move 时的移动目录，默认是根目录下的 .duplicate_trash。")
    parser.add_argument("--algorithm", choices=HASH_ALGORITHMS, default="sha256", help="哈希算法，默认 sha256。")
    parser.add_argument("--chunk-size", type=int, default=1024 * 1024, help="读取文件块大小，默认 1MB。")
    parser.add_argument("--once", action="store_true", help="只扫描一次后退出。")
    parser.add_argument("--log-file", type=Path, default=APP_DIR / "duplicate_monitor.log", help="日志文件路径，默认在脚本所在文件夹内。")
    return parser.parse_args()


def configure_console_encoding():
    for stream in (sys.stdout, sys.stderr):
        if hasattr(stream, "reconfigure"):
            stream.reconfigure(encoding="utf-8", errors="replace")


def configure_logging(log_file: Path):
    log_file.parent.mkdir(parents=True, exist_ok=True)
    formatter = logging.Formatter("%(asctime)s %(levelname)s %(message)s")

    handlers = []
    if sys.stdout is not None:
        console_handler = logging.StreamHandler(sys.stdout)
        console_handler.setFormatter(formatter)
        handlers.append(console_handler)

    file_handler = logging.FileHandler(log_file, encoding="utf-8")
    file_handler.setFormatter(formatter)
    handlers.append(file_handler)

    logging.basicConfig(level=logging.INFO, handlers=handlers)


def main():
    configure_console_encoding()
    args = parse_args()
    args.root = args.root.resolve()
    args.trash_dir = (args.trash_dir or args.root / ".duplicate_trash").resolve()
    args.log_file = args.log_file.resolve()
    configure_logging(args.log_file)

    if not args.root.exists() or not args.root.is_dir():
        raise SystemExit(f"根目录不存在或不是文件夹：{args.root}")

    month_dirs = [path for path in args.root.iterdir() if is_month_dir(path)]
    if not month_dirs:
        raise SystemExit(f"未找到 YYYY-MM 格式的月份子文件夹：{args.root}")

    logging.info("开始监测：%s", args.root)
    logging.info("处理方式：%s；只扫描 YYYY-MM 月份目录；只删除内容哈希完全一致的文件。", args.action)

    seen = {}
    while True:
        logging.info(SCAN_START_MESSAGE)
        scan_once(args.root, seen, args)
        logging.info(SCAN_END_MESSAGE)
        compact_log_file(args.log_file)
        if args.once:
            break
        time.sleep(args.interval)


if __name__ == "__main__":
    main()
