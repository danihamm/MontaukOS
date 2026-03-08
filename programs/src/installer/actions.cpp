/*
 * actions.cpp
 * MontaukOS Installer — install operations
 * Copyright (c) 2026 Daniel Hammer
 */

#include "installer.h"

// ============================================================================
// Partition type GUIDs
// ============================================================================

// EFI System Partition: C12A7328-F81F-11D2-BA4B-00A0C93EC93B
static Montauk::PartGuid esp_type_guid() {
    Montauk::PartGuid g;
    g.Data1 = 0xC12A7328;
    g.Data2 = 0xF81F;
    g.Data3 = 0x11D2;
    g.Data4[0] = 0xBA; g.Data4[1] = 0x4B;
    g.Data4[2] = 0x00; g.Data4[3] = 0xA0;
    g.Data4[4] = 0xC9; g.Data4[5] = 0x3E;
    g.Data4[6] = 0xC9; g.Data4[7] = 0x3B;
    return g;
}

// Linux Filesystem: 0FC63DAF-8483-4772-8E79-3D69D8477DE4
static Montauk::PartGuid linux_fs_type_guid() {
    Montauk::PartGuid g;
    g.Data1 = 0x0FC63DAF;
    g.Data2 = 0x8483;
    g.Data3 = 0x4772;
    g.Data4[0] = 0x8E; g.Data4[1] = 0x79;
    g.Data4[2] = 0x3D; g.Data4[3] = 0x69;
    g.Data4[4] = 0xD8; g.Data4[5] = 0x47;
    g.Data4[6] = 0x7D; g.Data4[7] = 0xE4;
    return g;
}

// ============================================================================
// Refresh disk list
// ============================================================================

void installer_refresh_parts() {
    auto& st = g_state;
    st.part_count = montauk::partlist(st.parts, MAX_PARTS);
    if (st.part_count < 0) st.part_count = 0;
    if (st.selected_part < 0 || st.selected_part >= st.part_count)
        st.selected_part = st.part_count > 0 ? 0 : -1;
}

void installer_refresh_disks() {
    auto& st = g_state;
    st.disk_count = 0;
    for (int port = 0; port < MAX_DISKS; port++) {
        Montauk::DiskInfo info;
        montauk::memset(&info, 0, sizeof(info));
        int r = montauk::diskinfo(&info, port);
        if (r == 0 && info.type != 0) {
            st.disks[st.disk_count++] = info;
        }
    }
    if (st.selected_disk < 0 || st.selected_disk >= st.disk_count)
        st.selected_disk = st.disk_count > 0 ? 0 : -1;
}

// ============================================================================
// String helpers
// ============================================================================

static int slen(const char* s) {
    int n = 0;
    while (s[n]) n++;
    return n;
}

static void path_join(char* out, int outsize, const char* dir, const char* name) {
    int i = 0;
    for (int j = 0; dir[j] && i < outsize - 2; j++) out[i++] = dir[j];
    if (i > 0 && out[i - 1] != '/') out[i++] = '/';
    for (int j = 0; name[j] && i < outsize - 1; j++) out[i++] = name[j];
    out[i] = '\0';
}

// ============================================================================
// Copy a single file from src_path to dst_path
// ============================================================================

static bool copy_file(const char* src_path, const char* dst_path) {
    int src = montauk::open(src_path);
    if (src < 0) return false;

    uint64_t size = montauk::getsize(src);

    // Create destination (returns handle)
    int dst = montauk::fcreate(dst_path);
    if (dst < 0) {
        montauk::close(src);
        return false;
    }

    if (size == 0) {
        // Empty file — just create it
        montauk::close(dst);
        montauk::close(src);
        return true;
    }

    // Use large buffer for faster copies (256 KB)
    static constexpr uint64_t CHUNK = 256 * 1024;
    uint8_t* buf = (uint8_t*)montauk::malloc(CHUNK);
    if (!buf) {
        montauk::close(dst);
        montauk::close(src);
        return false;
    }

    // Show progress for large files (> 1 MB)
    bool show_progress = (size > 1024 * 1024);
    uint64_t last_progress_mb = 0;

    uint64_t offset = 0;
    bool ok = true;
    while (offset < size) {
        uint64_t to_read = size - offset;
        if (to_read > CHUNK) to_read = CHUNK;

        int rd = montauk::read(src, buf, offset, to_read);
        if (rd <= 0) { ok = false; break; }

        int wr = montauk::fwrite(dst, buf, offset, rd);
        if (wr < rd) { ok = false; break; }

        offset += rd;

        if (show_progress) {
            uint64_t cur_mb = offset / (1024 * 1024);
            if (cur_mb > last_progress_mb) {
                last_progress_mb = cur_mb;
                uint64_t total_mb = size / (1024 * 1024);
                char prog[64];
                snprintf(prog, sizeof(prog), "    %lu / %lu MB",
                         (unsigned long)cur_mb, (unsigned long)total_mb);
                set_status(prog);
                flush_ui();
            }
        }
    }

    montauk::mfree(buf);
    montauk::close(dst);
    montauk::close(src);
    return ok;
}

// ============================================================================
// Recursively copy directory contents from ramdisk to target drive
// ============================================================================

static int g_files_copied;
static int g_dirs_created;

// Copy all entries from src_dir (on ramdisk) to dst_dir (on target drive).
// If skip_toplevel is non-null, skip that directory name at the top level.
static bool copy_recursive(const char* src_dir, const char* dst_dir,
                            const char* skip_toplevel = nullptr) {
    const char* names[256];
    int count = montauk::readdir(src_dir, names, 256);
    if (count < 0) return true; // not a directory or empty, skip

    // Ramdisk readdir returns names relative to the drive root with the
    // full internal path (e.g., for readdir("0:/os"), names come back as
    // "os/init.elf", "os/shell.elf", etc.).  We need to strip the prefix
    // that corresponds to the local path portion of src_dir.
    //
    // Find the local path after the "N:/" prefix.
    const char* src_local = src_dir;
    for (int k = 0; src_local[k]; k++) {
        if (src_local[k] == ':') {
            src_local += k + 1;
            if (src_local[0] == '/') src_local++;
            break;
        }
    }
    int prefix_len = slen(src_local);
    // If prefix is non-empty, we also skip the trailing '/'
    if (prefix_len > 0 && src_local[prefix_len - 1] != '/') prefix_len++;

    for (int i = 0; i < count; i++) {
        const char* raw_name = names[i];

        // Strip prefix to get basename
        const char* basename = raw_name;
        if (prefix_len > 0 && slen(raw_name) > prefix_len) {
            basename = raw_name + prefix_len;
        }

        // Skip "." and ".."
        if (basename[0] == '.' && (basename[1] == '\0' || basename[1] == '/')) continue;
        if (basename[0] == '.' && basename[1] == '.' && (basename[2] == '\0' || basename[2] == '/')) continue;

        int blen = slen(basename);

        // Check if this is a directory (trailing '/')
        bool is_dir = (blen > 0 && basename[blen - 1] == '/');

        if (is_dir) {
            // Strip trailing '/' for the name
            char dir_name[256];
            int j = 0;
            for (; j < blen - 1 && j < 255; j++) dir_name[j] = basename[j];
            dir_name[j] = '\0';

            // Skip the installer app — no need on the installed system
            if (strcmp(dir_name, "installer") == 0 &&
                strcmp(src_local, "apps") == 0) continue;

            // Skip requested top-level directory
            if (skip_toplevel && strcmp(dir_name, skip_toplevel) == 0) continue;

            // Create directory on target
            char target_path[256];
            path_join(target_path, sizeof(target_path), dst_dir, dir_name);

            montauk::fmkdir(target_path);
            g_dirs_created++;

            char log_msg[64];
            snprintf(log_msg, sizeof(log_msg), "  mkdir %s", dir_name);
            add_log(log_msg);
            flush_ui();

            // Recurse into this directory
            char src_subdir[256];
            path_join(src_subdir, sizeof(src_subdir), src_dir, dir_name);

            if (!copy_recursive(src_subdir, target_path))
                return false;
        } else {
            // Skip ramdisk and limine.conf — installed system boots from
            // disk and gets a fresh config without the ramdisk module.
            if (strcmp(basename, "ramdisk.tar") == 0) continue;
            if (strcmp(basename, "limine.conf") == 0) continue;

            // It's a file — copy it
            char src_path[256];
            // Build source path: drive prefix + raw_name (which is the full internal path)
            // src_dir starts with "0:/" so we need "0:/" + raw_name
            snprintf(src_path, sizeof(src_path), "0:/%s", raw_name);

            char dst_path[256];
            path_join(dst_path, sizeof(dst_path), dst_dir, basename);

            char log_msg[64];
            snprintf(log_msg, sizeof(log_msg), "  copy %s", basename);
            add_log(log_msg);
            flush_ui();

            if (!copy_file(src_path, dst_path))
                return false;

            g_files_copied++;
        }
    }

    return true;
}

// ============================================================================
// GPT + partition helper
// ============================================================================

static void set_gpt_name(Montauk::GptAddParams& params, const char* name) {
    int i = 0;
    for (; name[i] && i < 71; i++) params.name[i] = name[i];
    params.name[i] = '\0';
}

// ============================================================================
// Install: EFI + ext2 scheme
// ============================================================================

static void install_efi_ext2(int disk) {
    auto& st = g_state;
    char path_buf[64];
    int r;

    // Step 1: Initialize GPT
    add_log("Initializing GPT...");
    flush_ui();
    r = montauk::gpt_init(disk);
    if (r < 0) {
        add_log("ERROR: Failed to initialize GPT");
        flush_ui(); st.step = STEP_ERROR; return;
    }
    add_log("  GPT initialized");
    flush_ui();

    // Step 2: Create EFI System Partition (128 MB)
    // After gpt_init, FirstUsableLba = 34.
    static constexpr uint64_t ESP_SECTORS = 128 * 1024 * 2; // 128 MB in 512-byte sectors
    static constexpr uint64_t FIRST_USABLE_LBA = 34;

    add_log("Creating EFI partition (128 MB)...");
    flush_ui();
    {
        Montauk::GptAddParams params;
        montauk::memset(&params, 0, sizeof(params));
        params.blockDev = disk;
        params.startLba = FIRST_USABLE_LBA;
        params.endLba = FIRST_USABLE_LBA + ESP_SECTORS - 1;
        params.typeGuid = esp_type_guid();
        set_gpt_name(params, "EFI System");

        r = montauk::gpt_add(&params);
        if (r < 0) {
            add_log("ERROR: Failed to create EFI partition");
            flush_ui(); st.step = STEP_ERROR; return;
        }
    }
    add_log("  EFI partition created");
    flush_ui();

    // Step 3: Create Linux partition (remaining space)
    add_log("Creating ext2 partition...");
    flush_ui();
    {
        Montauk::GptAddParams params;
        montauk::memset(&params, 0, sizeof(params));
        params.blockDev = disk;
        params.startLba = 0; // auto-fill largest free region
        params.endLba = 0;
        params.typeGuid = linux_fs_type_guid();
        set_gpt_name(params, "MontaukOS");

        r = montauk::gpt_add(&params);
        if (r < 0) {
            add_log("ERROR: Failed to create Linux partition");
            flush_ui(); st.step = STEP_ERROR; return;
        }
    }
    add_log("  ext2 partition created");
    flush_ui();

    // Step 4: Find partition indices
    Montauk::PartInfo parts[MAX_PARTS];
    int part_count = montauk::partlist(parts, MAX_PARTS);
    int efi_idx = -1, linux_idx = -1;
    for (int p = 0; p < part_count; p++) {
        if (parts[p].blockDev != disk) continue;
        // EFI partition starts at FIRST_USABLE_LBA
        if (parts[p].startLba == FIRST_USABLE_LBA)
            efi_idx = p;
        else
            linux_idx = p;
    }
    if (efi_idx < 0 || linux_idx < 0) {
        add_log("ERROR: Could not find partitions");
        flush_ui(); st.step = STEP_ERROR; return;
    }

    // Step 5: Format EFI partition as FAT32
    add_log("Formatting EFI partition (FAT32)...");
    flush_ui();
    {
        Montauk::FsFormatParams fmt;
        montauk::memset(&fmt, 0, sizeof(fmt));
        fmt.partIndex = efi_idx;
        fmt.fsType = Montauk::FS_TYPE_FAT32;
        r = montauk::fs_format(&fmt);
        if (r < 0) {
            add_log("ERROR: FAT32 format failed");
            flush_ui(); st.step = STEP_ERROR; return;
        }
    }
    add_log("  FAT32 formatted");
    flush_ui();

    // Step 6: Format Linux partition as ext2
    add_log("Formatting root partition (ext2)...");
    flush_ui();
    {
        Montauk::FsFormatParams fmt;
        montauk::memset(&fmt, 0, sizeof(fmt));
        fmt.partIndex = linux_idx;
        fmt.fsType = Montauk::FS_TYPE_EXT2;
        r = montauk::fs_format(&fmt);
        if (r < 0) {
            add_log("ERROR: ext2 format failed");
            flush_ui(); st.step = STEP_ERROR; return;
        }
    }
    add_log("  ext2 formatted");
    flush_ui();

    // Step 7: Mount both partitions
    add_log("Mounting partitions...");
    flush_ui();
    int efi_drive = 14, root_drive = 15;
    r = montauk::fs_mount(efi_idx, efi_drive);
    if (r < 0) {
        add_log("ERROR: EFI mount failed");
        flush_ui(); st.step = STEP_ERROR; return;
    }
    r = montauk::fs_mount(linux_idx, root_drive);
    if (r < 0) {
        add_log("ERROR: ext2 mount failed");
        flush_ui(); st.step = STEP_ERROR; return;
    }
    add_log("  Mounted");
    flush_ui();

    char efi_root[8], ext2_root[8];
    snprintf(efi_root, sizeof(efi_root), "%d:/", efi_drive);
    snprintf(ext2_root, sizeof(ext2_root), "%d:/", root_drive);

    // Step 8: Create EFI boot directory structure
    add_log("Creating boot directories...");
    flush_ui();
    snprintf(path_buf, sizeof(path_buf), "%d:/EFI", efi_drive);
    montauk::fmkdir(path_buf);
    snprintf(path_buf, sizeof(path_buf), "%d:/EFI/BOOT", efi_drive);
    montauk::fmkdir(path_buf);
    snprintf(path_buf, sizeof(path_buf), "%d:/boot", efi_drive);
    montauk::fmkdir(path_buf);

    // Step 9: Copy boot files to EFI partition
    add_log("Copying boot files to EFI partition...");
    flush_ui();

    char efi_boot[16];
    snprintf(efi_boot, sizeof(efi_boot), "%d:/boot", efi_drive);
    if (!copy_recursive("0:/boot", efi_boot)) {
        add_log("ERROR: Boot file copy failed");
        flush_ui(); st.step = STEP_ERROR; return;
    }

    // Step 10: Copy root filesystem to ext2 partition (skip boot/)
    add_log("Copying root filesystem to ext2...");
    flush_ui();
    if (!copy_recursive("0:/", ext2_root, "boot")) {
        add_log("ERROR: Root filesystem copy failed");
        flush_ui(); st.step = STEP_ERROR; return;
    }

    char copy_msg[64];
    snprintf(copy_msg, sizeof(copy_msg), "  %d files, %d directories copied",
             g_files_copied, g_dirs_created);
    add_log(copy_msg);
    flush_ui();

    // Step 11: Write limine.conf to EFI partition
    add_log("Writing boot configuration...");
    flush_ui();
    snprintf(path_buf, sizeof(path_buf), "%d:/boot/limine/limine.conf", efi_drive);
    {
        static const char limine_conf[] =
            "timeout: 0\n"
            "\n"
            "/montaukos\n"
            "    protocol: limine\n"
            "    path: boot():/boot/kernel\n";

        int conf_fd = montauk::fcreate(path_buf);
        if (conf_fd < 0) {
            add_log("ERROR: Failed to write limine.conf");
            flush_ui(); st.step = STEP_ERROR; return;
        }
        montauk::fwrite(conf_fd, (const uint8_t*)limine_conf, 0, sizeof(limine_conf) - 1);
        montauk::close(conf_fd);
    }
    add_log("  limine.conf written");
    flush_ui();

    // Step 12: Copy BOOTX64.EFI to EFI/BOOT/
    add_log("Installing EFI bootloader...");
    flush_ui();
    snprintf(path_buf, sizeof(path_buf), "%d:/EFI/BOOT/BOOTX64.EFI", efi_drive);
    if (!copy_file("0:/boot/limine/BOOTX64.EFI", path_buf)) {
        add_log("ERROR: Failed to install BOOTX64.EFI");
        flush_ui(); st.step = STEP_ERROR; return;
    }
    add_log("  BOOTX64.EFI installed");
    flush_ui();

    // Done
    add_log("Installation complete!");
    st.step = STEP_DONE;
    set_status("MontaukOS installed successfully");
    flush_ui();
}

// ============================================================================
// Install: Single FAT32 scheme (legacy)
// ============================================================================

static void install_single_fat32(int disk) {
    auto& st = g_state;
    char path_buf[64];
    int r;

    // Step 1: Initialize GPT
    add_log("Initializing GPT...");
    flush_ui();
    r = montauk::gpt_init(disk);
    if (r < 0) {
        add_log("ERROR: Failed to initialize GPT");
        flush_ui(); st.step = STEP_ERROR; return;
    }
    add_log("  GPT initialized");
    flush_ui();

    // Step 2: Create EFI System Partition (entire disk)
    add_log("Creating EFI System Partition...");
    flush_ui();
    Montauk::GptAddParams params;
    montauk::memset(&params, 0, sizeof(params));
    params.blockDev = disk;
    params.startLba = 0;
    params.endLba = 0;
    params.typeGuid = esp_type_guid();
    set_gpt_name(params, "EFI System");

    r = montauk::gpt_add(&params);
    if (r < 0) {
        add_log("ERROR: Failed to create partition");
        flush_ui(); st.step = STEP_ERROR; return;
    }
    add_log("  Partition created");
    flush_ui();

    // Step 3: Format as FAT32
    add_log("Formatting as FAT32...");
    flush_ui();

    Montauk::PartInfo parts[MAX_PARTS];
    int part_count = montauk::partlist(parts, MAX_PARTS);
    int esp_index = -1;
    for (int p = 0; p < part_count; p++) {
        if (parts[p].blockDev == disk) {
            esp_index = p;
            break;
        }
    }

    if (esp_index < 0) {
        add_log("ERROR: Could not find partition");
        flush_ui(); st.step = STEP_ERROR; return;
    }

    Montauk::FsFormatParams fmt;
    montauk::memset(&fmt, 0, sizeof(fmt));
    fmt.partIndex = esp_index;
    fmt.fsType = Montauk::FS_TYPE_FAT32;

    r = montauk::fs_format(&fmt);
    if (r < 0) {
        add_log("ERROR: FAT32 format failed");
        flush_ui(); st.step = STEP_ERROR; return;
    }
    add_log("  FAT32 formatted");
    flush_ui();

    // Step 4: Mount the partition
    add_log("Mounting partition...");
    flush_ui();
    int drive_num = 15;
    r = montauk::fs_mount(esp_index, drive_num);
    if (r < 0) {
        add_log("ERROR: Mount failed");
        flush_ui(); st.step = STEP_ERROR; return;
    }

    char drive_root[8];
    snprintf(drive_root, sizeof(drive_root), "%d:/", drive_num);
    add_log("  Mounted");
    flush_ui();

    // Step 5: Create EFI boot directory structure
    add_log("Creating boot directories...");
    flush_ui();

    snprintf(path_buf, sizeof(path_buf), "%d:/EFI", drive_num);
    montauk::fmkdir(path_buf);
    snprintf(path_buf, sizeof(path_buf), "%d:/EFI/BOOT", drive_num);
    montauk::fmkdir(path_buf);

    // Step 6: Copy entire root filesystem
    add_log("Copying root filesystem...");
    flush_ui();

    if (!copy_recursive("0:/", drive_root)) {
        add_log("ERROR: File copy failed");
        flush_ui(); st.step = STEP_ERROR; return;
    }

    char copy_msg[64];
    snprintf(copy_msg, sizeof(copy_msg), "  %d files, %d directories copied",
             g_files_copied, g_dirs_created);
    add_log(copy_msg);
    flush_ui();

    // Step 7: Write limine.conf without ramdisk module
    add_log("Writing boot configuration...");
    flush_ui();
    snprintf(path_buf, sizeof(path_buf), "%d:/boot/limine/limine.conf", drive_num);
    {
        static const char limine_conf[] =
            "timeout: 0\n"
            "\n"
            "/montaukos\n"
            "    protocol: limine\n"
            "    path: boot():/boot/kernel\n";

        int conf_fd = montauk::fcreate(path_buf);
        if (conf_fd < 0) {
            add_log("ERROR: Failed to write limine.conf");
            flush_ui(); st.step = STEP_ERROR; return;
        }
        montauk::fwrite(conf_fd, (const uint8_t*)limine_conf, 0, sizeof(limine_conf) - 1);
        montauk::close(conf_fd);
    }
    add_log("  limine.conf written");
    flush_ui();

    // Step 8: Copy BOOTX64.EFI to EFI/BOOT/
    add_log("Installing EFI bootloader...");
    flush_ui();
    snprintf(path_buf, sizeof(path_buf), "%d:/EFI/BOOT/BOOTX64.EFI", drive_num);
    if (!copy_file("0:/boot/limine/BOOTX64.EFI", path_buf)) {
        add_log("ERROR: Failed to install BOOTX64.EFI");
        flush_ui(); st.step = STEP_ERROR; return;
    }
    add_log("  BOOTX64.EFI installed");
    flush_ui();

    // Done
    add_log("Installation complete!");
    st.step = STEP_DONE;
    set_status("MontaukOS installed successfully");
    flush_ui();
}

// ============================================================================
// Install MontaukOS to the selected disk
// ============================================================================

void do_install() {
    auto& st = g_state;
    int disk = st.selected_disk;

    if (disk < 0 || disk >= st.disk_count) {
        st.step = STEP_ERROR;
        add_log("No disk selected");
        flush_ui();
        return;
    }

    g_files_copied = 0;
    g_dirs_created = 0;

    switch (st.partition_scheme) {
    case SCHEME_EFI_EXT2:
        install_efi_ext2(disk);
        break;
    case SCHEME_SINGLE_FAT32:
        install_single_fat32(disk);
        break;
    default:
        st.step = STEP_ERROR;
        add_log("Unknown partition scheme");
        flush_ui();
        break;
    }
}

// ============================================================================
// Update OS and apps on an existing partition
// ============================================================================

void do_update() {
    auto& st = g_state;

    if (st.selected_part < 0 || st.selected_part >= st.part_count) {
        st.step = STEP_ERROR;
        add_log("No partition selected");
        flush_ui();
        return;
    }

    g_files_copied = 0;
    g_dirs_created = 0;

    int part_idx = st.selected_part;
    int r;

    // Step 1: Mount the target partition
    add_log("Mounting target partition...");
    flush_ui();
    int drive_num = 15;
    r = montauk::fs_mount(part_idx, drive_num);
    if (r < 0) {
        add_log("ERROR: Mount failed");
        flush_ui(); st.step = STEP_ERROR; return;
    }

    char drive_root[8];
    snprintf(drive_root, sizeof(drive_root), "%d:/", drive_num);
    add_log("  Mounted");
    flush_ui();

    // Step 2: Update os/ — remove old and copy fresh
    add_log("Updating os/...");
    flush_ui();

    char path_buf[64];
    snprintf(path_buf, sizeof(path_buf), "%d:/os", drive_num);
    montauk::fmkdir(path_buf); // ensure it exists

    if (!copy_recursive("0:/os", path_buf)) {
        add_log("ERROR: Failed to update os/");
        flush_ui(); st.step = STEP_ERROR; return;
    }

    // Step 3: Update apps/ — copy all, overwriting existing and adding new
    add_log("Updating apps/...");
    flush_ui();

    snprintf(path_buf, sizeof(path_buf), "%d:/apps", drive_num);
    montauk::fmkdir(path_buf); // ensure it exists

    if (!copy_recursive("0:/apps", path_buf)) {
        add_log("ERROR: Failed to update apps/");
        flush_ui(); st.step = STEP_ERROR; return;
    }

    char copy_msg[64];
    snprintf(copy_msg, sizeof(copy_msg), "  %d files, %d directories updated",
             g_files_copied, g_dirs_created);
    add_log(copy_msg);
    flush_ui();

    // Done
    add_log("Update complete!");
    st.step = STEP_DONE;
    set_status("MontaukOS updated successfully");
    flush_ui();
}
