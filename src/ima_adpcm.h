#pragma once

#include <stdint.h>

// ATVV uses IMA ADPCM. Callers feed the high nibble before the low nibble.
class ImaAdpcm {
public:
    void reset(int16_t predictor = 0, uint8_t index = 0) {
        predictor_ = predictor;
        index_ = index <= 88 ? index : 88;
    }

    int16_t decode(uint8_t nibble) {
        static constexpr uint16_t steps[] = {
            7, 8, 9, 10, 11, 12, 13, 14, 16, 17, 19, 21, 23, 25, 28, 31, 34, 37,
            41, 45, 50, 55, 60, 66, 73, 80, 88, 97, 107, 118, 130, 143, 157, 173, 190,
            209, 230, 253, 279, 307, 337, 371, 408, 449, 494, 544, 598, 658, 724, 796,
            876, 963, 1060, 1166, 1282, 1411, 1552, 1707, 1878, 2066, 2272, 2499, 2749,
            3024, 3327, 3660, 4026, 4428, 4871, 5358, 5894, 6484, 7132, 7845, 8630, 9493,
            10442, 11487, 12635, 13899, 15289, 16818, 18500, 20350, 22385, 24623, 27086,
            29794, 32767
        };
        static constexpr int8_t changes[] = {-1, -1, -1, -1, 2, 4, 6, 8};
        const int step = steps[index_];
        int delta = step >> 3;
        if (nibble & 1) delta += step >> 2;
        if (nibble & 2) delta += step >> 1;
        if (nibble & 4) delta += step;
        predictor_ += (nibble & 8) ? -delta : delta;
        if (predictor_ > 32767) predictor_ = 32767;
        if (predictor_ < -32768) predictor_ = -32768;
        index_ += changes[nibble & 7];
        if (index_ < 0) index_ = 0;
        if (index_ > 88) index_ = 88;
        return int16_t(predictor_);
    }

private:
    int32_t predictor_ = 0;
    int index_ = 0;
};
