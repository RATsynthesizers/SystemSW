/**
 * @file      fx_frame.h
 *
 * @details   The wire frame: a fixed 32-slot layout led by a sync slot.
 *
 *            ------------------------------------------------------------------
 *            WHY THIS EXISTS
 *            ------------------------------------------------------------------
 *
 *            The SPI stream between the two controllers is POSITIONALLY FRAMED.
 *            There is no start-of-frame pattern in the data, no length, no CRC
 *            per frame. The receiver knows which slot it is looking at purely by
 *            counting words since the transfer began.
 *
 *            That has one catastrophic failure mode. Lose or gain a single word
 *            - a glitch on the clock, a DMA that stopped and restarted, an
 *            underrun at the transmitter - and every slot after it is rotated by
 *            one, permanently. Channel 2 lands in channel 3's file. The loop
 *            payload lands in a recorder plane. And NOTHING REPORTS IT, because
 *            rotated audio is still audio: it is the right sample rate, the
 *            right amplitude, plausible in every measurable way. You find out
 *            weeks later when a recording sounds like the wrong instrument.
 *
 *            The sync slot converts that into a fault that announces itself. One
 *            slot per frame is spent on a value the receiver can predict. If the
 *            prediction fails, the receiver knows the stream is not where it
 *            thinks it is, and can write silence instead of guessing.
 *
 *            The trade is deliberate and it is a good one:
 *
 *              - COST     one slot in thirty-two, 3.1% of the wire, plus one
 *                         32-bit compare per frame at the receiver.
 *              - BOUGHT   a silent, permanent, undetectable channel rotation
 *                         becomes an audible dropout with a counter beside it.
 *
 *            Silence is a far better failure than plausible wrong audio. A gap
 *            sounds like a fault and gets reported. A rerouted channel sounds
 *            like a mix-up and gets blamed on the musician.
 *
 *            ------------------------------------------------------------------
 *            THE LAYOUT - FIXED, ALWAYS
 *            ------------------------------------------------------------------
 *
 *                slot   0         1   2   3   4      5 ............... 31
 *                     [SYNC]    [R0][R1][R2][R3]   [L0][L1] ....... [L26]
 *                       |         \________ ___/     \_______ ______/
 *                       |                  v                 v
 *                       |          4 recorder planes   27 loop slots
 *                       |
 *                       +-- mark:16 | seq:16
 *
 *            32 slots x 4 bytes = 128 bytes per frame, at 48 kHz:
 *
 *                6.144 MB/s on the wire, 51.2% of the 96 MHz link
 *                4096-word half / 32 = 128 frames, exactly - see below
 *
 *            THE WIDTH NEVER CHANGES. It is 32 whether or not a loop transfer is
 *            running; the loop slots simply carry zeros when idle. That costs
 *            5.4 MB/s of zeros at idle and is worth every byte, because the
 *            alternative was renegotiating the frame width mid-stream on every
 *            loop save and load. Both ends had to switch on precisely the same
 *            frame boundary, and if they ever disagreed the result was the exact
 *            permanent rotation described above - at the one moment the link is
 *            busiest. That moment no longer exists.
 *
 *            A fixed width also makes the receiver's geometry compile-time
 *            constant again. Frames per half is 128, not a division by a value
 *            that arrived in an ACK, which is what let the de-interleave be told
 *            1024 frames of a buffer holding 128 of them.
 *
 *            ------------------------------------------------------------------
 *            THE SYNC WORD
 *            ------------------------------------------------------------------
 *
 *                31              16 15               0
 *                +-----------------+-----------------+
 *                |   MARK 0xFB5A   |   SEQ 0..65535  |
 *                +-----------------+-----------------+
 *
 *            The MARK is constant and catches rotation. 0xFB5A was chosen so its
 *            top byte is neither 0x00 nor 0xFF, which means it can never be
 *            mistaken for the upper half of a sign-extended 24-bit sample - the
 *            only thing a rotated recorder slot can contain. It is also not
 *            0x0000, 0xFFFF, 0x5555 or 0xAAAA, so a dead bus, a stuck line or a
 *            clock-like pattern cannot forge it.
 *
 *            The SEQ is a free-running per-frame counter and catches LOSS. A
 *            rotation and a dropped block are different faults with different
 *            causes, and telling them apart from a counter beats guessing:
 *
 *              mark wrong             -> stream is rotated or corrupt
 *              mark right, seq jumped -> frames were lost, and seq says how many
 *
 *            Together they also make a false accept vanishingly unlikely. Random
 *            data at the frame boundary must match BOTH the 16-bit mark and the
 *            exact expected sequence number - 2^-32 per frame, about one per
 *            25 hours at 48 kHz, and the very next frame catches it anyway.
 *
 * @version   1.0.0
 *
 * @authors   Claude (design draft)
 *
 * \date      04.09.2026 - First release
 *
 * @copyright RAT Synthesizers
 */

#ifndef FX_FRAME_H
#define FX_FRAME_H



/***************************************************************************************************
* Module includes
***************************************************************************************************/

#include "general.h"
#include "fx_defs.h"

/* One wire contract, two compilers: the audio controller builds this as C, the
 * interface controller includes it from C++. */
#ifdef __cplusplus
extern "C" {
#endif



/***************************************************************************************************
* Definitions of global (public) constants
***************************************************************************************************/

/** Bytes per slot. One S32 carrying a 24-bit sample, or four packed bytes. */
#define FX_FRAME_BYTES_PER_SLOT         (4U)

/**
 * Slots per frame. FIXED - this is not negotiated and does not change.
 *
 * 32 is chosen because it divides the receiver's 4096-word half exactly (128
 * frames), which is what keeps a half-transfer interrupt from ever landing in
 * the middle of a frame.
 */
#define FX_FRAME_SLOT_QTY               (32U)

/** Bytes per frame. 128. */
#define FX_FRAME_BYTES                  (FX_FRAME_SLOT_QTY * FX_FRAME_BYTES_PER_SLOT)

/** The sync slot leads every frame. */
#define FX_FRAME_SYNC_SLOT              (0U)
#define FX_FRAME_SYNC_SLOT_QTY          (1U)

/** Recorder planes follow the sync slot. */
#define FX_FRAME_REC_SLOT_BASE          (1U)
#define FX_FRAME_REC_SLOT_QTY           (4U)

/** The loop payload occupies everything left. */
#define FX_FRAME_LOOP_SLOT_BASE         (FX_FRAME_REC_SLOT_BASE + FX_FRAME_REC_SLOT_QTY)
#define FX_FRAME_LOOP_SLOT_QTY          (FX_FRAME_SLOT_QTY - FX_FRAME_LOOP_SLOT_BASE)

/**
 * The constant half of the sync word.
 *
 * See the file header for why this value and not another. Changing it breaks
 * the wire in both directions at once, so both boards must be reflashed
 * together - which is the point of it living in the shared library.
 */
#define FX_FRAME_SYNC_MARK              (0xFB5AU)

/** Build the sync word for a given frame sequence number. */
#define FX_FRAME_SYNC_WORD(seq)                                                             \
            ((S32)(U32)(((U32)FX_FRAME_SYNC_MARK << 16) | ((U32)(seq) & 0xFFFFUL)))

/** Take the mark and the sequence number back out of a received word. */
#define FX_FRAME_MARK_OF(w)             ((U16)(((U32)(w) >> 16) & 0xFFFFUL))
#define FX_FRAME_SEQ_OF(w)              ((U16)((U32)(w) & 0xFFFFUL))

/**
 * Pass as nExpectSeq to accept whatever sequence number arrives.
 *
 * Used for the first frame after a resync, when there is nothing to predict
 * against yet - the mark still has to be right, only the counter is adopted
 * rather than checked.
 */
#define FX_FRAME_SEQ_ANY                (0xFFFFFFFFUL)

/** Reported as nPhase when the mark could not be found anywhere in the frame. */
#define FX_FRAME_PHASE_NONE             (0xFFU)



/***************************************************************************************************
* Declarations of global (public) data types
***************************************************************************************************/

/** What went wrong at the first bad frame, if anything. */
typedef enum
{
    /** Every frame examined was where it should be. */
    FX_FRAME_OK = 0,

    /**
     * The mark was wrong. The stream is not aligned where the receiver thinks -
     * a slipped word, a restarted DMA, a transmitter underrun. nPhase says
     * where the mark actually turned up, which is the rotation in words.
     */
    FX_FRAME_FAULT_MARK = 1,

    /**
     * The mark was right but the sequence number jumped. Alignment is intact
     * and whole frames went missing; nSeqGap says how many.
     */
    FX_FRAME_FAULT_SEQ = 2
} FX_FRAME_FAULT;


/** Result of a scan - the diagnosis, not just a pass or fail. */
typedef struct
{
    /** Frames from the start that were good. Equals nFrames when all passed. */
    U16 nGoodFrames;

    /** The sequence number to expect at the start of the NEXT half. */
    U16 nNextSeq;

    /** Raw sync word of the first bad frame, for a human to look at. */
    U32 nBadWord;

    /**
     * Frames lost, when eFault is FX_FRAME_FAULT_SEQ. The arithmetic wraps, so
     * this is right across the 65536 boundary.
     */
    U16 nSeqGap;

    /**
     * Word offset at which the mark was actually found, when eFault is
     * FX_FRAME_FAULT_MARK - that is, the rotation. FX_FRAME_PHASE_NONE if the
     * mark was not in the frame at all, which points at corruption rather than
     * a slip.
     */
    U8 nPhase;

    /** What kind of fault, if any. */
    FX_FRAME_FAULT eFault;
} FX_FRAME_SCAN;



/***************************************************************************************************
* Declarations of global (public) functions
***************************************************************************************************/

/**
 * @brief   Check the sync slot of every frame in a block, stopping at the first
 *          bad one, and describe what was wrong.
 *
 * @details Reads only slot 0 of each frame - one word per FX_FRAME_SLOT_QTY -
 *          so the cost is a load and a compare per frame, not per sample.
 *
 *          Stops at the first failure ON PURPOSE. A slipped word rotates
 *          everything after it, so once one frame is wrong the rest of the
 *          block is wrong too; counting how many more would be counting the
 *          same fault repeatedly.
 *
 * @param   pWords      Start of the block, at what the caller believes is a
 *                      frame boundary. Must hold nFrames * FX_FRAME_SLOT_QTY
 *                      words.
 * @param   nFrames     Frames to examine.
 * @param   nExpectSeq  Sequence number expected at the first frame, or
 *                      FX_FRAME_SEQ_ANY to adopt whatever arrives.
 * @param   pOut        Filled with the diagnosis. Must not be NULL.
 *
 * @return  Number of leading good frames - 0 if the very first frame was bad,
 *          nFrames if the whole block passed.
 */
extern U16 FxFrame_Scan(const S32* pWords,
                        U16        nFrames,
                        U32        nExpectSeq,
                        FX_FRAME_SCAN* pOut);


#ifdef __cplusplus
}
#endif

#endif /* FX_FRAME_H */

/****************************************** end of file *******************************************/
