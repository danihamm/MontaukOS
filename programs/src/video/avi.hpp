#pragma once

#include <cstdint>

namespace avi {

enum class VideoCodec {
    None,
    RGB24,
    RGB32,
    MJPG,
};

struct ChunkRef {
    uint32_t offset;
    uint32_t size;
};

struct File {
    int fd;
    uint64_t size;

    int width;
    int height;
    bool top_down;
    int bits_per_pixel;
    VideoCodec codec;

    uint32_t stream_scale;
    uint32_t stream_rate;
    uint32_t microsec_per_frame;
    uint32_t declared_frame_count;

    uint16_t audio_format;
    uint16_t audio_channels;
    uint16_t audio_bits;
    uint16_t audio_block_align;
    uint32_t audio_rate;
    uint32_t audio_avg_bytes_per_sec;
    bool audio_supported;

    ChunkRef* video_chunks;
    int video_chunk_count;
    int video_chunk_cap;

    ChunkRef* audio_chunks;
    int audio_chunk_count;
    int audio_chunk_cap;

    uint64_t total_audio_bytes;
    uint64_t total_audio_frames;

    uint8_t* decode_buffer;
    uint32_t decode_buffer_cap;

    bool valid;
    char error[128];
};

void reset(File& file);
bool parse(File& file, int fd, uint64_t size);
bool read_bytes(const File& file, uint64_t offset, uint8_t* out, uint32_t size);
bool decode_frame(File& file, int frame_index, uint32_t* out_argb);

uint64_t video_duration_us(const File& file);
uint64_t audio_duration_us(const File& file);
uint64_t duration_us(const File& file);

int frame_index_for_time(const File& file, uint64_t time_us);
const char* codec_name(const File& file);
bool has_supported_audio(const File& file);

} // namespace avi
