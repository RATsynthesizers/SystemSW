/**
 * @file      fx_interleave.c
 *
 * @details   Recorder stream geometry. See fx_interleave.h for the layout.
 *
 *            ############################################################
 *            #  DUPLICATED IN THE INTERFACE CONTROLLER - keep in sync.  #
 *            #  Use TestBenchmarks/sync_shared.py.                      #
 *            ############################################################
 *
 * @version   1.0.0
 *
 * @authors   Claude (design draft)
 *
 * \date      02.09.2026 - First release
 *
 * @copyright RAT Synthesizers
 */



/***************************************************************************************************
* Module includes
***************************************************************************************************/

#include "fx_interleave.h"



/***************************************************************************************************
* Definitions of global (public) functions
***************************************************************************************************/

STD_RESULT FxInterleave_Xfer(FX_IL_XFER* const pOut,
                             const U8 nSlot,
                             const U8 nSlotWidth,
                             const U8 nStreamWidth,
                             const U32 nFrames)
{
    U32 nStrideBytes;

    if (pOut == NULL_PTR)
    {
        return RESULT_INVALID_PARAM_1;
    }

    /*
     * A RECORDER chain is mono or a stereo pair, and nothing else exists - see
     * the width invariant in fx_defs.h.
     *
     * The LOOP transport is the exception, and the reason this is a range now
     * rather than "1 or 2": its slots are contiguous on the wire, so it lifts
     * the whole run as ONE transfer of up to FX_LOOP_SLOT_QTY_MAX slots. That
     * is what makes it cost one MDMA route instead of one per slot, which is
     * the only reason it fits alongside the four recorder planes.
     *
     * Zero is still refused - a transfer of nothing is a caller bug, not a
     * no-op worth computing geometry for.
     */
    if ((nSlotWidth == 0U) || (nSlotWidth > (U8)FX_IL_SLOT_WIDTH_MAX))
    {
        return RESULT_INVALID_PARAM_3;
    }

    /*
     * The frame WIDENS during a loop transfer: REC_SLOT_QTY recorder slots
     * followed by the loop run. This used to cap at REC_SLOT_QTY, which was
     * right when the recorder was all there was and would now refuse every
     * block of a transfer.
     */
    if ((nStreamWidth == 0U) || (nStreamWidth > (U8)FX_IL_STREAM_WIDTH_MAX))
    {
        return RESULT_INVALID_PARAM_4;
    }

    /*
     * THE CHECK THAT MATTERS.
     *
     * A stereo chain in the last slot, or a slot index at or past the stream
     * width, would read the next frame's samples as if they were this frame's.
     * That is not a transfer to clamp - it is a configuration that must never
     * have been ACKed, so refuse it and let the caller stop the stream rather
     * than record something plausible from the wrong channel.
     */
    if (((U32)nSlot + (U32)nSlotWidth) > (U32)nStreamWidth)
    {
        return RESULT_INVALID_PARAM_2;
    }

    if (nFrames == 0UL)
    {
        return RESULT_INVALID_PARAM_5;
    }

    nStrideBytes = (U32)nStreamWidth * FX_IL_BYTES_PER_SLOT;

    pOut->nSrcOffsetBytes = (U32)nSlot * FX_IL_BYTES_PER_SLOT;
    pOut->nBytesPerBeat   = (U32)nSlotWidth * FX_IL_BYTES_PER_SLOT;
    pOut->nSrcSkipBytes   = nStrideBytes - pOut->nBytesPerBeat;
    pOut->nBeats          = nFrames;
    pOut->nDstBytes       = nFrames * pOut->nBytesPerBeat;

    return RESULT_OK;
}

//--------------------------------------------------------------------------------------------------

U32 FxInterleave_BlockBytes(const U8 nStreamWidth, const U32 nFrames)
{
    U32 nBytes = 0UL;

    if ((nStreamWidth > 0U) && (nStreamWidth <= (U8)REC_SLOT_QTY) && (nFrames > 0UL))
    {
        nBytes = nFrames * (U32)nStreamWidth * FX_IL_BYTES_PER_SLOT;
    }

    return nBytes;
}

/****************************************** end of file *******************************************/
