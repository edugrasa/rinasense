
/* Standard includes. */
#include <stdint.h>
#include <string.h>

/* FreeRTOS includes. */
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/semphr.h"

/* RINA includes. */
#include "ARP826.h"
#include "ShimIPCP.h"
#include "NetworkInterface.h"
#include "BufferManagement.h"
#include "configSensor.h"

#include "esp_log.h"

/* The obtained network buffer must be large enough to hold a packet that might
 * replace the packet that was requested to be sent. */
#if ipconfigUSE_TCP == 1
    #define baMINIMAL_BUFFER_SIZE    sizeof( TCPPacket_t )
#else
    #define baMINIMAL_BUFFER_SIZE    sizeof( ARPPacket_t )
#endif /* ipconfigUSE_TCP == 1 */

/*_RB_ This is too complex not to have an explanation. */
#if defined( ipconfigETHERNET_MINIMUM_PACKET_BYTES )
    #define ASSERT_CONCAT_( a, b )    a ## b
    #define ASSERT_CONCAT( a, b )     ASSERT_CONCAT_( a, b )
    #define STATIC_ASSERT( e ) \
    ; enum { ASSERT_CONCAT( assert_line_, __LINE__ ) = 1 / ( !!( e ) ) }

    STATIC_ASSERT( ipconfigETHERNET_MINIMUM_PACKET_BYTES <= baMINIMAL_BUFFER_SIZE );
#endif


#define BUFFER_PADDING    0U

int count = 0;

/* A list of free (available) NetworkBufferDescriptor_t structures. */
static List_t xFreeBuffersList;

/* Some statistics about the use of buffers. */
static size_t uxMinimumFreeNetworkBuffers;

/* Declares the pool of NetworkBufferDescriptor_t structures that are available
 * to the system.  All the network buffers referenced from xFreeBuffersList exist
 * in this array.  The array is not accessed directly except during initialisation,
 * when the xFreeBuffersList is filled (as all the buffers are free when the system
 * is booted). */
static NetworkBufferDescriptor_t xNetworkBufferDescriptors[ NUM_NETWORK_BUFFER_DESCRIPTORS ];

/* This constant is defined as false to let FreeRTOS_TCP_IP.c know that the
 * network buffers have a variable size: resizing may be necessary */
const BaseType_t xBufferAllocFixedSize = pdFALSE;

/* The semaphore used to obtain network buffers. */
static SemaphoreHandle_t xNetworkBufferSemaphore = NULL;

static portMUX_TYPE mutex = portMUX_INITIALIZER_UNLOCKED;

/*-----------------------------------------------------------*/

BaseType_t xNetworkBuffersInitialise( void )
{
    BaseType_t xReturn;
    uint32_t x;

    /* Only initialise the buffers and their associated kernel objects if they
     * have not been initialised before. */

    if( xNetworkBufferSemaphore == NULL )
    {
        #if ( SUPPORT_STATIC_ALLOCATION == 1 )
            {
                static StaticSemaphore_t xNetworkBufferSemaphoreBuffer;
                xNetworkBufferSemaphore = xSemaphoreCreateCountingStatic(
                    NUM_NETWORK_BUFFER_DESCRIPTORS,
                    NUM_NETWORK_BUFFER_DESCRIPTORS,
                    &xNetworkBufferSemaphoreBuffer );
            }
        #else
            {
                xNetworkBufferSemaphore = xSemaphoreCreateCounting( NUM_NETWORK_BUFFER_DESCRIPTORS, NUM_NETWORK_BUFFER_DESCRIPTORS );
            }
        #endif /* configSUPPORT_STATIC_ALLOCATION */

        configASSERT( xNetworkBufferSemaphore != NULL );

        if( xNetworkBufferSemaphore != NULL )
        {
            #if ( QUEUE_REGISTRY_SIZE > 0 )
                {
                    vQueueAddToRegistry( xNetworkBufferSemaphore, "NetBufSem" );
                }
            #endif /* configQUEUE_REGISTRY_SIZE */

            /* If the trace recorder code is included name the semaphore for viewing
             * in FreeRTOS+Trace.  */
            #if ( ipconfigINCLUDE_EXAMPLE_FREERTOS_PLUS_TRACE_CALLS == 1 )
                {
                    extern QueueHandle_t xNetworkEventQueue;
                    vTraceSetQueueName( xNetworkEventQueue, "IPStackEvent" );
                    vTraceSetQueueName( xNetworkBufferSemaphore, "NetworkBufferCount" );
                }
            #endif /*  ipconfigINCLUDE_EXAMPLE_FREERTOS_PLUS_TRACE_CALLS == 1 */

            vListInitialise( &xFreeBuffersList );

            /* Initialise all the network buffers.  No storage is allocated to
             * the buffers yet. */
            for( x = 0U; x < NUM_NETWORK_BUFFER_DESCRIPTORS; x++ )
            {
                /* Initialise and set the owner of the buffer list items. */
                xNetworkBufferDescriptors[ x ].pucEthernetBuffer = NULL;
                vListInitialiseItem( &( xNetworkBufferDescriptors[ x ].xBufferListItem ) );
                listSET_LIST_ITEM_OWNER( &( xNetworkBufferDescriptors[ x ].xBufferListItem ), &xNetworkBufferDescriptors[ x ] );

                /* Currently, all buffers are available for use. */
                vListInsert( &xFreeBuffersList, &( xNetworkBufferDescriptors[ x ].xBufferListItem ) );
            }

            uxMinimumFreeNetworkBuffers = NUM_NETWORK_BUFFER_DESCRIPTORS;
        }
    }

    if( xNetworkBufferSemaphore == NULL )
    {
        xReturn = pdFAIL;
    }
    else
    {
        xReturn = pdPASS;
    }

    ESP_LOGD(TAG_NETBUFFER, "Initialized Buffers");

    return xReturn;
}
/*-----------------------------------------------------------*/

uint8_t * pucGetNetworkBuffer( size_t * pxRequestedSizeBytes )
{
    uint8_t * pucEthernetBuffer;
    size_t xSize = *pxRequestedSizeBytes;

    if( xSize < baMINIMAL_BUFFER_SIZE )
    {
        /* Buffers must be at least large enough to hold a TCP-packet with
         * headers, or an ARP packet, in case TCP is not included. */
        xSize = baMINIMAL_BUFFER_SIZE;
    }

    /* Round up xSize to the nearest multiple of N bytes,
     * where N equals 'sizeof( size_t )'. */
    if( ( xSize & ( sizeof( size_t ) - 1U ) ) != 0U )
    {
        xSize = ( xSize | ( sizeof( size_t ) - 1U ) ) + 1U;
    }

    *pxRequestedSizeBytes = xSize;

    /* Allocate a buffer large enough to store the requested Ethernet frame size
     * and a pointer to a network buffer structure (hence the addition of
     * ipBUFFER_PADDING bytes). */
    pucEthernetBuffer = ( uint8_t * ) pvPortMalloc( xSize + BUFFER_PADDING );
    configASSERT( pucEthernetBuffer != NULL );

    if( pucEthernetBuffer != NULL )
    {
        /* Enough space is left at the start of the buffer to place a pointer to
         * the network buffer structure that references this Ethernet buffer.
         * Return a pointer to the start of the Ethernet buffer itself. */
        pucEthernetBuffer += BUFFER_PADDING;
    }

    return pucEthernetBuffer;
}
/*-----------------------------------------------------------*/

void vReleaseNetworkBuffer( uint8_t * pucEthernetBuffer )
{
    /* There is space before the Ethernet buffer in which a pointer to the
     * network buffer that references this Ethernet buffer is stored.  Remove the
     * space before freeing the buffer. */
    if( pucEthernetBuffer != NULL )
    {
        pucEthernetBuffer -= BUFFER_PADDING;
        vPortFree( ( void * ) pucEthernetBuffer );
    }
}
/*-----------------------------------------------------------*/

NetworkBufferDescriptor_t * pxGetNetworkBufferWithDescriptor( size_t xRequestedSizeBytes,
                                                              TickType_t xBlockTimeTicks )
{
    NetworkBufferDescriptor_t * pxReturn = NULL;
    size_t uxCount;

    if( xNetworkBufferSemaphore != NULL )
    {
        /* If there is a semaphore available, there is a network buffer available. */
        if( xSemaphoreTake( xNetworkBufferSemaphore, xBlockTimeTicks ) == pdPASS )
        {
            /* Protect the structure as it is accessed from tasks and interrupts. */
            taskENTER_CRITICAL(&mutex);
            {
                pxReturn = ( NetworkBufferDescriptor_t * ) listGET_OWNER_OF_HEAD_ENTRY( &xFreeBuffersList );
                ( void ) uxListRemove( &( pxReturn->xBufferListItem ) );
            }
            taskEXIT_CRITICAL(&mutex);

            /* Reading UBaseType_t, no critical section needed. */
            uxCount = listCURRENT_LIST_LENGTH( &xFreeBuffersList );

            if( uxMinimumFreeNetworkBuffers > uxCount )
            {
                uxMinimumFreeNetworkBuffers = uxCount;
            }

            /* Allocate storage of exactly the requested size to the buffer. */
            configASSERT( pxReturn->pucEthernetBuffer == NULL );

            if( xRequestedSizeBytes > 0U )
            {
                if( ( xRequestedSizeBytes < ( size_t ) baMINIMAL_BUFFER_SIZE ) )
                {
                    /* ARP packets can replace application packets, so the storage must be
                     * at least large enough to hold an ARP. */
                    xRequestedSizeBytes = baMINIMAL_BUFFER_SIZE;
                }

                /* Add 2 bytes to xRequestedSizeBytes and round up xRequestedSizeBytes
                 * to the nearest multiple of N bytes, where N equals 'sizeof( size_t )'. */
                xRequestedSizeBytes += 2U;

                if( ( xRequestedSizeBytes & ( sizeof( size_t ) - 1U ) ) != 0U )
                {
                    xRequestedSizeBytes = ( xRequestedSizeBytes | ( sizeof( size_t ) - 1U ) ) + 1U;
                }

                /* Extra space is obtained so a pointer to the network buffer can
                 * be stored at the beginning of the buffer. */
                pxReturn->pucEthernetBuffer = ( uint8_t * ) pvPortMalloc( xRequestedSizeBytes + BUFFER_PADDING );

                if( pxReturn->pucEthernetBuffer == NULL )
                {
                    /* The attempt to allocate storage for the buffer payload failed,
                     * so the network buffer structure cannot be used and must be
                     * released. */
                    vReleaseNetworkBufferAndDescriptor( pxReturn );
                    pxReturn = NULL;
                }
                else
                {
                    /* Store a pointer to the network buffer structure in the
                     * buffer storage area, then move the buffer pointer on past the
                     * stored pointer so the pointer value is not overwritten by the
                     * application when the buffer is used. */
                    *( ( NetworkBufferDescriptor_t ** ) ( pxReturn->pucEthernetBuffer ) ) = pxReturn;
                    pxReturn->pucEthernetBuffer += BUFFER_PADDING;

                    /* Store the actual size of the allocated buffer, which may be
                     * greater than the original requested size. */
                    pxReturn->xDataLength = xRequestedSizeBytes;

                    #if ( ipconfigUSE_LINKED_RX_MESSAGES != 0 )
                        {
                            /* make sure the buffer is not linked */
                            pxReturn->pxNextBuffer = NULL;
                        }
                    #endif /* ipconfigUSE_LINKED_RX_MESSAGES */
                }
            }
            else
            {
                /* A descriptor is being returned without an associated buffer being
                 * allocated. */
            }
        }
    }

    if( pxReturn == NULL )
    {

    }
    else
    {
        /* No action. */

    }
    count = count + 1;
    //ESP_LOGI(TAG_NETBUFFER, "Taking Buffer");
    //ESP_LOGI(TAG_NETBUFFER, "Count:%d", count);

    return pxReturn;
}
/*-----------------------------------------------------------*/

void vReleaseNetworkBufferAndDescriptor( NetworkBufferDescriptor_t * const pxNetworkBuffer )
{
    BaseType_t xListItemAlreadyInFreeList;

    /* Ensure the buffer is returned to the list of free buffers before the
    * counting semaphore is 'given' to say a buffer is available.  Release the
    * storage allocated to the buffer payload.  THIS FILE SHOULD NOT BE USED
    * IF THE PROJECT INCLUDES A MEMORY ALLOCATOR THAT WILL FRAGMENT THE HEAP
    * MEMORY.  For example, heap_2 must not be used, heap_4 can be used. */
    vReleaseNetworkBuffer( pxNetworkBuffer->pucEthernetBuffer );
    pxNetworkBuffer->pucEthernetBuffer = NULL;
    pxNetworkBuffer->xDataLength = 0U;

    taskENTER_CRITICAL(&mutex);
    {
        xListItemAlreadyInFreeList = listIS_CONTAINED_WITHIN( &xFreeBuffersList, &( pxNetworkBuffer->xBufferListItem ) );

        if( xListItemAlreadyInFreeList == pdFALSE )
        {
            vListInsertEnd( &xFreeBuffersList, &( pxNetworkBuffer->xBufferListItem ) );
        }
    }
    taskEXIT_CRITICAL(&mutex);

    /*
     * Update the network state machine, unless the program fails to release its 'xNetworkBufferSemaphore'.
     * The program should only try to release its semaphore if 'xListItemAlreadyInFreeList' is false.
     */
    if( xListItemAlreadyInFreeList == pdFALSE )
    {
        if( xSemaphoreGive( xNetworkBufferSemaphore ) == pdTRUE )
        {

        }
    }
    else
    {
        /* No action. */

    }
    count = count - 1 ;
    //ESP_LOGI(TAG_NETBUFFER, "Releasing Buffer....!!!");
    //ESP_LOGI(TAG_NETBUFFER, "Buffer actived:%d", count);
}
/*-----------------------------------------------------------*/

/*
 * Returns the number of free network buffers
 */
UBaseType_t uxGetNumberOfFreeNetworkBuffers( void )
{
    return listCURRENT_LIST_LENGTH( &xFreeBuffersList );
}
/*-----------------------------------------------------------*/

UBaseType_t uxGetMinimumFreeNetworkBuffers( void )
{
    return uxMinimumFreeNetworkBuffers;
}
/*-----------------------------------------------------------*/

NetworkBufferDescriptor_t * pxResizeNetworkBufferWithDescriptor( NetworkBufferDescriptor_t * pxNetworkBuffer,
                                                                 size_t xNewSizeBytes )
{
    size_t xOriginalLength;
    uint8_t * pucBuffer;

    xOriginalLength = pxNetworkBuffer->xDataLength + BUFFER_PADDING;
    xNewSizeBytes = xNewSizeBytes + BUFFER_PADDING;

    pucBuffer = pucGetNetworkBuffer( &( xNewSizeBytes ) );

    if( pucBuffer == NULL )
    {
        /* In case the allocation fails, return NULL. */
        pxNetworkBuffer = NULL;
    }
    else
    {
        pxNetworkBuffer->xDataLength = xNewSizeBytes;

        if( xNewSizeBytes > xOriginalLength )
        {
            xNewSizeBytes = xOriginalLength;
        }

        ( void ) memcpy( pucBuffer - BUFFER_PADDING, pxNetworkBuffer->pucEthernetBuffer - BUFFER_PADDING, xNewSizeBytes );
        vReleaseNetworkBuffer( pxNetworkBuffer->pucEthernetBuffer );
        pxNetworkBuffer->pucEthernetBuffer = pucBuffer;
    }

    return pxNetworkBuffer;
}
