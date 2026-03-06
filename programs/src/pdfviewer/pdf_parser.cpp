/*
 * pdf_parser.cpp
 * PDF file structure parsing and DEFLATE decompression
 * Copyright (c) 2026 Daniel Hammer
 */

#include "pdfviewer.h"

// ============================================================================
// DEFLATE Decompression (RFC 1951)
// ============================================================================

struct BitStream {
    const uint8_t* data;
    int len;
    int byte_pos;
    uint32_t buf;
    int buf_bits;
};

static void bs_init(BitStream* bs, const uint8_t* data, int len) {
    bs->data = data;
    bs->len = len;
    bs->byte_pos = 0;
    bs->buf = 0;
    bs->buf_bits = 0;
}

static void bs_ensure(BitStream* bs, int n) {
    while (bs->buf_bits < n && bs->byte_pos < bs->len) {
        bs->buf |= (uint32_t)bs->data[bs->byte_pos++] << bs->buf_bits;
        bs->buf_bits += 8;
    }
}

static uint32_t bs_read(BitStream* bs, int n) {
    if (n == 0) return 0;
    bs_ensure(bs, n);
    uint32_t val = bs->buf & ((1u << n) - 1);
    bs->buf >>= n;
    bs->buf_bits -= n;
    return val;
}

// Huffman tree: children[node][bit]
// Leaf: -(symbol + 1), Internal: child node index (>= 0)
#define HUFF_UNSET ((int16_t)0x7FFF)
#define HUFF_MAX_NODES 620

struct HuffTree {
    int16_t ch[HUFF_MAX_NODES][2];
    int cnt;
};

static void huff_init(HuffTree* t) {
    t->cnt = 1;
    t->ch[0][0] = t->ch[0][1] = HUFF_UNSET;
}

static void huff_build(HuffTree* t, const int* lens, int n) {
    huff_init(t);

    int max_len = 0;
    for (int i = 0; i < n; i++)
        if (lens[i] > max_len) max_len = lens[i];
    if (max_len == 0) return;

    int bl_count[16] = {};
    for (int i = 0; i < n; i++)
        if (lens[i]) bl_count[lens[i]]++;

    int next_code[16] = {};
    int code = 0;
    for (int b = 1; b <= max_len; b++) {
        code = (code + bl_count[b - 1]) << 1;
        next_code[b] = code;
    }

    for (int sym = 0; sym < n; sym++) {
        int len = lens[sym];
        if (!len) continue;
        int c = next_code[len]++;
        int node = 0;
        for (int bit = len - 1; bit >= 0; bit--) {
            int b = (c >> bit) & 1;
            if (bit == 0) {
                t->ch[node][b] = (int16_t)(-(sym + 1));
            } else {
                if (t->ch[node][b] == HUFF_UNSET || t->ch[node][b] < 0) {
                    int nn = t->cnt++;
                    if (nn >= HUFF_MAX_NODES) return;
                    t->ch[nn][0] = t->ch[nn][1] = HUFF_UNSET;
                    t->ch[node][b] = (int16_t)nn;
                    node = nn;
                } else {
                    node = t->ch[node][b];
                }
            }
        }
    }
}

static int huff_decode(HuffTree* t, BitStream* bs) {
    int node = 0;
    for (;;) {
        bs_ensure(bs, 1);
        if (bs->buf_bits == 0) return -1;
        int bit = bs->buf & 1;
        bs->buf >>= 1;
        bs->buf_bits--;
        int16_t val = t->ch[node][bit];
        if (val == HUFF_UNSET) return -1;
        if (val < 0) return -(val) - 1;
        node = val;
    }
}

static const int LEN_BASE[29] = {
    3,4,5,6,7,8,9,10,11,13,15,17,19,23,27,31,
    35,43,51,59,67,83,99,115,131,163,195,227,258
};
static const int LEN_EXTRA[29] = {
    0,0,0,0,0,0,0,0,1,1,1,1,2,2,2,2,
    3,3,3,3,4,4,4,4,5,5,5,5,0
};
static const int DIST_BASE[30] = {
    1,2,3,4,5,7,9,13,17,25,33,49,65,97,129,193,
    257,385,513,769,1025,1537,2049,3073,4097,6145,8193,12289,16385,24577
};
static const int DIST_EXTRA[30] = {
    0,0,0,0,1,1,2,2,3,3,4,4,5,5,6,6,
    7,7,8,8,9,9,10,10,11,11,12,12,13,13
};
static const int CL_ORDER[19] = {
    16,17,18,0,8,7,9,6,10,5,11,4,12,3,13,2,14,1,15
};

// Inflate raw DEFLATE data (no zlib header). Returns bytes written or -1.
static int inflate_raw(BitStream* bs, uint8_t* out, int out_cap) {
    HuffTree* lit = (HuffTree*)montauk::malloc(sizeof(HuffTree));
    HuffTree* dist = (HuffTree*)montauk::malloc(sizeof(HuffTree));
    HuffTree* cl = (HuffTree*)montauk::malloc(sizeof(HuffTree));
    if (!lit || !dist || !cl) goto fail;

    {
        int out_pos = 0;
        int bfinal = 0;

        while (!bfinal) {
            bfinal = (int)bs_read(bs, 1);
            int btype = (int)bs_read(bs, 2);

            if (btype == 0) {
                // Stored block: align to byte boundary
                bs->buf = 0;
                bs->buf_bits = 0;
                if (bs->byte_pos + 4 > bs->len) goto fail;
                int len = bs->data[bs->byte_pos] | (bs->data[bs->byte_pos + 1] << 8);
                bs->byte_pos += 4; // skip len and nlen
                for (int i = 0; i < len && out_pos < out_cap; i++) {
                    if (bs->byte_pos >= bs->len) goto fail;
                    out[out_pos++] = bs->data[bs->byte_pos++];
                }
            } else if (btype == 1 || btype == 2) {
                if (btype == 1) {
                    // Fixed Huffman codes
                    int ll[288];
                    for (int i = 0; i <= 143; i++) ll[i] = 8;
                    for (int i = 144; i <= 255; i++) ll[i] = 9;
                    for (int i = 256; i <= 279; i++) ll[i] = 7;
                    for (int i = 280; i <= 287; i++) ll[i] = 8;
                    huff_build(lit, ll, 288);
                    int dd[32];
                    for (int i = 0; i < 32; i++) dd[i] = 5;
                    huff_build(dist, dd, 32);
                } else {
                    // Dynamic Huffman codes
                    int hlit = (int)bs_read(bs, 5) + 257;
                    int hdist = (int)bs_read(bs, 5) + 1;
                    int hclen = (int)bs_read(bs, 4) + 4;

                    int cl_lens[19] = {};
                    for (int i = 0; i < hclen; i++)
                        cl_lens[CL_ORDER[i]] = (int)bs_read(bs, 3);
                    huff_build(cl, cl_lens, 19);

                    int all_lens[320] = {};
                    int total = hlit + hdist;
                    int idx = 0;
                    while (idx < total) {
                        int sym = huff_decode(cl, bs);
                        if (sym < 0) goto fail;
                        if (sym < 16) {
                            all_lens[idx++] = sym;
                        } else if (sym == 16) {
                            int rep = (int)bs_read(bs, 2) + 3;
                            int val = idx > 0 ? all_lens[idx - 1] : 0;
                            for (int j = 0; j < rep && idx < total; j++)
                                all_lens[idx++] = val;
                        } else if (sym == 17) {
                            int rep = (int)bs_read(bs, 3) + 3;
                            for (int j = 0; j < rep && idx < total; j++)
                                all_lens[idx++] = 0;
                        } else {
                            int rep = (int)bs_read(bs, 7) + 11;
                            for (int j = 0; j < rep && idx < total; j++)
                                all_lens[idx++] = 0;
                        }
                    }

                    huff_build(lit, all_lens, hlit);
                    huff_build(dist, all_lens + hlit, hdist);
                }

                // Decode symbols
                for (;;) {
                    int sym = huff_decode(lit, bs);
                    if (sym < 0) goto fail;
                    if (sym < 256) {
                        if (out_pos >= out_cap) goto fail;
                        out[out_pos++] = (uint8_t)sym;
                    } else if (sym == 256) {
                        break;
                    } else {
                        int li = sym - 257;
                        if (li >= 29) goto fail;
                        int length = LEN_BASE[li];
                        if (LEN_EXTRA[li])
                            length += (int)bs_read(bs, LEN_EXTRA[li]);

                        int di = huff_decode(dist, bs);
                        if (di < 0 || di >= 30) goto fail;
                        int distance = DIST_BASE[di];
                        if (DIST_EXTRA[di])
                            distance += (int)bs_read(bs, DIST_EXTRA[di]);

                        if (distance > out_pos) goto fail;
                        for (int j = 0; j < length; j++) {
                            if (out_pos >= out_cap) goto fail;
                            out[out_pos] = out[out_pos - distance];
                            out_pos++;
                        }
                    }
                }
            } else {
                goto fail;
            }
        }

        montauk::mfree(lit);
        montauk::mfree(dist);
        montauk::mfree(cl);
        return out_pos;
    }

fail:
    if (lit) montauk::mfree(lit);
    if (dist) montauk::mfree(dist);
    if (cl) montauk::mfree(cl);
    return -1;
}

// Inflate zlib-wrapped data (2-byte header + deflate + 4-byte checksum).
// Returns heap-allocated buffer and length, or nullptr on failure.
static uint8_t* inflate_zlib(const uint8_t* src, int src_len, int* out_len) {
    if (src_len < 6) return nullptr;

    // Skip 2-byte zlib header
    int deflate_start = 2;
    // Check for FDICT flag
    if (src[1] & 0x20) deflate_start += 4;

    BitStream bs;
    bs_init(&bs, src + deflate_start, src_len - deflate_start - 4);

    // Try progressively larger buffers
    for (int mult = 8; mult <= 64; mult *= 2) {
        int cap = src_len * mult;
        if (cap < 65536) cap = 65536;
        uint8_t* dst = (uint8_t*)montauk::malloc(cap);
        if (!dst) return nullptr;

        bs_init(&bs, src + deflate_start, src_len - deflate_start - 4);
        int result = inflate_raw(&bs, dst, cap);
        if (result >= 0) {
            *out_len = result;
            return dst;
        }
        montauk::mfree(dst);
    }
    return nullptr;
}

// ============================================================================
// PDF Parsing Utilities
// ============================================================================

int skip_ws(const uint8_t* d, int len, int p) {
    while (p < len) {
        if (d[p] == '%') {
            while (p < len && d[p] != '\n' && d[p] != '\r') p++;
            continue;
        }
        if (d[p] == ' ' || d[p] == '\t' || d[p] == '\n' || d[p] == '\r' || d[p] == '\0')
            p++;
        else
            break;
    }
    return p;
}

bool starts_with(const uint8_t* d, int len, int p, const char* s) {
    for (int i = 0; s[i]; i++) {
        if (p + i >= len || d[p + i] != (uint8_t)s[i]) return false;
    }
    return true;
}

int parse_int_at(const uint8_t* d, int len, int p, int* val) {
    p = skip_ws(d, len, p);
    bool neg = false;
    if (p < len && d[p] == '-') { neg = true; p++; }
    else if (p < len && d[p] == '+') p++;

    int v = 0;
    bool has = false;
    while (p < len && d[p] >= '0' && d[p] <= '9') {
        v = v * 10 + (d[p] - '0');
        has = true;
        p++;
    }
    if (!has) return -1;
    *val = neg ? -v : v;
    return p;
}

int parse_real_at(const uint8_t* d, int len, int p, float* val) {
    p = skip_ws(d, len, p);
    bool neg = false;
    if (p < len && d[p] == '-') { neg = true; p++; }
    else if (p < len && d[p] == '+') p++;

    float v = 0;
    bool has = false;
    while (p < len && d[p] >= '0' && d[p] <= '9') {
        v = v * 10 + (d[p] - '0');
        has = true;
        p++;
    }
    if (p < len && d[p] == '.') {
        p++;
        float frac = 0.1f;
        while (p < len && d[p] >= '0' && d[p] <= '9') {
            v += (d[p] - '0') * frac;
            frac *= 0.1f;
            has = true;
            p++;
        }
    }
    if (!has) return -1;
    *val = neg ? -v : v;
    return p;
}

// Parse an indirect reference "N G R" at position p.
// Returns position after 'R', or -1 if not a reference.
int parse_ref_at(const uint8_t* d, int len, int p, int* obj_num) {
    int saved = p;
    int num;
    p = parse_int_at(d, len, p, &num);
    if (p < 0) return -1;

    int gen;
    p = parse_int_at(d, len, p, &gen);
    if (p < 0) return -1;

    p = skip_ws(d, len, p);
    if (p >= len || d[p] != 'R') return -1;
    p++;

    *obj_num = num;
    return p;
    (void)saved;
}

// Skip over a PDF value (number, string, name, array, dict, ref, bool, null).
// Returns position after the value.
static int skip_value(const uint8_t* d, int len, int p) {
    p = skip_ws(d, len, p);
    if (p >= len) return p;

    // Dictionary
    if (p + 1 < len && d[p] == '<' && d[p + 1] == '<') {
        p += 2;
        int depth = 1;
        while (p + 1 < len && depth > 0) {
            if (d[p] == '<' && d[p + 1] == '<') { depth++; p += 2; }
            else if (d[p] == '>' && d[p + 1] == '>') { depth--; p += 2; }
            else p++;
        }
        return p;
    }
    // Hex string
    if (d[p] == '<') {
        p++;
        while (p < len && d[p] != '>') p++;
        if (p < len) p++;
        return p;
    }
    // Literal string
    if (d[p] == '(') {
        p++;
        int depth = 1;
        while (p < len && depth > 0) {
            if (d[p] == '\\') p += 2;
            else if (d[p] == '(') { depth++; p++; }
            else if (d[p] == ')') { depth--; p++; }
            else p++;
        }
        return p;
    }
    // Array
    if (d[p] == '[') {
        p++;
        while (p < len && d[p] != ']') {
            p = skip_value(d, len, p);
            p = skip_ws(d, len, p);
        }
        if (p < len) p++;
        return p;
    }
    // Name
    if (d[p] == '/') {
        p++;
        while (p < len && d[p] != ' ' && d[p] != '\t' && d[p] != '\n' &&
               d[p] != '\r' && d[p] != '/' && d[p] != '<' && d[p] != '>' &&
               d[p] != '[' && d[p] != ']' && d[p] != '(' && d[p] != ')')
            p++;
        return p;
    }
    // Try reference (N G R)
    {
        int obj_num;
        int rp = parse_ref_at(d, len, p, &obj_num);
        if (rp > 0) return rp;
    }
    // Number (int or real)
    {
        float v;
        int rp = parse_real_at(d, len, p, &v);
        if (rp > 0) return rp;
    }
    // Boolean or null
    if (starts_with(d, len, p, "true")) return p + 4;
    if (starts_with(d, len, p, "false")) return p + 5;
    if (starts_with(d, len, p, "null")) return p + 4;

    // Unknown - skip one byte
    return p + 1;
}

// Find a key in a PDF dictionary. pos should point at or after "<<".
// Returns position of the value (after the key name), or -1 if not found.
int dict_lookup(const uint8_t* d, int len, int pos, const char* key) {
    int p = pos;

    // Find the start of the dictionary
    while (p + 1 < len) {
        if (d[p] == '<' && d[p + 1] == '<') { p += 2; break; }
        p++;
    }

    int depth = 1;
    int key_len = 0;
    while (key[key_len]) key_len++;

    while (p < len && depth > 0) {
        p = skip_ws(d, len, p);
        if (p >= len) return -1;

        // End of dict
        if (p + 1 < len && d[p] == '>' && d[p + 1] == '>') {
            depth--;
            p += 2;
            if (depth == 0) return -1;
            continue;
        }

        // Nested dict start (skip it)
        if (p + 1 < len && d[p] == '<' && d[p + 1] == '<') {
            if (depth > 1) {
                p = skip_value(d, len, p);
                continue;
            }
        }

        // Key must be a name
        if (d[p] != '/') {
            p = skip_value(d, len, p);
            continue;
        }

        // Check if this key matches
        p++; // skip '/'
        bool match = true;
        int kp = 0;
        int name_start = p;
        while (p < len && d[p] != ' ' && d[p] != '\t' && d[p] != '\n' &&
               d[p] != '\r' && d[p] != '/' && d[p] != '<' && d[p] != '>' &&
               d[p] != '[' && d[p] != ']' && d[p] != '(' && d[p] != ')') {
            if (kp < key_len && d[p] == (uint8_t)key[kp]) kp++;
            else match = false;
            p++;
        }
        if (match && kp == key_len && (p - name_start) == key_len) {
            // Found the key, return position of the value
            return skip_ws(d, len, p);
        }

        // Skip the value
        p = skip_ws(d, len, p);
        p = skip_value(d, len, p);
    }
    return -1;
}

// ============================================================================
// Object Access
// ============================================================================

// Find object content by number. Sets start to first byte after "N G obj\n"
// and end to position of "endobj". Returns 0 on success, -1 on failure.
int find_obj_content(int obj_num, int* start, int* end) {
    if (obj_num < 0 || obj_num >= g_doc.xref_count) return -1;
    int off = g_doc.xref[obj_num];
    if (off <= 0) return -1;

    const uint8_t* d = g_doc.data;
    int len = g_doc.data_len;
    int p = off;

    // Skip "N G obj"
    while (p < len && d[p] != 'o') p++;
    if (!starts_with(d, len, p, "obj")) return -1;
    p += 3;
    p = skip_ws(d, len, p);

    *start = p;

    // Find endobj
    // Search from the object start for "endobj"
    int ep = p;
    while (ep + 5 < len) {
        if (starts_with(d, len, ep, "endobj")) {
            *end = ep;
            return 0;
        }
        ep++;
    }
    *end = len;
    return 0;
}

// ============================================================================
// Stream Data Extraction
// ============================================================================

// Get decompressed stream data for a stream object.
// Returns heap-allocated buffer (caller must free) and sets out_len.
uint8_t* get_stream_data(int obj_num, int* out_len) {
    int obj_start, obj_end;
    if (find_obj_content(obj_num, &obj_start, &obj_end) < 0)
        return nullptr;

    const uint8_t* d = g_doc.data;
    int len = g_doc.data_len;

    // Check for /Filter
    int filter_pos = dict_lookup(d, len, obj_start, "Filter");
    bool is_flate = false;
    if (filter_pos >= 0) {
        int fp = skip_ws(d, len, filter_pos);
        // Could be /FlateDecode or [/FlateDecode]
        if (fp < len && d[fp] == '/') {
            if (starts_with(d, len, fp + 1, "FlateDecode")) is_flate = true;
        } else if (fp < len && d[fp] == '[') {
            // Array of filters - check first one
            fp++;
            fp = skip_ws(d, len, fp);
            if (fp < len && d[fp] == '/' && starts_with(d, len, fp + 1, "FlateDecode"))
                is_flate = true;
        }
    }

    // Get /Length
    int stream_len = 0;
    int len_pos = dict_lookup(d, len, obj_start, "Length");
    if (len_pos >= 0) {
        int lp = skip_ws(d, len, len_pos);
        // Length could be a direct int or an indirect reference
        int ref_num;
        int rp = parse_ref_at(d, len, lp, &ref_num);
        if (rp > 0) {
            // Resolve indirect length
            int rs, re;
            if (find_obj_content(ref_num, &rs, &re) == 0) {
                parse_int_at(d, len, rs, &stream_len);
            }
        } else {
            parse_int_at(d, len, lp, &stream_len);
        }
    }

    // Find "stream" keyword
    int sp = obj_start;
    while (sp + 6 < obj_end) {
        if (starts_with(d, len, sp, "stream")) {
            sp += 6;
            // Skip \r\n or \n
            if (sp < len && d[sp] == '\r') sp++;
            if (sp < len && d[sp] == '\n') sp++;
            break;
        }
        sp++;
    }

    if (sp >= len || stream_len <= 0) return nullptr;

    // Clamp stream_len to available file data
    // (Don't clamp to obj_end — binary streams may contain false "endobj" matches)
    if (sp + stream_len > len) stream_len = len - sp;

    if (is_flate) {
        return inflate_zlib(d + sp, stream_len, out_len);
    } else {
        // Uncompressed - copy the data
        uint8_t* buf = (uint8_t*)montauk::malloc(stream_len);
        if (!buf) return nullptr;
        montauk::memcpy(buf, d + sp, stream_len);
        *out_len = stream_len;
        return buf;
    }
}

// ============================================================================
// Xref Parsing
// ============================================================================

static bool parse_xref_table(int pos) {
    const uint8_t* d = g_doc.data;
    int len = g_doc.data_len;
    int p = pos;

    // Skip "xref" and whitespace
    if (!starts_with(d, len, p, "xref")) return false;
    p += 4;
    p = skip_ws(d, len, p);

    // Parse subsections
    while (p < len && d[p] >= '0' && d[p] <= '9') {
        int start_num, count;
        p = parse_int_at(d, len, p, &start_num);
        if (p < 0) return false;
        p = parse_int_at(d, len, p, &count);
        if (p < 0) return false;
        p = skip_ws(d, len, p);

        // Ensure xref array is big enough
        int need = start_num + count;
        if (need > g_doc.xref_count) {
            int* new_xref = (int*)montauk::malloc(need * sizeof(int));
            if (!new_xref) return false;
            montauk::memset(new_xref, 0, need * sizeof(int));
            if (g_doc.xref) {
                montauk::memcpy(new_xref, g_doc.xref, g_doc.xref_count * sizeof(int));
                montauk::mfree(g_doc.xref);
            }
            g_doc.xref = new_xref;
            g_doc.xref_count = need;
        }

        for (int i = 0; i < count; i++) {
            // Each entry: 10 digits offset, space, 5 digits gen, space, f/n, end
            if (p + 18 > len) return false;

            int offset = 0;
            for (int j = 0; j < 10 && p + j < len; j++) {
                if (d[p + j] >= '0' && d[p + j] <= '9')
                    offset = offset * 10 + (d[p + j] - '0');
            }

            char type = (char)d[p + 17];
            if (type == 'n' && offset > 0) {
                g_doc.xref[start_num + i] = offset;
            }

            // Advance past this entry (20 bytes typical, but handle variable endings)
            p += 18;
            while (p < len && (d[p] == ' ' || d[p] == '\r' || d[p] == '\n')) p++;
        }
    }

    return true;
}

static bool parse_xref_stream(int pos) {
    const uint8_t* d = g_doc.data;
    int len = g_doc.data_len;

    // At pos we have "N 0 obj << ... >> stream ..."
    // Parse as a stream object, then decode binary xref data

    // First, let's find the object number
    int obj_num;
    int p = parse_int_at(d, len, pos, &obj_num);
    if (p < 0) return false;
    int gen;
    p = parse_int_at(d, len, p, &gen);
    if (p < 0) return false;
    p = skip_ws(d, len, p);
    if (!starts_with(d, len, p, "obj")) return false;
    p += 3;
    p = skip_ws(d, len, p);

    int dict_start = p;

    // Get /Size
    int size = 0;
    int size_pos = dict_lookup(d, len, dict_start, "Size");
    if (size_pos >= 0) parse_int_at(d, len, size_pos, &size);
    if (size <= 0) return false;

    // Get /W array [w1 w2 w3]
    int w[3] = {1, 2, 1};
    int w_pos = dict_lookup(d, len, dict_start, "W");
    if (w_pos >= 0) {
        int wp = skip_ws(d, len, w_pos);
        if (wp < len && d[wp] == '[') {
            wp++;
            for (int i = 0; i < 3; i++)
                wp = parse_int_at(d, len, wp, &w[i]);
        }
    }

    // Get /Index array (optional, defaults to [0 Size])
    int index_pairs[32]; // start, count pairs
    int index_count = 0;
    int idx_pos = dict_lookup(d, len, dict_start, "Index");
    if (idx_pos >= 0) {
        int ip = skip_ws(d, len, idx_pos);
        if (ip < len && d[ip] == '[') {
            ip++;
            while (index_count < 30) {
                ip = skip_ws(d, len, ip);
                if (ip >= len || d[ip] == ']') break;
                int v;
                ip = parse_int_at(d, len, ip, &v);
                if (ip < 0) break;
                index_pairs[index_count++] = v;
            }
        }
    }
    if (index_count == 0) {
        index_pairs[0] = 0;
        index_pairs[1] = size;
        index_count = 2;
    }

    // Allocate xref array
    g_doc.xref = (int*)montauk::malloc(size * sizeof(int));
    if (!g_doc.xref) return false;
    montauk::memset(g_doc.xref, 0, size * sizeof(int));
    g_doc.xref_count = size;

    // Temporarily add this xref stream object to the xref table
    // so get_stream_data can find it
    if (obj_num < size) g_doc.xref[obj_num] = pos;

    // Get /Filter and /Length, then extract stream data
    int filter_pos = dict_lookup(d, len, dict_start, "Filter");
    bool is_flate = false;
    if (filter_pos >= 0) {
        int fp = skip_ws(d, len, filter_pos);
        if (fp < len && d[fp] == '/' && starts_with(d, len, fp + 1, "FlateDecode"))
            is_flate = true;
        else if (fp < len && d[fp] == '[') {
            fp++;
            fp = skip_ws(d, len, fp);
            if (fp < len && d[fp] == '/' && starts_with(d, len, fp + 1, "FlateDecode"))
                is_flate = true;
        }
    }

    int stream_len = 0;
    int lp_pos = dict_lookup(d, len, dict_start, "Length");
    if (lp_pos >= 0) parse_int_at(d, len, lp_pos, &stream_len);

    // Find "stream" keyword
    int sp = dict_start;
    while (sp + 6 < len) {
        if (starts_with(d, len, sp, "stream")) {
            sp += 6;
            if (sp < len && d[sp] == '\r') sp++;
            if (sp < len && d[sp] == '\n') sp++;
            break;
        }
        sp++;
    }

    if (stream_len <= 0) return false;

    uint8_t* stream_data;
    int stream_data_len;

    if (is_flate) {
        stream_data = inflate_zlib(d + sp, stream_len, &stream_data_len);
    } else {
        stream_data = (uint8_t*)montauk::malloc(stream_len);
        if (stream_data) {
            montauk::memcpy(stream_data, d + sp, stream_len);
            stream_data_len = stream_len;
        }
    }
    if (!stream_data) return false;

    // Parse binary xref entries
    int entry_size = w[0] + w[1] + w[2];
    int data_off = 0;
    for (int pair = 0; pair + 1 < index_count; pair += 2) {
        int start_obj = index_pairs[pair];
        int count = index_pairs[pair + 1];

        for (int i = 0; i < count && data_off + entry_size <= stream_data_len; i++) {
            // Read type field
            int type = 0;
            for (int b = 0; b < w[0]; b++)
                type = (type << 8) | stream_data[data_off + b];

            // Read field 2
            int field2 = 0;
            for (int b = 0; b < w[1]; b++)
                field2 = (field2 << 8) | stream_data[data_off + w[0] + b];

            // Read field 3
            int field3 = 0;
            for (int b = 0; b < w[2]; b++)
                field3 = (field3 << 8) | stream_data[data_off + w[0] + w[1] + b];

            int obj_idx = start_obj + i;
            if (type == 1 && obj_idx < size && field2 > 0) {
                g_doc.xref[obj_idx] = field2;
            }
            // Type 0 = free, type 2 = compressed (not supported)

            data_off += entry_size;
        }
    }

    montauk::mfree(stream_data);
    return true;
}

// ============================================================================
// Page Tree Traversal
// ============================================================================

static void add_page(int obj_num) {
    if (g_doc.page_count >= g_doc.page_cap) {
        int new_cap = g_doc.page_cap ? g_doc.page_cap * 2 : 64;
        int* new_objs = (int*)montauk::malloc(new_cap * sizeof(int));
        PdfPage* new_pages = (PdfPage*)montauk::malloc(new_cap * sizeof(PdfPage));
        if (!new_objs || !new_pages) return;
        montauk::memset(new_pages, 0, new_cap * sizeof(PdfPage));
        if (g_doc.page_objs) {
            montauk::memcpy(new_objs, g_doc.page_objs, g_doc.page_count * sizeof(int));
            montauk::mfree(g_doc.page_objs);
        }
        if (g_doc.pages) {
            montauk::memcpy(new_pages, g_doc.pages, g_doc.page_count * sizeof(PdfPage));
            montauk::mfree(g_doc.pages);
        }
        g_doc.page_objs = new_objs;
        g_doc.pages = new_pages;
        g_doc.page_cap = new_cap;
    }
    g_doc.page_objs[g_doc.page_count] = obj_num;
    g_doc.pages[g_doc.page_count].items = nullptr;
    g_doc.pages[g_doc.page_count].item_count = 0;
    g_doc.pages[g_doc.page_count].item_cap = 0;
    g_doc.pages[g_doc.page_count].width = 612;
    g_doc.pages[g_doc.page_count].height = 792;
    g_doc.page_count++;
}

static void collect_pages(int obj_num, int depth) {
    if (depth > 20) return; // prevent infinite recursion
    int start, end;
    if (find_obj_content(obj_num, &start, &end) < 0) return;

    const uint8_t* d = g_doc.data;
    int len = g_doc.data_len;

    // Check /Type
    int type_pos = dict_lookup(d, len, start, "Type");
    if (type_pos < 0) return;

    if (starts_with(d, len, type_pos + 1, "Page") &&
        !starts_with(d, len, type_pos + 1, "Pages")) {
        // This is a leaf page
        add_page(obj_num);

        // Get MediaBox
        int mb_pos = dict_lookup(d, len, start, "MediaBox");
        if (mb_pos >= 0) {
            int mp = skip_ws(d, len, mb_pos);
            // Could be a reference
            int ref_num;
            int rp = parse_ref_at(d, len, mp, &ref_num);
            if (rp > 0) {
                int rs, re;
                if (find_obj_content(ref_num, &rs, &re) == 0)
                    mp = skip_ws(d, len, rs);
            }
            if (mp < len && d[mp] == '[') {
                mp++;
                float vals[4];
                for (int i = 0; i < 4; i++)
                    mp = parse_real_at(d, len, mp, &vals[i]);
                PdfPage* page = &g_doc.pages[g_doc.page_count - 1];
                page->width = vals[2] - vals[0];
                page->height = vals[3] - vals[1];
            }
        }
    } else if (starts_with(d, len, type_pos + 1, "Pages")) {
        // This is a pages node - recurse into /Kids
        int kids_pos = dict_lookup(d, len, start, "Kids");
        if (kids_pos < 0) return;

        int kp = skip_ws(d, len, kids_pos);
        if (kp >= len || d[kp] != '[') return;
        kp++;

        while (kp < len && d[kp] != ']') {
            kp = skip_ws(d, len, kp);
            if (kp >= len || d[kp] == ']') break;

            int child_num;
            int rp = parse_ref_at(d, len, kp, &child_num);
            if (rp > 0) {
                collect_pages(child_num, depth + 1);
                kp = rp;
            } else {
                kp++;
            }
        }
    }
}

// ============================================================================
// Font Map Building
// ============================================================================

static bool str_contains(const char* haystack, const char* needle) {
    int nlen = 0;
    while (needle[nlen]) nlen++;
    for (int i = 0; haystack[i]; i++) {
        bool match = true;
        for (int j = 0; j < nlen; j++) {
            char h = haystack[i + j];
            char n = needle[j];
            // Case-insensitive
            if (h >= 'a' && h <= 'z') h -= 32;
            if (n >= 'a' && n <= 'z') n -= 32;
            if (h != n) { match = false; break; }
        }
        if (match) return true;
    }
    return false;
}

// Parse a ToUnicode CMap stream and build a glyph->Unicode mapping table.
// Returns heap-allocated array of 256 uint16_t entries, or nullptr on failure.
static uint16_t* parse_tounicode(int cmap_obj_num) {
    int stream_len;
    uint8_t* cmap_data = get_stream_data(cmap_obj_num, &stream_len);
    if (!cmap_data) return nullptr;

    uint16_t* table = (uint16_t*)montauk::malloc(256 * sizeof(uint16_t));
    if (!table) { montauk::mfree(cmap_data); return nullptr; }
    // Initialize: identity mapping for printable ASCII, 0 for rest
    for (int i = 0; i < 256; i++) table[i] = 0;

    // Scan for "beginbfchar" sections
    for (int p = 0; p + 11 < stream_len; p++) {
        if (starts_with(cmap_data, stream_len, p, "beginbfchar")) {
            p += 11;
            // Parse entries until "endbfchar"
            while (p < stream_len) {
                // Skip whitespace
                while (p < stream_len && (cmap_data[p] == ' ' || cmap_data[p] == '\n' ||
                       cmap_data[p] == '\r' || cmap_data[p] == '\t'))
                    p++;
                if (starts_with(cmap_data, stream_len, p, "endbfchar")) break;

                // Parse <XX> <XXXX>
                if (p >= stream_len || cmap_data[p] != '<') break;
                p++; // skip <
                // Read source code (1-2 hex bytes)
                int src = 0;
                while (p < stream_len && cmap_data[p] != '>') {
                    int v = -1;
                    if (cmap_data[p] >= '0' && cmap_data[p] <= '9') v = cmap_data[p] - '0';
                    else if (cmap_data[p] >= 'a' && cmap_data[p] <= 'f') v = cmap_data[p] - 'a' + 10;
                    else if (cmap_data[p] >= 'A' && cmap_data[p] <= 'F') v = cmap_data[p] - 'A' + 10;
                    if (v >= 0) src = (src << 4) | v;
                    p++;
                }
                if (p < stream_len) p++; // skip >

                // Skip whitespace
                while (p < stream_len && (cmap_data[p] == ' ' || cmap_data[p] == '\n' ||
                       cmap_data[p] == '\r' || cmap_data[p] == '\t'))
                    p++;

                // Read dest Unicode (2-4 hex bytes)
                if (p >= stream_len || cmap_data[p] != '<') break;
                p++; // skip <
                int dst = 0;
                while (p < stream_len && cmap_data[p] != '>') {
                    int v = -1;
                    if (cmap_data[p] >= '0' && cmap_data[p] <= '9') v = cmap_data[p] - '0';
                    else if (cmap_data[p] >= 'a' && cmap_data[p] <= 'f') v = cmap_data[p] - 'a' + 10;
                    else if (cmap_data[p] >= 'A' && cmap_data[p] <= 'F') v = cmap_data[p] - 'A' + 10;
                    if (v >= 0) dst = (dst << 4) | v;
                    p++;
                }
                if (p < stream_len) p++; // skip >

                if (src >= 0 && src < 256 && dst > 0)
                    table[src] = (uint16_t)dst;
            }
        }

        // Also handle "beginbfrange" sections: <XX> <XX> <XXXX>
        if (starts_with(cmap_data, stream_len, p, "beginbfrange")) {
            p += 12;
            while (p < stream_len) {
                while (p < stream_len && (cmap_data[p] == ' ' || cmap_data[p] == '\n' ||
                       cmap_data[p] == '\r' || cmap_data[p] == '\t'))
                    p++;
                if (starts_with(cmap_data, stream_len, p, "endbfrange")) break;

                // Parse <start> <end> <dst_start>
                int vals[3] = {};
                for (int vi = 0; vi < 3; vi++) {
                    if (p >= stream_len || cmap_data[p] != '<') goto done_range;
                    p++;
                    while (p < stream_len && cmap_data[p] != '>') {
                        int v = -1;
                        if (cmap_data[p] >= '0' && cmap_data[p] <= '9') v = cmap_data[p] - '0';
                        else if (cmap_data[p] >= 'a' && cmap_data[p] <= 'f') v = cmap_data[p] - 'a' + 10;
                        else if (cmap_data[p] >= 'A' && cmap_data[p] <= 'F') v = cmap_data[p] - 'A' + 10;
                        if (v >= 0) vals[vi] = (vals[vi] << 4) | v;
                        p++;
                    }
                    if (p < stream_len) p++;
                    while (p < stream_len && (cmap_data[p] == ' ' || cmap_data[p] == '\n' ||
                           cmap_data[p] == '\r' || cmap_data[p] == '\t'))
                        p++;
                }
                for (int c = vals[0]; c <= vals[1] && c < 256; c++)
                    table[c] = (uint16_t)(vals[2] + (c - vals[0]));
            }
            done_range:;
        }
    }

    montauk::mfree(cmap_data);
    return table;
}

// Load an embedded font from a FontFile stream, with caching.
static TrueTypeFont* load_embedded_font(int stream_obj_num) {
    // Check cache
    for (int i = 0; i < g_doc.emb_font_count; i++) {
        if (g_doc.emb_fonts[i].stream_obj == stream_obj_num)
            return g_doc.emb_fonts[i].font;
    }

    // Extract stream data
    int data_len;
    uint8_t* data = get_stream_data(stream_obj_num, &data_len);
    if (!data || data_len < 12) {
        if (data) montauk::mfree(data);
        return nullptr;
    }

    // Init TrueTypeFont
    TrueTypeFont* f = (TrueTypeFont*)montauk::malloc(sizeof(TrueTypeFont));
    if (!f) { montauk::mfree(data); return nullptr; }
    montauk::memset(f, 0, sizeof(TrueTypeFont));
    f->data = data;
    f->cache_count = 0;
    f->valid = false;

    int offset = stbtt_GetFontOffsetForIndex(data, 0);
    if (offset < 0 || !stbtt_InitFont(&f->info, data, offset)) {
        montauk::mfree(data);
        montauk::mfree(f);
        return nullptr;
    }
    f->valid = true;
    f->em_scaling = true;  // PDF fonts use em-square sizing

    // Add to cache
    int idx = g_doc.emb_font_count;
    if (idx == 0) {
        g_doc.emb_fonts = (EmbeddedFontEntry*)montauk::malloc(16 * sizeof(EmbeddedFontEntry));
        if (!g_doc.emb_fonts) { return f; } // still return the font, just don't cache
    } else if ((idx & (idx - 1)) == 0 && idx >= 16) {
        // Power of 2 - grow
        auto* nf = (EmbeddedFontEntry*)montauk::malloc(idx * 2 * sizeof(EmbeddedFontEntry));
        if (nf) {
            montauk::memcpy(nf, g_doc.emb_fonts, idx * sizeof(EmbeddedFontEntry));
            montauk::mfree(g_doc.emb_fonts);
            g_doc.emb_fonts = nf;
        }
    }
    if (g_doc.emb_fonts) {
        g_doc.emb_fonts[idx].stream_obj = stream_obj_num;
        g_doc.emb_fonts[idx].font = f;
        g_doc.emb_fonts[idx].font_data = data;
        g_doc.emb_font_count++;
    }

    return f;
}

void build_font_map(int page_obj_num, FontMap* out) {
    out->count = 0;

    int start, end;
    if (find_obj_content(page_obj_num, &start, &end) < 0) return;

    const uint8_t* d = g_doc.data;
    int len = g_doc.data_len;

    // Find /Resources - could be inline or a reference
    int res_pos = dict_lookup(d, len, start, "Resources");
    if (res_pos < 0) {
        // Try parent /Pages for inherited resources
        int parent_pos = dict_lookup(d, len, start, "Parent");
        if (parent_pos >= 0) {
            int parent_num;
            if (parse_ref_at(d, len, parent_pos, &parent_num) > 0) {
                int ps, pe;
                if (find_obj_content(parent_num, &ps, &pe) == 0) {
                    res_pos = dict_lookup(d, len, ps, "Resources");
                }
            }
        }
    }
    if (res_pos < 0) return;

    // Resolve if it's a reference
    int rp = skip_ws(d, len, res_pos);
    int ref_num;
    int rrp = parse_ref_at(d, len, rp, &ref_num);
    int res_dict_start;
    if (rrp > 0) {
        int rs, re;
        if (find_obj_content(ref_num, &rs, &re) < 0) return;
        res_dict_start = rs;
    } else {
        res_dict_start = rp;
    }

    // Find /Font within resources
    int font_pos = dict_lookup(d, len, res_dict_start, "Font");
    if (font_pos < 0) return;

    // Resolve if reference
    int fp = skip_ws(d, len, font_pos);
    int font_ref;
    int frp = parse_ref_at(d, len, fp, &font_ref);
    int font_dict_start;
    if (frp > 0) {
        int fs, fe;
        if (find_obj_content(font_ref, &fs, &fe) < 0) return;
        font_dict_start = fs;
    } else {
        font_dict_start = fp;
    }

    // Parse font dictionary: /F1 5 0 R /F2 6 0 R etc.
    int fdp = skip_ws(d, len, font_dict_start);
    if (fdp >= len || d[fdp] != '<') return;
    fdp += 2; // skip <<

    while (fdp < len && out->count < 32) {
        fdp = skip_ws(d, len, fdp);
        if (fdp >= len) break;
        if (d[fdp] == '>' && fdp + 1 < len && d[fdp + 1] == '>') break;

        // Read font name (e.g., /F1)
        if (d[fdp] != '/') { fdp++; continue; }
        fdp++; // skip '/'

        FontInfo* fi = &out->fonts[out->count];
        int ni = 0;
        while (fdp < len && ni < 31 && d[fdp] != ' ' && d[fdp] != '\t' &&
               d[fdp] != '\n' && d[fdp] != '\r' && d[fdp] != '/' &&
               d[fdp] != '<' && d[fdp] != '>') {
            fi->name[ni++] = (char)d[fdp++];
        }
        fi->name[ni] = '\0';
        fi->flags = 0;
        fi->tounicode = nullptr;

        // Read font reference
        fdp = skip_ws(d, len, fdp);
        int font_obj;
        int ref_end = parse_ref_at(d, len, fdp, &font_obj);
        if (ref_end > 0) {
            fdp = ref_end;

            // Look up /BaseFont in the font object
            int fs2, fe2;
            if (find_obj_content(font_obj, &fs2, &fe2) == 0) {
                int bf_pos = dict_lookup(d, len, fs2, "BaseFont");
                if (bf_pos >= 0 && bf_pos < len && d[bf_pos] == '/') {
                    bf_pos++;
                    char base_font[64] = {};
                    int bi = 0;
                    while (bf_pos < len && bi < 63 && d[bf_pos] != ' ' &&
                           d[bf_pos] != '\t' && d[bf_pos] != '\n' &&
                           d[bf_pos] != '\r' && d[bf_pos] != '/' &&
                           d[bf_pos] != '<' && d[bf_pos] != '>') {
                        base_font[bi++] = (char)d[bf_pos++];
                    }
                    base_font[bi] = '\0';

                    if (str_contains(base_font, "Bold"))
                        fi->flags |= 1;
                    if (str_contains(base_font, "Italic") || str_contains(base_font, "Oblique"))
                        fi->flags |= 2;
                    if (str_contains(base_font, "Courier") || str_contains(base_font, "Mono"))
                        fi->flags |= 4;
                }

                // Parse /ToUnicode CMap if present
                fi->tounicode = nullptr;
                int tu_pos = dict_lookup(d, len, fs2, "ToUnicode");
                if (tu_pos >= 0) {
                    int tu_ref;
                    if (parse_ref_at(d, len, tu_pos, &tu_ref) > 0) {
                        fi->tounicode = parse_tounicode(tu_ref);
                    }
                }

                // Try to load embedded font from FontDescriptor
                fi->embedded_font = nullptr;
                int fdesc_pos = dict_lookup(d, len, fs2, "FontDescriptor");
                if (fdesc_pos >= 0) {
                    int fdesc_num;
                    if (parse_ref_at(d, len, fdesc_pos, &fdesc_num) > 0) {
                        int fds, fde;
                        if (find_obj_content(fdesc_num, &fds, &fde) == 0) {
                            // Try /FontFile2 (TrueType)
                            int ff_pos = dict_lookup(d, len, fds, "FontFile2");
                            if (ff_pos >= 0) {
                                int ff_num;
                                if (parse_ref_at(d, len, ff_pos, &ff_num) > 0)
                                    fi->embedded_font = load_embedded_font(ff_num);
                            }
                            // Try /FontFile3 (CFF/OpenType)
                            if (!fi->embedded_font) {
                                ff_pos = dict_lookup(d, len, fds, "FontFile3");
                                if (ff_pos >= 0) {
                                    int ff_num;
                                    if (parse_ref_at(d, len, ff_pos, &ff_num) > 0)
                                        fi->embedded_font = load_embedded_font(ff_num);
                                }
                            }
                        }
                    }
                }
            }
        } else {
            // Inline font dict or unknown - skip value
            fdp = skip_value(d, len, fdp);
        }

        out->count++;
    }
}

// ============================================================================
// Top-Level Load / Free
// ============================================================================

bool load_pdf(const char* path) {
    free_pdf();

    int fd = montauk::open(path);
    if (fd < 0) {
        str_cpy(g_status_msg, "Cannot open file", 128);
        return false;
    }

    uint64_t fsize = montauk::getsize(fd);
    if (fsize < 32 || fsize > 64 * 1024 * 1024) {
        montauk::close(fd);
        str_cpy(g_status_msg, "File too small or too large", 128);
        return false;
    }

    g_doc.data = (uint8_t*)montauk::malloc((int)fsize);
    if (!g_doc.data) {
        montauk::close(fd);
        str_cpy(g_status_msg, "Out of memory", 128);
        return false;
    }
    montauk::read(fd, g_doc.data, 0, fsize);
    montauk::close(fd);
    g_doc.data_len = (int)fsize;

    // Check PDF header
    if (!starts_with(g_doc.data, g_doc.data_len, 0, "%PDF")) {
        str_cpy(g_status_msg, "Not a PDF file", 128);
        free_pdf();
        return false;
    }

    // Find startxref from end of file
    int p = g_doc.data_len - 1;
    while (p > 0 && (g_doc.data[p] == '\n' || g_doc.data[p] == '\r' ||
           g_doc.data[p] == ' ' || g_doc.data[p] == '%'))
        p--;
    // Skip past %%EOF
    while (p > 0 && g_doc.data[p] != '\n' && g_doc.data[p] != '\r') p--;
    // Now find "startxref"
    while (p > 0) {
        if (starts_with(g_doc.data, g_doc.data_len, p, "startxref")) break;
        p--;
    }
    if (p <= 0) {
        str_cpy(g_status_msg, "Cannot find startxref", 128);
        free_pdf();
        return false;
    }

    int xref_off = 0;
    parse_int_at(g_doc.data, g_doc.data_len, p + 9, &xref_off);
    if (xref_off <= 0 || xref_off >= g_doc.data_len) {
        str_cpy(g_status_msg, "Invalid xref offset", 128);
        free_pdf();
        return false;
    }

    // Parse xref (traditional table or cross-reference stream)
    bool xref_ok = false;
    if (starts_with(g_doc.data, g_doc.data_len, xref_off, "xref")) {
        xref_ok = parse_xref_table(xref_off);
    } else {
        xref_ok = parse_xref_stream(xref_off);
    }

    if (!xref_ok || g_doc.xref_count == 0) {
        str_cpy(g_status_msg, "Failed to parse xref", 128);
        free_pdf();
        return false;
    }

    // Find /Root in trailer
    int root_num = -1;
    if (starts_with(g_doc.data, g_doc.data_len, xref_off, "xref")) {
        // Traditional: find "trailer" after xref
        int tp = xref_off;
        while (tp + 7 < g_doc.data_len) {
            if (starts_with(g_doc.data, g_doc.data_len, tp, "trailer")) {
                tp += 7;
                int root_pos = dict_lookup(g_doc.data, g_doc.data_len, tp, "Root");
                if (root_pos >= 0)
                    parse_ref_at(g_doc.data, g_doc.data_len, root_pos, &root_num);
                break;
            }
            tp++;
        }
    } else {
        // Xref stream: /Root is in the stream object's dict
        // The xref stream obj was already added to xref table
        // Find its dict and look for /Root
        int obj_num;
        int op = parse_int_at(g_doc.data, g_doc.data_len, xref_off, &obj_num);
        if (op > 0) {
            int os, oe;
            if (find_obj_content(obj_num, &os, &oe) == 0) {
                int root_pos = dict_lookup(g_doc.data, g_doc.data_len, os, "Root");
                if (root_pos >= 0)
                    parse_ref_at(g_doc.data, g_doc.data_len, root_pos, &root_num);
            }
        }
    }

    if (root_num < 0) {
        str_cpy(g_status_msg, "Cannot find document root", 128);
        free_pdf();
        return false;
    }

    // Find /Pages from catalog
    int cat_start, cat_end;
    if (find_obj_content(root_num, &cat_start, &cat_end) < 0) {
        str_cpy(g_status_msg, "Cannot read catalog", 128);
        free_pdf();
        return false;
    }

    int pages_pos = dict_lookup(g_doc.data, g_doc.data_len, cat_start, "Pages");
    if (pages_pos < 0) {
        str_cpy(g_status_msg, "Cannot find pages", 128);
        free_pdf();
        return false;
    }

    int pages_num;
    if (parse_ref_at(g_doc.data, g_doc.data_len, pages_pos, &pages_num) < 0) {
        str_cpy(g_status_msg, "Invalid pages reference", 128);
        free_pdf();
        return false;
    }

    // Collect all pages
    collect_pages(pages_num, 0);

    if (g_doc.page_count == 0) {
        str_cpy(g_status_msg, "No pages found", 128);
        free_pdf();
        return false;
    }

    // Parse content for each page
    for (int i = 0; i < g_doc.page_count; i++) {
        parse_page(i, g_doc.page_objs[i]);
    }

    g_doc.valid = true;
    snprintf(g_status_msg, 128, "%d page%s loaded", g_doc.page_count,
                 g_doc.page_count == 1 ? "" : "s");
    return true;
}

void free_pdf() {
    if (g_doc.data) { montauk::mfree(g_doc.data); g_doc.data = nullptr; }
    if (g_doc.xref) { montauk::mfree(g_doc.xref); g_doc.xref = nullptr; }
    if (g_doc.pages) {
        for (int i = 0; i < g_doc.page_count; i++) {
            if (g_doc.pages[i].items) montauk::mfree(g_doc.pages[i].items);
        }
        montauk::mfree(g_doc.pages);
        g_doc.pages = nullptr;
    }
    if (g_doc.page_objs) { montauk::mfree(g_doc.page_objs); g_doc.page_objs = nullptr; }
    if (g_doc.emb_fonts) {
        for (int i = 0; i < g_doc.emb_font_count; i++) {
            if (g_doc.emb_fonts[i].font_data) montauk::mfree(g_doc.emb_fonts[i].font_data);
            if (g_doc.emb_fonts[i].font) montauk::mfree(g_doc.emb_fonts[i].font);
        }
        montauk::mfree(g_doc.emb_fonts);
        g_doc.emb_fonts = nullptr;
    }
    g_doc.emb_font_count = 0;
    g_doc.data_len = 0;
    g_doc.xref_count = 0;
    g_doc.page_count = 0;
    g_doc.page_cap = 0;
    g_doc.valid = false;
}
