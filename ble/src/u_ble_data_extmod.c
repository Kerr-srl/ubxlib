/*
 * Copyright 2020 u-blox Ltd
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * You may obtain a copy of the License at
 *
 *  http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 */

/* Only #includes of u_* and the C standard library are allowed here,
 * no platform stuff and no OS stuff.  Anything required from
 * the platform/OS must be brought in through u_port* to maintain
 * portability.
 */

/** @file
 * @brief Implementation of the data API for ble.
 */

#ifndef U_CFG_BLE_MODULE_INTERNAL

#ifdef U_CFG_OVERRIDE
# include "u_cfg_override.h" // For a customer's configuration override
#endif

#include "stdlib.h"    // malloc() and free()
#include "stddef.h"    // NULL, size_t etc.
#include "stdint.h"    // int32_t etc.
#include "stdbool.h"
#include "string.h"    // memset()

#include "u_error_common.h"

#include "u_cfg_sw.h"
#include "u_port_os.h"
#include "u_port_debug.h"
#include "u_port_gatt.h"
#include "u_port_event_queue.h"
#include "u_cfg_os_platform_specific.h"

#include "u_at_client.h"
#include "u_ble_data.h"
#include "u_ble_private.h"
#include "u_short_range_module_type.h"
#include "u_short_range.h"
#include "u_short_range_private.h"
#include "u_short_range_edm_stream.h"

/* ----------------------------------------------------------------
 * COMPILE-TIME MACROS
 * -------------------------------------------------------------- */

#define U_SHORT_RANGE_BT_ADDRESS_SIZE 14

#define U_BLE_DATA_EVENT_STACK_SIZE 1536
#define U_BLE_DATA_EVENT_PRIORITY (U_CFG_OS_PRIORITY_MAX - 5)

/* ----------------------------------------------------------------
 * TYPES
 * -------------------------------------------------------------- */

typedef struct {
    uShortRangePrivateInstance_t *pInstance;
    int32_t connHandle;
    int32_t type;
    char address[U_SHORT_RANGE_BT_ADDRESS_SIZE];
    int32_t dataChannel;
    int32_t mtu;
    void (*pCallback) (int32_t, char *, int32_t, int32_t, int32_t, void *);
    void *pCallbackParameter;
} uBleDataSpsConnection_t;

// Linked list with channel info like rx buffer and tx timeout
typedef struct uBleDataSpsChannel_s {
    int32_t                       channel;
    uShortRangePrivateInstance_t  *pInstance;
    char                          pRxBuffer[U_BLE_DATA_BUFFER_SIZE];
    ringBuffer_t                  rxRingBuffer;
    uint32_t                      txTimeout;
    struct uBleDataSpsChannel_s   *pNext;
} uBleDataSpsChannel_t;

typedef struct {
    int32_t channel;
    uShortRangePrivateInstance_t *pInstance;
} bleDataEvent_t;

/* ----------------------------------------------------------------
 * STATIC PROTOTYPES
 * -------------------------------------------------------------- */
//lint -esym(818, pParameter) Suppress pParameter could be const, need to
// follow prototype
static void UUBTACLC_urc(uAtClientHandle_t atHandle, void *pParameter);
//lint -esym(818, pParameter) Suppress pParameter could be const, need to
// follow prototype
static void UUBTACLD_urc(uAtClientHandle_t atHandle, void *pParameter);
static void createSpsChannel(uShortRangePrivateInstance_t *pInstance,
                             int32_t channel, uBleDataSpsChannel_t **ppListHead);
static uBleDataSpsChannel_t *getSpsChannel(const uShortRangePrivateInstance_t *pInstance,
                                           int32_t channel, uBleDataSpsChannel_t *pListHead);
static void deleteSpsChannel(const uShortRangePrivateInstance_t *pInstance,
                             int32_t channel, uBleDataSpsChannel_t **ppListHead);
static void deleteAllSpsChannels(uBleDataSpsChannel_t **ppListHead) ;
static void spsEventCallback(uAtClientHandle_t atHandle, void *pParameter);
//lint -e{818} suppress "address could be declared as pointing to const":
// need to follow function signature
static void btEdmConnectionCallback(int32_t streamHandle, uint32_t type,
                                    uint32_t channel, bool ble, int32_t mtu,
                                    uint8_t *address, void *pParam);
static void atConnectionEvent(int32_t connHandle, int32_t type, void *pParameter);
static void dataCallback(int32_t handle, int32_t channel, int32_t length,
                         char *pData, void *pParameters);
static void onBleDataEvent(void *pParam, size_t eventSize);

/* ----------------------------------------------------------------
 * STATIC VARIABLES
 * -------------------------------------------------------------- */
static uBleDataSpsChannel_t *gpChannelList = NULL;
static int32_t gBleDataEventQueue = (int32_t)U_ERROR_COMMON_NOT_INITIALISED;
static uPortMutexHandle_t gBleDataMutex;

/* ----------------------------------------------------------------
 * EXPORTED VARIABLES
 * -------------------------------------------------------------- */

/* ----------------------------------------------------------------
 * STATIC FUNCTIONS
 * -------------------------------------------------------------- */

//lint -esym(818, pParameter) Suppress pParameter could be const, need to
// follow prototype
static void UUBTACLC_urc(uAtClientHandle_t atHandle,
                         void *pParameter)
{
    (void)pParameter;
    // We only need to read out to clean up for the at client, all data we need
    // will arrive in later events.
    char address[U_SHORT_RANGE_BT_ADDRESS_SIZE];

    (void)uAtClientReadInt(atHandle); // Connection handle
    (void)uAtClientReadInt(atHandle); // Type (always 0 meaning GATT)
    (void)uAtClientReadString(atHandle, address, U_SHORT_RANGE_BT_ADDRESS_SIZE, false);
}

//lint -esym(818, pParameter) Suppress pParameter could be const, need to
// follow prototype
static void UUBTACLD_urc(uAtClientHandle_t atHandle,
                         void *pParameter)
{
    (void)pParameter;
    // We only need to read out to clean up for the at client, all data we need
    // will arrive in later events.
    (void)uAtClientReadInt(atHandle); // Connection handle
}

// Allocate and add SPS channel info to linked list
static void createSpsChannel(uShortRangePrivateInstance_t *pInstance,
                             int32_t channel, uBleDataSpsChannel_t **ppListHead)
{
    uBleDataSpsChannel_t *pChannel = *ppListHead;

    U_PORT_MUTEX_LOCK(gBleDataMutex);

    if (pChannel == NULL) {
        pChannel = (uBleDataSpsChannel_t *)malloc(sizeof(uBleDataSpsChannel_t));
        *ppListHead = pChannel;
    } else {
        uint32_t nbrOfChannels = 1;
        while (pChannel->pNext != NULL) {
            pChannel = pChannel->pNext;
            nbrOfChannels++;
        }
        if (nbrOfChannels < U_BLE_DATA_MAX_CONNECTIONS) {
            pChannel->pNext = (uBleDataSpsChannel_t *)malloc(sizeof(uBleDataSpsChannel_t));
        }
        pChannel = pChannel->pNext;
    }

    if (pChannel != NULL) {
        ringBufferCreate(&(pChannel->rxRingBuffer), pChannel->pRxBuffer, sizeof(pChannel->pRxBuffer));
        pChannel->channel = channel;
        pChannel->pInstance = pInstance;
        pChannel->pNext = NULL;
        pChannel->txTimeout = U_BLE_DATA_DEFAULT_SEND_TIMEOUT_MS;
    } else {
        uPortLog("U_BLE_DATA: Failed to create data channel!\n");
    }

    U_PORT_MUTEX_UNLOCK(gBleDataMutex);
}

// Get SPS channel info related to channel at instance
static uBleDataSpsChannel_t *getSpsChannel(const uShortRangePrivateInstance_t *pInstance,
                                           int32_t channel, uBleDataSpsChannel_t *pListHead)
{
    uBleDataSpsChannel_t *pChannel;

    U_PORT_MUTEX_LOCK(gBleDataMutex);

    pChannel = pListHead;
    while (pChannel != NULL) {
        if ((pChannel->pInstance == pInstance) && (pChannel->channel == channel)) {
            break;
        }
        pChannel = pChannel->pNext;
    }

    U_PORT_MUTEX_UNLOCK(gBleDataMutex);

    return pChannel;
}

// Delete SPS channel info (after disconnection)
static void deleteSpsChannel(const uShortRangePrivateInstance_t *pInstance,
                             int32_t channel, uBleDataSpsChannel_t **ppListHead)
{
    uBleDataSpsChannel_t *pChannel;
    uBleDataSpsChannel_t *pPrevChannel = NULL;

    U_PORT_MUTEX_LOCK(gBleDataMutex);

    pChannel = *ppListHead;
    while ((pChannel != NULL) &&
           ((pChannel->pInstance != pInstance) || (pChannel->channel != channel))) {
        pPrevChannel = pChannel;
        pChannel = pChannel->pNext;
    }

    if (pChannel != NULL) {
        // Relink the list and free the channel
        if (pPrevChannel != NULL) {
            pPrevChannel->pNext = pChannel->pNext;
        } else {
            // This happens when the list only has one item
            *ppListHead = NULL;
        }
        ringBufferDelete(&pChannel->rxRingBuffer);
        free(pChannel);
    }

    U_PORT_MUTEX_UNLOCK(gBleDataMutex);
}

static void deleteAllSpsChannels(uBleDataSpsChannel_t **ppListHead)
{
    uBleDataSpsChannel_t *pChannel = *ppListHead;

    while (pChannel != NULL) {
        uBleDataSpsChannel_t *pChanToFree;

        ringBufferDelete(&pChannel->rxRingBuffer);
        pChanToFree = pChannel;
        pChannel = pChannel->pNext;
        free(pChanToFree);
    }
    *ppListHead = NULL;
}

static void spsEventCallback(uAtClientHandle_t atHandle,
                             void *pParameter)
{
    uBleDataSpsConnection_t *pStatus = (uBleDataSpsConnection_t *)pParameter;

    (void) atHandle;

    if (pStatus != NULL) {
        if (pStatus->pCallback != NULL) {
            // We have to create the SPS channel info before calling the
            // callback since it will assume that e.g. the rx buffer exists,
            // for the same reason we have to delete it after calling the callback
            if (pStatus->type == U_SHORT_RANGE_EVENT_CONNECTED) {
                createSpsChannel(pStatus->pInstance, pStatus->dataChannel, &gpChannelList);
            }
            pStatus->pCallback(pStatus->connHandle, pStatus->address, pStatus->type,
                               pStatus->dataChannel, pStatus->mtu, pStatus->pCallbackParameter);
            if (pStatus->type == U_SHORT_RANGE_EVENT_DISCONNECTED) {
                deleteSpsChannel(pStatus->pInstance, pStatus->dataChannel, &gpChannelList);
            }
        }
        if (pStatus->pInstance != NULL) {
            pStatus->pInstance->pPendingSpsConnectionEvent = NULL;
        }

        free(pStatus);
    }
}

//lint -e{818} suppress "address could be declared as pointing to const":
// need to follow function signature
static void btEdmConnectionCallback(int32_t streamHandle, uint32_t type,
                                    uint32_t channel, bool ble, int32_t mtu,
                                    uint8_t *address, void *pParam)
{
    (void) ble;
    (void) streamHandle;
    uShortRangePrivateInstance_t *pInstance = (uShortRangePrivateInstance_t *) pParam;
    // Type 0 == connected
    if (pInstance != NULL && pInstance->atHandle != NULL) {
        uBleDataSpsConnection_t *pStatus = (uBleDataSpsConnection_t *)
                                           pInstance->pPendingSpsConnectionEvent;
        bool send = false;

        if (pStatus == NULL) {
            //lint -esym(593, pStatus) Suppress pStatus not being free()ed here
            pStatus = (uBleDataSpsConnection_t *) malloc(sizeof(*pStatus));
        } else {
            send = true;
        }

        if (pStatus != NULL) {
            pStatus->pInstance = pInstance;
            addrArrayToString(address, U_PORT_BT_LE_ADDRESS_TYPE_UNKNOWN, false, pStatus->address);
            pStatus->type = (int32_t) type;
            pStatus->dataChannel = (int32_t) channel;
            pStatus->mtu = mtu;
            pStatus->pCallback = pInstance->pSpsConnectionCallback;
            pStatus->pCallbackParameter = pInstance->pSpsConnectionCallbackParameter;
        }
        if (send) {
            uAtClientCallback(pInstance->atHandle, spsEventCallback, pStatus);
        } else {
            pInstance->pPendingSpsConnectionEvent = (void *) pStatus;
        }
    }
}

static void atConnectionEvent(int32_t connHandle, int32_t type, void *pParameter)
{
    (void)type;
    uShortRangePrivateInstance_t *pInstance = (uShortRangePrivateInstance_t *) pParameter;
    bool send = false;

    if (pInstance->pSpsConnectionCallback != NULL) {
        uBleDataSpsConnection_t *pStatus = (uBleDataSpsConnection_t *)
                                           pInstance->pPendingSpsConnectionEvent;

        if (pStatus == NULL) {
            //lint -esym(429, pStatus) Suppress pStatus not being free()ed here
            pStatus = (uBleDataSpsConnection_t *) malloc(sizeof(*pStatus));
        } else {
            send = true;
        }
        if (pStatus != NULL) {
            pStatus->connHandle = connHandle;
            // AT (this) event info: connHandle, type, profile, address, mtu
            // EDM event info: type, profile, address, mtu, channel
            // use connHandle from here, the rest from the EDM event

            if (send) {
                uAtClientCallback(pInstance->atHandle, spsEventCallback, pStatus);
            } else {
                pInstance->pPendingSpsConnectionEvent = (void *) pStatus;
            }
        }
    }
}

static void dataCallback(int32_t handle, int32_t channel, int32_t length,
                         char *pData, void *pParameters)
{
    (void)handle;
    uShortRangePrivateInstance_t *pInstance = (uShortRangePrivateInstance_t *)pParameters;

    if (pInstance != NULL) {
        if (pInstance->pBtDataCallback != NULL) {
            pInstance->pBtDataCallback(channel, length, pData, pInstance->pBtDataCallbackParameter);
        } else if (pInstance->pBtDataAvailableCallback != NULL) {
            uBleDataSpsChannel_t *pChannel = getSpsChannel(pInstance, channel, gpChannelList);

            if (pChannel != NULL) {
                bool bufferWasEmtpy = (ringBufferDataSize(&(pChannel->rxRingBuffer)) == 0);
                // If the buffer can't fit the data we will just drop it for now
                if (!ringBufferAdd(&(pChannel->rxRingBuffer), pData, length)) {
                    uPortLog("U_BLE_DATA: RX FIFO full, dropping %d bytes!\n", length);
                }

                if (bufferWasEmtpy) {
                    bleDataEvent_t event;
                    event.channel = channel;
                    event.pInstance = pInstance;
                    uPortEventQueueSend(gBleDataEventQueue, &event, sizeof(event));
                }
            }
        }
    }
}

static void onBleDataEvent(void *pParam, size_t eventSize)
{
    (void)eventSize;

    bleDataEvent_t *pEvent = (bleDataEvent_t *)pParam;
    if (pEvent->pInstance->pBtDataAvailableCallback != NULL) {
        pEvent->pInstance->pBtDataAvailableCallback(pEvent->channel,
                                                    pEvent->pInstance->pBtDataCallbackParameter);
    }
}

/* ----------------------------------------------------------------
 * PUBLIC FUNCTIONS
 * -------------------------------------------------------------- */
int32_t uBleDataSetCallbackConnectionStatus(int32_t bleHandle,
                                            uBleDataConnectionStatusCallback_t pCallback,
                                            void *pCallbackParameter)
{
    int32_t errorCode = (int32_t) U_ERROR_COMMON_NOT_INITIALISED;
    uShortRangePrivateInstance_t *pInstance;

    if (uShortRangeLock() == (int32_t) U_ERROR_COMMON_SUCCESS) {

        pInstance = pUShortRangePrivateGetInstance(bleHandle);
        errorCode = (int32_t) U_ERROR_COMMON_INVALID_PARAMETER;
        if (pInstance != NULL) {
            bool cleanUp = false;
            if (pCallback != NULL && pInstance->pSpsConnectionCallback == NULL) {
                pInstance->pSpsConnectionCallback = pCallback;
                pInstance->pSpsConnectionCallbackParameter = pCallbackParameter;

                errorCode = uAtClientSetUrcHandler(pInstance->atHandle, "+UUBTACLC:",
                                                   UUBTACLC_urc, pInstance);

                if (errorCode == (int32_t) U_ERROR_COMMON_SUCCESS) {
                    errorCode = uAtClientSetUrcHandler(pInstance->atHandle, "+UUBTACLD:",
                                                       UUBTACLD_urc, pInstance);
                }

                if (errorCode == (int32_t) U_ERROR_COMMON_SUCCESS) {
                    errorCode = uShortRangeConnectionStatusCallback(bleHandle, U_SHORT_RANGE_CONNECTION_TYPE_BT,
                                                                    atConnectionEvent, (void *) pInstance);
                }

                if (errorCode == (int32_t) U_ERROR_COMMON_SUCCESS) {
                    errorCode = uShortRangeEdmStreamBtEventCallbackSet(pInstance->streamHandle, btEdmConnectionCallback,
                                                                       pInstance);
                }

                if (errorCode != (int32_t) U_ERROR_COMMON_SUCCESS) {
                    cleanUp = true;
                }

            } else if (pCallback == NULL && pInstance->pSpsConnectionCallback != NULL) {
                errorCode = (int32_t) U_ERROR_COMMON_SUCCESS;
                cleanUp = true;
            }

            if (cleanUp) {
                uAtClientRemoveUrcHandler(pInstance->atHandle, "+UUBTACLC:");
                uAtClientRemoveUrcHandler(pInstance->atHandle, "+UUBTACLD:");
                uShortRangeConnectionStatusCallback(bleHandle, U_SHORT_RANGE_CONNECTION_TYPE_BT, NULL, NULL);
                uShortRangeEdmStreamBtEventCallbackSet(pInstance->streamHandle, NULL, NULL);
                pInstance->pSpsConnectionCallback = NULL;
                pInstance->pSpsConnectionCallbackParameter = NULL;
            }
        }

        uShortRangeUnlock();
    }

    return errorCode;
}

int32_t uBleDataConnectSps(int32_t bleHandle, const char *pAddress)
{
    int32_t errorCode = (int32_t) U_ERROR_COMMON_NOT_INITIALISED;
    uShortRangePrivateInstance_t *pInstance;
    uAtClientHandle_t atHandle;

    if (uShortRangeLock() == (int32_t) U_ERROR_COMMON_SUCCESS) {

        pInstance = pUShortRangePrivateGetInstance(bleHandle);
        errorCode = (int32_t) U_ERROR_COMMON_INVALID_PARAMETER;
        if (pInstance != NULL) {
            errorCode = (int32_t) U_SHORT_RANGE_ERROR_INVALID_MODE;
            if (pInstance->mode == U_SHORT_RANGE_MODE_COMMAND ||
                pInstance->mode == U_SHORT_RANGE_MODE_EDM) {
                char url[20];
                memset(url, 0, 20);
                char start[] = "sps://";
                memcpy(url, start, 6);
                memcpy((url + 6), pAddress, 13);
                atHandle = pInstance->atHandle;
                uPortLog("U_BLE_DATA: Sending AT+UDCP\n");

                uAtClientLock(atHandle);
                uAtClientCommandStart(atHandle, "AT+UDCP=");
                uAtClientWriteString(atHandle, (char *)&url[0], false);
                uAtClientCommandStop(atHandle);
                uAtClientResponseStart(atHandle, "+UDCP:");
                uAtClientReadInt(atHandle); // conn handle
                uAtClientResponseStop(atHandle);
                errorCode = uAtClientUnlock(atHandle);
            }
        }

        uShortRangeUnlock();
    }

    return errorCode;
}

int32_t uBleDataDisconnect(int32_t bleHandle, int32_t connHandle)
{
    int32_t errorCode = (int32_t) U_ERROR_COMMON_NOT_INITIALISED;
    uShortRangePrivateInstance_t *pInstance;

    if (uShortRangeLock() == (int32_t) U_ERROR_COMMON_SUCCESS) {

        pInstance = pUShortRangePrivateGetInstance(bleHandle);
        errorCode = (int32_t) U_ERROR_COMMON_INVALID_PARAMETER;
        if (pInstance != NULL) {
            uAtClientHandle_t atHandle = pInstance->atHandle;
            uPortLog("U_SHORT_RANGE: Sending disconnect\n");

            uAtClientLock(atHandle);
            uAtClientCommandStart(atHandle, "AT+UDCPC=");
            uAtClientWriteInt(atHandle, connHandle);
            uAtClientCommandStopReadResponse(atHandle);
            errorCode = uAtClientUnlock(atHandle);
        }

        uShortRangeUnlock();
    }

    return errorCode;
}

int32_t uBleDataReceive(int32_t bleHandle, int32_t channel, char *pData, int32_t length)
{
    uShortRangePrivateInstance_t *pInstance = pUShortRangePrivateGetInstance(bleHandle);
    int32_t sizeOrErrorCode = (int32_t)U_ERROR_COMMON_INVALID_PARAMETER;

    if (pInstance != NULL) {
        uBleDataSpsChannel_t *pChannel = getSpsChannel(pInstance, channel, gpChannelList);

        if (pChannel != NULL) {
            sizeOrErrorCode = (int32_t)ringBufferRead(&(pChannel->rxRingBuffer), pData, length);
        }
    }

    return sizeOrErrorCode;
}

int32_t uBleDataSend(int32_t bleHandle, int32_t channel, const char *pData, int32_t length)
{
    int32_t errorCode = (int32_t) U_ERROR_COMMON_NOT_INITIALISED;
    uShortRangePrivateInstance_t *pInstance;

    if (uShortRangeLock() == (int32_t) U_ERROR_COMMON_SUCCESS) {

        pInstance = pUShortRangePrivateGetInstance(bleHandle);
        errorCode = (int32_t) U_ERROR_COMMON_INVALID_PARAMETER;
        if (pInstance != NULL) {
            uBleDataSpsChannel_t *pChannel = getSpsChannel(pInstance, channel, gpChannelList);
            errorCode = uShortRangeEdmStreamWrite(pInstance->streamHandle, channel, pData, length,
                                                  pChannel->txTimeout);
        }

        uShortRangeUnlock();
    }

    return errorCode;
}

int32_t uBleDataSetSendTimeout(int32_t bleHandle, int32_t channel, uint32_t timeout)
{
    int32_t returnValue = (int32_t)U_ERROR_COMMON_UNKNOWN;

    if (uShortRangeLock() == (int32_t) U_ERROR_COMMON_SUCCESS) {
        uShortRangePrivateInstance_t *pInstance;

        pInstance = pUShortRangePrivateGetInstance(bleHandle);
        if (pInstance != NULL) {
            uBleDataSpsChannel_t *pChannel = getSpsChannel(pInstance, channel, gpChannelList);

            if (pChannel != NULL) {
                pChannel->txTimeout = timeout;
                returnValue = (int32_t)U_ERROR_COMMON_SUCCESS;
            }
        }

        uShortRangeUnlock();
    }

    return returnValue;
}

__attribute__((deprecated))
int32_t uBleDataSetCallbackData(int32_t bleHandle,
                                void (*pCallback) (int32_t, size_t, char *, void *),
                                void *pCallbackParameter)
{
    int32_t errorCode = (int32_t) U_ERROR_COMMON_NOT_INITIALISED;
    uShortRangePrivateInstance_t *pInstance;

    if (uShortRangeLock() == (int32_t) U_ERROR_COMMON_SUCCESS) {

        pInstance = pUShortRangePrivateGetInstance(bleHandle);
        errorCode = (int32_t) U_ERROR_COMMON_INVALID_PARAMETER;
        if (pInstance != NULL) {
            if (pInstance->pBtDataCallback == NULL && pCallback != NULL) {
                pInstance->pBtDataCallback = pCallback;
                pInstance->pBtDataCallbackParameter = pCallbackParameter;

                errorCode = uShortRangeEdmStreamDataEventCallbackSet(pInstance->streamHandle, 0, dataCallback,
                                                                     pInstance);
            } else if (pInstance->pBtDataCallback != NULL && pCallback == NULL) {
                pInstance->pBtDataCallback = NULL;
                pInstance->pBtDataCallbackParameter = NULL;

                errorCode = uShortRangeEdmStreamDataEventCallbackSet(pInstance->streamHandle, 0, NULL, NULL);
            }
        }

        uShortRangeUnlock();
    }

    return errorCode;
}

int32_t uBleDataSetDataAvailableCallback(int32_t bleHandle,
                                         uBleDataAvailableCallback_t pCallback,
                                         void *pCallbackParameter)
{
    int32_t errorCode = (int32_t) U_ERROR_COMMON_NOT_INITIALISED;
    uShortRangePrivateInstance_t *pInstance;

    if (uShortRangeLock() == (int32_t) U_ERROR_COMMON_SUCCESS) {

        pInstance = pUShortRangePrivateGetInstance(bleHandle);
        errorCode = (int32_t) U_ERROR_COMMON_INVALID_PARAMETER;
        if (pInstance != NULL) {
            if (pInstance->pBtDataAvailableCallback == NULL && pCallback != NULL) {
                pInstance->pBtDataAvailableCallback = pCallback;
                pInstance->pBtDataCallbackParameter = pCallbackParameter;

                if (gBleDataEventQueue == (int32_t)U_ERROR_COMMON_NOT_INITIALISED) {
                    gBleDataEventQueue = uPortEventQueueOpen(onBleDataEvent,
                                                             "uBleDataEventQueue", sizeof(bleDataEvent_t),
                                                             U_BLE_DATA_EVENT_STACK_SIZE,
                                                             U_BLE_DATA_EVENT_PRIORITY,
                                                             2 * U_BLE_DATA_MAX_CONNECTIONS);
                    if (gBleDataEventQueue < 0) {
                        gBleDataEventQueue = (int32_t)U_ERROR_COMMON_NOT_INITIALISED;
                    }
                }

                errorCode =
                    uShortRangeEdmStreamDataEventCallbackSet(pInstance->streamHandle,
                                                             (int32_t)U_SHORT_RANGE_EDM_STREAM_CONNECTION_TYPE_BT,
                                                             dataCallback, pInstance);
            } else if (pInstance->pBtDataAvailableCallback != NULL && pCallback == NULL) {
                pInstance->pBtDataAvailableCallback = NULL;
                pInstance->pBtDataCallbackParameter = NULL;

                errorCode =
                    uShortRangeEdmStreamDataEventCallbackSet(pInstance->streamHandle,
                                                             (int32_t)U_SHORT_RANGE_EDM_STREAM_CONNECTION_TYPE_BT,
                                                             NULL, NULL);
                if (gBleDataEventQueue != (int32_t)U_ERROR_COMMON_NOT_INITIALISED) {
                    uPortEventQueueClose(gBleDataEventQueue);
                    gBleDataEventQueue = (int32_t)U_ERROR_COMMON_NOT_INITIALISED;
                }
            }
        }

        uShortRangeUnlock();
    }

    return errorCode;
}

void uBleDataPrivateInit(void)
{
    if (uPortMutexCreate(&gBleDataMutex) != (int32_t)U_ERROR_COMMON_SUCCESS) {
        gBleDataMutex = NULL;
    }
}

void uBleDataPrivateDeinit(void)
{
    if (gBleDataEventQueue != (int32_t)U_ERROR_COMMON_NOT_INITIALISED) {
        uPortEventQueueClose(gBleDataEventQueue);
        gBleDataEventQueue = (int32_t)U_ERROR_COMMON_NOT_INITIALISED;
    }
    deleteAllSpsChannels(&gpChannelList);
    if (gBleDataMutex != NULL) {
        uPortMutexDelete(gBleDataMutex);
        gBleDataMutex = NULL;
    }
}

//lint -esym(818, pHandles) Suppress pHandles could be const, need to
// follow prototype
int32_t uBleDataGetSpsServerHandles(int32_t bleHandle, int32_t channel,
                                    uBleDataSpsHandles_t *pHandles)
{
    (void)channel;
    (void)bleHandle;
    (void)pHandles;
    return (int32_t)U_ERROR_COMMON_NOT_IMPLEMENTED;
}

int32_t uBleDataPresetSpsServerHandles(int32_t bleHandle, const uBleDataSpsHandles_t *pHandles)
{
    (void)bleHandle;
    (void)pHandles;
    return (int32_t)U_ERROR_COMMON_NOT_IMPLEMENTED;
}

int32_t uBleDataDisableFlowCtrlOnNext(int32_t bleHandle)
{
    (void)bleHandle;
    return (int32_t)U_ERROR_COMMON_NOT_IMPLEMENTED;
}

#endif

// End of file
