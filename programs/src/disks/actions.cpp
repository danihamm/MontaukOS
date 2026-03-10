/*
 * actions.cpp
 * MontaukOS Disk Tool — disk operations
 * Copyright (c) 2026 Daniel Hammer
 */

#include "disks.h"

// ============================================================================
// Refresh
// ============================================================================

void disktool_refresh() {
    auto& dt = g_state;
    dt.disk_count = 0;
    for (int port = 0; port < MAX_DISKS; port++) {
        Montauk::DiskInfo info;
        montauk::memset(&info, 0, sizeof(info));
        int r = montauk::diskinfo(&info, port);
        if (r == 0 && info.type != 0) {
            dt.disks[dt.disk_count++] = info;
        }
    }
    dt.part_count = montauk::partlist(dt.parts, MAX_PARTS);
    if (dt.selected_disk >= dt.disk_count)
        dt.selected_disk = dt.disk_count > 0 ? 0 : -1;
    dt.selected_part = -1;
    dt.scroll_y = 0;
}

// ============================================================================
// Create partition — confirmation dialog
// ============================================================================

void open_newpart_dialog() {
    auto& dt = g_state;
    if (dt.selected_disk < 0 || dt.selected_disk >= dt.disk_count) return;
    if (dt.np_dlg.open) return;

    int part_indices[MAX_PARTS];
    int nparts = get_disk_parts(part_indices, MAX_PARTS);

    auto& dlg = dt.np_dlg;
    dlg.will_init_gpt = (nparts == 0);
    dlg.hover_confirm = false;
    dlg.hover_cancel = false;

    Montauk::DiskInfo& disk = dt.disks[dt.selected_disk];
    char sz[24];
    format_disk_size(sz, sizeof(sz), disk.sectorCount, disk.sectorSizeLog);
    snprintf(dlg.disk_desc, sizeof(dlg.disk_desc), "Disk %d: %s (%s)",
             dt.selected_disk, disk.model, sz);

    if (dlg.will_init_gpt) {
        snprintf(dlg.warn_line1, sizeof(dlg.warn_line1),
                 "This will initialize a new GPT on this disk.");
        snprintf(dlg.warn_line2, sizeof(dlg.warn_line2),
                 "ALL existing partitions will be destroyed!");
    } else {
        snprintf(dlg.warn_line1, sizeof(dlg.warn_line1),
                 "This will add a new partition in the largest");
        snprintf(dlg.warn_line2, sizeof(dlg.warn_line2),
                 "free region on this disk.");
    }

    Montauk::WinCreateResult wres;
    const char* title = dlg.will_init_gpt ? "Initialize Disk" : "New Partition";
    if (montauk::win_create(title, NP_DLG_W, NP_DLG_H, &wres) < 0 || wres.id < 0) {
        set_status("Failed to open dialog");
        return;
    }
    dlg.win_id = wres.id;
    dlg.pixels = (uint32_t*)(uintptr_t)wres.pixelVa;
    dlg.open = true;

    render_newpart_window();
    montauk::win_present(dlg.win_id);
}

void close_newpart_dialog() {
    auto& dlg = g_state.np_dlg;
    if (!dlg.open) return;
    montauk::win_destroy(dlg.win_id);
    dlg.open = false;
}

void newpart_dialog_confirm() {
    auto& dt = g_state;
    auto& dlg = dt.np_dlg;

    if (dlg.will_init_gpt) {
        int r = montauk::gpt_init(dt.selected_disk);
        if (r < 0) {
            set_status("Failed to initialize GPT on disk");
            close_newpart_dialog();
            return;
        }
        set_status("Initialized GPT");
    }

    Montauk::GptAddParams params;
    montauk::memset(&params, 0, sizeof(params));
    params.blockDev = dt.selected_disk;
    params.startLba = 0;
    params.endLba = 0;
    params.typeGuid.Data1 = 0xEBD0A0A2;
    params.typeGuid.Data2 = 0xB9E5;
    params.typeGuid.Data3 = 0x4433;
    params.typeGuid.Data4[0] = 0x87; params.typeGuid.Data4[1] = 0xC0;
    params.typeGuid.Data4[2] = 0x68; params.typeGuid.Data4[3] = 0xB6;
    params.typeGuid.Data4[4] = 0xB7; params.typeGuid.Data4[5] = 0x26;
    params.typeGuid.Data4[6] = 0x99; params.typeGuid.Data4[7] = 0xC7;

    const char* pname = "Data";
    int i = 0;
    for (; pname[i] && i < 71; i++) params.name[i] = pname[i];
    params.name[i] = '\0';

    int r = montauk::gpt_add(&params);
    if (r < 0) {
        set_status("Failed to create partition");
    } else {
        set_status("Partition created successfully");
    }

    close_newpart_dialog();
    disktool_refresh();
}

void do_create_partition() {
    open_newpart_dialog();
}

// ============================================================================
// Mount partition
// ============================================================================

void do_mount_partition() {
    auto& dt = g_state;
    if (dt.selected_part < 0) {
        set_status("Select a partition first");
        return;
    }

    int part_indices[MAX_PARTS];
    int nparts = get_disk_parts(part_indices, MAX_PARTS);
    if (dt.selected_part >= nparts) return;

    int global_idx = part_indices[dt.selected_part];
    int driveNum = 1 + global_idx;
    if (driveNum >= 16) driveNum = 15;

    int r = montauk::fs_mount(global_idx, driveNum);
    if (r < 0) {
        set_status("Mount failed (no recognized filesystem)");
        return;
    }

    char msg[80];
    snprintf(msg, sizeof(msg), "Mounted as drive %d:/", driveNum);
    set_status(msg);
}

// ============================================================================
// Format dialog
// ============================================================================

void open_format_dialog() {
    auto& dt = g_state;
    if (dt.selected_part < 0) {
        set_status("Select a partition first");
        return;
    }

    int part_indices[MAX_PARTS];
    int nparts = get_disk_parts(part_indices, MAX_PARTS);
    if (dt.selected_part >= nparts) return;

    int global_idx = part_indices[dt.selected_part];

    auto& dlg = dt.fmt_dlg;
    dlg.global_part_index = global_idx;
    dlg.selected_fs = 0;
    dlg.hover_format = false;
    dlg.hover_cancel = false;

    // Build partition description
    Montauk::PartInfo& p = dt.parts[global_idx];
    Montauk::DiskInfo& disk = dt.disks[dt.selected_disk];
    char sz[24];
    format_disk_size(sz, sizeof(sz), p.sectorCount, disk.sectorSizeLog);
    const char* pname = p.name[0] ? p.name : "Partition";
    snprintf(dlg.part_desc, sizeof(dlg.part_desc), "%s (%s)", pname, sz);

    // Create dialog window
    Montauk::WinCreateResult wres;
    if (montauk::win_create("Format Partition", FMT_DLG_W, FMT_DLG_H, &wres) < 0 || wres.id < 0) {
        set_status("Failed to open format dialog");
        return;
    }
    dlg.win_id = wres.id;
    dlg.pixels = (uint32_t*)(uintptr_t)wres.pixelVa;
    dlg.open = true;

    render_format_window();
    montauk::win_present(dlg.win_id);
}

void close_format_dialog() {
    auto& dlg = g_state.fmt_dlg;
    if (!dlg.open) return;
    montauk::win_destroy(dlg.win_id);
    dlg.open = false;
}

void format_dialog_do_format() {
    auto& dt = g_state;
    auto& dlg = dt.fmt_dlg;

    Montauk::FsFormatParams params;
    montauk::memset(&params, 0, sizeof(params));
    params.partIndex = dlg.global_part_index;
    params.fsType = g_fsTypes[dlg.selected_fs].id;

    int r = montauk::fs_format(&params);
    if (r == 0) {
        set_status("Format complete");
        disktool_refresh();
    } else {
        set_status("Format failed");
    }

    close_format_dialog();
}
