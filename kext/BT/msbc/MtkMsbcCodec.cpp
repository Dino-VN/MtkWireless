#include "MtkMsbcCodec.hpp"

static const uint8_t kH2Sequence[] = { 0x08, 0x38, 0xc8, 0xf8 };
static const sbc_frame kMsbcFrame = {
    true, SBC_FREQ_16K, SBC_MODE_MONO, SBC_BAM_LOUDNESS, 15, 8, 26
};

void MtkMsbcCodec::reset()
{
    resetTransmit();
    resetReceive();
}

void MtkMsbcCodec::resetReceive()
{
    sbc_reset(&decoder_);
    decoder_.nchannels = 1;
    decoder_.nblocks = 15;
    decoder_.nsubbands = 8;
    rxExpected_ = 0;
    rxStarted_ = false;
}

void MtkMsbcCodec::resetTransmit()
{
    sbc_reset(&encoder_);
    txSequence_ = 0;
}

void MtkMsbcCodec::saveTransmit(TxState &state) const
{
    state.encoder = encoder_;
    state.sequence = txSequence_;
}

void MtkMsbcCodec::restoreTransmit(const TxState &state)
{
    encoder_ = state.encoder;
    txSequence_ = state.sequence;
}

bool MtkMsbcCodec::encode(const int16_t *pcm, size_t sampleCount, uint8_t *h2, size_t capacity)
{
    if (!pcm || !h2 || sampleCount != kSamples || capacity < kFrameBytes)
        return false;
    if (sbc_encode(&encoder_, pcm, 1, nullptr, 0, &kMsbcFrame, h2 + 2, SBC_MSBC_SIZE))
        return false;
    h2[0] = 0x01;
    h2[1] = kH2Sequence[txSequence_];
    h2[59] = 0;
    txSequence_ = (txSequence_ + 1) & 3;
    return true;
}

bool MtkMsbcCodec::decode(const uint8_t *h2, size_t length, int16_t *pcm, size_t capacity,
                          unsigned *missingFrames)
{
    if (missingFrames)
        *missingFrames = 0;
    if (!h2 || !pcm || length != kFrameBytes || capacity < kSamples ||
        h2[0] != 0x01 || h2[2] != 0xad || h2[3] || h2[4])
        return false;
    unsigned sequence = 0;
    while (sequence < 4 && h2[1] != kH2Sequence[sequence])
        ++sequence;
    if (sequence == 4)
        return false;
    sbc_frame description;
    if (sbc_decode(&decoder_, h2 + 2, SBC_MSBC_SIZE, &description, pcm, 1, nullptr, 0))
        return false;
    // H2 alone cannot distinguish a duplicate from three missing frames.
    if (missingFrames && rxStarted_)
        *missingFrames = (sequence - rxExpected_) & 3;
    rxExpected_ = (sequence + 1) & 3;
    rxStarted_ = true;
    return true;
}

bool MtkMsbcCodec::conceal(int16_t *pcm, size_t capacity)
{
    if (!pcm || capacity < kSamples)
        return false;
    sbc_frame description = kMsbcFrame;
    return sbc_decode(&decoder_, nullptr, 0, &description, pcm, 1, nullptr, 0) == 0;
}
