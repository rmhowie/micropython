/*
 * This file is part of the Micro Python project, http://micropython.org/
 *
 * Taken from ST Cube library and heavily modified.  See below for original
 * copyright header.
 */

/**
  ******************************************************************************
  * @file    USB_Device/CDC_Standalone/Src/usbd_cdc_interface.c
  * @author  MCD Application Team
  * @version V1.0.1
  * @date    26-February-2014
  * @brief   Source file for USBD CDC interface
  ******************************************************************************
  * @attention
  *
  * <h2><center>&copy; COPYRIGHT(c) 2014 STMicroelectronics</center></h2>
  *
  * Licensed under MCD-ST Liberty SW License Agreement V2, (the "License");
  * You may not use this file except in compliance with the License.
  * You may obtain a copy of the License at:
  *
  *        http://www.st.com/software_license_agreement_liberty_v2
  *
  * Unless required by applicable law or agreed to in writing, software 
  * distributed under the License is distributed on an "AS IS" BASIS, 
  * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
  * See the License for the specific language governing permissions and
  * limitations under the License.
  *
  ******************************************************************************
  */

/* Includes ------------------------------------------------------------------*/

#include <stdbool.h>
#include <stdint.h>

#include "usbd_cdc_msc_hid.h"
#include "usbd_cdc_interface.h"
#include "pendsv.h"

#include "py/obj.h"
#include "usb.h"

// CDC control commands
#define CDC_SEND_ENCAPSULATED_COMMAND               0x00
#define CDC_GET_ENCAPSULATED_RESPONSE               0x01
#define CDC_SET_COMM_FEATURE                        0x02
#define CDC_GET_COMM_FEATURE                        0x03
#define CDC_CLEAR_COMM_FEATURE                      0x04
#define CDC_SET_LINE_CODING                         0x20
#define CDC_GET_LINE_CODING                         0x21
#define CDC_SET_CONTROL_LINE_STATE                  0x22
#define CDC_SEND_BREAK                              0x23

/* Private typedef -----------------------------------------------------------*/
/* Private define ------------------------------------------------------------*/
#define APP_RX_DATA_SIZE  512 // I think this must be at least CDC_DATA_FS_OUT_PACKET_SIZE=64 (APP_RX_DATA_SIZE was 2048)
#define APP_TX_DATA_SIZE  512 // I think this can be any value (was 2048)

#define IDE_BAUDRATE_SLOW    (921600)
#define IDE_BAUDRATE_FAST    (12000000)
/* Private macro -------------------------------------------------------------*/
/* Private variables ---------------------------------------------------------*/

static __IO uint8_t dev_is_connected = 0; // indicates if we are connected
static __IO uint8_t debug_mode = 0; 

static uint32_t last_packet = 0;

static uint32_t baudrate = 115200;
static uint32_t dbg_xfer_length=0;
extern void usbdbg_data_in(void *buffer, int length);
extern void usbdbg_data_out(void *buffer, int length);
extern void usbdbg_control(void *buffer, uint8_t brequest, uint32_t wlength);
#define DBG_MAX_PACKET  (64)
__ALIGN_BEGIN static uint8_t dbg_xfer_buffer[DBG_MAX_PACKET] __ALIGN_END;

static uint8_t UserRxBuffer[APP_RX_DATA_SIZE]; // received data from USB OUT endpoint is stored in this buffer
static uint16_t UserRxBufCur = 0; // points to next available character in UserRxBuffer
static uint16_t UserRxBufLen = 0; // counts number of valid characters in UserRxBuffer

static uint8_t UserTxBuffer[APP_TX_DATA_SIZE]; // data for USB IN endpoind is stored in this buffer
static uint16_t UserTxBufPtrIn = 0; // increment this pointer modulo APP_TX_DATA_SIZE when new data is available
static __IO uint16_t UserTxBufPtrOut = 0; // increment this pointer modulo APP_TX_DATA_SIZE when data is drained
static uint16_t UserTxBufPtrOutShadow = 0; // shadow of above
static uint8_t UserTxBufPtrWaitCount = 0; // used to implement a timeout waiting for low-level USB driver
static uint8_t UserTxNeedEmptyPacket = 0; // used to flush the USB IN endpoint if the last packet was exactly the endpoint packet size

static int user_interrupt_char = -1;
static void *user_interrupt_data = NULL;

/* Private function prototypes -----------------------------------------------*/
static int8_t CDC_Itf_Init     (void);
static int8_t CDC_Itf_DeInit   (void);
static int8_t CDC_Itf_Control  (uint8_t cmd, uint8_t* pbuf, uint16_t length);
static int8_t CDC_Itf_Receive  (uint8_t* pbuf, uint32_t *Len);
static int8_t CDC_Itf_TxSent   ();

const USBD_CDC_ItfTypeDef USBD_CDC_fops = {
    CDC_Itf_Init,
    CDC_Itf_DeInit,
    CDC_Itf_Control,
    CDC_Itf_Receive,
    CDC_Itf_TxSent
};

/* Private functions ---------------------------------------------------------*/

/**
  * @brief  CDC_Itf_Init
  *         Initializes the CDC media low layer
  * @param  None
  * @retval Result of the opeartion: USBD_OK if all operations are OK else USBD_FAIL
  */
static int8_t CDC_Itf_Init(void)
{
    /* Start Channel1 */
    //__HAL_TIM_ENABLE_IT(&TIM3_Handle, TIM_IT_UPDATE);
  
    /*##-5- Set Application Buffers ############################################*/
    USBD_CDC_SetTxBuffer(&hUSBDDevice, UserTxBuffer, 0);
    USBD_CDC_SetRxBuffer(&hUSBDDevice, UserRxBuffer);

    UserRxBufCur = 0;
    UserRxBufLen = 0;
  
    /* NOTE: we cannot reset these here, because USBD_CDC_SetInterrupt
     * may be called before this init function to set these values.
     * This can happen if the USB enumeration occurs after the call to
     * USBD_CDC_SetInterrupt.
    user_interrupt_char = -1;
    user_interrupt_data = NULL;
    */

    return (USBD_OK);
}

/**
  * @brief  CDC_Itf_DeInit
  *         DeInitializes the CDC media low layer
  * @param  None
  * @retval Result of the opeartion: USBD_OK if all operations are OK else USBD_FAIL
  */
static int8_t CDC_Itf_DeInit(void)
{
#if 0
  /* DeInitialize the UART peripheral */
  if(HAL_UART_DeInit(&UartHandle) != HAL_OK)
  {
    /* Initialization Error */
  }
#endif
  return (USBD_OK);
}

/**
  * @brief  CDC_Itf_Control
  *         Manage the CDC class requests
  * @param  Cmd: Command code            
  * @param  Buf: Buffer containing command data (request parameters)
  * @param  Len: Number of data to be sent (in bytes)
  * @retval Result of the opeartion: USBD_OK if all operations are OK else USBD_FAIL
  */
static int8_t CDC_Itf_Control(uint8_t cmd, uint8_t* pbuf, uint16_t length) {
    switch (cmd) {
        case CDC_SEND_ENCAPSULATED_COMMAND:
            /* Add your code here */
            break;

        case CDC_GET_ENCAPSULATED_RESPONSE:
            /* Add your code here */
            break;

        case CDC_SET_COMM_FEATURE:
            /* Add your code here */
            break;

        case CDC_GET_COMM_FEATURE:
            /* Add your code here */
            break;

        case CDC_CLEAR_COMM_FEATURE:
            /* Add your code here */
            break;

        case CDC_SET_LINE_CODING: {
            baudrate = *((uint32_t*)pbuf);
            // The slow baudrate can be used on OSs that don't support custom baudrates
            if (baudrate == IDE_BAUDRATE_SLOW || baudrate == IDE_BAUDRATE_FAST) {
                debug_mode = 1;
                dbg_xfer_length=0;
                UserTxBufPtrIn = UserTxBufPtrOut = UserTxBufPtrOutShadow =0;
                __HAL_TIM_DISABLE_IT(&TIM3_Handle, TIM_IT_UPDATE);
            } else {
                debug_mode = 0;
                UserTxBufPtrIn = UserTxBufPtrOut = UserTxBufPtrOutShadow =0;
                __HAL_TIM_ENABLE_IT(&TIM3_Handle, TIM_IT_UPDATE);
            }
            break;
        }

        case CDC_GET_LINE_CODING:
            /* Add your code here */
            pbuf[0] = (uint8_t)(baudrate);
            pbuf[1] = (uint8_t)(baudrate >> 8);
            pbuf[2] = (uint8_t)(baudrate >> 16);
            pbuf[3] = (uint8_t)(baudrate >> 24);
            pbuf[4] = 0; // stop bits (1)
            pbuf[5] = 0; // parity (none)
            pbuf[6] = 8; // number of bits (8)
            break;

        case CDC_SET_CONTROL_LINE_STATE:
            dev_is_connected = length & 1; // wValue is passed in Len (bit of a hack)
            break;

        case CDC_SEND_BREAK:
            /* Add your code here */
            break;

        default:
            break;
    }

    return USBD_OK;
}

/**
  * @brief  TIM period elapsed callback
  * @param  htim: TIM handle
  * @retval None
  */
void USBD_CDC_HAL_TIM_PeriodElapsedCallback(void) {
    if (debug_mode || !dev_is_connected) {
        // CDC device is not connected to a host, so we are unable to send any data
        return;
    }

    if (UserTxBufPtrOut == UserTxBufPtrIn && !UserTxNeedEmptyPacket) {
        // No outstanding data to send
        return;
    }

    if (UserTxBufPtrOut != UserTxBufPtrOutShadow) {
        // We have sent data and are waiting for the low-level USB driver to
        // finish sending it over the USB in-endpoint.
        // We have a 15 * 10ms = 150ms timeout
        if (UserTxBufPtrWaitCount < 15) {
            PCD_HandleTypeDef *hpcd = hUSBDDevice.pData;
            USB_OTG_GlobalTypeDef *USBx = hpcd->Instance;
            if (USBx_INEP(CDC_IN_EP & 0x7f)->DIEPTSIZ & USB_OTG_DIEPTSIZ_XFRSIZ) {
                // USB in-endpoint is still reading the data
                UserTxBufPtrWaitCount++;
                return;
            }
        }
        UserTxBufPtrOut = UserTxBufPtrOutShadow;
    }

    if (UserTxBufPtrOutShadow != UserTxBufPtrIn || UserTxNeedEmptyPacket) {
        uint32_t buffptr;
        uint32_t buffsize;

        if (UserTxBufPtrOutShadow > UserTxBufPtrIn) { // rollback
            buffsize = APP_TX_DATA_SIZE - UserTxBufPtrOutShadow;
        } else {
            buffsize = UserTxBufPtrIn - UserTxBufPtrOutShadow;
        }

        buffptr = UserTxBufPtrOutShadow;

        USBD_CDC_SetTxBuffer(&hUSBDDevice, (uint8_t*)&UserTxBuffer[buffptr], buffsize);

        if (USBD_CDC_TransmitPacket(&hUSBDDevice) == USBD_OK) {
            UserTxBufPtrOutShadow += buffsize;
            if (UserTxBufPtrOutShadow == APP_TX_DATA_SIZE) {
                UserTxBufPtrOutShadow = 0;
            }
            UserTxBufPtrWaitCount = 0;

            // According to the USB specification, a packet size of 64 bytes (CDC_DATA_FS_MAX_PACKET_SIZE)
            // gets held at the USB host until the next packet is sent.  This is because a
            // packet of maximum size is considered to be part of a longer chunk of data, and
            // the host waits for all data to arrive (ie, waits for a packet < max packet size).
            // To flush a packet of exactly max packet size, we need to send a zero-size packet.
            // See eg http://www.cypress.com/?id=4&rID=92719
            UserTxNeedEmptyPacket = (buffsize > 0 && buffsize % CDC_DATA_FS_MAX_PACKET_SIZE == 0 && UserTxBufPtrOutShadow == UserTxBufPtrIn);
        }
    }
}

uint32_t usbd_cdc_tx_buf_len() {
    uint32_t buffsize;
    if (UserTxBufPtrOutShadow > UserTxBufPtrIn) { // rollback
        buffsize = APP_TX_DATA_SIZE - UserTxBufPtrOutShadow;
    } else {
        buffsize = UserTxBufPtrIn - UserTxBufPtrOutShadow;
    }
    return buffsize;
}

uint8_t *usbd_cdc_tx_buf(uint32_t bytes) {
    uint8_t *tx_buf = UserTxBuffer+UserTxBufPtrOutShadow;
    UserTxBufPtrOutShadow = (UserTxBufPtrOutShadow + bytes) % APP_TX_DATA_SIZE;
    return tx_buf;
}

static void send_packet() {
    int bytes = MIN(dbg_xfer_length, CDC_DATA_FS_MAX_PACKET_SIZE);
    last_packet = bytes;
    usbdbg_data_in(dbg_xfer_buffer, bytes);
    dbg_xfer_length -= bytes;
    USBD_CDC_SetTxBuffer(&hUSBDDevice, dbg_xfer_buffer, bytes);
    USBD_CDC_TransmitPacket(&hUSBDDevice);
}

static int8_t CDC_Itf_TxSent() {
    if (debug_mode == 1) {
        if (dbg_xfer_length) {
            send_packet();
        } else {
            if (UserTxBufPtrOut != UserTxBufPtrOutShadow) {
                UserTxBufPtrOut = UserTxBufPtrOutShadow;
            }
            if (last_packet == CDC_DATA_FS_MAX_PACKET_SIZE) {
                last_packet = 0;
                USBD_CDC_SetTxBuffer(&hUSBDDevice, dbg_xfer_buffer, 0);
                USBD_CDC_TransmitPacket(&hUSBDDevice);
            }
        }
    } 
    return USBD_OK;
}

/**
  * @brief  CDC_Itf_DataRx
  *         Data received over USB OUT endpoint is processed here.
  * @param  Buf: Buffer of data received
  * @param  Len: Number of data received (in bytes)
  * @retval Result of the opeartion: USBD_OK if all operations are OK else USBD_FAIL
  * @note   The buffer we are passed here is just UserRxBuffer, so we are
  *         free to modify it.
  */
static int8_t CDC_Itf_Receive(uint8_t* Buf, uint32_t *Len) {
    if (debug_mode == 1) {
        if (dbg_xfer_length) {
            int bytes = *Len;
            usbdbg_data_out(Buf, bytes);
            dbg_xfer_length -= bytes;
        } else if (Buf[0] == '\x30') { // command
            uint8_t request = Buf[1];
            dbg_xfer_length = *((uint32_t*)(Buf+2));
            usbdbg_control(Buf+6, request, dbg_xfer_length);
            if (dbg_xfer_length && (request & 0x80)) { //request has a device-to-host data phase
                send_packet(); //prime tx buffer
            }
        }

        // initiate next USB packet transfer
        USBD_CDC_SetRxBuffer(&hUSBDDevice, UserRxBuffer);
        USBD_CDC_ReceivePacket(&hUSBDDevice);

        return USBD_OK;
    }

    // TODO improve this function to implement a circular buffer

    // if we have processed all the characters, reset the buffer counters
    if (UserRxBufCur > 0 && UserRxBufCur >= UserRxBufLen) {
        memmove(UserRxBuffer, UserRxBuffer + UserRxBufLen, *Len);
        UserRxBufCur = 0;
        UserRxBufLen = 0;
    }

    uint32_t delta_len;

    if (user_interrupt_char == -1) {
        // no special interrupt character
        delta_len = *Len;

    } else {
        // filter out sepcial interrupt character from the buffer
        bool char_found = false;
        uint8_t *dest = Buf;
        uint8_t *src = Buf;
        uint8_t *buf_top = Buf + *Len;
        for (; src < buf_top; src++) {
            if (*src == user_interrupt_char) {
                char_found = true;
                // raise exception when interrupts are finished
                pendsv_nlr_jump(user_interrupt_data);
            } else {
                if (char_found) {
                    *dest = *src;
                }
                dest++;
            }
        }

        // length of remaining characters
        delta_len = dest - Buf;
    }

    if (UserRxBufLen + delta_len + CDC_DATA_FS_MAX_PACKET_SIZE > APP_RX_DATA_SIZE) {
        // if we keep this data then the buffer can overflow on the next USB rx
        // so we don't increment the length, and throw this data away
    } else {
        // data fits, leaving room for another CDC_DATA_FS_OUT_PACKET_SIZE
        UserRxBufLen += delta_len;
    }

    // initiate next USB packet transfer, to append to existing data in buffer
    USBD_CDC_SetRxBuffer(&hUSBDDevice, UserRxBuffer + UserRxBufLen);
    USBD_CDC_ReceivePacket(&hUSBDDevice);

    return USBD_OK;
}

int USBD_CDC_IsConnected(void) {
    return dev_is_connected;
}

void USBD_CDC_SetInterrupt(int chr, void *data) {
    user_interrupt_char = chr;
    user_interrupt_data = data;
}

int USBD_CDC_TxHalfEmpty(void) {
    int32_t tx_waiting = (int32_t)UserTxBufPtrIn - (int32_t)UserTxBufPtrOut;
    if (tx_waiting < 0) {
        tx_waiting += APP_TX_DATA_SIZE;
    }
    return tx_waiting <= APP_TX_DATA_SIZE / 2;
}

// timout in milliseconds.
// Returns number of bytes written to the device.
int USBD_CDC_Tx(const uint8_t *buf, uint32_t len, uint32_t timeout) {
    for (uint32_t i = 0; i < len; i++) {
        // Wait until the device is connected and the buffer has space, with a given timeout
        uint32_t start = HAL_GetTick();
        while (!dev_is_connected || ((UserTxBufPtrIn + 1) & (APP_TX_DATA_SIZE - 1)) == UserTxBufPtrOut) {
            // Wraparound of tick is taken care of by 2's complement arithmetic.
            if (HAL_GetTick() - start >= timeout) {
                // timeout
                return i;
            }
            __WFI(); // enter sleep mode, waiting for interrupt
        }

        // Write data to device buffer
        UserTxBuffer[UserTxBufPtrIn] = buf[i];
        UserTxBufPtrIn = (UserTxBufPtrIn + 1) & (APP_TX_DATA_SIZE - 1);
    }

    // Success, return number of bytes read
    return len;
}

// Always write all of the data to the device tx buffer, even if the
// device is not connected, or if the buffer is full.  Has a small timeout
// to wait for the buffer to be drained, in the case the device is connected.
void USBD_CDC_TxAlways(const uint8_t *buf, uint32_t len) {
    for (int i = 0; i < len; i++) {
        // If the CDC device is not connected to the host then we don't have anyone to receive our data.
        // The device may become connected in the future, so we should at least try to fill the buffer
        // and hope that it doesn't overflow by the time the device connects.
        // If the device is not connected then we should go ahead and fill the buffer straight away,
        // ignoring overflow.  Otherwise, we should make sure that we have enough room in the buffer.
        if (dev_is_connected) {
            // If the buffer is full, wait until it gets drained, with a timeout of 500ms
            // (wraparound of tick is taken care of by 2's complement arithmetic).
            uint32_t start = HAL_GetTick();
            while (((UserTxBufPtrIn + 1) & (APP_TX_DATA_SIZE - 1)) == UserTxBufPtrOut && HAL_GetTick() - start <= 500) {
                __WFI(); // enter sleep mode, waiting for interrupt
            }

            // Some unused code that makes sure the low-level USB buffer is drained.
            // Waiting for low-level is handled in USBD_CDC_HAL_TIM_PeriodElapsedCallback.
            /*
            start = HAL_GetTick();
            PCD_HandleTypeDef *hpcd = hUSBDDevice.pData;
            if (hpcd->IN_ep[0x83 & 0x7f].is_in) {
                //volatile uint32_t *xfer_count = &hpcd->IN_ep[0x83 & 0x7f].xfer_count;
                //volatile uint32_t *xfer_len = &hpcd->IN_ep[0x83 & 0x7f].xfer_len;
                USB_OTG_GlobalTypeDef *USBx = hpcd->Instance;
                while (
                    // *xfer_count < *xfer_len // using this works
                    // (USBx_INEP(3)->DIEPTSIZ & USB_OTG_DIEPTSIZ_XFRSIZ) // using this works
                    && HAL_GetTick() - start <= 2000) {
                    __WFI(); // enter sleep mode, waiting for interrupt
                }
            }
            */
        }

        UserTxBuffer[UserTxBufPtrIn] = buf[i];
        UserTxBufPtrIn = (UserTxBufPtrIn + 1) & (APP_TX_DATA_SIZE - 1);
    }
}

// Returns number of bytes in the rx buffer.
int USBD_CDC_RxNum(void) {
    if (debug_mode == 1) {
        return 0;
    }

    return UserRxBufLen - UserRxBufCur;
}

// timout in milliseconds.
// Returns number of bytes read from the device.
int USBD_CDC_Rx(uint8_t *buf, uint32_t len, uint32_t timeout) {
    if (debug_mode == 1) {
        return 0;
    }

    // loop to read bytes
    for (uint32_t i = 0; i < len; i++) {
        // Wait until we have at least 1 byte to read
        uint32_t start = HAL_GetTick();
        while (UserRxBufLen == UserRxBufCur) {
            // Wraparound of tick is taken care of by 2's complement arithmetic.
            if (HAL_GetTick() - start >= timeout) {
                // timeout
                return i;
            }
            __WFI(); // enter sleep mode, waiting for interrupt
        }

        // Copy byte from device to user buffer
        buf[i] = UserRxBuffer[UserRxBufCur++];
    }

    // Success, return number of bytes read
    return len;
}
