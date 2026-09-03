/**
 * @file      spi_tp.c
 *
 * @details   SPI bulk transport. See spi_tp.h.
 *
 * @copyright RAT Synthesizers
 */



/***************************************************************************************************
* Module includes
***************************************************************************************************/

#include "spi_tp.h"

#if (SPI_TP_IN_USE == ON)

#include <string.h>

// Get the CubeMX SPI handles
#include "spi.h"



/***************************************************************************************************
* Definitions of local (private) constants
***************************************************************************************************/

/* Resolve the configured peripheral to its CubeMX handle at compile time, so
   an instance the project has not generated fails here by name rather than at
   link time on a missing symbol. */
#if   (SPI_TP_INSTANCE == SPI_TP_SPI1)
    #define SPI_TP_HANDLE               (hspi1)
#elif (SPI_TP_INSTANCE == SPI_TP_SPI2)
    #define SPI_TP_HANDLE               (hspi2)
#elif (SPI_TP_INSTANCE == SPI_TP_SPI3)
    #define SPI_TP_HANDLE               (hspi3)
#elif (SPI_TP_INSTANCE == SPI_TP_SPI4)
    #define SPI_TP_HANDLE               (hspi4)
#elif (SPI_TP_INSTANCE == SPI_TP_SPI5)
    #define SPI_TP_HANDLE               (hspi5)
#elif (SPI_TP_INSTANCE == SPI_TP_SPI6)
    #define SPI_TP_HANDLE               (hspi6)
#else
    #error "spi_tp_cfg.h: SPI_TP_INSTANCE is not one of the SPI_TP_* selectors"
#endif


/*
 * ======== CLOCK ==============================
 *
 * THE TWO ROLES CONSTRAIN THE CLOCK DIFFERENTLY, and conflating them is a trap
 * worth spelling out: a master DIVIDES its kernel down to SCK, while a slave
 * takes SCK off the wire and only has to be able to SAMPLE it. So the
 * power-of-two rule below applies to the master alone - requiring it of a slave
 * would reject perfectly good configurations, and the natural slave setting
 * (kernel 2.5x SCK, for sampling margin) is one of them.
 */
#if (SPI_TP_ROLE == SPI_TP_ROLE_MASTER)

    /*
     * SCK = kernel / 2^(MBR+1), so the divider is a power of two from 2 to 256
     * and nothing in between. Asking for a clock that is not kernel/2^n is not
     * a rounding matter - it is a different clock - so the ratio is checked
     * exactly and a build that cannot produce it fails here naming both
     * numbers.
     *
     * Getting this wrong is otherwise close to invisible: a stream running at
     * half the intended rate still carries plausible data, just not enough of
     * it, and the symptom is dropped frames somewhere else entirely.
     */
    #define SPI_TP_DIVIDER              (SPI_TP_KERNEL_HZ / SPI_TP_SCK_HZ)

    #if ((SPI_TP_KERNEL_HZ % SPI_TP_SCK_HZ) != 0UL)
        #error "spi_tp_cfg.h: SPI_TP_KERNEL_HZ is not a whole multiple of SPI_TP_SCK_HZ"
    #endif

    #if   (SPI_TP_DIVIDER == 2UL)
        #define SPI_TP_PRESCALER        SPI_BAUDRATEPRESCALER_2
    #elif (SPI_TP_DIVIDER == 4UL)
        #define SPI_TP_PRESCALER        SPI_BAUDRATEPRESCALER_4
    #elif (SPI_TP_DIVIDER == 8UL)
        #define SPI_TP_PRESCALER        SPI_BAUDRATEPRESCALER_8
    #elif (SPI_TP_DIVIDER == 16UL)
        #define SPI_TP_PRESCALER        SPI_BAUDRATEPRESCALER_16
    #elif (SPI_TP_DIVIDER == 32UL)
        #define SPI_TP_PRESCALER        SPI_BAUDRATEPRESCALER_32
    #elif (SPI_TP_DIVIDER == 64UL)
        #define SPI_TP_PRESCALER        SPI_BAUDRATEPRESCALER_64
    #elif (SPI_TP_DIVIDER == 128UL)
        #define SPI_TP_PRESCALER        SPI_BAUDRATEPRESCALER_128
    #elif (SPI_TP_DIVIDER == 256UL)
        #define SPI_TP_PRESCALER        SPI_BAUDRATEPRESCALER_256
    #else
        #error "spi_tp_cfg.h: SPI_TP_SCK_HZ is not SPI_TP_KERNEL_HZ / 2^n for n in 1..8"
    #endif

#else

    /*
     * A slave resynchronises the incoming clock into its own kernel domain, so
     * the kernel must be at least twice the bit clock - and wants more. At
     * exactly 2x there is no margin left for cable delay or the master's
     * clock-to-out, and the failure is silent corruption of a stream with no
     * CRC to catch it.
     *
     * No power-of-two requirement: nothing here divides anything.
     */
    #if ((SPI_TP_KERNEL_HZ / SPI_TP_SCK_HZ) < 2UL)
        #error "spi_tp_cfg.h: a slave needs SPI_TP_KERNEL_HZ >= 2 * SPI_TP_SCK_HZ"
    #endif

#endif



/***************************************************************************************************
* Definitions of local (private) variables
***************************************************************************************************/

static SPI_TP_HalfCallback  pfHalfCb  = NULL_PTR;
static SPI_TP_SentCallback  pfSentCb  = NULL_PTR;
static SPI_TP_ErrorCallback pfErrorCb = NULL_PTR;

static SPI_TP_STATS tStats;

static volatile BOOLEAN bInitDone = FALSE;
static volatile BOOLEAN bTxBusy   = FALSE;



/***************************************************************************************************
* Definitions of global (public) functions
***************************************************************************************************/

STD_RESULT SPI_TP_Init(void)
{
    (void)memset(&tStats, 0, sizeof(tStats));

    bTxBusy = FALSE;

    /*
     * The configured clock WINS over whatever CubeMX generated, for the same
     * reason the UART baud rate does: it is a property of the link that both
     * ends must agree on, and a generated file is reverted by the next
     * regeneration.
     *
     * The prescaler only exists on a master - a slave takes its clock from the
     * wire - so setting it on a slave is harmless but meaningless, and the
     * kernel-ratio check above is what matters there instead.
     */
#if (SPI_TP_ROLE == SPI_TP_ROLE_MASTER)
    SPI_TP_HANDLE.Init.BaudRatePrescaler = SPI_TP_PRESCALER;
#endif

    if (HAL_SPI_Init(&SPI_TP_HANDLE) != HAL_OK)
    {
        return RESULT_NOT_OK;
    }

    bInitDone = TRUE;

    return RESULT_OK;
}


STD_RESULT SPI_TP_DeInit(void)
{
    bInitDone = FALSE;

    (void)HAL_SPI_Abort(&SPI_TP_HANDLE);

    if (HAL_SPI_DeInit(&SPI_TP_HANDLE) != HAL_OK)
    {
        return RESULT_NOT_OK;
    }

    return RESULT_OK;
}


STD_RESULT SPI_TP_RegisterHalfCb(const SPI_TP_HalfCallback pfCb)
{
    pfHalfCb = pfCb;

    return RESULT_OK;
}


STD_RESULT SPI_TP_RegisterSentCb(const SPI_TP_SentCallback pfCb)
{
    pfSentCb = pfCb;

    return RESULT_OK;
}


STD_RESULT SPI_TP_RegisterErrorCb(const SPI_TP_ErrorCallback pfCb)
{
    pfErrorCb = pfCb;

    return RESULT_OK;
}


STD_RESULT SPI_TP_StartReceive(void* const pRing, const U16 nWords)
{
#if (SPI_TP_ROLE == SPI_TP_ROLE_SLAVE)

    if ((pRing == NULL_PTR) || (nWords == 0U))
    {
        return RESULT_INVALID_PARAM_1;
    }

    /* Odd sizes would put the half boundary between words, and the callback
       would hand out a half-filled sample. */
    if ((nWords & 1U) != 0U)
    {
        return RESULT_INVALID_PARAM_2;
    }

    if (bInitDone == FALSE)
    {
        return RESULT_NOT_INIT;
    }

    /*
     * Armed ONCE. HAL_SPI_Receive_DMA on a circular DMA channel runs forever,
     * giving a half and a full callback per lap and never a gap.
     *
     * The DMA channel must be configured CIRCULAR in CubeMX. With a normal-mode
     * channel this call still succeeds and the stream stops after one lap,
     * which looks like the master died.
     */
    if (HAL_SPI_Receive_DMA(&SPI_TP_HANDLE, (uint8_t*)pRing, nWords) != HAL_OK)
    {
        return RESULT_NOT_OK;
    }

    return RESULT_OK;

#else

    (void)pRing;
    (void)nWords;

    /* A master does not receive a stream. Refusing loudly beats silently doing
       nothing, because the caller has just handed over a ring it believes is
       being filled. */
    return RESULT_NOT_OK;

#endif
}


STD_RESULT SPI_TP_StartTransceive(void* const pRxRing,
                                  void* const pTxRing,
                                  const U16 nWords)
{
#if (SPI_TP_ROLE == SPI_TP_ROLE_SLAVE)

    if ((pRxRing == NULL_PTR) || (pTxRing == NULL_PTR) || (nWords == 0U))
    {
        return RESULT_INVALID_PARAM_1;
    }

    if ((nWords & 1U) != 0U)
    {
        return RESULT_INVALID_PARAM_3;
    }

    if (bInitDone == FALSE)
    {
        return RESULT_NOT_INIT;
    }

    /*
     * Armed once, both directions, and never re-armed - the same reasoning as
     * the receive-only case. The transmit side inherits it: a gap between
     * transfers would shift every subsequent word by however many the master
     * clocked while nothing was armed, and the far end de-interleaves by
     * position with nothing to detect that with.
     *
     * BOTH DMA channels must be CIRCULAR in CubeMX. A normal-mode transmit
     * channel is the subtle one: reception carries on looking healthy while the
     * transmit side silently stops after one lap, so the link appears to work
     * in the direction being watched.
     */
    if (HAL_SPI_TransmitReceive_DMA(&SPI_TP_HANDLE,
                                    (uint8_t*)pTxRing,
                                    (uint8_t*)pRxRing,
                                    nWords) != HAL_OK)
    {
        return RESULT_NOT_OK;
    }

    return RESULT_OK;

#else

    (void)pRxRing;
    (void)pTxRing;
    (void)nWords;

    /* A master sends frames on its own schedule; it has no continuously armed
       ring to transceive into. */
    return RESULT_NOT_OK;

#endif
}


STD_RESULT SPI_TP_SendFrame(const void* const pWords, const U16 nWords)
{
#if (SPI_TP_ROLE == SPI_TP_ROLE_MASTER)

    if ((pWords == NULL_PTR) || (nWords == 0U))
    {
        return RESULT_INVALID_PARAM_1;
    }

    if (bInitDone == FALSE)
    {
        return RESULT_NOT_INIT;
    }

    /*
     * REFUSED, NOT QUEUED, WHEN THE PREVIOUS FRAME IS STILL GOING.
     *
     * This means the frame no longer fits the producer period at this clock.
     * The caller is typically an audio interrupt, where waiting would turn one
     * late frame into a missed block - so it is told immediately and counts a
     * drop, which is the number that says "the link is too slow for this frame
     * size" out loud.
     */
    if (bTxBusy == TRUE)
    {
        tStats.nFramesDropped++;
        return RESULT_BUSY;
    }

    bTxBusy = TRUE;

    if (HAL_SPI_Transmit_DMA(&SPI_TP_HANDLE, (uint8_t*)pWords, nWords) != HAL_OK)
    {
        bTxBusy = FALSE;
        tStats.nSpiErrors++;
        return RESULT_NOT_OK;
    }

    tStats.nFramesSent++;

    return RESULT_OK;

#else

    (void)pWords;
    (void)nWords;

    return RESULT_NOT_OK;

#endif
}


BOOLEAN SPI_TP_IsBusy(void)
{
    return bTxBusy;
}


U32 SPI_TP_ActualSckHz(void)
{
#if (SPI_TP_ROLE == SPI_TP_ROLE_MASTER)
    /* What the divider really produces, not what was asked for. The two agree
       here only because the build refuses configurations where they cannot. */
    return (U32)(SPI_TP_KERNEL_HZ / SPI_TP_DIVIDER);
#else
    /* A slave has no divider - the clock arrives on the wire. This is the rate
       it has been told to expect, reported so a mismatch between the two ends
       can be seen from a diagnostic frame rather than inferred from CRC errors
       on a stream that has no CRC. */
    return (U32)SPI_TP_SCK_HZ;
#endif
}


void SPI_TP_GetStats(SPI_TP_STATS* const pStats)
{
    if (pStats != NULL_PTR)
    {
        *pStats = tStats;
    }
}



/***************************************************************************************************
* HAL callbacks
*
* Weak HAL symbols, so this module owns them for the configured peripheral.
* Every one tests the handle first rather than assuming it is the only user.
***************************************************************************************************/

void HAL_SPI_RxHalfCpltCallback(SPI_HandleTypeDef* hspi)
{
    if (hspi != &SPI_TP_HANDLE)
    {
        return;
    }

    tStats.nHalvesReceived++;

    if (pfHalfCb != NULL_PTR)
    {
        /* The FIRST half has filled, so the consumer takes that one while the
           DMA moves into the second. */
        pfHalfCb(FALSE);
    }
}


void HAL_SPI_RxCpltCallback(SPI_HandleTypeDef* hspi)
{
    if (hspi != &SPI_TP_HANDLE)
    {
        return;
    }

    tStats.nHalvesReceived++;

    if (pfHalfCb != NULL_PTR)
    {
        pfHalfCb(TRUE);
    }
}


/*
 * A full-duplex transfer raises the TxRx callbacks, NOT the Rx ones - a
 * distinction that costs an afternoon if it is missed, because reception
 * plainly works and yet nothing is ever called. Both routes lead to the same
 * place: the application only cares which half is free.
 */
void HAL_SPI_TxRxHalfCpltCallback(SPI_HandleTypeDef* hspi)
{
    if (hspi != &SPI_TP_HANDLE)
    {
        return;
    }

    tStats.nHalvesReceived++;

    if (pfHalfCb != NULL_PTR)
    {
        pfHalfCb(FALSE);
    }
}


void HAL_SPI_TxRxCpltCallback(SPI_HandleTypeDef* hspi)
{
    if (hspi != &SPI_TP_HANDLE)
    {
        return;
    }

    tStats.nHalvesReceived++;

    if (pfHalfCb != NULL_PTR)
    {
        pfHalfCb(TRUE);
    }
}


void HAL_SPI_TxCpltCallback(SPI_HandleTypeDef* hspi)
{
    if (hspi != &SPI_TP_HANDLE)
    {
        return;
    }

    bTxBusy = FALSE;

    if (pfSentCb != NULL_PTR)
    {
        pfSentCb();
    }
}


void HAL_SPI_ErrorCallback(SPI_HandleTypeDef* hspi)
{
    if (hspi != &SPI_TP_HANDLE)
    {
        return;
    }

    tStats.nSpiErrors++;

    bTxBusy = FALSE;

    /*
     * NOT re-armed here.
     *
     * A positionally framed stream that has errored has almost certainly lost
     * word alignment, and restarting the DMA would resume it rotated - reading
     * plausible data into the wrong channels for as long as it runs. Recovery
     * has to go back through the arm-then-start handshake, which only the
     * application can sequence, so this counts the error, stops, and tells it.
     */
    if (pfErrorCb != NULL_PTR)
    {
        pfErrorCb();
    }
}

#endif  // #if (SPI_TP_IN_USE == ON)

/****************************************** end of file *******************************************/
