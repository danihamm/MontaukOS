/*
 * printers.cpp
 * Virtual printer-device registration for the MontaukOS Device Explorer
 * Copyright (c) 2026 Daniel Hammer
 */

#include "devexplorer.h"
#include <print/print.hpp>

using namespace print;

static void format_printer_detail(const ProbeState& probe,
                                  const IppUri* normalized,
                                  char* out, int out_len) {
    if (out == nullptr || out_len <= 0) return;
    out[0] = '\0';

    const char* host = nullptr;
    if (probe.caps.host[0]) host = probe.caps.host;
    else if (normalized && normalized->host[0]) host = normalized->host;

    if (probe.ok) {
        if (host && *host)
            snprintf(out, (size_t)out_len, "Ready on %s", host);
        else
            snprintf(out, (size_t)out_len, "Ready");
        return;
    }

    if (probe.message[0]) {
        if (strstr(probe.message, "resolve"))
            snprintf(out, (size_t)out_len, "Host resolution failed");
        else if (strstr(probe.message, "mDNS"))
            snprintf(out, (size_t)out_len, "mDNS host not supported");
        else if (strstr(probe.message, "Unsupported format"))
            snprintf(out, (size_t)out_len, "Unsupported format");
        else
            safe_copy(out, out_len, probe.message);
        return;
    }

    if (host && *host)
        snprintf(out, (size_t)out_len, "Awaiting probe on %s", host);
    else
        snprintf(out, (size_t)out_len, "Awaiting spooler probe");
}

int append_printer_devices(Montauk::DevInfo* out, int max_count) {
    if (out == nullptr || max_count <= 0) return 0;

    char current_uri[MAX_PATH_LEN] = {};
    bool has_current_uri = read_default_printer_uri(current_uri, sizeof(current_uri));

    ProbeState probe = {};
    bool has_probe = load_printer_probe_state(&probe);
    if (has_probe && has_current_uri && !montauk::streq(probe.printer_uri, current_uri))
        has_probe = false;

    const char* effective_uri = nullptr;
    if (has_current_uri) effective_uri = current_uri;
    else if (has_probe && probe.printer_uri[0]) effective_uri = probe.printer_uri;
    else return 0;

    IppUri normalized = {};
    char uri_err[128] = {};
    bool normalized_ok = normalize_ipp_uri(effective_uri, &normalized, uri_err, sizeof(uri_err));

    Montauk::DevInfo dev = {};
    dev.category = CAT_PRINTER;

    if (has_probe && probe.caps.printer_name[0]) {
        safe_copy(dev.name, sizeof(dev.name), probe.caps.printer_name);
    } else if (normalized_ok && normalized.host[0]) {
        safe_copy(dev.name, sizeof(dev.name), normalized.host);
    } else {
        safe_copy(dev.name, sizeof(dev.name), "Configured Printer");
    }

    if (has_probe) {
        format_printer_detail(probe, normalized_ok ? &normalized : nullptr,
                              dev.detail, sizeof(dev.detail));
    } else if (normalized_ok && normalized.host[0]) {
        snprintf(dev.detail, sizeof(dev.detail), "Awaiting probe on %s", normalized.host);
    } else {
        safe_copy(dev.detail, sizeof(dev.detail), "Awaiting spooler probe");
    }

    out[0] = dev;
    return 1;
}
