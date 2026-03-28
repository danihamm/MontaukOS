#include "avi.hpp"

#include <montauk/heap.h>
#include <montauk/string.h>
#include <montauk/syscall.h>

extern "C" {
#include <stdio.h>
#include <string.h>
#include <gui/stb_image.h>
}

namespace avi {

static constexpr uint32_t fourcc(char a, char b, char c, char d) {
    return (uint32_t)(uint8_t)a |
           ((uint32_t)(uint8_t)b << 8) |
           ((uint32_t)(uint8_t)c << 16) |
           ((uint32_t)(uint8_t)d << 24);
}

static constexpr uint32_t FCC_RIFF = fourcc('R', 'I', 'F', 'F');
static constexpr uint32_t FCC_LIST = fourcc('L', 'I', 'S', 'T');
static constexpr uint32_t FCC_AVI  = fourcc('A', 'V', 'I', ' ');
static constexpr uint32_t FCC_hdrl = fourcc('h', 'd', 'r', 'l');
static constexpr uint32_t FCC_strl = fourcc('s', 't', 'r', 'l');
static constexpr uint32_t FCC_movi = fourcc('m', 'o', 'v', 'i');
static constexpr uint32_t FCC_rec  = fourcc('r', 'e', 'c', ' ');
static constexpr uint32_t FCC_avih = fourcc('a', 'v', 'i', 'h');
static constexpr uint32_t FCC_strh = fourcc('s', 't', 'r', 'h');
static constexpr uint32_t FCC_strf = fourcc('s', 't', 'r', 'f');
static constexpr uint32_t FCC_vids = fourcc('v', 'i', 'd', 's');
static constexpr uint32_t FCC_auds = fourcc('a', 'u', 'd', 's');
static constexpr uint32_t FCC_MJPG = fourcc('M', 'J', 'P', 'G');
static constexpr uint32_t FCC_DIB  = fourcc('D', 'I', 'B', ' ');

static uint16_t rd_u16(const uint8_t* p) {
    return (uint16_t)p[0] | ((uint16_t)p[1] << 8);
}

static uint32_t rd_u32(const uint8_t* p) {
    return (uint32_t)p[0] |
           ((uint32_t)p[1] << 8) |
           ((uint32_t)p[2] << 16) |
           ((uint32_t)p[3] << 24);
}

static int32_t rd_s32(const uint8_t* p) {
    return (int32_t)rd_u32(p);
}

static uint32_t min_u32(uint32_t a, uint32_t b) {
    return a < b ? a : b;
}

static uint64_t align2(uint64_t n) {
    return (n + 1u) & ~1u;
}

static void set_error(File& file, const char* msg) {
    montauk::strncpy(file.error, msg, (int)sizeof(file.error));
}

static bool read_exact(const File& file, uint64_t offset, uint8_t* out, uint32_t size) {
    if (size == 0) return true;
    if (file.fd < 0 || !out) return false;
    if (offset > file.size || (uint64_t)size > file.size - offset) return false;

    uint32_t done = 0;
    while (done < size) {
        uint32_t chunk = size - done;
        if (chunk > 32768u) chunk = 32768u;
        int rd = montauk::read(file.fd, out + done, offset + (uint64_t)done, chunk);
        if (rd <= 0) return false;
        done += (uint32_t)rd;
    }

    return true;
}

bool read_bytes(const File& file, uint64_t offset, uint8_t* out, uint32_t size) {
    return read_exact(file, offset, out, size);
}

static bool push_chunk(ChunkRef*& arr, int& count, int& cap,
                       uint32_t offset, uint32_t size) {
    if (count >= cap) {
        int new_cap = cap ? cap * 2 : 256;
        auto* next = (ChunkRef*)montauk::realloc(arr, (uint64_t)new_cap * sizeof(ChunkRef));
        if (!next) return false;
        arr = next;
        cap = new_cap;
    }
    arr[count].offset = offset;
    arr[count].size = size;
    count++;
    return true;
}

struct StreamDesc {
    uint32_t type = 0;
    uint32_t handler = 0;
    uint32_t scale = 0;
    uint32_t rate = 0;
    uint32_t length = 0;

    int width = 0;
    int height = 0;
    bool top_down = false;
    int bits_per_pixel = 0;
    VideoCodec codec = VideoCodec::None;

    uint16_t audio_format = 0;
    uint16_t audio_channels = 0;
    uint16_t audio_bits = 0;
    uint16_t audio_block_align = 0;
    uint32_t audio_rate = 0;
    uint32_t audio_avg_bytes_per_sec = 0;
};

static bool chunk_stream_id(uint32_t id, int* stream_index, char* kind0, char* kind1) {
    char a = (char)(id & 0xFF);
    char b = (char)((id >> 8) & 0xFF);
    char c = (char)((id >> 16) & 0xFF);
    char d = (char)((id >> 24) & 0xFF);

    if (a < '0' || a > '9' || b < '0' || b > '9')
        return false;

    if (stream_index) *stream_index = (a - '0') * 10 + (b - '0');
    if (kind0) *kind0 = c;
    if (kind1) *kind1 = d;
    return true;
}

static void apply_video_stream(File& file, int stream_index, const StreamDesc& desc,
                               int& video_stream_index) {
    if (video_stream_index >= 0) return;
    if (desc.type != FCC_vids) return;
    if (desc.codec == VideoCodec::None) return;
    if (desc.width <= 0 || desc.height <= 0) return;

    file.width = desc.width;
    file.height = desc.height;
    file.top_down = desc.top_down;
    file.bits_per_pixel = desc.bits_per_pixel;
    file.codec = desc.codec;
    file.stream_scale = desc.scale;
    file.stream_rate = desc.rate;
    file.declared_frame_count = desc.length;
    video_stream_index = stream_index;
}

static void apply_audio_stream(File& file, int stream_index, const StreamDesc& desc,
                               int& audio_stream_index) {
    if (audio_stream_index >= 0) return;
    if (desc.type != FCC_auds) return;
    if (desc.audio_format != 1) return;
    if (desc.audio_channels < 1 || desc.audio_channels > 2) return;
    if (desc.audio_bits != 8 && desc.audio_bits != 16) return;
    if (desc.audio_block_align == 0 || desc.audio_rate == 0) return;

    file.audio_format = desc.audio_format;
    file.audio_channels = desc.audio_channels;
    file.audio_bits = desc.audio_bits;
    file.audio_block_align = desc.audio_block_align;
    file.audio_rate = desc.audio_rate;
    file.audio_avg_bytes_per_sec = desc.audio_avg_bytes_per_sec;
    file.audio_supported = true;
    audio_stream_index = stream_index;
}

static bool parse_strl(File& file, uint64_t start, uint64_t size,
                       int& stream_counter, int& video_stream_index, int& audio_stream_index) {
    StreamDesc desc = {};
    uint64_t pos = start;
    uint64_t end = start + size;
    uint8_t buf[64];

    while (pos + 8 <= end) {
        if (!read_exact(file, pos, buf, 8)) {
            set_error(file, "Failed to read AVI stream header");
            return false;
        }

        uint32_t id = rd_u32(buf + 0);
        uint32_t chunk_size = rd_u32(buf + 4);
        uint64_t payload = pos + 8;
        uint64_t next = payload + align2(chunk_size);
        if (next > end || next > file.size) {
            set_error(file, "AVI stream chunk is truncated");
            return false;
        }

        if (id == FCC_strh && chunk_size >= 48) {
            uint32_t bytes = min_u32(chunk_size, (uint32_t)sizeof(buf));
            if (!read_exact(file, payload, buf, bytes)) {
                set_error(file, "Failed to read AVI stream format");
                return false;
            }
            desc.type = rd_u32(buf + 0);
            desc.handler = rd_u32(buf + 4);
            desc.scale = rd_u32(buf + 20);
            desc.rate = rd_u32(buf + 24);
            desc.length = rd_u32(buf + 32);
        } else if (id == FCC_strf) {
            uint32_t bytes = min_u32(chunk_size, (uint32_t)sizeof(buf));
            if (!read_exact(file, payload, buf, bytes)) {
                set_error(file, "Failed to read AVI stream data");
                return false;
            }

            if (desc.type == FCC_vids && chunk_size >= 40) {
                int32_t width = rd_s32(buf + 4);
                int32_t height = rd_s32(buf + 8);
                uint16_t bit_count = rd_u16(buf + 14);
                uint32_t compression = rd_u32(buf + 16);

                desc.width = width > 0 ? width : -width;
                desc.height = height > 0 ? height : -height;
                desc.top_down = height < 0;
                desc.bits_per_pixel = bit_count;

                if ((compression == 0 || compression == FCC_DIB) && bit_count == 24)
                    desc.codec = VideoCodec::RGB24;
                else if ((compression == 0 || compression == FCC_DIB) && bit_count == 32)
                    desc.codec = VideoCodec::RGB32;
                else if (compression == FCC_MJPG)
                    desc.codec = VideoCodec::MJPG;
            } else if (desc.type == FCC_auds && chunk_size >= 16) {
                desc.audio_format = rd_u16(buf + 0);
                desc.audio_channels = rd_u16(buf + 2);
                desc.audio_rate = rd_u32(buf + 4);
                desc.audio_avg_bytes_per_sec = rd_u32(buf + 8);
                desc.audio_block_align = rd_u16(buf + 12);
                desc.audio_bits = rd_u16(buf + 14);
            }
        }

        pos = next;
    }

    apply_video_stream(file, stream_counter, desc, video_stream_index);
    apply_audio_stream(file, stream_counter, desc, audio_stream_index);
    stream_counter++;
    return true;
}

static bool parse_hdrl(File& file, uint64_t start, uint64_t size,
                       int& stream_counter, int& video_stream_index, int& audio_stream_index) {
    uint64_t pos = start;
    uint64_t end = start + size;
    uint8_t buf[48];

    while (pos + 8 <= end) {
        if (!read_exact(file, pos, buf, 8)) {
            set_error(file, "Failed to read AVI header");
            return false;
        }

        uint32_t id = rd_u32(buf + 0);
        uint32_t chunk_size = rd_u32(buf + 4);
        uint64_t payload = pos + 8;
        uint64_t next = payload + align2(chunk_size);
        if (next > end || next > file.size) {
            set_error(file, "AVI header chunk is truncated");
            return false;
        }

        if (id == FCC_avih && chunk_size >= 40) {
            if (!read_exact(file, payload, buf, 40)) {
                set_error(file, "Failed to read AVI main header");
                return false;
            }
            file.microsec_per_frame = rd_u32(buf + 0);
            if (file.declared_frame_count == 0)
                file.declared_frame_count = rd_u32(buf + 16);
        } else if (id == FCC_LIST && chunk_size >= 4) {
            if (!read_exact(file, payload, buf, 4)) {
                set_error(file, "Failed to read AVI list header");
                return false;
            }
            uint32_t list_type = rd_u32(buf + 0);
            if (list_type == FCC_strl) {
                if (!parse_strl(file, payload + 4, chunk_size - 4,
                                stream_counter, video_stream_index, audio_stream_index)) {
                    return false;
                }
            }
        }

        pos = next;
    }

    return true;
}

static bool parse_movi_list(File& file, uint64_t start, uint64_t size,
                            int video_stream_index, int audio_stream_index) {
    uint64_t pos = start;
    uint64_t end = start + size;
    uint8_t buf[12];

    while (pos + 8 <= end) {
        if (!read_exact(file, pos, buf, 8)) {
            set_error(file, "Failed to read AVI media index");
            return false;
        }

        uint32_t id = rd_u32(buf + 0);
        uint32_t chunk_size = rd_u32(buf + 4);
        uint64_t payload = pos + 8;
        uint64_t next = payload + align2(chunk_size);
        if (next > end || next > file.size) {
            set_error(file, "AVI media chunk is truncated");
            return false;
        }

        if (id == FCC_LIST && chunk_size >= 4) {
            if (!read_exact(file, payload, buf, 4)) {
                set_error(file, "Failed to read AVI media list header");
                return false;
            }
            uint32_t list_type = rd_u32(buf + 0);
            if (list_type == FCC_rec || list_type == FCC_movi) {
                if (!parse_movi_list(file, payload + 4, chunk_size - 4,
                                     video_stream_index, audio_stream_index)) {
                    return false;
                }
            }
            pos = next;
            continue;
        }

        int stream_index = -1;
        char kind0 = 0;
        char kind1 = 0;
        if (!chunk_stream_id(id, &stream_index, &kind0, &kind1)) {
            pos = next;
            continue;
        }

        if (stream_index == video_stream_index &&
            ((kind0 == 'd' && kind1 == 'b') || (kind0 == 'd' && kind1 == 'c') ||
             (kind0 == 'D' && kind1 == 'B') || (kind0 == 'D' && kind1 == 'C'))) {
            if (!push_chunk(file.video_chunks, file.video_chunk_count, file.video_chunk_cap,
                            (uint32_t)payload, chunk_size)) {
                set_error(file, "Out of memory indexing video frames");
                return false;
            }
        } else if (stream_index == audio_stream_index && kind0 == 'w' && kind1 == 'b') {
            if (!push_chunk(file.audio_chunks, file.audio_chunk_count, file.audio_chunk_cap,
                            (uint32_t)payload, chunk_size)) {
                set_error(file, "Out of memory indexing audio chunks");
                return false;
            }
            file.total_audio_bytes += chunk_size;
        }

        pos = next;
    }

    return true;
}

void reset(File& file) {
    if (file.fd >= 0) montauk::close(file.fd);
    if (file.video_chunks) montauk::mfree(file.video_chunks);
    if (file.audio_chunks) montauk::mfree(file.audio_chunks);
    if (file.decode_buffer) montauk::mfree(file.decode_buffer);
    montauk::memset(&file, 0, sizeof(file));
    file.fd = -1;
}

bool parse(File& file, int fd, uint64_t size) {
    reset(file);
    file.fd = fd;
    file.size = size;

    uint8_t buf[12];
    if (size < 12) {
        set_error(file, "File is too small to be a valid AVI");
        return false;
    }

    if (!read_exact(file, 0, buf, 12)) {
        set_error(file, "Failed to read AVI header");
        return false;
    }

    if (rd_u32(buf + 0) != FCC_RIFF || rd_u32(buf + 8) != FCC_AVI) {
        set_error(file, "File is not a RIFF AVI container");
        return false;
    }

    int stream_counter = 0;
    int video_stream_index = -1;
    int audio_stream_index = -1;

    uint64_t pos = 12;
    while (pos + 8 <= size) {
        if (!read_exact(file, pos, buf, 8)) {
            set_error(file, "Failed to read AVI top-level chunk");
            return false;
        }

        uint32_t id = rd_u32(buf + 0);
        uint32_t chunk_size = rd_u32(buf + 4);
        uint64_t payload = pos + 8;
        uint64_t next = payload + align2(chunk_size);
        if (next > size) {
            set_error(file, "AVI top-level chunk is truncated");
            return false;
        }

        if (id == FCC_LIST && chunk_size >= 4) {
            if (!read_exact(file, payload, buf, 4)) {
                set_error(file, "Failed to read AVI list type");
                return false;
            }
            uint32_t list_type = rd_u32(buf + 0);
            if (list_type == FCC_hdrl) {
                if (!parse_hdrl(file, payload + 4, chunk_size - 4,
                                stream_counter, video_stream_index, audio_stream_index)) {
                    return false;
                }
            } else if (list_type == FCC_movi) {
                if (!parse_movi_list(file, payload + 4, chunk_size - 4,
                                     video_stream_index, audio_stream_index)) {
                    return false;
                }
            }
        }

        pos = next;
    }

    if (video_stream_index < 0 || file.video_chunk_count <= 0 || file.width <= 0 || file.height <= 0) {
        set_error(file, "AVI video stream is missing or unsupported");
        return false;
    }

    if (file.audio_supported && file.audio_block_align > 0) {
        file.total_audio_frames = file.total_audio_bytes / file.audio_block_align;
    }

    file.valid = true;
    return true;
}

static bool ensure_decode_buffer(File& file, uint32_t need) {
    if (need <= file.decode_buffer_cap) return true;

    uint32_t new_cap = file.decode_buffer_cap ? file.decode_buffer_cap : 4096u;
    while (new_cap < need) {
        if (new_cap > 0x7FFFFFFFu / 2u) {
            new_cap = need;
            break;
        }
        new_cap *= 2u;
    }

    auto* next = (uint8_t*)montauk::realloc(file.decode_buffer, new_cap);
    if (!next) return false;
    file.decode_buffer = next;
    file.decode_buffer_cap = new_cap;
    return true;
}

static bool decode_rgb(File& file, const ChunkRef& chunk, uint32_t* out_argb) {
    if (!out_argb || file.width <= 0 || file.height <= 0) return false;

    int bytes_per_pixel = (file.codec == VideoCodec::RGB32) ? 4 : 3;
    int stride = ((file.width * bytes_per_pixel) + 3) & ~3;
    uint64_t needed64 = (uint64_t)stride * (uint64_t)file.height;
    if (chunk.size < needed64 || needed64 > 0xFFFFFFFFull) return false;

    uint32_t needed = (uint32_t)needed64;
    if (!ensure_decode_buffer(file, needed)) return false;
    if (!read_exact(file, chunk.offset, file.decode_buffer, needed)) return false;

    const uint8_t* src = file.decode_buffer;
    for (int y = 0; y < file.height; y++) {
        int src_y = file.top_down ? y : (file.height - 1 - y);
        const uint8_t* row = src + (uint64_t)src_y * (uint64_t)stride;
        uint32_t* dst = out_argb + (uint64_t)y * (uint64_t)file.width;

        for (int x = 0; x < file.width; x++) {
            uint8_t b = row[x * bytes_per_pixel + 0];
            uint8_t g = row[x * bytes_per_pixel + 1];
            uint8_t r = row[x * bytes_per_pixel + 2];
            dst[x] = 0xFF000000u | ((uint32_t)r << 16) | ((uint32_t)g << 8) | b;
        }
    }

    return true;
}

static void scale_rgba_to_argb(uint32_t* out_argb, int out_w, int out_h,
                               const uint8_t* src_rgba, int src_w, int src_h) {
    for (int y = 0; y < out_h; y++) {
        int sy = (int)((uint64_t)y * (uint64_t)src_h / (uint64_t)out_h);
        const uint8_t* src_row = src_rgba + (uint64_t)sy * (uint64_t)src_w * 4u;
        uint32_t* dst = out_argb + (uint64_t)y * (uint64_t)out_w;

        for (int x = 0; x < out_w; x++) {
            int sx = (int)((uint64_t)x * (uint64_t)src_w / (uint64_t)out_w);
            const uint8_t* p = src_row + (uint64_t)sx * 4u;
            dst[x] = 0xFF000000u | ((uint32_t)p[0] << 16) | ((uint32_t)p[1] << 8) | p[2];
        }
    }
}

static bool decode_mjpg(File& file, const ChunkRef& chunk, uint32_t* out_argb) {
    if (chunk.size > 0x7FFFFFFFu) return false;
    if (!ensure_decode_buffer(file, chunk.size)) return false;
    if (!read_exact(file, chunk.offset, file.decode_buffer, chunk.size)) return false;

    int w = 0;
    int h = 0;
    int channels = 0;
    unsigned char* rgba = stbi_load_from_memory(file.decode_buffer, (int)chunk.size,
                                                &w, &h, &channels, 4);
    if (!rgba || w <= 0 || h <= 0) {
        if (rgba) stbi_image_free(rgba);
        return false;
    }

    if (w == file.width && h == file.height) {
        for (int i = 0; i < w * h; i++) {
            uint8_t r = rgba[i * 4 + 0];
            uint8_t g = rgba[i * 4 + 1];
            uint8_t b = rgba[i * 4 + 2];
            out_argb[i] = 0xFF000000u | ((uint32_t)r << 16) | ((uint32_t)g << 8) | b;
        }
    } else {
        scale_rgba_to_argb(out_argb, file.width, file.height, rgba, w, h);
    }

    stbi_image_free(rgba);
    return true;
}

bool decode_frame(File& file, int frame_index, uint32_t* out_argb) {
    if (!file.valid || !out_argb) return false;
    if (frame_index < 0 || frame_index >= file.video_chunk_count) return false;

    const ChunkRef& chunk = file.video_chunks[frame_index];
    if (file.codec == VideoCodec::RGB24 || file.codec == VideoCodec::RGB32)
        return decode_rgb(file, chunk, out_argb);
    if (file.codec == VideoCodec::MJPG)
        return decode_mjpg(file, chunk, out_argb);
    return false;
}

uint64_t video_duration_us(const File& file) {
    if (file.video_chunk_count <= 0) return 0;

    if (file.stream_rate > 0 && file.stream_scale > 0) {
        return ((uint64_t)file.video_chunk_count * (uint64_t)file.stream_scale * 1000000ull) /
               (uint64_t)file.stream_rate;
    }
    if (file.microsec_per_frame > 0)
        return (uint64_t)file.video_chunk_count * (uint64_t)file.microsec_per_frame;
    return 0;
}

uint64_t audio_duration_us(const File& file) {
    if (!file.audio_supported || file.audio_rate == 0 || file.total_audio_frames == 0) return 0;
    return (file.total_audio_frames * 1000000ull) / (uint64_t)file.audio_rate;
}

uint64_t duration_us(const File& file) {
    uint64_t v = video_duration_us(file);
    uint64_t a = audio_duration_us(file);
    return v > a ? v : a;
}

int frame_index_for_time(const File& file, uint64_t time_us) {
    if (file.video_chunk_count <= 0) return -1;
    if (file.video_chunk_count == 1) return 0;

    int index = 0;
    if (file.stream_rate > 0 && file.stream_scale > 0) {
        uint64_t denom = (uint64_t)file.stream_scale * 1000000ull;
        if (denom > 0)
            index = (int)((time_us * (uint64_t)file.stream_rate) / denom);
    } else if (file.microsec_per_frame > 0) {
        index = (int)(time_us / (uint64_t)file.microsec_per_frame);
    }

    if (index < 0) index = 0;
    if (index >= file.video_chunk_count) index = file.video_chunk_count - 1;
    return index;
}

const char* codec_name(const File& file) {
    switch (file.codec) {
        case VideoCodec::RGB24: return "RGB24";
        case VideoCodec::RGB32: return "RGB32";
        case VideoCodec::MJPG:  return "MJPEG";
        default:                return "Unknown";
    }
}

bool has_supported_audio(const File& file) {
    return file.audio_supported && file.audio_chunk_count > 0;
}

} // namespace avi
