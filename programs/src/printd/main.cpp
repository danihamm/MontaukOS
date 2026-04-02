/*
 * main.cpp
 * MontaukOS userspace print spooler daemon
 * Copyright (c) 2026 Daniel Hammer
 */

#include <montauk/syscall.h>
#include <print/print.hpp>
#include "test_page_jpeg.hpp"

extern "C" {
#include <stdio.h>
#include <string.h>
}

using namespace print;

static constexpr bool ENABLE_BACKGROUND_PROBE = false;

enum class ClaimResult {
    None,
    Claimed,
    Failed,
};

static void write_daemon_state(const char* phase, const char* subject = nullptr,
                               const char* detail = nullptr) {
    char now[32] = {};
    now_string(now, sizeof(now));

    char buf[512];
    int n = snprintf(buf, sizeof(buf),
                     "time=%s\npid=%d\nphase=%s\nsubject=%s\ndetail=%s\n",
                     now,
                     montauk::getpid(),
                     phase ? phase : "",
                     subject ? subject : "",
                     detail ? detail : "");
    if (n < 0) return;
    write_text_file_atomic(SPOOL_DAEMON_STATE_PATH, buf);
}

static void log_msg(const char* fmt, ...) {
    va_list ap;
    va_start(ap, fmt);
    printf("[printd] ");
    vprintf(fmt, ap);
    printf("\n");
    va_end(ap);
}

static void set_job_state(JobMeta* job, const char* state, const char* message) {
    safe_copy(job->state, sizeof(job->state), state);
    if (message && *message) safe_copy(job->status_message, sizeof(job->status_message), message);
    now_string(job->updated_at, sizeof(job->updated_at));
}

static void set_job_debug(JobMeta* job, const char* detail) {
    if (job == nullptr) return;
    safe_copy(job->debug_info, sizeof(job->debug_info), detail ? detail : "");
}

static void set_job_debug_from_result(JobMeta* job, const IppPrintResult* result, const char* prefix) {
    if (job == nullptr) return;
    char detail[MAX_DEBUG_LEN] = {};
    summarize_ipp_print_result(result, detail, sizeof(detail));
    if (prefix && *prefix) {
        if (detail[0]) {
            char joined[MAX_DEBUG_LEN];
            snprintf(joined, sizeof(joined), "%s; %s", prefix, detail);
            safe_copy(job->debug_info, sizeof(job->debug_info), joined);
        } else {
            safe_copy(job->debug_info, sizeof(job->debug_info), prefix);
        }
    } else {
        safe_copy(job->debug_info, sizeof(job->debug_info), detail);
    }
}

static void persist_job_debug(const char* active_path, JobMeta* job, const char* detail) {
    if (active_path == nullptr || *active_path == '\0' || job == nullptr) return;
    set_job_debug(job, detail);
    now_string(job->updated_at, sizeof(job->updated_at));
    save_job_to_path_atomic(active_path, job);
}

static bool write_pid_file() {
    char pid[32];
    snprintf(pid, sizeof(pid), "%d\n", montauk::getpid());
    return write_text_file_atomic(SPOOL_DAEMON_PID_PATH, pid);
}

static bool move_job_file(const char* src, const char* dst, char* err = nullptr, int err_len = 0) {
    if (err && err_len > 0) err[0] = '\0';
    if (src == nullptr || dst == nullptr || *src == '\0' || *dst == '\0') {
        safe_copy(err, err_len, "invalid source or destination path");
        return false;
    }
    if (rename(src, dst) == 0) return true;

    uint8_t* data = nullptr;
    int len = 0;
    char io_err[128] = {};
    if (!read_file_bytes(src, &data, &len, io_err, sizeof(io_err))) {
        char detail[160];
        snprintf(detail, sizeof(detail), "rename failed; could not read source (%s)", io_err);
        safe_copy(err, err_len, detail);
        return false;
    }

    bool ok = write_file_atomic(dst, data, len, io_err, sizeof(io_err));
    free(data);
    if (!ok) {
        char detail[160];
        snprintf(detail, sizeof(detail), "rename failed; could not write destination (%s)", io_err);
        safe_copy(err, err_len, detail);
        return false;
    }

    if (remove(src) != 0) {
        remove(dst);
        safe_copy(err, err_len, "rename failed; copied destination but could not delete source");
        return false;
    }
    return true;
}

static void auto_probe_configured_printer(char* last_uri, int last_uri_len,
                                          uint64_t* last_probe_ms, bool* last_probe_ok,
                                          bool force = false) {
    if (last_uri == nullptr || last_probe_ms == nullptr || last_probe_ok == nullptr) return;

    char current_uri[MAX_PATH_LEN] = {};
    bool have_uri = read_default_printer_uri(current_uri, sizeof(current_uri));
    uint64_t now = montauk::get_milliseconds();

    if (!have_uri || current_uri[0] == '\0') {
        bool had_cached_uri = last_uri[0] != '\0';
        if (had_cached_uri || file_exists(SPOOL_PROBE_STATE_PATH)) {
            clear_printer_probe_state();
            if (had_cached_uri) log_msg("cleared printer probe state (no default printer configured)");
        }
        last_uri[0] = '\0';
        *last_probe_ms = 0;
        *last_probe_ok = false;
        return;
    }

    bool uri_changed = strcmp(last_uri, current_uri) != 0;
    if (!force && !uri_changed && *last_probe_ms != 0)
        return;

    ProbeState state = {};
    char err[128] = {};
    bool ok = probe_printer_uri(current_uri, &state, err, sizeof(err));
    if (!save_printer_probe_state(&state))
        log_msg("warning: failed to persist printer probe state for %s", current_uri);

    if (ok) {
        if (state.caps.printer_name[0])
            log_msg("auto-probed printer %s (%s)", state.printer_uri, state.caps.printer_name);
        else
            log_msg("auto-probed printer %s", state.printer_uri);
        if (state.detail[0]) log_msg("probe detail: %s", state.detail);
    } else {
        log_msg("auto-probe failed for %s: %s",
                state.printer_uri[0] ? state.printer_uri : current_uri,
                state.message[0] ? state.message : (err[0] ? err : "probe failed"));
        if (state.detail[0]) log_msg("probe detail: %s", state.detail);
    }

    safe_copy(last_uri, last_uri_len, state.printer_uri[0] ? state.printer_uri : current_uri);
    *last_probe_ms = now;
    *last_probe_ok = ok;
}

static void requeue_stale_active_jobs() {
    DIR* dir = opendir(SPOOL_ACTIVE_DIR);
    if (!dir) return;

    struct dirent* ent = nullptr;
    while ((ent = readdir(dir)) != nullptr) {
        if (ent->d_name[0] == '.') continue;

        char src[MAX_PATH_LEN];
        char dst[MAX_PATH_LEN];
        path_join(src, sizeof(src), SPOOL_ACTIVE_DIR, ent->d_name);
        path_join(dst, sizeof(dst), SPOOL_QUEUE_DIR, ent->d_name);

        JobMeta job = {};
        if (load_job_from_path(src, &job)) {
            set_job_state(&job, "queued", "Re-queued after daemon restart");
            save_job_to_path_atomic(src, &job);
        }
        char move_err[160] = {};
        if (!move_job_file(src, dst, move_err, sizeof(move_err))) {
            log_msg("warning: failed to requeue stale job %s: %s",
                    ent->d_name, move_err[0] ? move_err : "move failed");
        }
    }
    closedir(dir);
}

static ClaimResult claim_next_job(char* out_active_path, int out_active_path_len) {
    DIR* dir = opendir(SPOOL_QUEUE_DIR);
    if (!dir) return ClaimResult::None;

    char best_name[64] = {};
    struct dirent* ent = nullptr;
    while ((ent = readdir(dir)) != nullptr) {
        if (ent->d_name[0] == '.') continue;
        if (best_name[0] == '\0' || strcmp(ent->d_name, best_name) < 0)
            safe_copy(best_name, sizeof(best_name), ent->d_name);
    }
    closedir(dir);
    if (best_name[0] == '\0') return ClaimResult::None;

    char queued[MAX_PATH_LEN];
    char active[MAX_PATH_LEN];
    path_join(queued, sizeof(queued), SPOOL_QUEUE_DIR, best_name);
    path_join(active, sizeof(active), SPOOL_ACTIVE_DIR, best_name);

    write_daemon_state("claiming", best_name, queued);
    char move_err[160] = {};
    if (!move_job_file(queued, active, move_err, sizeof(move_err))) {
        char detail[256];
        snprintf(detail, sizeof(detail), "%s -> %s (%s)",
                 queued, active,
                 move_err[0] ? move_err : "move failed");
        write_daemon_state("claim-failed", best_name, detail);
        log_msg("failed to claim job %s: %s", best_name, detail);
        return ClaimResult::Failed;
    }
    write_daemon_state("claimed", best_name, active);
    safe_copy(out_active_path, out_active_path_len, active);
    return ClaimResult::Claimed;
}

static void finalize_job(const char* active_path, const char* target_dir, JobMeta* job) {
    save_job_to_path_atomic(active_path, job);
    char target[MAX_PATH_LEN];
    make_job_file_path(target_dir, job->id, target, sizeof(target));
    char move_err[160] = {};
    if (!move_job_file(active_path, target, move_err, sizeof(move_err))) {
        log_msg("warning: failed to move job %s to %s", job->id, target_dir);
        write_daemon_state("finalize-failed", job->id,
                           move_err[0] ? move_err : target_dir);
    }
}

static bool try_print_test_page(const char* active_path, JobMeta* job,
                                IppPrintResult* result, char* err, int err_len) {
    persist_job_debug(active_path, job, "querying printer capabilities");

    IppCapabilities caps = {};
    bool have_caps = ipp_get_printer_capabilities(job->printer_uri, &caps, err, err_len);
    if (have_caps) {
        char detail[MAX_DEBUG_LEN] = {};
        summarize_ipp_capabilities(&caps, detail, sizeof(detail));
        persist_job_debug(active_path, job, detail);
    } else if (err && err[0]) {
        char detail[MAX_DEBUG_LEN];
        snprintf(detail, sizeof(detail), "capability probe failed; attempting direct submit (%s)", err);
        persist_job_debug(active_path, job, detail);
    }

    const struct {
        const char* mime;
        bool (*generate)(uint8_t**, int*);
        bool allowed;
    } attempts[] = {
        {"application/pdf", generate_test_page_pdf, !have_caps || caps.supports_pdf},
        {"image/jpeg",      generate_test_page_jpeg, !have_caps || caps.supports_jpeg},
        {"text/plain",      generate_test_page_text, !have_caps || caps.supports_text},
    };

    if (have_caps && !caps.supports_pdf && !caps.supports_jpeg && !caps.supports_text) {
        if (caps.supported_formats[0]) {
            snprintf(err, (size_t)err_len,
                     "printer does not advertise a built-in test-page format (supports: %s)",
                     caps.supported_formats);
        } else {
            safe_copy(err, err_len, "printer does not advertise a built-in test-page format");
        }
        return false;
    }

    for (size_t i = 0; i < sizeof(attempts) / sizeof(attempts[0]); i++) {
        if (!attempts[i].allowed) continue;

        uint8_t* data = nullptr;
        int len = 0;
        if (!attempts[i].generate(&data, &len)) {
            safe_copy(err, err_len, "failed to generate test page");
            return false;
        }

        char detail[MAX_DEBUG_LEN];
        snprintf(detail, sizeof(detail), "submitting test page as %s (%d bytes, %d %s)",
                 attempts[i].mime, len,
                 job->copies > 0 ? job->copies : 1,
                 (job->copies > 1) ? "copies" : "copy");
        persist_job_debug(active_path, job, detail);

        bool ok = ipp_print_buffer(job->printer_uri, job->job_name, job->user_name,
                                   job->copies, attempts[i].mime, data, len, result, err, err_len);
        free(data);
        if (ok) return true;
    }

    return false;
}

static bool try_print_document(const char* active_path, JobMeta* job,
                               IppPrintResult* result, char* err, int err_len) {
    persist_job_debug(active_path, job, "loading document bytes");

    uint8_t* data = nullptr;
    int len = 0;
    if (!read_file_bytes(job->doc_path, &data, &len, err, err_len)) return false;

    char detail[MAX_DEBUG_LEN];
    snprintf(detail, sizeof(detail), "submitting %s document (%d bytes, %d %s)",
             job->doc_format[0] ? job->doc_format : "application/octet-stream", len,
             job->copies > 0 ? job->copies : 1,
             (job->copies > 1) ? "copies" : "copy");
    persist_job_debug(active_path, job, detail);

    bool ok = ipp_print_buffer(job->printer_uri, job->job_name, job->user_name,
                               job->copies, job->doc_format, data, len, result, err, err_len);
    free(data);
    return ok;
}

static void process_job(const char* active_path) {
    JobMeta job = {};
    if (!load_job_from_path(active_path, &job)) {
        log_msg("dropping unreadable job file: %s", active_path);
        write_daemon_state("unreadable-job", active_path, nullptr);
        remove(active_path);
        return;
    }

    log_msg("processing job %s (%s)", job.id, job.job_name);
    write_daemon_state("processing", job.id, job.job_name);
    set_job_state(&job, "printing", "Sending to printer");
    set_job_debug(&job, "dispatching queued job");
    save_job_to_path_atomic(active_path, &job);

    IppPrintResult result = {};
    char err[128] = {};
    bool ok = false;

    if (strcmp(job.source_kind, "test-page") == 0)
        ok = try_print_test_page(active_path, &job, &result, err, sizeof(err));
    else
        ok = try_print_document(active_path, &job, &result, err, sizeof(err));

    if (ok) {
        job.remote_job_id = result.job_id;
        set_job_state(&job, "completed",
                      result.status_message[0] ? result.status_message : "Printed");
        set_job_debug_from_result(&job, &result, nullptr);
        finalize_job(active_path, SPOOL_DONE_DIR, &job);
        log_msg("job %s completed (printer job id %d)", job.id, result.job_id);
        write_daemon_state("completed", job.id, job.status_message);
    } else {
        set_job_state(&job, "failed", err[0] ? err : "Print failed");
        set_job_debug_from_result(&job, &result, err[0] ? err : nullptr);
        finalize_job(active_path, SPOOL_FAILED_DIR, &job);
        log_msg("job %s failed: %s", job.id, job.status_message);
        if (job.debug_info[0]) log_msg("job %s debug: %s", job.id, job.debug_info);
        write_daemon_state("failed", job.id, job.status_message);
    }

    if (job.doc_path[0] != '\0') remove(job.doc_path);
}

extern "C" void _start() {
    if (!ensure_spool_dirs()) {
        log_msg("failed to initialize spool directories");
        montauk::exit(1);
    }

    int running_pid = -1;
    if (daemon_is_running(&running_pid) && running_pid != montauk::getpid()) {
        log_msg("another print daemon is already running (pid %d)", running_pid);
        montauk::exit(0);
    }

    if (!write_pid_file()) {
        log_msg("failed to write daemon pid file");
        montauk::exit(1);
    }

    requeue_stale_active_jobs();
    char last_probed_uri[MAX_PATH_LEN] = {};
    uint64_t last_probe_ms = 0;
    bool last_probe_ok = false;
    log_msg("ready");
    write_daemon_state("ready", nullptr, nullptr);

    for (;;) {
        char active_path[MAX_PATH_LEN];
        ClaimResult claim = claim_next_job(active_path, sizeof(active_path));
        if (claim == ClaimResult::Claimed) {
            process_job(active_path);
            continue;
        }
        if (claim == ClaimResult::Failed) {
            montauk::sleep_ms(1000);
            continue;
        }
        write_daemon_state("idle", nullptr, nullptr);
        if (ENABLE_BACKGROUND_PROBE) {
            auto_probe_configured_printer(last_probed_uri, sizeof(last_probed_uri),
                                          &last_probe_ms, &last_probe_ok);
        }
        montauk::sleep_ms(1000);
    }
}
