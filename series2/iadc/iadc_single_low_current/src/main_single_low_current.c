/***************************************************************************//**
 * @file main_single_low_current.c
 * @brief Uses the internal IADC timer to trigger a single-ended conversion,
 * which in turn triggers the LDMA to transfer the IADC measurement to memory,
 * all while remaining in EM2. After NUM_SAMPLES conversions the LDMA will
 * trigger an interrupt from EM2 and turn on LED1 on the WSTK.
 * This example is meant to be profiled using Energy Profiler to observe lower
 * current draw in EM2.
 *******************************************************************************
 * # License
 * <b>Copyright 2020 Silicon Laboratories Inc. www.silabs.com</b>
 *******************************************************************************
 *
 * SPDX-License-Identifier: Zlib
 *
 * The licensor of this software is Silicon Laboratories Inc.
 *
 * This software is provided 'as-is', without any express or implied
 * warranty. In no event will the authors be held liable for any damages
 * arising from the use of this software.
 *
 * Permission is granted to anyone to use this software for any purpose,
 * including commercial applications, and to alter it and redistribute it
 * freely, subject to the following restrictions:
 *
 * 1. The origin of this software must not be misrepresented; you must not
 *    claim that you wrote the original software. If you use this software
 *    in a product, an acknowledgment in the product documentation would be
 *    appreciated but is not required.
 * 2. Altered source versions must be plainly marked as such, and must not be
 *    misrepresented as being the original software.
 * 3. This notice may not be removed or altered from any source distribution.
 *
 *******************************************************************************
 * # Evaluation Quality
 * This code has been minimally tested to ensure that it builds and is suitable 
 * as a demonstration for evaluation purposes only. This code will be maintained
 * at the sole discretion of Silicon Labs.
 ******************************************************************************/

/*
This project demonstrates one of the lowest current consumption configurations
for the IADC peripheral. The IADC is configured to utilize the internal IADC timer
to trigger a single single-ended conversion. Upon completion, an LDMA data transfer 
occurs storing the raw data to memory. Both the IADC and LDMA run in EM2, and this
project goes a step further to shutdown onboard SPI FLASH on the WSTK, along with 
changing the system clock from FSRCO. The RTCC peripheral is also included in this 
example and run from the LFRCO. This mode of operation is documented in the EFR32xG21
datasheet as having a typical current consumption while retaining RAM of approximately
5uA. This approximate current value does not include additional current for the clock 
trees and IADC. This project is intended to be profiled with Simplicity Studio's Energy
Profiler to observe the current consumption while in EM2.

How To Test:
1.  Update the kit's firmware from the Simplicity Launcher (if necessary)
2.  Build the project and download to the Starter Kit
3.  Open Simplicity Studio and right click on the main project directory in Project Explorer
4.  In the drop-down menu, select "Profile As Simplicity Energy Profiler Target"
5.  The project will compile and load and start-up in EM0 awaiting user input to proceed
6.  Observe the current consumption in Energy Profiler of the kit in EM0 - typically ~1.5mA
6.  Press and release PB0 on the WSTK to initiate the IADC and LDMA and enter EM2
7.  Observe in Energy Profiler the drop in current consumption while in EM2; for this 
    example's configuration (100 samples/second) the EM2 current should drop to ~45uA
    *There is some part-to-part/board-to-board variance; this is a rough typical current*	
8.  After completing NUM_SAMPLES conversions, the LDMA will wake the MCU from EM2 and 
    turn on LED1 to indicate transfer completion
9.  Again, observe in Energy Profiler the rise in current consumption when returning to EM0,
    now with the additional current sourcing the LED
10. Energy Profiler can at this point be paused and the data saved. UG343: Multi-Node Energy
    Profiler User's Guide and Simplicity Studio's built-in help menu contain additional
    information to help analyze power consumption and profile energy usage.	

Users can experiment with various IADC clock frequencies and timer cycles to determine
optimum settings for desired sampling rates.

Peripherals Used:
CLK_CMU_ADC  - 1 MHz HFRCOEM23 clock for Series 2
CLK_SRC_ADC  - 1 MHz for Series 2
CLK_ADC      - 1 MHz for Series 2
IADC         - 12-bit resolution, Automatic Two's Complement (single-ended = unipolar) 
               unbuffered 3.3V (AVDD) IADC voltage reference
               IADC and reference kept in normal mode, requiring a warm-up period prior
			   to each conversion
			 - Conversions triggered by IADC timer
LDMA         - 32-bit transfer from IADC to buffer, loop continuously until NUM_SAMPLES
               are captured triggering LDMA interrupt which halts IADC scan
RTCC         - 32768 Hz LFRCO clock source, 256 prescaler 
               
Board:  Silicon Labs EFR32xG21 Radio Board (BRD4181A) + 
        Wireless Starter Kit Mainboard
Device: EFR32MG21A010F1024IM32
PB01 - GPIO Push/Pull output, Expansion Header Pin 13, WSTK Pin 10, LED1
PC04 - IADC input, single-ended, J102 of BRD4001, Pin 25
*/
 
#include <stdio.h>
#include "em_device.h"
#include "em_chip.h"
#include "em_cmu.h"
#include "em_emu.h"
#include "em_rtcc.h"
#include "em_iadc.h"
#include "em_ldma.h"
#include "em_gpio.h"
#include "em_prs.h"
#include "bsp.h"

/*******************************************************************************
 *******************************   DEFINES   ***********************************
 ******************************************************************************/

// How many samples to capture
#define NUM_SAMPLES               1024

// Set HFRCOEM23 to lowest frequency (1MHz)
#define HFRCOEM23_FREQ            cmuHFRCOEM23Freq_1M0Hz

// Set CLK_ADC to 1 MHz (set to max for shortest IADC conversion/power-up time)
#define CLK_SRC_ADC_FREQ          1000000 // CLK_SRC_ADC
#define CLK_ADC_FREQ              1000000 // CLK_ADC

// Set IADC timer cycles
#define TIMER_CYCLES              10000  // 10000 => 100   samples/second
                                         // 1000  => 1000  samples/second
                                         // 200   => 5000  samples/second
                                         // 100   => 10000 samples/second
                                         // 40    => 25000 samples/second

// When changing GPIO port/pins above, make sure to change xBUSALLOC macro's
// accordingly.
#define IADC_INPUT_0_BUS          CDBUSALLOC
#define IADC_INPUT_0_BUSALLOC     GPIO_CDBUSALLOC_CDEVEN0_ADC0

// Push-buttons are active-low
#define PB_PRESSED (0)

/*******************************************************************************
 ***************************   GLOBAL VARIABLES   *******************************
 ******************************************************************************/

// Globally declared LDMA link descriptor
LDMA_Descriptor_t descriptor;

// buffer to store IADC samples
uint32_t singleBuffer[NUM_SAMPLES];

/**************************************************************************//**
 * @brief  IADC Initializer
 *****************************************************************************/
void initIADC (void)
{
  // Declare init structs
  IADC_Init_t init = IADC_INIT_DEFAULT;
  IADC_AllConfigs_t initAllConfigs = IADC_ALLCONFIGS_DEFAULT;
  IADC_InitSingle_t initSingle = IADC_INITSINGLE_DEFAULT;
  IADC_SingleInput_t initSingleInput = IADC_SINGLEINPUT_DEFAULT;

  // Reset IADC to reset configuration in case it has been modified
  IADC_reset(IADC0);

  // Configure IADC clock source for use while in EM2
  CMU_ClockSelectSet(cmuClock_IADCCLK, cmuSelect_HFRCOEM23); // 1MHz

  // Modify init structs and initialize
  init.warmup = iadcWarmupNormal;

  // Set the HFSCLK prescale value here
  init.srcClkPrescale = IADC_calcSrcClkPrescale(IADC0, CLK_SRC_ADC_FREQ, 0);

  // Set timer cycles to configure sampling rate
  init.timerCycles = TIMER_CYCLES;

  // Configuration 0 is used by both scan and single conversions by default
  // Use unbuffered AVDD as reference
  initAllConfigs.configs[0].reference = iadcCfgReferenceVddx;

  // Divides CLK_SRC_ADC to set the CLK_ADC frequency
  // Default oversampling (OSR) is 2x, and Conversion Time = ((4 * OSR) + 2) / fCLK_ADC
  initAllConfigs.configs[0].adcClkPrescale = IADC_calcAdcClkPrescale(IADC0,
                                                                    CLK_ADC_FREQ,
                                                                    0,
                                                                    iadcCfgModeNormal,
                                                                    init.srcClkPrescale);

  // Single initialization
  initSingle.triggerSelect = iadcTriggerSelTimer;
  initSingle.dataValidLevel = _IADC_SINGLEFIFOCFG_DVL_VALID1;

  // Enable triggering of single conversion
  initSingle.start = true;

  // Set to run in EM2
  initSingle.fifoDmaWakeup = true;

  // Configure Input sources for single ended conversion
  initSingleInput.posInput = iadcPosInputPortCPin4;
  initSingleInput.negInput = iadcNegInputGnd;

  // Initialize IADC
  IADC_init(IADC0, &init, &initAllConfigs);

  // Initialize Single
  IADC_initSingle(IADC0, &initSingle, &initSingleInput);
}

/**************************************************************************//**
 * @brief
 *   IADC Initializer
 *
 * @param[in] buffer
 *   pointer to the array where ADC data will be stored.
 * @param[in] size
 *   size of the array
 *****************************************************************************/
void initLDMA(uint32_t *buffer, uint32_t size)
{
  // Declare LDMA init structs
  LDMA_Init_t init = LDMA_INIT_DEFAULT;

  // Configure LDMA for transfer from IADC to memory
  // LDMA will loop continuously
  LDMA_TransferCfg_t transferCfg =
    LDMA_TRANSFER_CFG_PERIPHERAL(ldmaPeripheralSignal_IADC0_IADC_SINGLE);

  // Set up descriptors for dual buffer transfer
  LDMA_Descriptor_t xfer = LDMA_DESCRIPTOR_LINKREL_P2M_BYTE(&IADC0->SINGLEFIFODATA, buffer, size, 1);

  // Store descriptors globally
  // Note that this is required for the LDMA to function properly
  descriptor = xfer;

  // Modify descriptor for NUM_SAMPLES sized transfer from iadc to buffer
  // Transfer 32 bits per unit, increment by 32 bits
  descriptor.xfer.size = ldmaCtrlSizeWord;

  // Set descriptor to loop NUM_SAMPLES times and then complete
  descriptor.xfer.decLoopCnt = 1;
  descriptor.xfer.xferCnt = NUM_SAMPLES;

  // Interrupt after transfer complete
  descriptor.xfer.doneIfs = 1;
  descriptor.xfer.ignoreSrec = 0;

  // Initialize LDMA with default configuration
  LDMA_Init(&init);

  // Start transfer, LDMA will sample the IADC NUM_SAMPLES time, and then interrupt
  LDMA_StartTransfer(0, (void*)&transferCfg, (void*)&descriptor);
}

/**************************************************************************//**
 * @brief  LDMA Handler
 *****************************************************************************/
void LDMA_IRQHandler(void)
{
  // Clear interrupt flags
  LDMA_IntClear(LDMA_IF_DONE0);

  // Stop the IADC
  IADC_command(IADC0, iadcCmdStopSingle);

  // Set GPIO to notify that transfer is complete
  GPIO_PinOutSet(gpioPortB, 1);
}

/***************************************************************************//**
 * @brief   Disable high frequency clocks
 ******************************************************************************/
static void disableHFClocks(void)
{
  // Make sure all high frequency peripherals are disabled
  USART0->EN_CLR = 0x1;
  USART1->EN_CLR = 0x1;
  USART2->EN_CLR = 0x1;
  TIMER0->EN_CLR = 0x1;
  TIMER1->EN_CLR = 0x1;
  TIMER2->EN_CLR = 0x1;
  TIMER3->EN_CLR = 0x1;
  ACMP0->EN_CLR = 0x1;
  ACMP1->EN_CLR = 0x1;
  I2C0->EN_CLR = 0x1;
  I2C1->EN_CLR = 0x1;
  GPCRC->EN_CLR = 0x1;

  CMU_ClockSelectSet(cmuClock_SYSCLK, cmuSelect_FSRCO);

  // Check that HFRCODPLL and HFXO are not requested
  while (((HFRCO0->STATUS & _HFRCO_STATUS_ENS_MASK) != 0U)
         || ((HFXO0->STATUS & _HFXO_STATUS_ENS_MASK) != 0U));
}

/***************************************************************************//**
 * @brief   Disable low frequency clocks
 ******************************************************************************/
static void disableLFClocks(void)
{
  // Make sure all low frequency peripherals are disabled
  RTCC->EN_CLR = 0x1;
  WDOG0->EN_CLR = 0x1;
  WDOG1->EN_CLR = 0x1;
  LETIMER0->EN_CLR = 0x1;
  BURTC->EN_CLR = 0x1;

  // Check that all low frequency oscillators are stopped
  while ((LFRCO->STATUS != 0U) && (LFXO->STATUS != 0U));
}

/***************************************************************************//**
 * @brief   Disable all clocks to achieve lowest current consumption numbers.
 ******************************************************************************/
static void disableClocks(void)
{
  // Disable High Frequency Clocks
  disableHFClocks();

  // Disable Low Frequency Clocks
  disableLFClocks();
}

/***************************************************************************//**
 * @brief
 *   Enter EM2 with RTCC running on a low frequency oscillator.
 *
 * @param[in] osc
 *   Oscillator to run RTCC from (LFXO or LFRCO).
 * @param[in] powerdownRam
 *   Power down all RAM except the first 16 kB block or retain full RAM.
 *
 * @details
 *   Parameter:
 *     EM2. Deep Sleep Mode.@n
 *   Condition:
 *     RTCC, 32.768 kHz LFXO or LFRCO.@n
 *
 * @note
 *   To better understand disabling clocks and oscillators for specific modes,
 *   see Reference Manual section EMU-Energy Management Unit and Table 9.2.
 ******************************************************************************/
void em_EM2_RTCC(CMU_Select_TypeDef osc, bool powerdownRam)
{
  // Make sure clocks are disabled.
  disableClocks();

  // Route desired oscillator to RTCC clock tree.
  CMU_ClockSelectSet(cmuClock_RTCCCLK, osc);

  // Setup RTC parameters
  RTCC_Init_TypeDef rtccInit = RTCC_INIT_DEFAULT;
  rtccInit.presc = rtccCntPresc_256;
  rtccInit.cntWrapOnCCV1 = true;

  // Initialize RTCC. Configure RTCC with prescaler 256.
  RTCC_Init(&rtccInit);

  // Power down all RAM blocks except block 1
  if (powerdownRam)
    EMU_RamPowerDown(SRAM_BASE, 0);

  // Enter EM2.
  EMU_EnterEM2(true);
}

/**************************************************************************//**
 * @brief  Main function
 *****************************************************************************/
int main(void)
{
  // Use default settings for EM23 and HFXO
  EMU_EM23Init_TypeDef em23Init = EMU_EM23INIT_DEFAULT;
  CMU_HFXOInit_TypeDef hfxoInit = CMU_HFXOINIT_WSTK_DEFAULT;

  CHIP_Init();

  // Configure push button PB0 as a user input; will use as a toggle to indicate when inputs are ready
  GPIO_PinModeSet(gpioPortD, 2, gpioModeInputPullFilter, 1);

  // Debugging Catch; wait here before proceeding into EM2
  // Allows Energy Profiler to start up and show prior EM0 current
  while(GPIO_PinInGet(gpioPortD, 2) != PB_PRESSED); //user feedback
  while(GPIO_PinInGet(gpioPortD, 2) == PB_PRESSED); //make it a toggle

  // Turn off GPIO input
  GPIO_PinModeSet(gpioPortD, 2, gpioModeDisabled, 1);

  // Configure PB1 as output, will be set when LDMA transfer completes
  GPIO_PinModeSet(gpioPortB, 1, gpioModePushPull, 0);

  // Initialize EM2/EM3/EM4 with default parameters
  EMU_EM23Init(&em23Init);
  CMU_HFXOInit(&hfxoInit);

  // Set clock frequency to defined value
  CMU_HFRCOEM23BandSet(HFRCOEM23_FREQ);

  // Initialize the IADC
  initIADC();

  // Initialize LDMA
  initLDMA(singleBuffer, NUM_SAMPLES);

  // IADC single already enabled; must enable timer block in order to trigger
  IADC_command(IADC0, iadcCmdEnableTimer);

  // Sleep CPU until LDMA transfer completes
  // EM2 with RTCC running off LFRCO is a documented current mode in the DS
  em_EM2_RTCC(cmuSelect_LFRCO, false);

  // Infinite loop
  while(1);
}
