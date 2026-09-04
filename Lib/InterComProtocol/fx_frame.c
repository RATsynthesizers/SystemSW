/**
 * @file      fx_frame.c
 *
 * @details   Sync-slot validation for the fixed 32-slot wire frame.
 *
 *            See fx_frame.h for what the sync slot is for. This file is the
 *            receiver's half of that contract, and it is deliberately small:
 *            one load and one compare per frame, reading slot 0 and nothing
 *            else.
 *
 *            It lives in the shared library rather than in the interface
 *            controller's ISR because it is arithmetic, and arithmetic that
 *            decides whether audio is kept or discarded should be testable on a
 *            PC. The failure this guards against does not crash - it produces
 *            plausible audio in the wrong channel - so the only way to know the
 *            check works is to feed it a rotated buffer and watch it say so.
 *
 * @copyright RAT Synthesizers
 */



/***************************************************************************************************
* Module includes
***************************************************************************************************/

#include "fx_frame.h"



/***************************************************************************************************
* Declarations of local (private) functions
***************************************************************************************************/

static U8 FindMarkPhase(const S32* pWords, U32 nWordsAvail);



/***************************************************************************************************
* Definitions of global (public) functions
***************************************************************************************************/

U16 FxFrame_Scan(const S32* const pWords,
                 const U16        nFrames,
                 const U32        nExpectSeq,
                 FX_FRAME_SCAN* const pOut)
{
    U32 nSeq = nExpectSeq;
    U16 i;

    if (pOut == NULL_PTR)
    {
        return 0U;
    }

    /* Cleared up front so every early return leaves a defined diagnosis rather
       than whatever the caller's stack happened to hold. */
    pOut->nGoodFrames = 0U;
    pOut->nNextSeq    = 0U;
    pOut->nBadWord    = 0UL;
    pOut->nSeqGap     = 0U;
    pOut->nPhase      = (U8)FX_FRAME_PHASE_NONE;
    pOut->eFault      = FX_FRAME_OK;

    if ((pWords == NULL_PTR) || (nFrames == 0U))
    {
        return 0U;
    }

    for (i = 0U; i < nFrames; i++)
    {
        const U32 nAt   = (U32)i * (U32)FX_FRAME_SLOT_QTY;
        const S32 nWord = pWords[nAt];
        const U16 nMark = FX_FRAME_MARK_OF(nWord);
        const U16 nGot  = FX_FRAME_SEQ_OF(nWord);

        /*
         * The mark first. A wrong mark means the stream is not where we think
         * it is, and the sequence number read out of a misaligned word is
         * meaningless - checking it would only produce a second, misleading
         * fault code for the same event.
         */
        if (nMark != (U16)FX_FRAME_SYNC_MARK)
        {
            pOut->eFault   = FX_FRAME_FAULT_MARK;
            pOut->nBadWord = (U32)nWord;

            /* Where the mark actually is, which is the rotation in words. This
               is the number that says WHICH fault happened: 1 is a single
               slipped word, and anything else points elsewhere. */
            pOut->nPhase = FindMarkPhase(&pWords[nAt],
                                         (U32)(nFrames - i) * (U32)FX_FRAME_SLOT_QTY);
            break;
        }

        /*
         * FX_FRAME_SEQ_ANY only applies to the FIRST frame examined - after
         * that nSeq holds a real number, so continuity within the block is
         * always checked even when the caller had nothing to predict against.
         */
        if (nSeq != FX_FRAME_SEQ_ANY)
        {
            if (nGot != (U16)nSeq)
            {
                pOut->eFault   = FX_FRAME_FAULT_SEQ;
                pOut->nBadWord = (U32)nWord;

                /* Unsigned wrap gives the right answer across 65535 -> 0. */
                pOut->nSeqGap = (U16)(nGot - (U16)nSeq);

                /* Alignment is intact; frames went missing. Saying phase 0
                   here distinguishes this from a rotation at a glance. */
                pOut->nPhase = 0U;
                break;
            }
        }

        nSeq = (U32)(U16)(nGot + 1U);

        pOut->nGoodFrames = (U16)(i + 1U);
        pOut->nNextSeq    = (U16)nSeq;
    }

    return pOut->nGoodFrames;
}



/***************************************************************************************************
* Definitions of local (private) functions
***************************************************************************************************/

/**
 * @brief   Find how far off the frame boundary the mark actually is.
 *
 * @details Searches from the word AFTER the one that failed - index 0 is the
 *          failure itself, so including it would always answer zero.
 *
 *          Bounded to one frame. Past that the answer stops being a rotation
 *          and starts being a coincidence: every slot beyond 31 words is
 *          another frame's data, and finding the mark there says nothing about
 *          how this stream is misaligned.
 *
 * @return  Word offset of the mark, or FX_FRAME_PHASE_NONE if it is not in
 *          this frame at all - which points at corrupted data rather than a
 *          slipped word, and is worth telling apart.
 */
static U8 FindMarkPhase(const S32* const pWords, const U32 nWordsAvail)
{
    U32 nMax = nWordsAvail;
    U32 k;

    if (nMax > (U32)FX_FRAME_SLOT_QTY)
    {
        nMax = (U32)FX_FRAME_SLOT_QTY;
    }

    for (k = 1UL; k < nMax; k++)
    {
        if (FX_FRAME_MARK_OF(pWords[k]) == (U16)FX_FRAME_SYNC_MARK)
        {
            return (U8)k;
        }
    }

    return (U8)FX_FRAME_PHASE_NONE;
}

/****************************************** end of file *******************************************/
