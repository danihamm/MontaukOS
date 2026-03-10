/*
    * Sbc.cpp
    * SBC (Sub-Band Codec) encoder — fixed-point implementation
    * Based on the Bluetooth SIG specification (A2DP v1.3, Appendix B)
    * All arithmetic is 32-bit fixed-point (Q15.16 or Q1.30)
    * Copyright (c) 2026 Daniel Hammer
*/

#include "Sbc.hpp"
#include <Libraries/Memory.hpp>

namespace Drivers::USB::Bluetooth::Sbc {

    // =========================================================================
    // Fixed-point constants (Q1.30 format for filter coefficients)
    // =========================================================================

    // Scale factor: 1.0 = (1 << 15) in Q15.16
    constexpr int32_t FP_ONE = (1 << 15);

    // SBC 8-subband analysis filter prototype coefficients (Q1.30)
    // These are the 80 windowed prototype coefficients from the SBC spec
    // Scaled to Q15.16 for our fixed-point arithmetic
    static const int32_t g_proto8[80] = {
         0x0002,  0x0005,  0x000A,  0x0014,  0x0023,  0x0038,  0x0054,  0x0078,
         0x00A5,  0x00DB,  0x011B,  0x0164,  0x01B5,  0x020E,  0x026D,  0x02D0,
         0x0335,  0x039A,  0x03FC,  0x0458,  0x04AB,  0x04F1,  0x0527,  0x054A,
         0x0557,  0x054A,  0x0527,  0x04F1,  0x04AB,  0x0458,  0x03FC,  0x039A,
         0x0335,  0x02D0,  0x026D,  0x020E,  0x01B5,  0x0164,  0x011B,  0x00DB,
         0x00A5,  0x0078,  0x0054,  0x0038,  0x0023,  0x0014,  0x000A,  0x0005,
         0x0002, -0x0002, -0x0005, -0x000A, -0x0014, -0x0023, -0x0038, -0x0054,
        -0x0078, -0x00A5, -0x00DB, -0x011B, -0x0164, -0x01B5, -0x020E, -0x026D,
        -0x02D0, -0x0335, -0x039A, -0x03FC, -0x0458, -0x04AB, -0x04F1, -0x0527,
        -0x054A, -0x0557, -0x054A, -0x0527, -0x04F1, -0x04AB, -0x0458, -0x03FC,
    };

    // Cosine matrix for 8-subband DCT-II (Q15.16)
    // cos_matrix[k][i] = cos((k + 0.5) * (2*i + 1) * PI / 16) * FP_ONE
    static const int32_t g_cosMatrix8[8][16] = {
        { 32138, 31650, 30679, 29246, 27381, 25126, 22529, 19644, 16531, 13254,  9882,  6484,  3134,  -199, -3509, -6758},
        { 30679, 25126, 16531,  6484, -3509,-13254,-22529,-29246,-32138,-31650,-27381,-19644, -9882,   199,  9882, 19644},
        { 27381, 13254, -3509,-19644,-30679,-32138,-22529, -6484,  9882, 25126, 32138, 29246, 16531,  -199,-16531,-29246},
        { 22529,  -199,-22529,  -199, 22529,   199,-22529,  -199, 22529,   199,-22529,  -199, 22529,   199,-22529,  -199},
        { 16531,-13254,-30679,  6484, 32138, -199,-32138, -6484, 30679, 13254,-16531,-25126,  3509, 29246,  9882,-27381},
        {  9882,-25126,-16531, 29246,  3509,-32138,  9882, 25126,-16531,-29246,  3509, 32138, -9882,-25126, 16531, 29246},
        {  3134,-31650, 27381, -6484,-22529, 32138,-13254, -9882, 30679,-29246,  6484, 19644,-32138, 16531,  3509,-25126},
        { -3509, 32138,-22529, -6484, 30679,-27381,  3509, 25126,-32138, 16531,  9882,-30679, 22529, -199,-25126, 32138},
    };

    // =========================================================================
    // CRC-8 table (SBC spec CRC polynomial: x^8 + x^4 + x^3 + x^2 + 1)
    // =========================================================================

    static uint8_t SbcCrc8(const uint8_t* data, uint32_t len, uint8_t bits_last_byte) {
        uint8_t crc = 0x0F;
        for (uint32_t i = 0; i < len; i++) {
            uint8_t byte = data[i];
            uint8_t nbits = (i == len - 1) ? bits_last_byte : 8;
            for (uint8_t bit = 0; bit < nbits; bit++) {
                uint8_t msb = (crc >> 7) & 1;
                crc <<= 1;
                if (((byte >> (7 - bit)) & 1) ^ msb) {
                    crc ^= 0x1D;
                }
            }
        }
        return crc;
    }

    // =========================================================================
    // Init
    // =========================================================================

    void Init(SbcEncoder* enc, uint32_t sampleRate, uint8_t channels, uint8_t /*bitsPerSample*/) {
        memset(enc, 0, sizeof(SbcEncoder));

        enc->Subbands = SBC_SUBBANDS;
        enc->Blocks = SBC_BLOCKS;
        enc->Bitpool = SBC_BITPOOL;
        enc->AllocMethod = ALLOC_LOUDNESS;

        if (channels >= 2) {
            enc->Channels = 2;
            enc->ChannelMode = MODE_JOINT_STEREO;
        } else {
            enc->Channels = 1;
            enc->ChannelMode = MODE_MONO;
        }

        switch (sampleRate) {
            case 16000: enc->Frequency = FREQ_16000; break;
            case 32000: enc->Frequency = FREQ_32000; break;
            case 44100: enc->Frequency = FREQ_44100; break;
            default:    enc->Frequency = FREQ_48000; break;
        }

        enc->SamplesPerFrame = enc->Blocks * enc->Subbands;

        // Calculate frame size
        // For joint stereo: 4 + (4 * subbands * channels) / 8 + ceil(blocks * bitpool / 8) + subbands/8
        uint32_t headerBits = 32 + (4 * enc->Subbands * enc->Channels);
        if (enc->ChannelMode == MODE_JOINT_STEREO) {
            headerBits += enc->Subbands;  // join bits
        }
        uint32_t dataBits = enc->Blocks * enc->Bitpool;
        enc->FrameSize = (headerBits + dataBits + 7) / 8;
    }

    // =========================================================================
    // Analysis filter bank (8 subbands)
    // =========================================================================

    static void AnalysisFilter(SbcEncoder* enc, const int16_t* pcm, int ch,
                               int32_t sb_samples[SBC_BLOCKS][SBC_SUBBANDS]) {
        for (int blk = 0; blk < enc->Blocks; blk++) {
            // Shift in new samples
            int pos = enc->XPos[ch];
            for (int i = enc->Subbands - 1; i >= 0; i--) {
                pos = (pos + 1) % (enc->Subbands * 10);
                enc->X[ch][pos] = (int32_t)pcm[blk * enc->Subbands * enc->Channels + i * enc->Channels + ch];
            }
            enc->XPos[ch] = pos;

            // Windowing and partial calculation
            int32_t Z[2 * SBC_SUBBANDS];
            for (int i = 0; i < 2 * enc->Subbands; i++) {
                Z[i] = 0;
                for (int j = 0; j < 5; j++) {
                    int idx = (pos + i + j * 2 * enc->Subbands) % (enc->Subbands * 10);
                    int protoIdx = i + j * 2 * enc->Subbands;
                    if (protoIdx < 80) {
                        Z[i] += (int32_t)(((int64_t)enc->X[ch][idx] * g_proto8[protoIdx]) >> 15);
                    }
                }
            }

            // Matrixing (DCT)
            for (int k = 0; k < enc->Subbands; k++) {
                int32_t sum = 0;
                for (int i = 0; i < 2 * enc->Subbands; i++) {
                    sum += (int32_t)(((int64_t)Z[i] * g_cosMatrix8[k][i]) >> 15);
                }
                sb_samples[blk][k] = sum;
            }
        }
    }

    // =========================================================================
    // Bit allocation (Loudness method)
    // =========================================================================

    static void BitAllocation(SbcEncoder* enc,
                              int32_t sb_samples[SBC_BLOCKS][SBC_SUBBANDS],
                              int ch,
                              int32_t scale_factors[SBC_SUBBANDS],
                              uint8_t bits[SBC_SUBBANDS]) {
        // Compute scale factors
        for (int sb = 0; sb < enc->Subbands; sb++) {
            int32_t maxVal = 0;
            for (int blk = 0; blk < enc->Blocks; blk++) {
                int32_t val = sb_samples[blk][sb];
                if (val < 0) val = -val;
                if (val > maxVal) maxVal = val;
            }

            // Find scale factor (highest bit position)
            scale_factors[sb] = 0;
            int32_t tmp = maxVal;
            while (tmp > 0) {
                scale_factors[sb]++;
                tmp >>= 1;
            }
        }

        // Loudness offset table for 8 subbands (from SBC spec)
        static const int8_t loudness_offset_8[4][8] = {
            {-2, 0, 0, 0, 0, 0, 0, 1},   // 16kHz
            {-3, 0, 0, 0, 0, 0, 1, 2},   // 32kHz
            {-4, 0, 0, 0, 0, 0, 1, 2},   // 44.1kHz
            {-4, 0, 0, 0, 0, 0, 1, 2},   // 48kHz
        };

        // Compute bitneed
        int32_t bitneed[SBC_SUBBANDS];
        for (int sb = 0; sb < enc->Subbands; sb++) {
            if (enc->AllocMethod == ALLOC_LOUDNESS) {
                bitneed[sb] = scale_factors[sb] - loudness_offset_8[enc->Frequency][sb];
            } else {
                bitneed[sb] = scale_factors[sb];
            }
        }

        // Bit allocation loop
        int32_t bitcount = 0;
        int32_t slicecount = 0;
        int32_t bitslice = (int32_t)(scale_factors[0] > 0 ? scale_factors[0] : 1);

        // Find max bitneed
        for (int sb = 0; sb < enc->Subbands; sb++) {
            if (bitneed[sb] > bitslice) bitslice = bitneed[sb];
        }
        bitslice++;

        // Iterative allocation
        for (int sb = 0; sb < enc->Subbands; sb++) bits[sb] = 0;

        while (true) {
            bitslice--;
            bitcount = 0;
            slicecount = 0;
            for (int sb = 0; sb < enc->Subbands; sb++) {
                if (bitneed[sb] >= bitslice + 1 && bitneed[sb] < bitslice + 16) {
                    if (bitneed[sb] == bitslice + 1) {
                        bitcount += 2;
                        slicecount++;
                    } else {
                        bitcount++;
                        slicecount++;
                    }
                }
            }
            if (bitcount + slicecount >= enc->Bitpool) break;
            if (bitslice <= -16) break;

            for (int sb = 0; sb < enc->Subbands; sb++) {
                if (bitneed[sb] >= bitslice + 1 && bitneed[sb] < bitslice + 16) {
                    if (bitneed[sb] == bitslice + 1) {
                        bits[sb] = 2;
                    } else if (bits[sb] < 16) {
                        bits[sb]++;
                    }
                }
            }
        }

        // Distribute remaining bits
        int32_t remaining = enc->Bitpool - bitcount;
        for (int sb = 0; sb < enc->Subbands && remaining > 0; sb++) {
            if (bits[sb] >= 2 && bits[sb] < 16) {
                bits[sb]++;
                remaining--;
            } else if (bitneed[sb] == bitslice && bits[sb] == 0) {
                bits[sb] = 2;
                remaining -= 2;
                if (remaining < 0) { bits[sb] = 0; break; }
            }
        }

        for (int sb = 0; sb < enc->Subbands && remaining > 0; sb++) {
            if (bits[sb] < 16) {
                bits[sb]++;
                remaining--;
            }
        }
    }

    // =========================================================================
    // Bit packing helpers
    // =========================================================================

    struct BitWriter {
        uint8_t* Data;
        uint32_t BitPos;
    };

    static void WriteBits(BitWriter* bw, uint32_t value, uint8_t nbits) {
        for (int i = nbits - 1; i >= 0; i--) {
            uint32_t bytePos = bw->BitPos / 8;
            uint8_t bitOff = 7 - (bw->BitPos % 8);
            if (value & (1u << i)) {
                bw->Data[bytePos] |= (1u << bitOff);
            }
            bw->BitPos++;
        }
    }

    // =========================================================================
    // Encode
    // =========================================================================

    uint32_t Encode(SbcEncoder* enc, const int16_t* pcm, uint8_t* out) {
        int32_t sb_samples[SBC_CHANNELS][SBC_BLOCKS][SBC_SUBBANDS];
        int32_t scale_factors[SBC_CHANNELS][SBC_SUBBANDS];
        uint8_t bits[SBC_CHANNELS][SBC_SUBBANDS];

        // Clear output
        memset(out, 0, enc->FrameSize);

        // Analysis filter for each channel
        for (int ch = 0; ch < enc->Channels; ch++) {
            AnalysisFilter(enc, pcm, ch, sb_samples[ch]);
            BitAllocation(enc, sb_samples[ch], ch, scale_factors[ch], bits[ch]);
        }

        // Joint stereo processing
        uint8_t joint = 0;
        if (enc->ChannelMode == MODE_JOINT_STEREO) {
            for (int sb = 0; sb < enc->Subbands - 1; sb++) {
                // Simple heuristic: use joint coding if it saves bits
                int32_t maxMid = 0, maxSide = 0;
                for (int blk = 0; blk < enc->Blocks; blk++) {
                    int32_t mid = (sb_samples[0][blk][sb] + sb_samples[1][blk][sb]) / 2;
                    int32_t side = (sb_samples[0][blk][sb] - sb_samples[1][blk][sb]) / 2;
                    if (mid < 0) mid = -mid;
                    if (side < 0) side = -side;
                    if (mid > maxMid) maxMid = mid;
                    if (side > maxSide) maxSide = side;
                }
                int32_t maxOrig = 0;
                for (int blk = 0; blk < enc->Blocks; blk++) {
                    int32_t v0 = sb_samples[0][blk][sb]; if (v0 < 0) v0 = -v0;
                    int32_t v1 = sb_samples[1][blk][sb]; if (v1 < 0) v1 = -v1;
                    if (v0 > maxOrig) maxOrig = v0;
                    if (v1 > maxOrig) maxOrig = v1;
                }
                if (maxMid + maxSide < maxOrig) {
                    joint |= (1 << (enc->Subbands - 1 - sb));
                    for (int blk = 0; blk < enc->Blocks; blk++) {
                        int32_t l = sb_samples[0][blk][sb];
                        int32_t r = sb_samples[1][blk][sb];
                        sb_samples[0][blk][sb] = (l + r) / 2;
                        sb_samples[1][blk][sb] = (l - r) / 2;
                    }
                    // Recalculate scale factors for joint channels
                    for (int ch = 0; ch < 2; ch++) {
                        int32_t maxVal = 0;
                        for (int blk = 0; blk < enc->Blocks; blk++) {
                            int32_t val = sb_samples[ch][blk][sb];
                            if (val < 0) val = -val;
                            if (val > maxVal) maxVal = val;
                        }
                        scale_factors[ch][sb] = 0;
                        int32_t tmp = maxVal;
                        while (tmp > 0) { scale_factors[ch][sb]++; tmp >>= 1; }
                    }
                }
            }
        }

        // Pack SBC frame header
        out[0] = 0x9C;  // Sync word
        out[1] = (enc->Frequency << 6) | ((enc->Blocks == 4 ? 0 : enc->Blocks == 8 ? 1 : enc->Blocks == 12 ? 2 : 3) << 4)
               | (enc->ChannelMode << 2) | (enc->AllocMethod << 1) | (enc->Subbands == 8 ? 1 : 0);
        out[2] = enc->Bitpool;

        // CRC (computed over header bytes 1-2 and scale factors)
        // Will be filled after scale factors are packed

        BitWriter bw = {out, 32};  // Start after 4-byte header

        // Joint stereo flags
        if (enc->ChannelMode == MODE_JOINT_STEREO) {
            WriteBits(&bw, joint, enc->Subbands);
        }

        // Pack scale factors (4 bits each)
        for (int ch = 0; ch < enc->Channels; ch++) {
            for (int sb = 0; sb < enc->Subbands; sb++) {
                uint32_t sf = scale_factors[ch][sb];
                if (sf > 15) sf = 15;
                WriteBits(&bw, sf, 4);
            }
        }

        // Compute CRC (over bytes 1, 2, and scale factor bits)
        uint32_t crcBits = 16 + (enc->Channels * enc->Subbands * 4);
        if (enc->ChannelMode == MODE_JOINT_STEREO) crcBits += enc->Subbands;
        out[3] = SbcCrc8(&out[1], (crcBits + 7) / 8, crcBits % 8 ? crcBits % 8 : 8);

        // Pack audio samples
        for (int blk = 0; blk < enc->Blocks; blk++) {
            for (int ch = 0; ch < enc->Channels; ch++) {
                for (int sb = 0; sb < enc->Subbands; sb++) {
                    if (bits[ch][sb] == 0) continue;

                    int32_t sf = scale_factors[ch][sb];
                    int32_t sample = sb_samples[ch][blk][sb];

                    // Quantize: levels = (1 << bits) - 1
                    uint32_t levels = (1u << bits[ch][sb]) - 1;
                    int32_t quantized;

                    if (sf > 0) {
                        // Normalize and quantize
                        int32_t maxRange = (1 << sf);
                        quantized = (int32_t)(((int64_t)(sample + maxRange) * levels) / (2 * maxRange));
                    } else {
                        quantized = levels / 2;
                    }

                    if (quantized < 0) quantized = 0;
                    if (quantized > (int32_t)levels) quantized = (int32_t)levels;

                    WriteBits(&bw, (uint32_t)quantized, bits[ch][sb]);
                }
            }
        }

        // Pad to byte boundary
        uint32_t totalBytes = (bw.BitPos + 7) / 8;
        return totalBytes > enc->FrameSize ? enc->FrameSize : totalBytes;
    }

    // =========================================================================
    // Queries
    // =========================================================================

    uint32_t GetFrameSize(const SbcEncoder* enc) {
        return enc->FrameSize;
    }

    uint32_t GetSamplesPerFrame(const SbcEncoder* enc) {
        return enc->SamplesPerFrame;
    }

}
