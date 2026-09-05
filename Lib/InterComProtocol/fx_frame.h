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
 *              slot  0      1 2 3 4     5 6 7 8      9       10 ......... 31
 *                  [SYNC]  [R0..R3]   [V0..V3]   [STAT]   [F0] ..... [F21]
 *                    |         |          |         |         |
 *                    |         |          |         |         +- loop FILE, 22
 *                    |         |          |         |            opaque bytes,
 *                    |         |          |         |            4 per slot
 *                    |         |          |         +- one looper per frame,
 *                    |         |          |            alternating
 *                    |         |          +- 4 LIVE looper planes, always on
 *                    |         +- 4 recorder planes
 *                    +- mark:16 | seq:16
 *
 *            32 slots x 4 bytes = 128 bytes per frame, at 48 kHz:
 *
 *                6.144 MB/s on the wire, 51.2% of the 96 MHz link
 *                4096-word half / 32 = 128 frames, exactly - see below
 *
 *            EVERY SLOT NOW HAS A JOB. The live planes exist because a loop
 *            being recorded lives only in audio SDRAM, so the interface - which
 *            owns the display, the card and the looper SDRAM - had no way to
 *            draw its waveform. Streaming it continuously costs four slots and
 *            removes the need to ship the audio a second time just to see it.
 *
 *            The file run pays for them: 27 -> 22 slots, so a full-length take
 *            crosses in 1.364 s instead of 1.111 s. That is the whole cost.
 *
 *            THE WIDTH NEVER CHANGES. It is 32 whether or not a loop transfer is
 *            running; the file slots simply carry zeros when idle. That costs
 *            4.2 MB/s of zeros at idle and is worth every byte, because the
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

/** Recorder planes follow the sync slot. One 24-bit sample per slot. */
#define FX_FRAME_REC_SLOT_BASE          (1U)
#define FX_FRAME_REC_SLOT_QTY           (4U)

/**
 * LIVE LOOPER PLANES - the same shape as the recorder's, and always present.
 *
 * The recorder and the looper are separate chain blocks and can sit at
 * different points, so these carry genuinely different audio from slots 1..4 -
 * a looper at the head of a chain and a recorder at its tail see different
 * signal, and no wire arrangement could share them.
 *
 * Positional like the recorder's, NOT the opaque byte stream the file slots
 * use: the receiver has to separate loopers by hardware, and only a positional
 * layout lets the MDMA do it without a CPU pass.
 */
#define FX_FRAME_LIVE_SLOT_BASE         (FX_FRAME_REC_SLOT_BASE + FX_FRAME_REC_SLOT_QTY)
#define FX_FRAME_LIVE_SLOT_QTY          (4U)

/**
 * LOOP STATUS - one looper described per frame, alternating.
 *
 * Without this the live planes above are unusable. The take is a ring at a
 * moving head, so a live sample carries no inherent position; the only position
 * on the wire was PROTO_TELEMETRY.aLoopPos, which is 40 ms coarse and
 * last-writer-wins between the two chains of a pair. And an UNDO or a CLEAR on
 * the audio side rewrites the take with nothing marking the moment, so a
 * receiver would keep drawing peaks that are no longer true.
 *
 * One slot per frame fixes both. Alternating gives each looper 24 000 updates
 * per second, which is 480 times the telemetry rate it replaces.
 */
#define FX_FRAME_STAT_SLOT              (FX_FRAME_LIVE_SLOT_BASE + FX_FRAME_LIVE_SLOT_QTY)
#define FX_FRAME_STAT_SLOT_QTY          (1U)

/**
 * The loop FILE payload occupies everything left: 22 slots.
 *
 * 22 is not what was left over - it is what makes the destination arithmetic
 * exact. The interface arms the loop destination to a whole number of route
 * steps, one step being FX_FRAME_LOOP_SLOT_QTY * 128 frames * 4 bytes, and
 * refuses a session whose rounded-up length exceeds the 5632 KiB staging slot.
 *
 *     step   22 * 128 * 4 = 11 264 = 11 * 2^10
 *     slot   5632 KiB     = 5 767 168 = 11 * 2^19
 *     slot / step         = 512 EXACTLY
 *
 * Both share the factor 11, so the worst-case round-up lands precisely on the
 * end of the buffer and NO loop length can ever be refused. At 27 it happened
 * to fit because the longest take sat under a multiple; at 23 the same
 * arithmetic put it 3 072 bytes past the end and a full-length take became
 * unsaveable. 22 removes the coincidence.
 */
#define FX_FRAME_LOOP_SLOT_BASE         (FX_FRAME_STAT_SLOT + FX_FRAME_STAT_SLOT_QTY)
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


/***************************************************************************************************
* The loop status word - slot FX_FRAME_STAT_SLOT
***************************************************************************************************/

/*
 *  31   30 ............................. 8   7 ..... 4   3 ..... 0
 * +----+-----------------------------------+-----------+-----------+
 * | sel|            pos : 23                | gen : 4   | state : 4 |
 * +----+-----------------------------------+-----------+-----------+
 *
 * sel    which looper this frame describes. Explicit rather than derived from
 *        the sequence number's parity: a dropped block advances nSeq by a whole
 *        block, so parity survives today, but nothing should depend on that.
 * pos    playhead in frames. 8 388 608 against 960 000 for a 20 s loop - 8.7x
 *        headroom, so a longer LOOP_MAX_SEC does not silently wrap it.
 * gen    bumped on CLEAR, UNDO, or any length change. The receiver compares it
 *        with the last one it saw and throws away everything it had drawn when
 *        it differs. Four bits wrap after 16 events, which cannot be missed at
 *        24 000 samples per second per looper.
 * state  transport state, PROTO_TRANSPORT_ACT values.
 */

#define FX_FRAME_STAT_SEL_SHIFT         (31U)
#define FX_FRAME_STAT_POS_SHIFT         (8U)
#define FX_FRAME_STAT_POS_MASK          (0x7FFFFFUL)
#define FX_FRAME_STAT_GEN_SHIFT         (4U)
#define FX_FRAME_STAT_GEN_MASK          (0xFUL)
#define FX_FRAME_STAT_STATE_MASK        (0xFUL)

/** Largest playhead the status word can carry, in frames. */
#define FX_FRAME_STAT_POS_MAX           (FX_FRAME_STAT_POS_MASK)

/** Build the status word for one looper. */
#define FX_FRAME_STAT_WORD(sel, pos, gen, state)                                            \
            ((S32)(U32)((((U32)(sel) & 1UL) << FX_FRAME_STAT_SEL_SHIFT)                     \
                      | (((U32)(pos) & FX_FRAME_STAT_POS_MASK) << FX_FRAME_STAT_POS_SHIFT)  \
                      | (((U32)(gen) & FX_FRAME_STAT_GEN_MASK) << FX_FRAME_STAT_GEN_SHIFT)  \
                      | ((U32)(state) & FX_FRAME_STAT_STATE_MASK)))

/** Take the fields back out. */
#define FX_FRAME_STAT_SEL_OF(w)         ((U8)(((U32)(w) >> FX_FRAME_STAT_SEL_SHIFT) & 1UL))
#define FX_FRAME_STAT_POS_OF(w)         ((U32)(((U32)(w) >> FX_FRAME_STAT_POS_SHIFT)        \
                                               & FX_FRAME_STAT_POS_MASK))
#define FX_FRAME_STAT_GEN_OF(w)         ((U8)(((U32)(w) >> FX_FRAME_STAT_GEN_SHIFT)         \
                                              & FX_FRAME_STAT_GEN_MASK))
#define FX_FRAME_STAT_STATE_OF(w)       ((U8)((U32)(w) & FX_FRAME_STAT_STATE_MASK))

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
