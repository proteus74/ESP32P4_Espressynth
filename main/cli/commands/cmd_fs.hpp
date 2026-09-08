#pragma once

// "ls"/"cat"/"rm": basic file commands over the VFS (works for the LittleFS mount, and any
// other mounted filesystem given an absolute path, e.g. "/sdcard/..."). Only registered when
// LittleFS is enabled (CONFIG_APP_ENABLE_LITTLEFS); backed by main/features/filesystem/littlefs_manager.hpp
// for the default (LittleFS) mount point.

#include "esp_console.h"
#include "features/filesystem/littlefs_manager.hpp"

#include <cstdio>
#include <cstring>
#include <dirent.h>
#include <sys/stat.h>

namespace Feature::CLI::Commands {

namespace {

// Relative paths (no leading '/') resolve under the LittleFS mount point; absolute paths
// (e.g. "/sdcard/foo") are used as-is.
inline void resolve_path(const char* arg, char* out, size_t out_size) {
    if (arg == nullptr || arg[0] == '\0') {
        std::snprintf(out, out_size, "%s", Feature::LittleFS::MOUNT_POINT);
    } else if (arg[0] == '/') {
        std::snprintf(out, out_size, "%s", arg);
    } else {
        std::snprintf(out, out_size, "%s/%s", Feature::LittleFS::MOUNT_POINT, arg);
    }
}

inline int cli_cmd_ls(int argc, char** argv) {
    char path[160];
    resolve_path(argc >= 2 ? argv[1] : nullptr, path, sizeof(path));

    DIR* dir = opendir(path);
    if (dir == nullptr) {
        printf("ls: cannot open '%s'\n", path);
        return 1;
    }

    struct dirent* entry;
    while ((entry = readdir(dir)) != nullptr) {
        char full_path[sizeof(path) + sizeof(entry->d_name) + 2];
        std::snprintf(full_path, sizeof(full_path), "%s/%s", path, entry->d_name);
        struct stat st{};
        if (stat(full_path, &st) == 0) {
            printf("%8ld  %s\n", static_cast<long>(st.st_size), entry->d_name);
        } else {
            printf("%8s  %s\n", "?", entry->d_name);
        }
    }
    closedir(dir);
    return 0;
}

inline int cli_cmd_cat(int argc, char** argv) {
    if (argc != 2) {
        printf("Usage: cat <path>\n");
        return 1;
    }
    char path[160];
    resolve_path(argv[1], path, sizeof(path));

    FILE* file = fopen(path, "r");
    if (file == nullptr) {
        printf("cat: cannot open '%s'\n", path);
        return 1;
    }
    char buffer[128];
    size_t read_count;
    while ((read_count = fread(buffer, 1, sizeof(buffer), file)) > 0) {
        fwrite(buffer, 1, read_count, stdout);
    }
    fclose(file);
    printf("\n");
    return 0;
}

inline int cli_cmd_rm(int argc, char** argv) {
    if (argc != 2) {
        printf("Usage: rm <path>\n");
        return 1;
    }
    char path[160];
    resolve_path(argv[1], path, sizeof(path));

    if (remove(path) != 0) {
        printf("rm: cannot remove '%s'\n", path);
        return 1;
    }
    return 0;
}

} // namespace

inline void register_fs() {
    const esp_console_cmd_t ls_cmd = {
        .command = "ls",
        .help = "List files (default: LittleFS root; relative paths resolve under it)",
        .hint = "[path]",
        .func = &cli_cmd_ls,
        .argtable = nullptr,
        .func_w_context = nullptr,
        .context = nullptr,
    };
    register_command(ls_cmd);

    const esp_console_cmd_t cat_cmd = {
        .command = "cat",
        .help = "Print a file's contents",
        .hint = "<path>",
        .func = &cli_cmd_cat,
        .argtable = nullptr,
        .func_w_context = nullptr,
        .context = nullptr,
    };
    register_command(cat_cmd);

    const esp_console_cmd_t rm_cmd = {
        .command = "rm",
        .help = "Delete a file",
        .hint = "<path>",
        .func = &cli_cmd_rm,
        .argtable = nullptr,
        .func_w_context = nullptr,
        .context = nullptr,
    };
    register_command(rm_cmd);
}

} // namespace Feature::CLI::Commands
