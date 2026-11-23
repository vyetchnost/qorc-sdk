/*==========================================================
 * Copyright 2020 QuickLogic Corporation
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * You may obtain a copy of the License at
 *
 *    http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 *==========================================================*/

/*==========================================================
 *
 *    File   : main.c
 *    Purpose: main for Qomu Scheme REPL on the USB-serial console
 *                                                          
 *=========================================================*/

#include "Fw_global_config.h"   // This defines application specific charactersitics

#include <stdio.h>
#include "FreeRTOS.h"
#include "task.h"
#include "semphr.h"
#include "timers.h"
#include "RtosTask.h"

/*    Include the generic headers required for QORC */
#include "eoss3_hal_gpio.h"
#include "eoss3_hal_rtc.h"
#include "eoss3_hal_fpga_usbserial.h"
#include "ql_time.h"
#include "s3x_clock_hal.h"
#include "s3x_clock.h"
#include "s3x_pi.h"
#include "dbg_uart.h"

#include "cli.h"

#include "fpga_loader.h"    // API for loading FPGA
extern const uint32_t axFPGABitStream[]; // FPGA bitstream (const: stays in the image, not copied to RAM)
extern int      axFPGABitStream_length; // length of axFPGABitStream array in bytes

extern const struct cli_cmd_entry my_main_menu[];
extern void scheme_start_task(void);
/* Pad setup for the open-toolchain bitstream (fpga/qomu_fpga_iomux.c, generated
 * by gateware/usb2serial/build.sh). Weak: absent with the SpDE bitstream. */
extern void qomu_fpga_pads_init(void) __attribute__((weak));


const char *SOFTWARE_VERSION_STR;


/*
 * Global variable definition
 */


extern void qomu_hardwaresetup();
static void nvic_init(void);

int main(void)
{

    SOFTWARE_VERSION_STR = "qorc-sdk/qomu_apps/qomu_scheme";
    
    qomu_hardwaresetup();
    nvic_init();
    S3x_Clk_Disable(S3X_FB_21_CLK);
    S3x_Clk_Disable(S3X_FB_16_CLK);
    S3x_Clk_Enable(S3X_A1_CLK);
    S3x_Clk_Enable(S3X_CFG_DMA_A1_CLK);
    if (qomu_fpga_pads_init)
        qomu_fpga_pads_init();          // USB pads as the bitstream expects (input enabled on D+/D-)
    load_fpga(axFPGABitStream_length,(uint32_t *)axFPGABitStream);
    // Use 0x6141 as USB serial product ID (USB PID)
    HAL_usbserial_init2(false, false, 0x6141);        // Start USB serial not using interrupts
    for (volatile int i = 0; i != 4000000; i++) ;   // Give it time to enumerate
    
    dbg_str("\n\n");
    dbg_str( "##########################\n");
    dbg_str( "Quicklogic Qomu Scheme REPL\n");
    dbg_str( "SW Version: ");
    dbg_str( SOFTWARE_VERSION_STR );
    dbg_str( "\n" );
    dbg_str( __DATE__ " " __TIME__ "\n" );
    dbg_str( "##########################\n\n");
	

    scheme_start_task();    // Scheme REPL replaces the CLI on the USB-serial console
        
    /* Start the tasks and timer running. */
    vTaskStartScheduler();
    dbg_str("\n");
      
    while(1);
}

static void nvic_init(void)
 {
    // To initialize system, this interrupt should be triggered at main.
    // So, we will set its priority just before calling vTaskStartScheduler(), not the time of enabling each irq.
    NVIC_SetPriority(Ffe0_IRQn, configLIBRARY_MAX_SYSCALL_INTERRUPT_PRIORITY);
    NVIC_SetPriority(SpiMs_IRQn, configLIBRARY_MAX_SYSCALL_INTERRUPT_PRIORITY);
    NVIC_SetPriority(CfgDma_IRQn, configLIBRARY_MAX_SYSCALL_INTERRUPT_PRIORITY);
    NVIC_SetPriority(Uart_IRQn, configLIBRARY_MAX_SYSCALL_INTERRUPT_PRIORITY);
    NVIC_SetPriority(FbMsg_IRQn, configLIBRARY_MAX_SYSCALL_INTERRUPT_PRIORITY);
 }    

//needed for startup_EOSS3b.s asm file
void SystemInit(void)
{

}


