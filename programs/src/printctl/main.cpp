/*
 * main.cpp
 * MontaukOS print control utility
 * Copyright (c) 2026 Daniel Hammer
 */

#include <print/print.hpp>

extern "C" {
#include <dirent.h>
#include <stdio.h>
#include <string.h>
}

using namespace print;

static void usage() {
    printf("Usage:\n");
    printf("  printctl set-printer <ipp://host[:port]/path>\n");
    printf("  printctl show-printer\n");
    printf("  printctl print <file> [--printer <uri>] [--name <job>] [--wait]\n");
    printf("  printctl test-page [--printer <uri>] [--wait] [--no-wait]\n");
    printf("  printctl status [--verbose]\n");
    printf("  printctl inspect <job-id>\n");
    printf("  printctl probe [uri]\n");
}

static void print_host_details(const IppUri& uri) {
    char ip[20];
    if (uri.ip) format_ipv4(ip, sizeof(ip), uri.ip);
    else safe_copy(ip, sizeof(ip), "unresolved");

    printf("Host: %s\n", uri.host);
    printf("Resolved IP: %s\n", ip);
    printf("Transport: %s\n", uri.use_tls ? "TLS" : "Plain TCP");
    printf("Port: %u\n", (unsigned)uri.port);
    printf("Path: %s\n", uri.path);
}

static void print_probe_state_summary(const ProbeState& probe, bool verbose) {
    if (!probe.printer_uri[0] && !probe.message[0]) return;

    printf("Last probe: %s", probe.ok ? "ok" : "failed");
    if (probe.probed_at[0]) printf("  at %s", probe.probed_at);
    printf("\n");

    if (!verbose) return;

    if (probe.ok && probe.caps.printer_name[0]) printf("Printer name: %s\n", probe.caps.printer_name);
    else if (probe.message[0]) printf("Probe status: %s\n", probe.message);

    if (probe.ok) {
        printf("Supports PDF: %s\n", probe.caps.supports_pdf ? "yes" : "no");
        printf("Supports JPEG: %s\n", probe.caps.supports_jpeg ? "yes" : "no");
        printf("Supports text: %s\n", probe.caps.supports_text ? "yes" : "no");
        if (probe.caps.preferred_format[0]) printf("Preferred format: %s\n", probe.caps.preferred_format);
        if (probe.caps.default_format[0]) printf("Default format: %s\n", probe.caps.default_format);
        if (probe.caps.supported_formats[0]) printf("Supported formats: %s\n", probe.caps.supported_formats);
        if (probe.caps.status_message[0]) printf("Printer status: %s\n", probe.caps.status_message);
    }

    if (probe.detail[0]) printf("Probe detail: %s\n", probe.detail);
}

static void print_job_detail(const JobMeta& job, const char* state, bool verbose) {
    printf("  %s  %s\n", job.id, job.job_name);
    printf("    state: %s", state ? state : job.state);
    if (job.updated_at[0]) printf("  updated: %s", job.updated_at);
    printf("\n");

    if (job.printer_uri[0]) printf("    printer: %s\n", job.printer_uri);
    if (job.doc_format[0]) printf("    format: %s\n", job.doc_format);
    if (job.source_name[0]) printf("    source: %s\n", job.source_name);
    if (job.remote_job_id > 0) printf("    printer job id: %d\n", job.remote_job_id);
    if (job.status_message[0]) printf("    status: %s\n", job.status_message);
    if ((verbose || strcmp(state, "failed") == 0 || strcmp(state, "printing") == 0) &&
        job.debug_info[0]) {
        printf("    debug: %s\n", job.debug_info);
    }
}

static void print_job_line(const char* dir, const char* label, bool verbose) {
    DIR* d = opendir(dir);
    if (!d) {
        printf("%s: unavailable\n", label);
        return;
    }

    printf("%s:\n", label);
    int count = 0;
    struct dirent* ent = nullptr;
    while ((ent = readdir(d)) != nullptr) {
        if (ent->d_name[0] == '.') continue;
        char path[MAX_PATH_LEN];
        path_join(path, sizeof(path), dir, ent->d_name);
        JobMeta job = {};
        if (!load_job_from_path(path, &job)) continue;
        const char* state = strcmp(label, "Queued") == 0 ? "queued"
                          : strcmp(label, "Active") == 0 ? "printing"
                          : strcmp(label, "Done") == 0 ? "completed"
                          : strcmp(label, "Failed") == 0 ? "failed"
                          : job.state;
        print_job_detail(job, state, verbose);
        count++;
    }
    closedir(d);

    if (count == 0) printf("  empty\n");
}

static int cmd_status(const ArgList& args) {
    ensure_spool_dirs();
    bool verbose = false;
    for (int i = 1; i < args.count; i++) {
        if (strcmp(args.argv[i], "--verbose") == 0) verbose = true;
        else {
            usage();
            return 1;
        }
    }

    char uri[MAX_PATH_LEN] = {};
    if (read_default_printer_uri(uri, sizeof(uri))) {
        printf("Default printer: %s\n", uri);
        if (verbose) {
            char err[128] = {};
            IppUri parsed = {};
            if (normalize_ipp_uri(uri, &parsed, err, sizeof(err))) {
                resolve_host(parsed.host, &parsed.ip);
                print_host_details(parsed);
            } else {
                printf("Printer URI error: %s\n", err);
            }
        }
    } else
        printf("Default printer: not configured\n");

    if (uri[0]) {
        ProbeState probe = {};
        if (load_printer_probe_state(&probe) && strcmp(probe.printer_uri, uri) == 0)
            print_probe_state_summary(probe, verbose);
    }

    int pid = -1;
    if (daemon_is_running(&pid))
        printf("Daemon: running (pid %d)\n", pid);
    else
        printf("Daemon: stopped\n");

    print_job_line(SPOOL_QUEUE_DIR, "Queued", verbose);
    print_job_line(SPOOL_ACTIVE_DIR, "Active", verbose);
    print_job_line(SPOOL_DONE_DIR, "Done", verbose);
    print_job_line(SPOOL_FAILED_DIR, "Failed", verbose);
    return 0;
}

static int cmd_set_printer(const char* uri) {
    char err[128];
    IppUri parsed = {};
    if (!normalize_ipp_uri(uri, &parsed, err, sizeof(err))) {
        printf("printctl: %s\n", err);
        return 1;
    }
    if (!write_default_printer_uri(parsed.normalized)) {
        printf("printctl: failed to write default printer\n");
        return 1;
    }
    printf("%s\n", parsed.normalized);
    return 0;
}

static int print_submission_result(const char* job_id, bool wait) {
    printf("queued job %s\n", job_id);
    if (!wait) return 0;

    JobMeta job = {};
    char err[128] = {};
    bool ok = wait_for_job(job_id, &job, DEFAULT_WAIT_TIMEOUT_MS, err, sizeof(err));
    if (job.id[0] != '\0') {
        if (ok) {
            printf("completed: %s\n", job.status_message[0] ? job.status_message : "Printed");
            if (job.remote_job_id > 0) printf("printer job id: %d\n", job.remote_job_id);
            if (job.debug_info[0]) printf("detail: %s\n", job.debug_info);
            return 0;
        }
        printf("failed: %s\n", job.status_message[0] ? job.status_message : "Print failed");
        if (job.debug_info[0]) printf("detail: %s\n", job.debug_info);
        return 1;
    }

    printf("printctl: %s\n", err[0] ? err : "timed out waiting for job");
    char path[MAX_PATH_LEN];
    char state[16];
    if (find_job(job_id, path, sizeof(path), state, sizeof(state))) {
        JobMeta current = {};
        if (load_job_from_path(path, &current)) {
            printf("current state: %s\n", state);
            if (current.status_message[0]) printf("status: %s\n", current.status_message);
            if (current.debug_info[0]) printf("detail: %s\n", current.debug_info);
        }
    }
    return 1;
}

static int cmd_print(const ArgList& args) {
    const char* file = nullptr;
    const char* printer = nullptr;
    const char* name = nullptr;
    bool wait = false;

    for (int i = 1; i < args.count; i++) {
        if (strcmp(args.argv[i], "--printer") == 0 && i + 1 < args.count) {
            printer = args.argv[++i];
        } else if (strcmp(args.argv[i], "--name") == 0 && i + 1 < args.count) {
            name = args.argv[++i];
        } else if (strcmp(args.argv[i], "--wait") == 0) {
            wait = true;
        } else if (file == nullptr) {
            file = args.argv[i];
        } else {
            usage();
            return 1;
        }
    }

    if (file == nullptr) {
        usage();
        return 1;
    }

    char job_id[48];
    char err[128] = {};
    if (!submit_document_job(file, printer, name, job_id, sizeof(job_id), err, sizeof(err))) {
        printf("printctl: %s\n", err);
        return 1;
    }

    return print_submission_result(job_id, wait);
}

static int cmd_test_page(const ArgList& args) {
    const char* printer = nullptr;
    bool wait = true;

    for (int i = 1; i < args.count; i++) {
        if (strcmp(args.argv[i], "--printer") == 0 && i + 1 < args.count) {
            printer = args.argv[++i];
        } else if (strcmp(args.argv[i], "--wait") == 0) {
            wait = true;
        } else if (strcmp(args.argv[i], "--no-wait") == 0) {
            wait = false;
        } else {
            usage();
            return 1;
        }
    }

    char job_id[48];
    char err[128] = {};
    if (!submit_test_page_job(printer, job_id, sizeof(job_id), err, sizeof(err))) {
        printf("printctl: %s\n", err);
        return 1;
    }
    return print_submission_result(job_id, wait);
}

static int cmd_inspect(const char* job_id) {
    char path[MAX_PATH_LEN];
    char state[16];
    if (!find_job(job_id, path, sizeof(path), state, sizeof(state))) {
        printf("printctl: job %s not found\n", job_id);
        return 1;
    }

    JobMeta job = {};
    if (!load_job_from_path(path, &job)) {
        printf("printctl: failed to load job %s\n", job_id);
        return 1;
    }

    printf("Job ID: %s\n", job.id);
    printf("State: %s\n", state);
    printf("Name: %s\n", job.job_name);
    if (job.user_name[0]) printf("User: %s\n", job.user_name);
    if (job.printer_uri[0]) printf("Printer: %s\n", job.printer_uri);
    if (job.doc_format[0]) printf("Format: %s\n", job.doc_format);
    if (job.source_name[0]) printf("Source: %s\n", job.source_name);
    if (job.created_at[0]) printf("Created: %s\n", job.created_at);
    if (job.updated_at[0]) printf("Updated: %s\n", job.updated_at);
    if (job.remote_job_id > 0) printf("Printer job id: %d\n", job.remote_job_id);
    if (job.status_message[0]) printf("Status: %s\n", job.status_message);
    if (job.debug_info[0]) printf("Debug: %s\n", job.debug_info);
    return 0;
}

static int cmd_probe(const char* printer_uri) {
    char uri_buf[MAX_PATH_LEN];
    if (printer_uri && *printer_uri) {
        safe_copy(uri_buf, sizeof(uri_buf), printer_uri);
    } else if (!read_default_printer_uri(uri_buf, sizeof(uri_buf))) {
        printf("printctl: no default printer is configured\n");
        return 1;
    }

    char err[128] = {};
    IppUri normalized = {};
    if (!normalize_ipp_uri(uri_buf, &normalized, err, sizeof(err))) {
        printf("printctl: %s\n", err);
        return 1;
    }

    uint32_t ip = 0;
    if (resolve_host(normalized.host, &ip)) normalized.ip = ip;

    printf("Printer URI: %s\n", normalized.normalized);
    print_host_details(normalized);
    if (!normalized.ip) {
        if (host_looks_like_mdns(normalized.host))
            printf("Resolution: unresolved (.local/mDNS not supported yet)\n");
        else
            printf("Resolution: unresolved\n");
    } else {
        char ip_text[20];
        format_ipv4(ip_text, sizeof(ip_text), normalized.ip);
        printf("Resolution: ok (%s)\n", ip_text);
    }

    ProbeState probe = {};
    if (!probe_printer_uri(normalized.normalized, &probe, err, sizeof(err))) {
        printf("Probe failed: %s\n", probe.message[0] ? probe.message : (err[0] ? err : "IPP probe failed"));
        if (probe.detail[0]) printf("Detail: %s\n", probe.detail);
        return 1;
    }

    printf("Probe: ok\n");
    if (probe.caps.printer_name[0]) printf("Printer name: %s\n", probe.caps.printer_name);
    printf("Supports PDF: %s\n", probe.caps.supports_pdf ? "yes" : "no");
    printf("Supports JPEG: %s\n", probe.caps.supports_jpeg ? "yes" : "no");
    printf("Supports text: %s\n", probe.caps.supports_text ? "yes" : "no");
    if (probe.caps.preferred_format[0]) printf("Preferred format: %s\n", probe.caps.preferred_format);
    if (probe.caps.default_format[0]) printf("Default format: %s\n", probe.caps.default_format);
    if (probe.caps.supported_formats[0]) printf("Supported formats: %s\n", probe.caps.supported_formats);
    if (probe.caps.status_message[0]) printf("Printer status: %s\n", probe.caps.status_message);
    if (probe.detail[0]) printf("Detail: %s\n", probe.detail);
    return 0;
}

extern "C" void _start() {
    ArgList args = {};
    parse_args(&args);
    if (args.count <= 0) {
        usage();
        montauk::exit(1);
    }

    int rc = 1;
    if (strcmp(args.argv[0], "set-printer") == 0) {
        if (args.count < 2) usage();
        else rc = cmd_set_printer(args.argv[1]);
    } else if (strcmp(args.argv[0], "show-printer") == 0) {
        char uri[MAX_PATH_LEN];
        if (read_default_printer_uri(uri, sizeof(uri))) {
            printf("%s\n", uri);
            rc = 0;
        } else {
            printf("printctl: no default printer is configured\n");
            rc = 1;
        }
    } else if (strcmp(args.argv[0], "print") == 0) {
        rc = cmd_print(args);
    } else if (strcmp(args.argv[0], "test-page") == 0) {
        rc = cmd_test_page(args);
    } else if (strcmp(args.argv[0], "status") == 0) {
        rc = cmd_status(args);
    } else if (strcmp(args.argv[0], "inspect") == 0) {
        if (args.count < 2) usage();
        else rc = cmd_inspect(args.argv[1]);
    } else if (strcmp(args.argv[0], "probe") == 0) {
        rc = cmd_probe(args.count >= 2 ? args.argv[1] : nullptr);
    } else {
        usage();
        rc = 1;
    }

    montauk::exit(rc);
}
