#ifndef MTK_MSBC_CODEC_HPP
#define MTK_MSBC_CODEC_HPP

#include <stddef.h>
#include <stdint.h>
#include "vendor/google-libsbc/include/sbc.h"

class MtkMsbcCodec {
public:
    enum { kSamples = 120, kFrameBytes = 60 };
    struct TxState {
        sbc_t encoder;
        unsigned sequence;
    };

    MtkMsbcCodec() { reset(); }
    void reset();
    void resetTransmit();
    void resetReceive();
    void saveTransmit(TxState &state) const;
    void restoreTransmit(const TxState &state);
    bool encode(const int16_t *pcm, size_t sampleCount, uint8_t *h2, size_t capacity);
    bool decode(const uint8_t *h2, size_t length, int16_t *pcm, size_t capacity,
                unsigned *missingFrames = nullptr);
    bool conceal(int16_t *pcm, size_t capacity);

private:
    sbc_t encoder_, decoder_;
    unsigned txSequence_, rxExpected_;
    bool rxStarted_;
};

#endif
