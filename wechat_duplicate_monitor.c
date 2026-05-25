#define UNICODE
#define _UNICODE
#define WIN32_LEAN_AND_MEAN
#define _CRT_SECURE_NO_WARNINGS

#include <windows.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <wchar.h>
#include <stdarg.h>
#include <time.h>
#include <stdbool.h>

#define MONTH_DIR_LENGTH 7
#define DEFAULT_INTERVAL_SECONDS 3.0
#define DEFAULT_STABLE_SECONDS 3.0
#define DEFAULT_CHUNK_SIZE (1024 * 1024)
#define SHA256_DIGEST_LENGTH 32
#define MAX_READ_CHUNK (64 * 1024 * 1024)

static const wchar_t *SCAN_START_MESSAGE = L"===== 本轮检查开始 =====";
static const wchar_t *SCAN_END_MESSAGE = L"===== 本轮检查结束 =====";
static const wchar_t *PERSISTENT_LOG_KEYWORDS[] = {
    L"已移动重复文件：",
    L"已删除重复文件：",
    L"移动失败：",
    L"删除失败："
};

static wchar_t *g_log_file = NULL;

typedef enum {
    ACTION_DRY_RUN,
    ACTION_MOVE,
    ACTION_DELETE
} Action;

typedef struct {
    wchar_t *app_dir;
    wchar_t *root;
    double interval;
    double stable_seconds;
    Action action;
    wchar_t *trash_dir;
    wchar_t *algorithm;
    size_t chunk_size;
    int once;
    wchar_t *log_file;
} Options;

typedef struct {
    wchar_t *path;
    uint64_t size;
    FILETIME creation_time;
    FILETIME write_time;
} FileInfo;

typedef struct {
    FileInfo *items;
    size_t count;
    size_t capacity;
} FileList;

typedef struct {
    wchar_t *path;
    uint64_t size;
    FILETIME write_time;
    double first_stable_at;
    int current;
} SeenEntry;

typedef struct {
    SeenEntry *items;
    size_t count;
    size_t capacity;
} SeenList;

typedef struct {
    wchar_t *name;
    wchar_t *path;
} DirEntry;

typedef struct {
    DirEntry *items;
    size_t count;
    size_t capacity;
} DirList;

typedef struct {
    FileInfo *file;
    uint8_t digest[SHA256_DIGEST_LENGTH];
    wchar_t hex[SHA256_DIGEST_LENGTH * 2 + 1];
} HashEntry;

typedef struct {
    const wchar_t *path;
    uint64_t size;
    FILETIME creation_time;
    FILETIME write_time;
} CurrentFile;

typedef struct {
    CurrentFile *items;
    size_t count;
    size_t capacity;
} CurrentFileList;

typedef struct {
    int duplicate_groups;
    int removed_count;
    uint64_t saved_bytes;
} ProcessResult;

typedef struct {
    char *data;
    size_t length;
    size_t capacity;
} ByteBuffer;

static void *xmalloc(size_t size) {
    void *ptr = malloc(size ? size : 1);
    if (!ptr) {
        fputs("Out of memory\n", stderr);
        ExitProcess(1);
    }
    return ptr;
}

static void *xrealloc(void *ptr, size_t size) {
    void *new_ptr = realloc(ptr, size ? size : 1);
    if (!new_ptr) {
        fputs("Out of memory\n", stderr);
        ExitProcess(1);
    }
    return new_ptr;
}

static wchar_t *xwcsdup(const wchar_t *value) {
    size_t length = wcslen(value);
    wchar_t *copy = (wchar_t *)xmalloc((length + 1) * sizeof(wchar_t));
    memcpy(copy, value, (length + 1) * sizeof(wchar_t));
    return copy;
}

static int is_space_w(wchar_t ch) {
    return ch == L' ' || ch == L'\t' || ch == L'\r' || ch == L'\n';
}

static int is_digit_w(wchar_t ch) {
    return ch >= L'0' && ch <= L'9';
}

static int is_path_sep(wchar_t ch) {
    return ch == L'\\' || ch == L'/';
}

static const wchar_t *path_basename_ptr(const wchar_t *path) {
    const wchar_t *last = path;
    for (const wchar_t *p = path; *p; ++p) {
        if (is_path_sep(*p)) {
            last = p + 1;
        }
    }
    return last;
}

static wchar_t *path_join(const wchar_t *left, const wchar_t *right) {
    if (!left || !*left) {
        return xwcsdup(right);
    }
    if (!right || !*right) {
        return xwcsdup(left);
    }

    size_t left_len = wcslen(left);
    size_t right_len = wcslen(right);
    int needs_sep = !is_path_sep(left[left_len - 1]);
    wchar_t *joined = (wchar_t *)xmalloc((left_len + needs_sep + right_len + 1) * sizeof(wchar_t));
    wcscpy(joined, left);
    if (needs_sep) {
        joined[left_len] = L'\\';
        joined[left_len + 1] = L'\0';
    }
    wcscat(joined, right);
    return joined;
}

static wchar_t *path_dirname(const wchar_t *path) {
    size_t length = wcslen(path);
    while (length > 0 && is_path_sep(path[length - 1])) {
        if (length == 3 && path[1] == L':') {
            break;
        }
        --length;
    }

    size_t pos = length;
    while (pos > 0 && !is_path_sep(path[pos - 1])) {
        --pos;
    }

    if (pos == 0) {
        return xwcsdup(L".");
    }

    if (pos == 3 && path[1] == L':') {
        wchar_t *result = (wchar_t *)xmalloc(4 * sizeof(wchar_t));
        wcsncpy(result, path, 3);
        result[3] = L'\0';
        return result;
    }

    while (pos > 1 && is_path_sep(path[pos - 1])) {
        if (pos == 3 && path[1] == L':') {
            break;
        }
        --pos;
    }

    wchar_t *result = (wchar_t *)xmalloc((pos + 1) * sizeof(wchar_t));
    wcsncpy(result, path, pos);
    result[pos] = L'\0';
    return result;
}

static wchar_t *absolute_path(const wchar_t *path) {
    DWORD needed = GetFullPathNameW(path, 0, NULL, NULL);
    if (needed == 0) {
        return xwcsdup(path);
    }

    wchar_t *buffer = (wchar_t *)xmalloc((needed + 1) * sizeof(wchar_t));
    DWORD written = GetFullPathNameW(path, needed + 1, buffer, NULL);
    if (written == 0 || written > needed) {
        free(buffer);
        return xwcsdup(path);
    }
    return buffer;
}

static int path_exists(const wchar_t *path) {
    return GetFileAttributesW(path) != INVALID_FILE_ATTRIBUTES;
}

static int is_directory_path(const wchar_t *path) {
    DWORD attrs = GetFileAttributesW(path);
    return attrs != INVALID_FILE_ATTRIBUTES && (attrs & FILE_ATTRIBUTE_DIRECTORY);
}

static int ensure_dir(const wchar_t *path) {
    if (!path || !*path || wcscmp(path, L".") == 0) {
        return 1;
    }

    wchar_t *temp = xwcsdup(path);
    size_t len = wcslen(temp);
    while (len > 0 && is_path_sep(temp[len - 1])) {
        if (len == 3 && temp[1] == L':') {
            break;
        }
        temp[--len] = L'\0';
    }

    for (wchar_t *p = temp; *p; ++p) {
        if (!is_path_sep(*p)) {
            continue;
        }
        if (p == temp) {
            continue;
        }
        if (p == temp + 2 && temp[1] == L':') {
            continue;
        }

        wchar_t saved = *p;
        *p = L'\0';
        if (*temp && !CreateDirectoryW(temp, NULL)) {
            DWORD error = GetLastError();
            if (error != ERROR_ALREADY_EXISTS) {
                free(temp);
                return 0;
            }
        }
        *p = saved;
    }

    if (!CreateDirectoryW(temp, NULL)) {
        DWORD error = GetLastError();
        if (error != ERROR_ALREADY_EXISTS) {
            free(temp);
            return 0;
        }
    }

    free(temp);
    return 1;
}

static int ensure_parent_dir(const wchar_t *file_path) {
    wchar_t *parent = path_dirname(file_path);
    int ok = ensure_dir(parent);
    free(parent);
    return ok;
}

static wchar_t *relative_path(const wchar_t *path, const wchar_t *root) {
    size_t root_len = wcslen(root);
    if (root_len > 0 && is_path_sep(root[root_len - 1])) {
        if (_wcsnicmp(path, root, root_len) == 0) {
            return xwcsdup(path + root_len);
        }
    } else if (_wcsnicmp(path, root, root_len) == 0) {
        if (path[root_len] == L'\0') {
            return xwcsdup(L".");
        }
        if (is_path_sep(path[root_len])) {
            return xwcsdup(path + root_len + 1);
        }
    }
    return xwcsdup(path);
}

static char *wide_to_utf8(const wchar_t *value, int *byte_count) {
    int needed = WideCharToMultiByte(CP_UTF8, 0, value, -1, NULL, 0, NULL, NULL);
    if (needed <= 0) {
        char *empty = (char *)xmalloc(1);
        empty[0] = '\0';
        if (byte_count) {
            *byte_count = 0;
        }
        return empty;
    }

    char *buffer = (char *)xmalloc((size_t)needed);
    int written = WideCharToMultiByte(CP_UTF8, 0, value, -1, buffer, needed, NULL, NULL);
    if (written <= 0) {
        buffer[0] = '\0';
        if (byte_count) {
            *byte_count = 0;
        }
        return buffer;
    }

    if (byte_count) {
        *byte_count = written - 1;
    }
    return buffer;
}

static wchar_t *windows_error_message(DWORD error) {
    wchar_t *system_message = NULL;
    DWORD flags = FORMAT_MESSAGE_ALLOCATE_BUFFER | FORMAT_MESSAGE_FROM_SYSTEM | FORMAT_MESSAGE_IGNORE_INSERTS;
    DWORD length = FormatMessageW(flags, NULL, error, 0, (LPWSTR)&system_message, 0, NULL);
    if (length == 0 || !system_message) {
        wchar_t fallback[64];
        swprintf(fallback, 64, L"Win32 错误 %lu", (unsigned long)error);
        return xwcsdup(fallback);
    }

    while (length > 0 && (system_message[length - 1] == L'\r' || system_message[length - 1] == L'\n' || system_message[length - 1] == L' ' || system_message[length - 1] == L'.')) {
        system_message[--length] = L'\0';
    }

    wchar_t *copy = xwcsdup(system_message);
    LocalFree(system_message);
    return copy;
}

static wchar_t *format_wide_message(const wchar_t *format, va_list args) {
    size_t capacity = 1024;
    for (;;) {
        wchar_t *buffer = (wchar_t *)xmalloc(capacity * sizeof(wchar_t));
        va_list copy;
        va_copy(copy, args);
        int written = vswprintf(buffer, capacity, format, copy);
        va_end(copy);
        if (written >= 0 && (size_t)written < capacity) {
            return buffer;
        }
        free(buffer);
        capacity *= 2;
    }
}

static void write_line_to_console(const wchar_t *line) {
    HANDLE stdout_handle = GetStdHandle(STD_OUTPUT_HANDLE);
    DWORD mode = 0;
    if (stdout_handle != INVALID_HANDLE_VALUE && stdout_handle != NULL && GetConsoleMode(stdout_handle, &mode)) {
        DWORD written = 0;
        WriteConsoleW(stdout_handle, line, (DWORD)wcslen(line), &written, NULL);
        return;
    }

    int byte_count = 0;
    char *utf8 = wide_to_utf8(line, &byte_count);
    if (byte_count > 0) {
        fwrite(utf8, 1, (size_t)byte_count, stdout);
        fflush(stdout);
    }
    free(utf8);
}

static void append_line_to_log_file(const wchar_t *line) {
    if (!g_log_file) {
        return;
    }

    int byte_count = 0;
    char *utf8 = wide_to_utf8(line, &byte_count);
    if (byte_count <= 0) {
        free(utf8);
        return;
    }

    HANDLE file = CreateFileW(g_log_file, FILE_APPEND_DATA, FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE,
                              NULL, OPEN_ALWAYS, FILE_ATTRIBUTE_NORMAL, NULL);
    if (file != INVALID_HANDLE_VALUE) {
        SetFilePointer(file, 0, NULL, FILE_END);
        DWORD written = 0;
        WriteFile(file, utf8, (DWORD)byte_count, &written, NULL);
        CloseHandle(file);
    }
    free(utf8);
}

static void log_message(const wchar_t *level, const wchar_t *format, ...) {
    va_list args;
    va_start(args, format);
    wchar_t *message = format_wide_message(format, args);
    va_end(args);

    time_t now = time(NULL);
    struct tm *local = localtime(&now);
    wchar_t timestamp[32];
    if (local) {
        wcsftime(timestamp, 32, L"%Y-%m-%d %H:%M:%S", local);
    } else {
        wcscpy(timestamp, L"0000-00-00 00:00:00");
    }

    size_t line_len = wcslen(timestamp) + 1 + wcslen(level) + 1 + wcslen(message) + 2;
    wchar_t *line = (wchar_t *)xmalloc((line_len + 1) * sizeof(wchar_t));
    swprintf(line, line_len + 1, L"%ls %ls %ls\n", timestamp, level, message);

    write_line_to_console(line);
    append_line_to_log_file(line);

    free(line);
    free(message);
}

static void log_info(const wchar_t *format, ...) {
    va_list args;
    va_start(args, format);
    wchar_t *message = format_wide_message(format, args);
    va_end(args);
    log_message(L"INFO", L"%ls", message);
    free(message);
}

static void log_warning(const wchar_t *format, ...) {
    va_list args;
    va_start(args, format);
    wchar_t *message = format_wide_message(format, args);
    va_end(args);
    log_message(L"WARNING", L"%ls", message);
    free(message);
}

static void log_error(const wchar_t *format, ...) {
    va_list args;
    va_start(args, format);
    wchar_t *message = format_wide_message(format, args);
    va_end(args);
    log_message(L"ERROR", L"%ls", message);
    free(message);
}

static int parse_command_line(int *argc_out, wchar_t ***argv_out) {
    const wchar_t *command = GetCommandLineW();
    wchar_t **argv = NULL;
    size_t count = 0;
    size_t capacity = 0;
    const wchar_t *p = command;

    while (*p) {
        while (*p && is_space_w(*p)) {
            ++p;
        }
        if (!*p) {
            break;
        }

        size_t max_len = wcslen(p) + 1;
        wchar_t *arg = (wchar_t *)xmalloc(max_len * sizeof(wchar_t));
        size_t len = 0;
        int in_quotes = 0;

        while (*p && (in_quotes || !is_space_w(*p))) {
            if (*p == L'"') {
                in_quotes = !in_quotes;
                ++p;
                continue;
            }
            arg[len++] = *p++;
        }
        arg[len] = L'\0';

        if (count == capacity) {
            capacity = capacity ? capacity * 2 : 8;
            argv = (wchar_t **)xrealloc(argv, capacity * sizeof(wchar_t *));
        }
        argv[count++] = arg;
    }

    *argc_out = (int)count;
    *argv_out = argv;
    return 1;
}

static void free_argv(int argc, wchar_t **argv) {
    for (int i = 0; i < argc; ++i) {
        free(argv[i]);
    }
    free(argv);
}

static wchar_t *get_executable_dir(void) {
    DWORD capacity = MAX_PATH;
    for (;;) {
        wchar_t *buffer = (wchar_t *)xmalloc((capacity + 1) * sizeof(wchar_t));
        DWORD written = GetModuleFileNameW(NULL, buffer, capacity);
        if (written > 0 && written < capacity) {
            wchar_t *dir = path_dirname(buffer);
            free(buffer);
            return dir;
        }
        free(buffer);
        capacity *= 2;
        if (capacity > 32768) {
            return absolute_path(L".");
        }
    }
}

static const wchar_t *action_name(Action action) {
    switch (action) {
    case ACTION_DRY_RUN:
        return L"dry-run";
    case ACTION_MOVE:
        return L"move";
    case ACTION_DELETE:
        return L"delete";
    default:
        return L"dry-run";
    }
}

static void print_usage(void) {
    const wchar_t *usage =
        L"用法：wechat_duplicate_monitor.exe [选项]\n"
        L"\n"
        L"监测微信文件月份目录，按内容 sha256 删除重复文件，保留创建时间最早的一个。\n"
        L"\n"
        L"选项：\n"
        L"  --root PATH              微信文件根目录，默认是程序所在文件夹的上一级目录。\n"
        L"  --interval SECONDS       监测扫描间隔秒数，默认 30。\n"
        L"  --stable-seconds SECONDS 文件稳定多少秒后才参与判重，默认 3。\n"
        L"  --action dry-run|move|delete\n"
        L"                           处理方式，默认 dry-run。\n"
        L"  --trash-dir PATH         action=move 时的移动目录，默认是根目录下的 .duplicate_trash。\n"
        L"  --algorithm sha256       哈希算法。C 版支持 sha256。\n"
        L"  --chunk-size BYTES       读取文件块大小，默认 1048576。\n"
        L"  --once                   只扫描一次后退出。\n"
        L"  --log-file PATH          日志文件路径，默认在程序所在文件夹内。\n"
        L"  --help                   显示帮助。\n";
    write_line_to_console(usage);
}

static int parse_double_arg(const wchar_t *value, double *out) {
    wchar_t *end = NULL;
    double parsed = wcstod(value, &end);
    if (!end || *end != L'\0') {
        return 0;
    }
    *out = parsed;
    return 1;
}

static int parse_size_arg(const wchar_t *value, size_t *out) {
    wchar_t *end = NULL;
    unsigned long long parsed = wcstoull(value, &end, 10);
    if (!end || *end != L'\0' || parsed == 0) {
        return 0;
    }
    *out = (size_t)parsed;
    return 1;
}

static int option_value(int argc, wchar_t **argv, int *index, const wchar_t *name, wchar_t **value) {
    size_t name_len = wcslen(name);
    if (wcsncmp(argv[*index], name, name_len) == 0 && argv[*index][name_len] == L'=') {
        *value = argv[*index] + name_len + 1;
        return 1;
    }
    if (wcscmp(argv[*index], name) == 0) {
        if (*index + 1 >= argc) {
            fwprintf(stderr, L"缺少参数值：%ls\n", name);
            return 0;
        }
        ++(*index);
        *value = argv[*index];
        return 1;
    }
    return -1;
}

static int parse_args(int argc, wchar_t **argv, Options *options) {
    memset(options, 0, sizeof(*options));
    options->app_dir = get_executable_dir();
    wchar_t *default_root = path_dirname(options->app_dir);
    wchar_t *default_log = path_join(options->app_dir, L"duplicate_monitor.log");

    options->root = default_root;
    options->interval = DEFAULT_INTERVAL_SECONDS;
    options->stable_seconds = DEFAULT_STABLE_SECONDS;
    options->action = ACTION_DRY_RUN;
    options->trash_dir = NULL;
    options->algorithm = xwcsdup(L"sha256");
    options->chunk_size = DEFAULT_CHUNK_SIZE;
    options->once = 0;
    options->log_file = default_log;

    for (int i = 1; i < argc; ++i) {
        wchar_t *value = NULL;
        int result;

        if (wcscmp(argv[i], L"--help") == 0 || wcscmp(argv[i], L"-h") == 0) {
            print_usage();
            return 2;
        }
        if (wcscmp(argv[i], L"--once") == 0) {
            options->once = 1;
            continue;
        }

        result = option_value(argc, argv, &i, L"--root", &value);
        if (result == 1) {
            free(options->root);
            options->root = xwcsdup(value);
            continue;
        } else if (result == 0) {
            return 0;
        }

        result = option_value(argc, argv, &i, L"--interval", &value);
        if (result == 1) {
            if (!parse_double_arg(value, &options->interval)) {
                fwprintf(stderr, L"无效的 --interval：%ls\n", value);
                return 0;
            }
            continue;
        } else if (result == 0) {
            return 0;
        }

        result = option_value(argc, argv, &i, L"--stable-seconds", &value);
        if (result == 1) {
            if (!parse_double_arg(value, &options->stable_seconds)) {
                fwprintf(stderr, L"无效的 --stable-seconds：%ls\n", value);
                return 0;
            }
            continue;
        } else if (result == 0) {
            return 0;
        }

        result = option_value(argc, argv, &i, L"--action", &value);
        if (result == 1) {
            if (wcscmp(value, L"dry-run") == 0) {
                options->action = ACTION_DRY_RUN;
            } else if (wcscmp(value, L"move") == 0) {
                options->action = ACTION_MOVE;
            } else if (wcscmp(value, L"delete") == 0) {
                options->action = ACTION_DELETE;
            } else {
                fwprintf(stderr, L"无效的 --action：%ls\n", value);
                return 0;
            }
            continue;
        } else if (result == 0) {
            return 0;
        }

        result = option_value(argc, argv, &i, L"--trash-dir", &value);
        if (result == 1) {
            free(options->trash_dir);
            options->trash_dir = xwcsdup(value);
            continue;
        } else if (result == 0) {
            return 0;
        }

        result = option_value(argc, argv, &i, L"--algorithm", &value);
        if (result == 1) {
            free(options->algorithm);
            options->algorithm = xwcsdup(value);
            continue;
        } else if (result == 0) {
            return 0;
        }

        result = option_value(argc, argv, &i, L"--chunk-size", &value);
        if (result == 1) {
            if (!parse_size_arg(value, &options->chunk_size)) {
                fwprintf(stderr, L"无效的 --chunk-size：%ls\n", value);
                return 0;
            }
            continue;
        } else if (result == 0) {
            return 0;
        }

        result = option_value(argc, argv, &i, L"--log-file", &value);
        if (result == 1) {
            free(options->log_file);
            options->log_file = xwcsdup(value);
            continue;
        } else if (result == 0) {
            return 0;
        }

        fwprintf(stderr, L"未知参数：%ls\n", argv[i]);
        return 0;
    }

    if (_wcsicmp(options->algorithm, L"sha256") != 0) {
        fwprintf(stderr, L"C 版当前支持的 --algorithm 是 sha256。\n");
        return 0;
    }

    wchar_t *abs_root = absolute_path(options->root);
    free(options->root);
    options->root = abs_root;

    if (options->trash_dir) {
        wchar_t *abs_trash = absolute_path(options->trash_dir);
        free(options->trash_dir);
        options->trash_dir = abs_trash;
    } else {
        options->trash_dir = path_join(options->root, L".duplicate_trash");
    }

    wchar_t *abs_log = absolute_path(options->log_file);
    free(options->log_file);
    options->log_file = abs_log;

    return 1;
}

static void free_options(Options *options) {
    free(options->app_dir);
    free(options->root);
    free(options->trash_dir);
    free(options->algorithm);
    free(options->log_file);
}

static uint32_t rotr32(uint32_t value, uint32_t count) {
    return (value >> count) | (value << (32 - count));
}

#define SHA256_CH(x, y, z) (((x) & (y)) ^ (~(x) & (z)))
#define SHA256_MAJ(x, y, z) (((x) & (y)) ^ ((x) & (z)) ^ ((y) & (z)))
#define SHA256_EP0(x) (rotr32((x), 2) ^ rotr32((x), 13) ^ rotr32((x), 22))
#define SHA256_EP1(x) (rotr32((x), 6) ^ rotr32((x), 11) ^ rotr32((x), 25))
#define SHA256_SIG0(x) (rotr32((x), 7) ^ rotr32((x), 18) ^ ((x) >> 3))
#define SHA256_SIG1(x) (rotr32((x), 17) ^ rotr32((x), 19) ^ ((x) >> 10))

typedef struct {
    uint8_t data[64];
    uint32_t datalen;
    uint64_t bitlen;
    uint32_t state[8];
} Sha256Context;

static const uint32_t SHA256_K[64] = {
    0x428a2f98UL, 0x71374491UL, 0xb5c0fbcfUL, 0xe9b5dba5UL,
    0x3956c25bUL, 0x59f111f1UL, 0x923f82a4UL, 0xab1c5ed5UL,
    0xd807aa98UL, 0x12835b01UL, 0x243185beUL, 0x550c7dc3UL,
    0x72be5d74UL, 0x80deb1feUL, 0x9bdc06a7UL, 0xc19bf174UL,
    0xe49b69c1UL, 0xefbe4786UL, 0x0fc19dc6UL, 0x240ca1ccUL,
    0x2de92c6fUL, 0x4a7484aaUL, 0x5cb0a9dcUL, 0x76f988daUL,
    0x983e5152UL, 0xa831c66dUL, 0xb00327c8UL, 0xbf597fc7UL,
    0xc6e00bf3UL, 0xd5a79147UL, 0x06ca6351UL, 0x14292967UL,
    0x27b70a85UL, 0x2e1b2138UL, 0x4d2c6dfcUL, 0x53380d13UL,
    0x650a7354UL, 0x766a0abbUL, 0x81c2c92eUL, 0x92722c85UL,
    0xa2bfe8a1UL, 0xa81a664bUL, 0xc24b8b70UL, 0xc76c51a3UL,
    0xd192e819UL, 0xd6990624UL, 0xf40e3585UL, 0x106aa070UL,
    0x19a4c116UL, 0x1e376c08UL, 0x2748774cUL, 0x34b0bcb5UL,
    0x391c0cb3UL, 0x4ed8aa4aUL, 0x5b9cca4fUL, 0x682e6ff3UL,
    0x748f82eeUL, 0x78a5636fUL, 0x84c87814UL, 0x8cc70208UL,
    0x90befffaUL, 0xa4506cebUL, 0xbef9a3f7UL, 0xc67178f2UL
};

static void sha256_transform(Sha256Context *ctx, const uint8_t data[]) {
    uint32_t m[64];
    for (uint32_t i = 0, j = 0; i < 16; ++i, j += 4) {
        m[i] = ((uint32_t)data[j] << 24) | ((uint32_t)data[j + 1] << 16) |
               ((uint32_t)data[j + 2] << 8) | (uint32_t)data[j + 3];
    }
    for (uint32_t i = 16; i < 64; ++i) {
        m[i] = SHA256_SIG1(m[i - 2]) + m[i - 7] + SHA256_SIG0(m[i - 15]) + m[i - 16];
    }

    uint32_t a = ctx->state[0];
    uint32_t b = ctx->state[1];
    uint32_t c = ctx->state[2];
    uint32_t d = ctx->state[3];
    uint32_t e = ctx->state[4];
    uint32_t f = ctx->state[5];
    uint32_t g = ctx->state[6];
    uint32_t h = ctx->state[7];

    for (uint32_t i = 0; i < 64; ++i) {
        uint32_t t1 = h + SHA256_EP1(e) + SHA256_CH(e, f, g) + SHA256_K[i] + m[i];
        uint32_t t2 = SHA256_EP0(a) + SHA256_MAJ(a, b, c);
        h = g;
        g = f;
        f = e;
        e = d + t1;
        d = c;
        c = b;
        b = a;
        a = t1 + t2;
    }

    ctx->state[0] += a;
    ctx->state[1] += b;
    ctx->state[2] += c;
    ctx->state[3] += d;
    ctx->state[4] += e;
    ctx->state[5] += f;
    ctx->state[6] += g;
    ctx->state[7] += h;
}

static void sha256_init(Sha256Context *ctx) {
    ctx->datalen = 0;
    ctx->bitlen = 0;
    ctx->state[0] = 0x6a09e667UL;
    ctx->state[1] = 0xbb67ae85UL;
    ctx->state[2] = 0x3c6ef372UL;
    ctx->state[3] = 0xa54ff53aUL;
    ctx->state[4] = 0x510e527fUL;
    ctx->state[5] = 0x9b05688cUL;
    ctx->state[6] = 0x1f83d9abUL;
    ctx->state[7] = 0x5be0cd19UL;
}

static void sha256_update(Sha256Context *ctx, const uint8_t *data, size_t len) {
    for (size_t i = 0; i < len; ++i) {
        ctx->data[ctx->datalen++] = data[i];
        if (ctx->datalen == 64) {
            sha256_transform(ctx, ctx->data);
            ctx->bitlen += 512;
            ctx->datalen = 0;
        }
    }
}

static void sha256_final(Sha256Context *ctx, uint8_t hash[]) {
    uint32_t i = ctx->datalen;

    if (ctx->datalen < 56) {
        ctx->data[i++] = 0x80;
        while (i < 56) {
            ctx->data[i++] = 0x00;
        }
    } else {
        ctx->data[i++] = 0x80;
        while (i < 64) {
            ctx->data[i++] = 0x00;
        }
        sha256_transform(ctx, ctx->data);
        memset(ctx->data, 0, 56);
    }

    ctx->bitlen += (uint64_t)ctx->datalen * 8;
    ctx->data[63] = (uint8_t)(ctx->bitlen);
    ctx->data[62] = (uint8_t)(ctx->bitlen >> 8);
    ctx->data[61] = (uint8_t)(ctx->bitlen >> 16);
    ctx->data[60] = (uint8_t)(ctx->bitlen >> 24);
    ctx->data[59] = (uint8_t)(ctx->bitlen >> 32);
    ctx->data[58] = (uint8_t)(ctx->bitlen >> 40);
    ctx->data[57] = (uint8_t)(ctx->bitlen >> 48);
    ctx->data[56] = (uint8_t)(ctx->bitlen >> 56);
    sha256_transform(ctx, ctx->data);

    for (i = 0; i < 4; ++i) {
        hash[i] = (uint8_t)((ctx->state[0] >> (24 - i * 8)) & 0xff);
        hash[i + 4] = (uint8_t)((ctx->state[1] >> (24 - i * 8)) & 0xff);
        hash[i + 8] = (uint8_t)((ctx->state[2] >> (24 - i * 8)) & 0xff);
        hash[i + 12] = (uint8_t)((ctx->state[3] >> (24 - i * 8)) & 0xff);
        hash[i + 16] = (uint8_t)((ctx->state[4] >> (24 - i * 8)) & 0xff);
        hash[i + 20] = (uint8_t)((ctx->state[5] >> (24 - i * 8)) & 0xff);
        hash[i + 24] = (uint8_t)((ctx->state[6] >> (24 - i * 8)) & 0xff);
        hash[i + 28] = (uint8_t)((ctx->state[7] >> (24 - i * 8)) & 0xff);
    }
}

static void digest_to_hex(const uint8_t digest[SHA256_DIGEST_LENGTH], wchar_t hex[SHA256_DIGEST_LENGTH * 2 + 1]) {
    static const wchar_t *digits = L"0123456789abcdef";
    for (int i = 0; i < SHA256_DIGEST_LENGTH; ++i) {
        hex[i * 2] = digits[(digest[i] >> 4) & 0x0f];
        hex[i * 2 + 1] = digits[digest[i] & 0x0f];
    }
    hex[SHA256_DIGEST_LENGTH * 2] = L'\0';
}

static int sha256_file(const wchar_t *path, size_t chunk_size, uint8_t digest[SHA256_DIGEST_LENGTH], DWORD *error_out) {
    DWORD read_size = (DWORD)(chunk_size > MAX_READ_CHUNK ? MAX_READ_CHUNK : chunk_size);
    if (read_size == 0) {
        read_size = DEFAULT_CHUNK_SIZE;
    }

    uint8_t *buffer = (uint8_t *)xmalloc(read_size);
    HANDLE file = CreateFileW(path, GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE,
                              NULL, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL | FILE_FLAG_SEQUENTIAL_SCAN, NULL);
    if (file == INVALID_HANDLE_VALUE) {
        if (error_out) {
            *error_out = GetLastError();
        }
        free(buffer);
        return 0;
    }

    Sha256Context ctx;
    sha256_init(&ctx);

    for (;;) {
        DWORD bytes_read = 0;
        if (!ReadFile(file, buffer, read_size, &bytes_read, NULL)) {
            if (error_out) {
                *error_out = GetLastError();
            }
            CloseHandle(file);
            free(buffer);
            return 0;
        }
        if (bytes_read == 0) {
            break;
        }
        sha256_update(&ctx, buffer, bytes_read);
    }

    sha256_final(&ctx, digest);
    CloseHandle(file);
    free(buffer);
    if (error_out) {
        *error_out = ERROR_SUCCESS;
    }
    return 1;
}

static uint64_t file_size_from_find_data(const WIN32_FIND_DATAW *data) {
    return ((uint64_t)data->nFileSizeHigh << 32) | (uint64_t)data->nFileSizeLow;
}

static int get_file_info(const wchar_t *path, FileInfo *info) {
    WIN32_FILE_ATTRIBUTE_DATA data;
    if (!GetFileAttributesExW(path, GetFileExInfoStandard, &data)) {
        return 0;
    }
    if (data.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) {
        return 0;
    }
    if (data.dwFileAttributes & FILE_ATTRIBUTE_REPARSE_POINT) {
        return 0;
    }
    info->path = (wchar_t *)path;
    info->size = ((uint64_t)data.nFileSizeHigh << 32) | (uint64_t)data.nFileSizeLow;
    info->creation_time = data.ftCreationTime;
    info->write_time = data.ftLastWriteTime;
    return 1;
}

static double now_seconds(void) {
    return (double)GetTickCount64() / 1000.0;
}

static double filetime_to_unix_seconds(FILETIME time_value) {
    ULARGE_INTEGER value;
    value.LowPart = time_value.dwLowDateTime;
    value.HighPart = time_value.dwHighDateTime;
    const unsigned long long epoch_diff = 116444736000000000ULL;
    if (value.QuadPart <= epoch_diff) {
        return 0.0;
    }
    return (double)(value.QuadPart - epoch_diff) / 10000000.0;
}

static int same_file_signature(const SeenEntry *seen, const FileInfo *info) {
    return seen->size == info->size &&
           seen->write_time.dwLowDateTime == info->write_time.dwLowDateTime &&
           seen->write_time.dwHighDateTime == info->write_time.dwHighDateTime;
}

static int is_month_name(const wchar_t *name) {
    if (wcslen(name) != MONTH_DIR_LENGTH) {
        return 0;
    }
    if (!is_digit_w(name[0]) || !is_digit_w(name[1]) || !is_digit_w(name[2]) || !is_digit_w(name[3])) {
        return 0;
    }
    if (name[4] != L'-' || !is_digit_w(name[5]) || !is_digit_w(name[6])) {
        return 0;
    }
    int month = (name[5] - L'0') * 10 + (name[6] - L'0');
    return month >= 1 && month <= 12;
}

static int is_file_in_use(const wchar_t *path) {
    const wchar_t *name = path_basename_ptr(path);
    if (wcsncmp(name, L"~$", 2) == 0) {
        return 1;
    }

    HANDLE handle = CreateFileW(path, DELETE | FILE_READ_ATTRIBUTES, 0, NULL, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, NULL);
    if (handle == INVALID_HANDLE_VALUE) {
        DWORD error = GetLastError();
        return error == ERROR_SHARING_VIOLATION || error == ERROR_LOCK_VIOLATION;
    }
    CloseHandle(handle);
    return 0;
}

static void file_list_append(FileList *list, const FileInfo *info) {
    if (list->count == list->capacity) {
        list->capacity = list->capacity ? list->capacity * 2 : 128;
        list->items = (FileInfo *)xrealloc(list->items, list->capacity * sizeof(FileInfo));
    }
    list->items[list->count] = *info;
    list->items[list->count].path = xwcsdup(info->path);
    ++list->count;
}

static void file_list_free(FileList *list) {
    for (size_t i = 0; i < list->count; ++i) {
        free(list->items[i].path);
    }
    free(list->items);
    list->items = NULL;
    list->count = 0;
    list->capacity = 0;
}

static int seen_find(const SeenList *list, const wchar_t *path) {
    for (size_t i = 0; i < list->count; ++i) {
        if (_wcsicmp(list->items[i].path, path) == 0) {
            return (int)i;
        }
    }
    return -1;
}

static void seen_remove_at(SeenList *list, size_t index) {
    free(list->items[index].path);
    if (index + 1 < list->count) {
        memmove(&list->items[index], &list->items[index + 1], (list->count - index - 1) * sizeof(SeenEntry));
    }
    --list->count;
}

static void seen_remove(SeenList *list, const wchar_t *path) {
    int index = seen_find(list, path);
    if (index >= 0) {
        seen_remove_at(list, (size_t)index);
    }
}

static void seen_set(SeenList *list, const wchar_t *path, const FileInfo *info, double first_stable_at) {
    int index = seen_find(list, path);
    if (index >= 0) {
        SeenEntry *entry = &list->items[index];
        entry->size = info->size;
        entry->write_time = info->write_time;
        entry->first_stable_at = first_stable_at;
        entry->current = 1;
        return;
    }

    if (list->count == list->capacity) {
        list->capacity = list->capacity ? list->capacity * 2 : 128;
        list->items = (SeenEntry *)xrealloc(list->items, list->capacity * sizeof(SeenEntry));
    }
    list->items[list->count].path = xwcsdup(path);
    list->items[list->count].size = info->size;
    list->items[list->count].write_time = info->write_time;
    list->items[list->count].first_stable_at = first_stable_at;
    list->items[list->count].current = 1;
    ++list->count;
}

static void seen_mark_all_missing(SeenList *list) {
    for (size_t i = 0; i < list->count; ++i) {
        list->items[i].current = 0;
    }
}

static void seen_remove_missing(SeenList *list) {
    size_t i = 0;
    while (i < list->count) {
        if (!list->items[i].current) {
            seen_remove_at(list, i);
        } else {
            ++i;
        }
    }
}

static void seen_free(SeenList *list) {
    for (size_t i = 0; i < list->count; ++i) {
        free(list->items[i].path);
    }
    free(list->items);
    list->items = NULL;
    list->count = 0;
    list->capacity = 0;
}

static void dir_list_append(DirList *list, const wchar_t *name, const wchar_t *path) {
    if (list->count == list->capacity) {
        list->capacity = list->capacity ? list->capacity * 2 : 16;
        list->items = (DirEntry *)xrealloc(list->items, list->capacity * sizeof(DirEntry));
    }
    list->items[list->count].name = xwcsdup(name);
    list->items[list->count].path = xwcsdup(path);
    ++list->count;
}

static void dir_list_free(DirList *list) {
    for (size_t i = 0; i < list->count; ++i) {
        free(list->items[i].name);
        free(list->items[i].path);
    }
    free(list->items);
    list->items = NULL;
    list->count = 0;
    list->capacity = 0;
}

static int compare_dir_entries(const void *left, const void *right) {
    const DirEntry *a = (const DirEntry *)left;
    const DirEntry *b = (const DirEntry *)right;
    return wcscmp(a->name, b->name);
}

static DirList collect_month_dirs(const wchar_t *root) {
    DirList list = {0};
    wchar_t *pattern = path_join(root, L"*");
    WIN32_FIND_DATAW data;
    HANDLE find = FindFirstFileW(pattern, &data);
    free(pattern);

    if (find == INVALID_HANDLE_VALUE) {
        return list;
    }

    do {
        if (wcscmp(data.cFileName, L".") == 0 || wcscmp(data.cFileName, L"..") == 0) {
            continue;
        }
        if (!(data.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY)) {
            continue;
        }
        if (data.dwFileAttributes & FILE_ATTRIBUTE_REPARSE_POINT) {
            continue;
        }
        if (!is_month_name(data.cFileName)) {
            continue;
        }
        wchar_t *path = path_join(root, data.cFileName);
        dir_list_append(&list, data.cFileName, path);
        free(path);
    } while (FindNextFileW(find, &data));

    FindClose(find);
    if (list.count > 1) {
        qsort(list.items, list.count, sizeof(DirEntry), compare_dir_entries);
    }
    return list;
}

static void handle_candidate_file(const wchar_t *root, const wchar_t *path, SeenList *seen, FileList *stable_files, double now, double stable_seconds) {
    FileInfo info;
    if (!get_file_info(path, &info)) {
        DWORD error = GetLastError();
        wchar_t *message = windows_error_message(error);
        log_warning(L"跳过无法读取状态的文件：%ls（%ls）", path, message);
        free(message);
        return;
    }

    int seen_index = seen_find(seen, path);
    if (seen_index >= 0) {
        seen->items[seen_index].current = 1;
    }

    if (is_file_in_use(path)) {
        seen_remove(seen, path);
        wchar_t *rel = relative_path(path, root);
        log_info(L"跳过正在使用的文件：%ls", rel);
        free(rel);
        return;
    }

    seen_index = seen_find(seen, path);
    if (seen_index < 0 || !same_file_signature(&seen->items[seen_index], &info)) {
        seen_set(seen, path, &info, now);
        if (stable_seconds <= 0) {
            file_list_append(stable_files, &info);
        }
        return;
    }

    if (now - seen->items[seen_index].first_stable_at >= stable_seconds) {
        file_list_append(stable_files, &info);
    }
}

static void scan_directory_recursive(const wchar_t *root, const wchar_t *dir, SeenList *seen, FileList *stable_files, double now, double stable_seconds) {
    wchar_t *pattern = path_join(dir, L"*");
    WIN32_FIND_DATAW data;
    HANDLE find = FindFirstFileW(pattern, &data);
    free(pattern);

    if (find == INVALID_HANDLE_VALUE) {
        DWORD error = GetLastError();
        wchar_t *message = windows_error_message(error);
        log_warning(L"跳过无法访问的文件夹：%ls（%ls）", dir, message);
        free(message);
        return;
    }

    do {
        if (wcscmp(data.cFileName, L".") == 0 || wcscmp(data.cFileName, L"..") == 0) {
            continue;
        }

        wchar_t *path = path_join(dir, data.cFileName);
        if (data.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) {
            if (data.cFileName[0] != L'.' && !(data.dwFileAttributes & FILE_ATTRIBUTE_REPARSE_POINT)) {
                scan_directory_recursive(root, path, seen, stable_files, now, stable_seconds);
            }
        } else if (!(data.dwFileAttributes & FILE_ATTRIBUTE_REPARSE_POINT)) {
            handle_candidate_file(root, path, seen, stable_files, now, stable_seconds);
        }
        free(path);
    } while (FindNextFileW(find, &data));

    FindClose(find);
}

static FileList update_stable_files(const wchar_t *root, SeenList *seen, double stable_seconds) {
    FileList stable_files = {0};
    double now = now_seconds();
    seen_mark_all_missing(seen);

    DirList month_dirs = collect_month_dirs(root);
    for (size_t i = 0; i < month_dirs.count; ++i) {
        scan_directory_recursive(root, month_dirs.items[i].path, seen, &stable_files, now, stable_seconds);
    }
    dir_list_free(&month_dirs);

    seen_remove_missing(seen);
    return stable_files;
}

static int compare_files_by_size(const void *left, const void *right) {
    const FileInfo *a = (const FileInfo *)left;
    const FileInfo *b = (const FileInfo *)right;
    if (a->size < b->size) {
        return -1;
    }
    if (a->size > b->size) {
        return 1;
    }
    return _wcsicmp(a->path, b->path);
}

static int compare_hash_entries(const void *left, const void *right) {
    const HashEntry *a = (const HashEntry *)left;
    const HashEntry *b = (const HashEntry *)right;
    int digest_cmp = memcmp(a->digest, b->digest, SHA256_DIGEST_LENGTH);
    if (digest_cmp != 0) {
        return digest_cmp;
    }
    return _wcsicmp(a->file->path, b->file->path);
}

static void current_file_list_append(CurrentFileList *list, const wchar_t *path, const FileInfo *info) {
    if (list->count == list->capacity) {
        list->capacity = list->capacity ? list->capacity * 2 : 8;
        list->items = (CurrentFile *)xrealloc(list->items, list->capacity * sizeof(CurrentFile));
    }
    list->items[list->count].path = path;
    list->items[list->count].size = info->size;
    list->items[list->count].creation_time = info->creation_time;
    list->items[list->count].write_time = info->write_time;
    ++list->count;
}

static int choose_keeper_index(const CurrentFileList *files) {
    int keeper = 0;
    for (size_t i = 1; i < files->count; ++i) {
        int time_cmp = CompareFileTime(&files->items[i].creation_time, &files->items[keeper].creation_time);
        if (time_cmp < 0 || (time_cmp == 0 && _wcsicmp(files->items[i].path, files->items[keeper].path) < 0)) {
            keeper = (int)i;
        }
    }
    return keeper;
}

static wchar_t *destination_with_counter(const wchar_t *destination, int counter) {
    const wchar_t *base = path_basename_ptr(destination);
    const wchar_t *dot = wcsrchr(base, L'.');
    int has_suffix = dot && dot > base;
    size_t prefix_len = has_suffix ? (size_t)(dot - destination) : wcslen(destination);
    const wchar_t *suffix = has_suffix ? dot : L"";

    wchar_t counter_text[32];
    swprintf(counter_text, 32, L".%d", counter);

    size_t total = prefix_len + wcslen(counter_text) + wcslen(suffix);
    wchar_t *candidate = (wchar_t *)xmalloc((total + 1) * sizeof(wchar_t));
    wcsncpy(candidate, destination, prefix_len);
    candidate[prefix_len] = L'\0';
    wcscat(candidate, counter_text);
    wcscat(candidate, suffix);
    return candidate;
}

static int move_duplicate_file(const wchar_t *path, const wchar_t *root, const wchar_t *trash_dir, wchar_t **destination_out, DWORD *error_out) {
    wchar_t *rel = relative_path(path, root);
    wchar_t *destination = path_join(trash_dir, rel);
    free(rel);

    if (!ensure_parent_dir(destination)) {
        if (error_out) {
            *error_out = GetLastError();
        }
        free(destination);
        return 0;
    }

    if (path_exists(destination)) {
        int counter = 1;
        for (;;) {
            wchar_t *candidate = destination_with_counter(destination, counter);
            if (!path_exists(candidate)) {
                free(destination);
                destination = candidate;
                break;
            }
            free(candidate);
            ++counter;
        }
    }

    if (!MoveFileExW(path, destination, MOVEFILE_COPY_ALLOWED)) {
        if (error_out) {
            *error_out = GetLastError();
        }
        free(destination);
        return 0;
    }

    if (destination_out) {
        *destination_out = destination;
    } else {
        free(destination);
    }
    if (error_out) {
        *error_out = ERROR_SUCCESS;
    }
    return 1;
}

static void process_duplicate_group(const wchar_t *root, const Options *options, HashEntry *entries, size_t count, uint64_t size, ProcessResult *result) {
    CurrentFileList current_files = {0};

    for (size_t i = 0; i < count; ++i) {
        const wchar_t *path = entries[i].file->path;
        if (is_file_in_use(path)) {
            wchar_t *rel = relative_path(path, root);
            log_info(L"跳过删除前仍在使用的文件：%ls", rel);
            free(rel);
            continue;
        }

        FileInfo current_info;
        if (!get_file_info(path, &current_info)) {
            DWORD error = GetLastError();
            wchar_t *message = windows_error_message(error);
            log_warning(L"跳过删除前无法复核的文件：%ls（%ls）", path, message);
            free(message);
            continue;
        }

        if (current_info.size != size) {
            wchar_t *rel = relative_path(path, root);
            log_warning(L"跳过已变化的文件：%ls", rel);
            free(rel);
            continue;
        }

        uint8_t current_digest[SHA256_DIGEST_LENGTH];
        DWORD hash_error = ERROR_SUCCESS;
        if (!sha256_file(path, options->chunk_size, current_digest, &hash_error)) {
            wchar_t *message = windows_error_message(hash_error);
            log_warning(L"跳过删除前无法复核的文件：%ls（%ls）", path, message);
            free(message);
            continue;
        }

        if (memcmp(current_digest, entries[0].digest, SHA256_DIGEST_LENGTH) == 0) {
            current_file_list_append(&current_files, path, &current_info);
        } else {
            wchar_t *rel = relative_path(path, root);
            log_warning(L"跳过已变化的文件：%ls", rel);
            free(rel);
        }
    }

    if (current_files.count < 2) {
        free(current_files.items);
        return;
    }

    int keeper_index = choose_keeper_index(&current_files);
    CurrentFile *keeper = &current_files.items[keeper_index];
    wchar_t *keeper_rel = relative_path(keeper->path, root);
    log_info(L"保留：%ls（创建时间 %.0f，大小 %llu，sha256 %ls）",
             keeper_rel,
             filetime_to_unix_seconds(keeper->creation_time),
             (unsigned long long)size,
             entries[0].hex);
    free(keeper_rel);

    for (size_t i = 0; i < current_files.count; ++i) {
        if ((int)i == keeper_index) {
            continue;
        }

        const wchar_t *path = current_files.items[i].path;
        wchar_t *rel = relative_path(path, root);
        int success = 1;

        if (options->action == ACTION_DRY_RUN) {
            log_info(L"演练：将删除重复文件：%ls", rel);
        } else if (options->action == ACTION_MOVE) {
            wchar_t *destination = NULL;
            DWORD error = ERROR_SUCCESS;
            if (!move_duplicate_file(path, root, options->trash_dir, &destination, &error)) {
                wchar_t *message = windows_error_message(error);
                log_error(L"移动失败：%ls（%ls）", path, message);
                free(message);
                success = 0;
            } else {
                wchar_t *destination_rel = relative_path(destination, root);
                log_info(L"已移动重复文件：%ls -> %ls", rel, destination_rel);
                free(destination_rel);
                free(destination);
            }
        } else {
            if (!DeleteFileW(path)) {
                DWORD error = GetLastError();
                wchar_t *message = windows_error_message(error);
                log_error(L"删除失败：%ls（%ls）", path, message);
                free(message);
                success = 0;
            } else {
                log_info(L"已删除重复文件：%ls", rel);
            }
        }

        if (success) {
            ++result->removed_count;
            result->saved_bytes += size;
        }
        free(rel);
    }

    free(current_files.items);
}

static ProcessResult find_and_process_duplicates(const wchar_t *root, const Options *options, FileList *stable_files) {
    ProcessResult result = {0};
    if (stable_files->count < 2) {
        return result;
    }

    qsort(stable_files->items, stable_files->count, sizeof(FileInfo), compare_files_by_size);

    size_t start = 0;
    while (start < stable_files->count) {
        size_t end = start + 1;
        while (end < stable_files->count && stable_files->items[end].size == stable_files->items[start].size) {
            ++end;
        }

        size_t same_size_count = end - start;
        if (same_size_count >= 2) {
            HashEntry *hashes = (HashEntry *)xmalloc(same_size_count * sizeof(HashEntry));
            size_t hash_count = 0;

            for (size_t i = start; i < end; ++i) {
                DWORD hash_error = ERROR_SUCCESS;
                if (!sha256_file(stable_files->items[i].path, options->chunk_size, hashes[hash_count].digest, &hash_error)) {
                    wchar_t *message = windows_error_message(hash_error);
                    log_warning(L"跳过无法读取内容的文件：%ls（%ls）", stable_files->items[i].path, message);
                    free(message);
                    continue;
                }
                hashes[hash_count].file = &stable_files->items[i];
                digest_to_hex(hashes[hash_count].digest, hashes[hash_count].hex);
                ++hash_count;
            }

            if (hash_count >= 2) {
                qsort(hashes, hash_count, sizeof(HashEntry), compare_hash_entries);
                size_t hash_start = 0;
                while (hash_start < hash_count) {
                    size_t hash_end = hash_start + 1;
                    while (hash_end < hash_count && memcmp(hashes[hash_end].digest, hashes[hash_start].digest, SHA256_DIGEST_LENGTH) == 0) {
                        ++hash_end;
                    }
                    if (hash_end - hash_start > 1) {
                        ++result.duplicate_groups;
                        process_duplicate_group(root, options, &hashes[hash_start], hash_end - hash_start, stable_files->items[start].size, &result);
                    }
                    hash_start = hash_end;
                }
            }
            free(hashes);
        }
        start = end;
    }

    return result;
}

static void byte_buffer_append(ByteBuffer *buffer, const char *data, size_t length) {
    if (length == 0) {
        return;
    }
    if (buffer->length + length > buffer->capacity) {
        size_t new_capacity = buffer->capacity ? buffer->capacity * 2 : 4096;
        while (new_capacity < buffer->length + length) {
            new_capacity *= 2;
        }
        buffer->data = (char *)xrealloc(buffer->data, new_capacity);
        buffer->capacity = new_capacity;
    }
    memcpy(buffer->data + buffer->length, data, length);
    buffer->length += length;
}

static int bytes_contains(const char *data, size_t length, const char *needle, size_t needle_length) {
    if (needle_length == 0 || length < needle_length) {
        return 0;
    }
    for (size_t i = 0; i + needle_length <= length; ++i) {
        if (memcmp(data + i, needle, needle_length) == 0) {
            return 1;
        }
    }
    return 0;
}

static size_t find_last_bytes(const char *data, size_t length, const char *needle, size_t needle_length) {
    if (needle_length == 0 || length < needle_length) {
        return (size_t)-1;
    }
    for (size_t i = length - needle_length + 1; i > 0; --i) {
        size_t pos = i - 1;
        if (memcmp(data + pos, needle, needle_length) == 0) {
            return pos;
        }
    }
    return (size_t)-1;
}

static void compact_log_file(const wchar_t *log_file) {
    HANDLE file = CreateFileW(log_file, GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE,
                              NULL, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, NULL);
    if (file == INVALID_HANDLE_VALUE) {
        return;
    }

    LARGE_INTEGER size;
    if (!GetFileSizeEx(file, &size) || size.QuadPart <= 0 || size.QuadPart > (LONGLONG)SIZE_MAX) {
        CloseHandle(file);
        return;
    }

    size_t length = (size_t)size.QuadPart;
    char *content = (char *)xmalloc(length);
    DWORD bytes_read = 0;
    if (!ReadFile(file, content, (DWORD)length, &bytes_read, NULL) || bytes_read != (DWORD)length) {
        free(content);
        CloseHandle(file);
        return;
    }
    CloseHandle(file);

    int start_len_i = 0;
    char *start_marker = wide_to_utf8(SCAN_START_MESSAGE, &start_len_i);
    size_t latest_start = find_last_bytes(content, length, start_marker, (size_t)start_len_i);
    if (latest_start == (size_t)-1) {
        free(start_marker);
        free(content);
        return;
    }

    char *keywords[sizeof(PERSISTENT_LOG_KEYWORDS) / sizeof(PERSISTENT_LOG_KEYWORDS[0])];
    int keyword_lengths[sizeof(PERSISTENT_LOG_KEYWORDS) / sizeof(PERSISTENT_LOG_KEYWORDS[0])];
    for (size_t i = 0; i < sizeof(PERSISTENT_LOG_KEYWORDS) / sizeof(PERSISTENT_LOG_KEYWORDS[0]); ++i) {
        keywords[i] = wide_to_utf8(PERSISTENT_LOG_KEYWORDS[i], &keyword_lengths[i]);
    }

    ByteBuffer output = {0};
    size_t line_start = 0;
    while (line_start < latest_start) {
        size_t line_end = line_start;
        while (line_end < latest_start && content[line_end] != '\n') {
            ++line_end;
        }
        if (line_end < latest_start) {
            ++line_end;
        }

        int keep = 0;
        for (size_t i = 0; i < sizeof(PERSISTENT_LOG_KEYWORDS) / sizeof(PERSISTENT_LOG_KEYWORDS[0]); ++i) {
            if (bytes_contains(content + line_start, line_end - line_start, keywords[i], (size_t)keyword_lengths[i])) {
                keep = 1;
                break;
            }
        }
        if (keep) {
            byte_buffer_append(&output, content + line_start, line_end - line_start);
        }
        line_start = line_end;
    }
    byte_buffer_append(&output, content + latest_start, length - latest_start);

    file = CreateFileW(log_file, GENERIC_WRITE, FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE,
                       NULL, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, NULL);
    if (file != INVALID_HANDLE_VALUE) {
        DWORD written = 0;
        if (output.length > 0) {
            WriteFile(file, output.data, (DWORD)output.length, &written, NULL);
        }
        CloseHandle(file);
    }

    for (size_t i = 0; i < sizeof(PERSISTENT_LOG_KEYWORDS) / sizeof(PERSISTENT_LOG_KEYWORDS[0]); ++i) {
        free(keywords[i]);
    }
    free(output.data);
    free(start_marker);
    free(content);
}

static void sleep_seconds(double seconds) {
    if (seconds <= 0) {
        return;
    }
    double remaining = seconds * 1000.0;
    while (remaining > 0) {
        DWORD chunk = remaining > (double)0xffffffffUL ? 0xffffffffUL : (DWORD)(remaining + 0.5);
        if (chunk == 0) {
            chunk = 1;
        }
        Sleep(chunk);
        remaining -= (double)chunk;
    }
}

static void scan_once(const Options *options, SeenList *seen) {
    FileList stable_files = update_stable_files(options->root, seen, options->stable_seconds);
    ProcessResult result = find_and_process_duplicates(options->root, options, &stable_files);

    if (result.duplicate_groups == 0) {
        log_info(L"未发现内容完全相同的重复文件。");
    } else {
        log_info(L"本轮处理重复文件 %d 个，涉及 %.2f MB。",
                 result.removed_count,
                 (double)result.saved_bytes / 1024.0 / 1024.0);
    }

    file_list_free(&stable_files);
}

int main(void) {
    SetConsoleOutputCP(CP_UTF8);

    int argc = 0;
    wchar_t **argv = NULL;
    parse_command_line(&argc, &argv);

    Options options;
    int parse_result = parse_args(argc, argv, &options);
    free_argv(argc, argv);

    if (parse_result == 2) {
        return 0;
    }
    if (parse_result == 0) {
        return 1;
    }

    ensure_parent_dir(options.log_file);
    g_log_file = options.log_file;

    if (!is_directory_path(options.root)) {
        log_error(L"根目录不存在或不是文件夹：%ls", options.root);
        free_options(&options);
        return 1;
    }

    DirList month_dirs = collect_month_dirs(options.root);
    if (month_dirs.count == 0) {
        log_error(L"未找到 YYYY-MM 格式的月份子文件夹：%ls", options.root);
        dir_list_free(&month_dirs);
        free_options(&options);
        return 1;
    }
    dir_list_free(&month_dirs);

    log_info(L"开始监测：%ls", options.root);
    log_info(L"处理方式：%ls；只扫描 YYYY-MM 月份目录；只删除内容哈希完全一致的文件。", action_name(options.action));

    SeenList seen = {0};
    for (;;) {
        log_info(L"%ls", SCAN_START_MESSAGE);
        scan_once(&options, &seen);
        log_info(L"%ls", SCAN_END_MESSAGE);
        compact_log_file(options.log_file);

        if (options.once) {
            break;
        }
        sleep_seconds(options.interval);
    }

    seen_free(&seen);
    free_options(&options);
    return 0;
}
