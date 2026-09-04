/***************************************************************************************************
* @file     fx_loop.h
*
* @brief    Loop transport session - the state machine BOTH controllers run.
*
* ONE MACHINE, TWO BOARDS
*
* This file is compiled into both firmwares, and both drive the same struct
* through the same states with the same functions. That is the whole point.
* The alternative - an interface-side transfer manager and an audio-side one,
* each written against the other's documentation - is how the two ends come to
* disagree about how many bytes a 43-second stereo loop is, and there is no
* moment at which that disagreement announces itself. Here, a byte count is
* wrong on both boards or on neither.
*
* Everything below is pure: no HAL, no RTOS, no memory of its own, nothing that
* needs hardware to run. The platform layers own the DMA, the MDMA routing, the
* card and the looper memory; they call in here to ask what state the session
* is in and to be told when the geometry does not add up. That is also what
* lets it be exercised on the host - see HostTests/test_loop.c - which matters
* rather a lot given the audio board does not exist yet.
*
* WHAT IT DELIBERATELY DOES NOT DO
*
* It does not move bytes. FxLoop_Advance is told how many moved and keeps the
* running count and the CRC; it never touches the payload. So a session can be
* stepped through its entire lifecycle in a test with no buffers at all, and
* the sequencing rules get checked separately from the transfer.
*
***************************************************************************************************/

#ifndef FX_LOOP_H
#define FX_LOOP_H

/***************************************************************************************************
* Included header files
***************************************************************************************************/

#include "fx_defs.h"
#include "fx_protocol.h"

/* FX_FRAME_LOOP_SLOT_QTY - the loop's slots are part of the fixed frame now,
 * not something a session negotiates. */
#include "fx_frame.h"

#ifdef __cplusplus
extern "C" {
#endif

/***************************************************************************************************
* Definitions of global (public) constants
***************************************************************************************************/

/**
 * Loop slots in the frame. Not negotiated - see fx_frame.h.
 *
 * ------------------------------------------------------------------------
 * THIS IS NO LONGER A CEILING, IT IS THE LAYOUT
 * ------------------------------------------------------------------------
 *
 * The frame used to widen for the life of a transfer and narrow again after,
 * which made this a maximum a session could ask for. It does not any more. The
 * frame is FX_FRAME_SLOT_QTY slots, always, and the loop occupies
 * FX_FRAME_LOOP_SLOT_QTY of them whether or not anything is being transferred -
 * they carry zeros when idle.
 *
 * Renegotiating the width mid-stream meant both ends had to switch on precisely
 * the same frame boundary, on every save and every load. Disagree by one frame
 * and every slot is rotated permanently, with no framing in the stream to
 * reveal it. Twenty-seven fixed slots cost 5.2 Mbit/s of zeros at idle and
 * delete that failure mode outright.
 *
 * Twenty-seven because the frame is 32: one sync slot, four recorder planes,
 * and the rest is loop. 32 was chosen to divide the receiver's 4096-word half
 * exactly - 128 frames - so the half-transfer interrupt can never land inside a
 * frame:
 *
 *      total width   frames per half
 *          4            1024.0   OK   (the old recorder-only frame)
 *          8             512.0   OK
 *         16             256.0   OK
 *         20             204.8   NO   <- does not divide, and was once shipped
 *         32             128.0   OK   <- this
 *
 * At 27 slots a 5.49 MiB stereo take crosses in 1.07 s, against 2.50 s at the
 * old 12. The session still reports nSlotQty in its OPEN so a future frame can
 * change without a protocol revision, but both ends now derive it from the
 * shared frame layout rather than agreeing on it at runtime.
 */
#define FX_LOOP_SLOT_QTY_MAX            (FX_FRAME_LOOP_SLOT_QTY)

/** Bytes per sample on the wire, by PROTO_LOOP_FMT. */
#define FX_LOOP_BYTES_S24               (3UL)
#define FX_LOOP_BYTES_S32               (4UL)

/***************************************************************************************************
* Declarations of global (public) data types
***************************************************************************************************/

/**
 * @brief Session state. Wire-visible: travels in PROTO_LOOP_STAT.eState.
 *
 *      IDLE -- open --> OPENING -- stat(ok) --> READY -- start --> RUNNING
 *                                                                     |
 *                                                        all bytes moved
 *                                                                     v
 *                                                                 COMPLETE
 *
 * Any state may fall to FAILED, and FAILED and COMPLETE both return to IDLE
 * only through FxLoop_Reset - so the platform layer cannot lose a result by
 * forgetting to read it before the next session starts.
 *
 * OPENING exists only on the initiating side. The audio board goes straight
 * from IDLE to READY when it accepts an OPEN, because for it there is nothing
 * to wait for: it IS the reply.
 */
typedef enum enFX_LOOP_STATE
{
    FX_LOOP_IDLE            = 0U,
    FX_LOOP_OPENING         = 1U,   /**< OPEN sent, awaiting STAT (initiator)    */
    FX_LOOP_READY           = 2U,   /**< geometry agreed, bulk not yet moving    */
    FX_LOOP_RUNNING         = 3U,   /**< bulk in flight                          */
    FX_LOOP_COMPLETE        = 4U,   /**< every byte moved                        */
    FX_LOOP_FAILED          = 5U

} FX_LOOP_STATE;


/**
 * @brief One transfer, as both boards see it.
 *
 * nBytesMoved and nCrc are maintained by whichever side is calling
 * FxLoop_Advance - which is both of them, independently, over the same bytes.
 * Comparing the two CRCs at the end is the only check that the link actually
 * delivered what was sent; the SPI has no acknowledgement of its own and a
 * marginal lead corrupts quietly.
 */
typedef struct stFX_LOOP_SESSION
{
    U32 nBytesTotal;            /**< agreed payload size                         */
    U32 nBytesMoved;            /**< progress, 0 .. nBytesTotal                  */
    U32 nCrc;                   /**< running, over what has moved so far         */

    U8  nSession;               /**< matches PROTO_LOOP_OPEN.nSession            */
    U8  eState;                 /**< FX_LOOP_STATE                               */
    U8  eDir;                   /**< PROTO_LOOP_DIR                              */
    U8  eResult;                /**< PROTO_RESULT; why it FAILED                 */

    U8  nLooper;
    U8  nPlaneQty;
    U8  eFormat;                /**< PROTO_LOOP_FMT                              */
    U8  nSlotQty;               /**< extra stream slots granted                   */

} FX_LOOP_SESSION;

/***************************************************************************************************
* Declarations of global (public) functions
***************************************************************************************************/

/**
 * @brief Put a session back to IDLE. Safe at any point.
 */
extern void FxLoop_Reset(FX_LOOP_SESSION* const pSession);

/**
 * @brief Payload bytes for a loop of nSamples per plane.
 *
 * BOTH boards size the transfer with this function, which is the reason it
 * lives in Shared rather than being open-coded twice.
 *
 * @param nSamples   samples PER PLANE
 * @param nPlaneQty  1 or 2
 * @param eFormat    PROTO_LOOP_FMT
 * @param pnBytes    result
 *
 * @return RESULT_NOT_OK on a bad format, a plane count that is not 1 or 2, or
 *         a size that would overflow U32
 */
extern STD_RESULT FxLoop_BytesFor(const U32 nSamples,
                                  const U8  nPlaneQty,
                                  const U8  eFormat,
                                  U32* const pnBytes);

/**
 * @brief Audio side: accept or refuse an incoming PROTO_LOOP_OPEN.
 *
 * Fills pStat either way - a refusal is a reply, not a silence, because the
 * interface is holding staging memory open waiting for one.
 *
 * @param nAvailBytes  what this side can supply (SAVE) or accept (LOAD). On a
 *                     SAVE with pOpen->nBytes 0 this becomes the byte count.
 *
 * @return RESULT_OK when the session was accepted
 */
extern STD_RESULT FxLoop_Accept(FX_LOOP_SESSION* const pSession,
                                const PROTO_LOOP_OPEN* const pOpen,
                                const U32 nAvailBytes,
                                PROTO_LOOP_STAT* const pStat);

/**
 * @brief Interface side: begin a session and fill the PROTO_LOOP_OPEN to send.
 */
extern STD_RESULT FxLoop_Open(FX_LOOP_SESSION* const pSession,
                              const U8  nSessionId,
                              const U8  eDir,
                              const U8  nLooper,
                              const U8  nPlaneQty,
                              const U8  eFormat,
                              const U8  nSlotQty,
                              const U32 nBytes,
                              PROTO_LOOP_OPEN* const pOpen);

/**
 * @brief Interface side: consume the PROTO_LOOP_STAT answering an OPEN.
 *
 * A STAT whose nSession does not match is IGNORED, not failed - it is the
 * reply to a session that has already been torn down, and letting it fail the
 * current one is how a slow reply kills the transfer that replaced it.
 *
 * @return RESULT_OK when the session reached FX_LOOP_READY
 */
extern STD_RESULT FxLoop_OpenReply(FX_LOOP_SESSION* const pSession,
                                   const PROTO_LOOP_STAT* const pStat);

/**
 * @brief READY -> RUNNING. Called once the routing is armed on both sides.
 */
extern STD_RESULT FxLoop_Start(FX_LOOP_SESSION* const pSession);

/**
 * @brief Account for bytes that have moved, and fold them into the CRC.
 *
 * pData may be NULL_PTR to advance the count without a CRC - which is what the
 * side that cannot see the payload does, and what the tests use to step a
 * session without buffers.
 *
 * Advancing past nBytesTotal is a failure, not a clamp: it means the two sides
 * disagree about the size, and silently truncating would put a short loop on
 * the card and report success.
 *
 * @return RESULT_OK while the session is healthy; check eState for COMPLETE
 */
extern STD_RESULT FxLoop_Advance(FX_LOOP_SESSION* const pSession,
                                 const U8* const pData,
                                 const U32 nBytes);

/**
 * @brief Fail a session, recording why.
 */
extern void FxLoop_Abort(FX_LOOP_SESSION* const pSession, const U8 eResult);

/**
 * @brief Fill a PROTO_LOOP_STAT describing the session as it stands.
 */
extern void FxLoop_Report(const FX_LOOP_SESSION* const pSession,
                          PROTO_LOOP_STAT* const pStat);

/**
 * @brief TRUE when the bulk should be moving - i.e. the stream is widened.
 */
extern BOOLEAN FxLoop_IsStreaming(const FX_LOOP_SESSION* const pSession);

/*
 * FxLoop_StreamWidth is GONE, deliberately.
 *
 * It returned REC_SLOT_QTY when idle and a wider value while a session ran,
 * because the frame used to change size mid-stream. The frame is fixed at
 * FX_FRAME_SLOT_QTY now, so the honest version of this function would return a
 * constant - and a function called StreamWidth that returns a constant invites
 * a reader to believe the width still varies, which is the single most
 * dangerous thing to be wrong about on this link.
 *
 * Use FX_FRAME_SLOT_QTY for the width and FxLoop_IsStreaming for whether the
 * loop slots carry payload or zeros.
 */

#ifdef __cplusplus
}
#endif

#endif // #ifndef FX_LOOP_H

/****************************************** end of file *******************************************/
