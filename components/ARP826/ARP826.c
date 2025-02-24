/*Standard includes. */
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <stdlib.h>

/* FreeRTOS includes. */
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/queue.h"
#include "freertos/semphr.h"

/* RINA includes. */
#include "ARP826.h"
#include "BufferManagement.h"
#include "NetworkInterface.h"
#include "ShimIPCP.h"
#include "configSensor.h"
#include "common.h"
#include "IPCP.h"

#include "esp_log.h"

#define MAX(a, b) ((a > b) ? a : b)

/* @brief Scan in the memory for a character. This implementation should be provided by
 * an external library, so it is implemented and included has FreeRTOS method.*/
void *FreeRTOS_memscan(void *addr, int c, size_t size);

/*-----------------------------------------------------------*/

/* @brief Generates an ARP Packet Request to be send following the structure ARP_Header
 * and copy this structure in the pxNetworkBuffer
 * */
static void prvARPGeneratePacket(NetworkBufferDescriptor_t *const pxNetworkBuffer,
								 const gha_t *pxSha, const gpa_t *pxSpa, const gpa_t *pxTpa, uint16_t usPtype);

/* FIXME: Move or delete*/
ARPPacket_t *vCastPointerTo_ARPPacket_t(void *pvArgument);

/* @brief Resolve a GPA by sending a Request ARP packet.
 * */
BaseType_t xARPResolveGPA(const gpa_t *pxSpa, const gpa_t *pxTpa, const gha_t *pxSha);

/* @brief create a broadcast MAC Address to send ARP packets*/
gha_t *pxARPCreateGHAUnknown(eGHAType_t xType);

/* @brief Add an entry into the ARP Cache*/
void vARPAddCacheEntry(struct rinarpHandle_t *pxHandle, uint8_t ucAge);

/** @brief The ARP cache.
 * Array of ARPCacheRows. The type ARPCacheRow_t has been set at ARP.h */
static ARPCacheRow_t xARPCache[ARP_CACHE_ENTRIES];

/** @brief Grow up an address GPA filling with 0x00 until the required GPA length*/
BaseType_t xARPAddressGPAGrow(gpa_t *pxGpa, size_t xlength, uint8_t ucFiller);

/** @brief Shrink an address GPA shrinking with 0x00 until the required GPA length*/
BaseType_t xARPAddressGPAShrink(gpa_t *pxGpa, uint8_t ucFiller);

/*-----------------------------------------------------------*/

/**
 * @brief Process the ARP packets.
 *
 * @param[in] pxARPFrame: The ARP Frame (the ARP packet).
 *
 * @return An enum which says whether to return the frame or to release it.
 * The eFrameProcessingResult_t is defined in the Shim_IPCP.h
 *
 */
/*-----------------------------------------------------------*/

/**
 * @brief Add/update the ARP cache entry MAC-address to IPCP-address mapping.
 *
 * @param[in] pxMACAddress: Pointer to the MAC address whose mapping is being
 *                          updated.
 * @param[in] ulIPCPAddress: 32-bit representation of the IPCP-address whose mapping
 *                         is being updated.
 */

BaseType_t xARPAddressGPAGrow(gpa_t *pxGpa, size_t uxLength, uint8_t ucFiller)
{
	uint8_t *new_address;

	if (!xShimIsGPAOK(pxGpa))
	{
		ESP_LOGE(TAG_ARP, "Bad input parameter, cannot grow the GPA");
		return pdFALSE;
	}

	if (uxLength == 0 || uxLength < pxGpa->uxLength)
	{
		ESP_LOGE(TAG_ARP, "Can't grow the GPA, bad length");
		return pdFALSE;
	}

	if (pxGpa->uxLength == uxLength)
	{
		ESP_LOGD(TAG_ARP, "No needs to grow the GPA");
		return pdTRUE;
	}

	configASSERT(uxLength > pxGpa->uxLength);

	ESP_LOGD(TAG_ARP, "Growing GPA to %zd with filler 0x%02X", uxLength, ucFiller);
	new_address = pvPortMalloc(uxLength);
	if (!new_address)
		return pdFALSE;

	memcpy(new_address, pxGpa->ucAddress, pxGpa->uxLength);
	memset(new_address + pxGpa->uxLength, ucFiller, uxLength - pxGpa->uxLength);
	vPortFree(pxGpa->ucAddress);
	pxGpa->ucAddress = new_address;
	pxGpa->uxLength = uxLength;

	ESP_LOGD(TAG_ARP, "GPA is now %zd characters long", pxGpa->uxLength);

	return pdTRUE;
}

BaseType_t xARPAddressGPAShrink(gpa_t *pxGpa, uint8_t ucFiller)
{
	uint8_t *pucNewAddress;
	uint8_t *pucPosition;
	size_t uxLength;

	if (!xShimIsGPAOK(pxGpa))
	{
		ESP_LOGE(TAG_ARP, "Bad input parameter, cannot shrink the GPA");
		return pdFALSE;
	}

	ESP_LOGI(TAG_ARP, "Looking for filler 0x%02X in GPA (length = %zd)",
			 ucFiller, pxGpa->uxLength);

	pucPosition = FreeRTOS_memscan(pxGpa->ucAddress, ucFiller, pxGpa->uxLength);
	if (pucPosition >= pxGpa->ucAddress + pxGpa->uxLength)
	{
		ESP_LOGI(TAG_ARP, "GPA doesn't need to be shrinked ...");
		return pdFALSE;
	}

	uxLength = pucPosition - pxGpa->ucAddress;

	ESP_LOGI(TAG_ARP, "Shrinking GPA to %zd", uxLength);

	pucNewAddress = pvPortMalloc(uxLength);
	if (!pucNewAddress)
		return pdFALSE;

	memcpy(pucNewAddress, pxGpa->ucAddress, uxLength);

	vPortFree(pxGpa->ucAddress);
	pxGpa->ucAddress = pucNewAddress;
	pxGpa->uxLength = uxLength;

	return pdTRUE;
}

void vARPRefreshCacheEntry(const gpa_t *pxGpa, const gha_t *pxMACAddress)
{
	BaseType_t x = 0;
	BaseType_t xIpcpEntry = -1;
	BaseType_t xMacEntry = -1;
	BaseType_t xUseEntry = 0;
	uint8_t ucMinAgeFound = 0U;

	/* Start with the maximum possible number. */
	ucMinAgeFound--;

	/* For each entry in the ARP cache table. */
	for (x = 0; x < ARP_CACHE_ENTRIES; x++) // lookup in the cache for the MacAddress
	{
		BaseType_t xMatchingMAC;

		if (pxMACAddress != NULL) // Check if pointer is not empty
		{

			// Comparison between Cache MACAddress and the MacAddress looking up for.
			if (memcmp(xARPCache[x].pxMACAddress->xAddress.ucBytes, pxMACAddress->xAddress.ucBytes, sizeof(pxMACAddress->xAddress.ucBytes)) == 0)
			{

				xMatchingMAC = pdTRUE;
			}
			else
			{

				xMatchingMAC = pdFALSE;
			}
		}
		else
		{
			xMatchingMAC = pdFALSE;
		}

		/* Check if the IPCPAddress is already on the Cache */
		if (xARPCache[x].pxProtocolAddress == pxGpa)
		{
			if (pxMACAddress == NULL)
			{
				/* In case the parameter pxMACAddress is NULL, an entry will be reserved to
				 * indicate that there is an outstanding ARP request, This entry will have
				 * "ucValid == pdFALSE". */
				xIpcpEntry = x;
				break; // break an keep position to update the ARP request.
			}

			/* See if the MAC-address also matches. */
			if (xMatchingMAC != pdFALSE)
			{
				/* This function will be called for each received packet
				 * As this is by far the most common path the coding standard
				 * is relaxed in this case and a return is permitted as an
				 * optimisation. */
				xARPCache[x].ucAge = (uint8_t)MAX_ARP_AGE; // update ARP Age
				xARPCache[x].ucValid = (uint8_t)pdTRUE;	   // update ucValid
				return;									   // Do not update cache entry because there is already an entry in cache.
			}

			/* Found an entry containing ulIPCPAddress, but the MAC address
			 * doesn't match.  Might be an entry with ucValid=pdFALSE, waiting
			 * for an ARP reply.  Still want to see if there is match with the
			 * given MAC address.ucBytes.  If found, either of the two entries
			 * must be cleared. */
			xIpcpEntry = x;
		}
		else if (xMatchingMAC != pdFALSE)
		{
			/* Found an entry with the given MAC-address, but the IPCP-address
			 * is different.  Continue looping to find a possible match with
			 * ulIPCPAddress. */

			xMacEntry = x;
		}

		/* _HT_
		 * Shouldn't we test for xARPCache[ x ].ucValid == pdFALSE here ? */
		else if (xARPCache[x].ucAge < ucMinAgeFound)
		{
			/* As the table is traversed, remember the table row that
			 * contains the oldest entry (the lowest age count, as ages are
			 * decremented to zero) so the row can be re-used if this function
			 * needs to add an entry that does not already exist. */
			ucMinAgeFound = xARPCache[x].ucAge;
			xUseEntry = x;
		}
		else
		{
			/* Nothing happens to this cache entry for now. */
		}
	}

	if (xMacEntry >= 0)
	{
		xUseEntry = xMacEntry;

		if (xIpcpEntry >= 0)
		{
			/* Both the MAC address as well as the IPCP address were found in
			 * different locations: clear the entry which matches the
			 * IPCP-address */
			(void)memset(&(xARPCache[xIpcpEntry]), 0, sizeof(ARPCacheRow_t));
		}
	}
	else if (xIpcpEntry >= 0)
	{
		/* An entry containing the IPCP-address was found, but it had a different MAC address */
		xUseEntry = xIpcpEntry;
	}
	else
	{
		/* No matching entry found. */
	}

	/* If the entry was not found, we use the oldest entry and set the IPCPaddress */
	xARPCache[xUseEntry].pxProtocolAddress = pxGpa->ucAddress;

	if (pxMACAddress != NULL)
	{
		(void)memcpy(xARPCache[xUseEntry].pxMACAddress->xAddress.ucBytes, pxMACAddress->xAddress.ucBytes, sizeof(pxMACAddress->xAddress.ucBytes));

		/* And this entry does not need immediate attention */
		xARPCache[xUseEntry].ucAge = (uint8_t)MAX_ARP_AGE;
		xARPCache[xUseEntry].ucValid = (uint8_t)pdTRUE;
	}
	else if (xIpcpEntry < 0)
	{
		xARPCache[xUseEntry].ucAge = (uint8_t)MAX_ARP_RETRANSMISSIONS;
		xARPCache[xUseEntry].ucValid = (uint8_t)pdFALSE;
	}
	else
	{
		/* Nothing will be stored. */
	}

	ESP_LOGI(TAG_ARP, "ARP Cache Refreshed! ");
}

/*-----------------------------------------------------------*/

/**
 * @brief Add/update the ARP cache entry MAC-address to IPCP-address mapping.
 *
 * @param[in] pxMACAddress: Pointer to the MAC address whose mapping is being
 *                          updated.
 * @param[in] ulIPCPAddress: 32-bit representation of the IPCP-address whose mapping
 *                         is being updated.
 */
BaseType_t vARPSendRequest(gpa_t *pxTpa, gpa_t *pxSpa, gha_t *pxSha)
{
	NetworkBufferDescriptor_t *pxNetworkBuffer;
	size_t xMaxLen;
	size_t xBufferSize;

	xMaxLen = MAX(pxSpa->uxLength, pxTpa->uxLength);

	if (!xARPAddressGPAGrow(pxSpa, xMaxLen, 0x00))
	{
		ESP_LOGI(TAG_ARP, "Failed to grow SPA\n");
		return pdFALSE;
	}

	if (!xARPAddressGPAGrow(pxTpa, xMaxLen, 0x00))
	{
		ESP_LOGI(TAG_ARP, "Failed to grow TPA\n");
		return pdFALSE;
	}

	xBufferSize = sizeof(ARPPacket_t) + (xMaxLen + sizeof(pxSha->xAddress)) * 2;
	/* This is called from the context of the IPCP event task, so a block time
	 * must not be used. */

	//ESP_LOGE(TAG_ARP, "Taking Buffer to send ARP Request: vARPSendRequest");
	pxNetworkBuffer = pxGetNetworkBufferWithDescriptor(xBufferSize, (TickType_t)0U); // sizeof length ARPPacket

	if (pxNetworkBuffer != NULL)
	{
		pxNetworkBuffer->ulGpa = pxTpa->ucAddress;

		prvARPGeneratePacket(pxNetworkBuffer, pxSha, pxSpa, pxTpa, ARP_REQUEST);

		if (xIsCallingFromIPCPTask() != pdFALSE)
		{

			/* Only the IPCP-task is allowed to call this function directly. */
			(void)xNetworkInterfaceOutput(pxNetworkBuffer, pdTRUE);
		}
		else
		{
			RINAStackEvent_t xSendEvent;

			/* Send a message to the IPCP-task to send this ARP packet. */
			xSendEvent.eEventType = eNetworkTxEvent;
			xSendEvent.pvData = pxNetworkBuffer;

			if (xSendEventStructToIPCPTask(&xSendEvent, (TickType_t)portMAX_DELAY) == pdFAIL)
			{
				/* Failed to send the message, so release the network buffer. */
				vReleaseNetworkBufferAndDescriptor(pxNetworkBuffer);
				return pdFALSE;
			}
			ESP_LOGI(TAG_SHIM, "send by event\n");
		}
	}
	else
	{
		ESP_LOGI(TAG_SHIM, "BufferNetwork Null\n");
		return pdFALSE;
	}
	return pdTRUE;
}

gha_t *pxARPCreateGHAUnknown(eGHAType_t xType)
{
	const uint8_t ucAddr[6] = {0xff, 0xff, 0xff, 0xff, 0xff, 0xff};

	if (xType == MAC_ADDR_802_3)
	{
		gha_t *pxGha;

		if (xType != MAC_ADDR_802_3)
		{
			printf("Wrong input parameters, cannot create GHA");
			return NULL;
		}

		pxGha = malloc(sizeof(*pxGha));
		if (!pxGha)
			return NULL;

		pxGha->xType = xType;
		switch (xType)
		{
		case MAC_ADDR_802_3:
			memcpy(pxGha->xAddress.ucBytes, ucAddr, sizeof(pxGha->xAddress));
			break;
		default:

			break; /* Only to stop the compiler from barfing */
		}

		return pxGha;

		// return gha_create_gfp(flags, MAC_ADDR_802_3, addr);
	}
	return NULL;
}

static void prvARPGeneratePacket(NetworkBufferDescriptor_t *const pxNetworkBuffer,
								 const gha_t *pxSha, const gpa_t *pxSpa, const gpa_t *pxTpa, uint16_t usPtype)
{

	ARPPacket_t *pxARPPacket;
	gha_t *pxTha = pvPortMalloc(sizeof(*pxTha));
	unsigned char *pucArpPtr;
	size_t uxLength;

	pxTha = pxARPCreateGHAUnknown(MAC_ADDR_802_3);

	uxLength = sizeof(*pxARPPacket) + (pxSpa->uxLength + sizeof(pxSha->xAddress)) * 2;

	/* Buffer allocation ensures that buffers always have space
	 * for an ARP packet.  */
	configASSERT(pxNetworkBuffer != NULL);
	configASSERT(pxNetworkBuffer->xDataLength >= uxLength);

	pxARPPacket = vCastPointerTo_ARPPacket_t(pxNetworkBuffer->pucEthernetBuffer);

	/* memcpy the const part of the header information into the correct
	 * location in the packet.  This copies:
	 *  xEthernetHeader.ulDestinationAddress
	 *  xEthernetHeader.usFrameType;
	 *  xARPHeader.usHardwareType;
	 *  xARPHeader.usProtocolType;
	 *  xARPHeader.ucHardwareAddressLength;
	 *  xARPHeader.ucProtocolAddressLength;
	 *  xARPHeader.usOperation;
	 *  xARPHeader.xTargetHardwareAddress;
	 */

	pxARPPacket->xARPHeader.usHType = FreeRTOS_htons(0x0001);
	pxARPPacket->xARPHeader.usPType = FreeRTOS_htons(ETH_P_RINA);
	pxARPPacket->xARPHeader.ucHALength = sizeof(pxSha->xAddress.ucBytes);
	pxARPPacket->xARPHeader.ucPALength = pxSpa->uxLength;
	pxARPPacket->xARPHeader.usOperation = usPtype;
	pxARPPacket->xEthernetHeader.usFrameType = FreeRTOS_htons(ETH_P_ARP);

	memcpy(pxARPPacket->xEthernetHeader.xSourceAddress.ucBytes, pxSha->xAddress.ucBytes, sizeof(pxSha->xAddress));
	memcpy(pxARPPacket->xEthernetHeader.xDestinationAddress.ucBytes, pxTha->xAddress.ucBytes, sizeof(pxTha->xAddress));

	pucArpPtr = (unsigned char *)(pxARPPacket + 1);

	memcpy(pucArpPtr, pxSha->xAddress.ucBytes, sizeof(pxSha->xAddress));
	pucArpPtr += sizeof(pxSha->xAddress);

	memcpy(pucArpPtr, pxSpa->ucAddress, pxSpa->uxLength);
	pucArpPtr += pxSpa->uxLength;

	/* THA */
	memcpy(pucArpPtr, pxTha->xAddress.ucBytes, sizeof(pxTha->xAddress));
	pucArpPtr += sizeof(pxTha->xAddress);

	/* TPA */
	memcpy(pucArpPtr, pxTpa->ucAddress, pxTpa->uxLength);
	pucArpPtr += pxTpa->uxLength;

	pxNetworkBuffer->xDataLength = uxLength;

	ESP_LOGD(TAG_ARP, "Generated Request Packet ");
}

void vARPRemoveCacheEntry(const gpa_t *pxGpa, const gha_t *pxMACAddress)
{
	BaseType_t x = 0;

	const string_t pcAd = "NULL\0";

	gha_t *tmp = pxARPCreateGHAUnknown(MAC_ADDR_802_3);
	gpa_t *protocolAddress = pxShimCreateGPA((uint8_t *)pcAd, 5);

	ARPCacheRow_t xNullCacheEntry;

	xNullCacheEntry.pxProtocolAddress = protocolAddress;
	xNullCacheEntry.pxMACAddress = tmp;
	xNullCacheEntry.ucAge = 0;
	xNullCacheEntry.ucValid = 0;

	if (pxMACAddress == NULL || pxGpa == NULL)
	{
		ESP_LOGE(TAG_ARP, "Bad input GPA and GHA to remove");
		return;
	}

	/* For each entry in the ARP cache table. */
	for (x = 0; x < ARP_CACHE_ENTRIES; x++) // lookup in the cache for the MacAddress
	{

		// Comparison between Cache MACAddress and the MacAddress looking up for.
		if (xARPCache[x].pxMACAddress->xAddress.ucBytes == pxMACAddress->xAddress.ucBytes)
		{
			if (xARPCache[x].pxProtocolAddress->ucAddress == pxGpa->ucAddress)
			{
				xARPCache[x] = xNullCacheEntry;
				ESP_LOGD(TAG_ARP, "ARPEntry Removed");
				break;
			}
		}
	}
}

/*-----------------------------------------------------------*/

/**
 * @brief Lookup an IPCP address in the ARP cache.
 *
 * @param[in] ulAddressToLookup: The 32-bit representation of an IP address to
 *                               lookup.
 * @param[out] pxMACAddress: A pointer to MACAddress_t variable where, if there
 *                          is an ARP cache hit, the MAC address corresponding to
 *                          the IP address will be stored.
 *
 * @return When the IPCP-address is found: eARPCacheHit, when not found: eARPCacheMiss,
 *         and when waiting for a ARP reply: eCantSendPacket.
 */

eFrameProcessingResult_t eARPProcessPacket(ARPPacket_t *const pxARPFrame)
{

	eFrameProcessingResult_t eReturn = eReleaseBuffer;
	ARPHeader_t *pxARPHeader;
	struct rinarpHandle_t *pxHandle;

	uint16_t usOperation;
	uint16_t usHtype;
	uint16_t usPtype;
	uint8_t ucHlen;
	uint8_t ucPlen;

	uint8_t *ucPtr; /* Pointer last byte of the ARP header*/

	uint8_t *ucSpa; /* Source protocol address pointer */
	uint8_t *ucTpa; /* Target protocol address pointer */
	uint8_t *ucSha; /* Source protocol address pointer */
	uint8_t *ucTha; /* Target protocol address pointer */

	gpa_t *pxTmpSpa;
	gha_t *pxTmpSha;
	gpa_t *pxTmpTpa;
	gha_t *pxTmpTha;

	// Get ARPHeader from the ARPFrame
	pxARPHeader = &(pxARPFrame->xARPHeader);

	// The field ulSpa is badly aligned, copy byte-by-byte.

	usHtype = FreeRTOS_ntohs(pxARPHeader->usHType);
	usPtype = FreeRTOS_ntohs(pxARPHeader->usPType);
	ucHlen = pxARPHeader->ucHALength;
	ucPlen = pxARPHeader->ucPALength;

	if (pxARPHeader->usHType != FreeRTOS_htons(ARP_HARDWARE_TYPE_ETHERNET))
	{
		ESP_LOGE(TAG_ARP, "Unhandled ARP hardware type 0x%04X", pxARPHeader->usHType);
		return eReturn;
	}
	if (ucHlen != 6)
	{
		ESP_LOGE(TAG_ARP, "Unhandled ARP hardware address length (%d)", ucHlen);
		return eReturn;
	}

	usOperation = pxARPHeader->usOperation;

	if (usOperation != ARP_REPLY && usOperation != ARP_REQUEST)
	{
		ESP_LOGE(TAG_ARP, "Unhandled ARP operation 0x%04X", usOperation);
		return eReturn;
	}

	/* Pointer to 8Bytes(after usOperation ARP Header)*/
	ucPtr = (uint8_t *)pxARPHeader + 8;

	/*First byte pointer source hardware address*/
	ucSha = ucPtr;
	ucPtr = ucPtr + pxARPHeader->ucHALength; /*+6 MAC address Length*/

	/*New Pointer after last by MAC Address*/
	ucSpa = ucPtr;
	ucPtr = ucPtr + pxARPHeader->ucPALength;

	/*First byte pointer dest hardware address*/
	ucTha = ucPtr;
	ucPtr = ucPtr + pxARPHeader->ucHALength;

	ucTpa = ucPtr;
	ucPtr = ucPtr + pxARPHeader->ucPALength;

	
	pxTmpSpa = pxShimCreateGPA(ucSpa, (size_t)ucPlen+1);
	pxTmpSha = pxShimCreateGHA(MAC_ADDR_802_3, (MACAddress_t *)ucSha);
	pxTmpTpa = pxShimCreateGPA(ucTpa, (size_t)ucPlen+1);
	pxTmpTha = pxShimCreateGHA(MAC_ADDR_802_3, (MACAddress_t *)ucTha);
	vARPPrintCache();

	if (xARPAddressGPAShrink(pxTmpSpa, 0x00))
	{
		ESP_LOGE(TAG_ARP, "Problems parsing the source GPA");
		vShimGPADestroy(pxTmpSpa);
		vShimGPADestroy(pxTmpTpa);
		vShimGHADestroy(pxTmpSha);
		vShimGHADestroy(pxTmpTha);
		return eReturn;
	}
	if (xARPAddressGPAShrink(pxTmpTpa, 0x00))
	{
		ESP_LOGE(TAG_ARP, "Got problems parsing the target GPA");
		vShimGPADestroy(pxTmpSpa);
		vShimGPADestroy(pxTmpTpa);
		vShimGHADestroy(pxTmpSha);
		vShimGHADestroy(pxTmpTha);
		return eReturn;
	}

	/*ESP_LOGE(TAG_ARP, "ShimGPAAddressToString");
	string_t *xTmpSpaString = xShimGPAAddressToString(pxTmpSpa);

	if (!xTmpSpaString)
	{
		ESP_LOGE(TAG_ARP, "Failed to convert GPA address to string");
	}
	ESP_LOGE(TAG_ARP, "xRINAstringToName");
	name_t *xTmpSpaName = xRINAstringToName(xTmpSpaString);
	gpa_t *pxSpa = pxShimNameToGPA(xTmpSpaName);*/

	switch (usOperation)
	{
	case ARP_REQUEST:

		// rinarpHandle_t * handle;
		// handle = pvPortMalloc(sizeof(*handle));
		// Check Cache by Address

		ESP_LOGE(TAG_ARP, "ARP_REQUEST ptype 0x%04X", usOperation);

		if (eARPLookupGPA(pxTmpSpa) == eARPCacheMiss)
		{
			ESP_LOGE(TAG_ARP, "I don't have a table for ptype 0x%04X",
					 usPtype);
			vShimGPADestroy(pxTmpSpa);
			vShimGPADestroy(pxTmpTpa);
			vShimGHADestroy(pxTmpSha);
			vShimGHADestroy(pxTmpTha);
			return eReturn;
		}

		// handle->ha = sha;
		// handle->pa = spa;

		// vARPAddCacheEntry( handle );

		// The request is for the address of this IoT node.  Add the
		// entry into the ARP cache, or refresh the entry if it
		// already exists.
		vARPRefreshCacheEntry(pxTmpSpa, pxTmpSha);
		// ESP_LOGE(TAG_ARP,"TArget:%s", pxTmpSha);

		// Generate a reply payload in the same buffer.
		pxARPHeader->usOperation = (uint16_t)ARP_REPLY;

		eReturn = eReturnEthernetFrame;

		break;

	case ARP_REPLY:

		pxHandle = pvPortMalloc(sizeof(*pxHandle));

		pxHandle->pxHa = pxTmpSha;
		pxHandle->pxPa = pxTmpSpa;

		vARPAddCacheEntry(pxHandle, 3);

		//vARPPrintCache(); // test

		eReturn = eProcessBuffer;

		break;

	default:
		// Invalid.
		break;
	}

	return eReturn;
}

gha_t *pxARPLookupGHA(const gpa_t *pxGpaToLookup)
{
	BaseType_t x;
	gha_t *pxGHA;

	pxGHA = pvPortMalloc(sizeof(*pxGHA));

	/*
	ESP_LOGE(TAG_ARP, "ARP Cache searching");
	ESP_LOGE(TAG_ARP, "ToBe found: %s", pxGpaToLookup->ucAddress);
	*/
	/* Loop through each entry in the ARP cache. */
	for (x = 0; x < ARP_CACHE_ENTRIES; x++)
	{
		/* Does this row in the ARP cache table hold an entry for the Protocol address
		 * being queried? */

		if (xARPCache[x].ucAge == 3)
		{
			//ESP_LOGE(TAG_ARP, "ARP Found: %s", xARPCache[x].pxProtocolAddress->ucAddress);
			pxGHA->xAddress = xARPCache[x].pxMACAddress->xAddress;
			pxGHA->xType = xARPCache[x].pxMACAddress->xType;
			return pxGHA;
		}
		//ESP_LOGE(TAG_ARP, "ARP not Found: %s", xARPCache[x].pxProtocolAddress->ucAddress);
	}

	return NULL;
}

eARPLookupResult_t eARPLookupGPA(const gpa_t *pxGpaToLookup)
{
	BaseType_t x;
	eARPLookupResult_t eReturn = eARPCacheMiss;

	/* Loop through each entry in the ARP cache. */
	for (x = 0; x < ARP_CACHE_ENTRIES; x++)
	{
		/* Does this row in the ARP cache table hold an entry for the Protocol address
		 * being queried? */

		if (xARPCache[x].pxProtocolAddress->ucAddress == pxGpaToLookup->ucAddress)
		{
			// ESP_LOGI(TAG_ARP, "ARP FOund: %s", xARPCache[ x ].ProtocolAddress->address == gpaToLookup->address);

			/* A matching valid entry was found. */
			if (xARPCache[x].ucValid == (uint8_t)pdFALSE)
			{
				/* This entry is waiting an ARP reply, so is not valid. */
				eReturn = eCantSendPacket;
			}
			else
			{
				/* A valid entry was found the copy the MacAddress to be used after. */

				eReturn = eARPCacheHit;
			}

			break;
		}
	}

	return eReturn;
}
/*-----------------------------------------------------------*/

/*-----------------------------------------------------------*/

/**
 * @brief Generate an ARP request packet by copying various constant details to
 *        the buffer.
 *
 * @param[in,out] pxNetworkBuffer: Pointer to the buffer which has to be filled with
 *                             the ARP request packet details.
 */
/*-----------------------------------------------------------*/

/**
 * @brief Create and send an ARP request packet.
 *
 * @param[in] ulIPCPAddress: A 32-bit representation of the IP-address whose
 *                         physical (MAC) address is required.
 */

/*-----------------------------------------------------------*/
/**
 * @brief A call to this function will update the default configuration MAC Address
 * after the WIFI driver is initialized.
 */
void vARPUpdateMACAddress(const uint8_t ucMACAddress[MAC_ADDRESS_LENGTH_BYTES], const MACAddress_t *pxPhyDev)
{

	(void)memcpy(pxPhyDev->ucBytes, ucMACAddress, (size_t)MAC_ADDRESS_LENGTH_BYTES);
	ESP_LOGI(TAG_ARP, "MAC Address updated");
}

/**
 * @brief A call to this function will clear the all ARP cache.
 */
void vARPRemoveAll(void)
{
	(void)memset(xARPCache, 0, sizeof(xARPCache));
}
/*-----------------------------------------------------------*/

ARPPacket_t *vCastPointerTo_ARPPacket_t(void *pvArgument)
{
	return (void *)(pvArgument);
}

/*-----------------------------------------------------------*/
/**
 * @brief Decide whether this packet should be processed or not based on the IPCP address in the packet.
 *
 * @param[in] pucEthernetBuffer: The ethernet packet under consideration.
 *
 * @return Enum saying whether to release or to process the packet.
 */

struct rinarpHandle_t *pxARPAdd(gpa_t *pxPa, gha_t *pxHa)
{

	struct rinarpHandle_t *pxHandle;

	pxHandle = pvPortMalloc(sizeof(*pxHandle));

	if (!xShimIsGPAOK(pxPa))
	{
		ESP_LOGI(TAG_SHIM, "GPA is not correct");
		return pdFALSE;
	}

	if (!xShimIsGHAOK(pxHa))
	{
		ESP_LOGI(TAG_SHIM, "GHA is not correct");
		return pdFALSE;
	}

	pxHandle->pxHa = pxHa;
	pxHandle->pxPa = pxPa;
	vARPAddCacheEntry(pxHandle,1);

	return pxHandle;
}

BaseType_t xARPRemove(const gpa_t *pxPa, const gha_t *pxha)
{
	if (!xShimIsGPAOK(pxPa->ucAddress))
	{
		ESP_LOGE(TAG_SHIM, "GPA is not correct");

		return pdFALSE;
	}
	if (!xShimIsGHAOK(pxha))
	{
		ESP_LOGE(TAG_SHIM, "GHA is not correct");

		return pdFALSE;
	}

	if (eARPLookupGPA(pxPa) == eARPCacheMiss)
	{
		ESP_LOGE(TAG_ARP, "GPA is not registered");
		ESP_LOGI(TAG_ARP, "GPA: %s", pxPa->ucAddress);
		return pdFALSE;
	}

	vARPRemoveCacheEntry(pxPa, pxha);

	return pdTRUE;
}

BaseType_t xARPResolveGPA(const gpa_t *pxTpa, const gpa_t *pxSpa, const gha_t *pxSha)
{

	if (!xShimIsGPAOK(pxTpa) || !xShimIsGHAOK(pxSha) || !xShimIsGPAOK(pxSpa))
	{
		ESP_LOGE(TAG_ARP, "Parameters are not correct, won't resolve GPA");
		return pdFALSE;
	}

	return vARPSendRequest(pxTpa, pxSpa, pxSha);
}

void vARPInitCache(void)
{
	const string_t pcAd = "NULL\0";

	gha_t *pxTmp = pxARPCreateGHAUnknown(MAC_ADDR_802_3);
	gpa_t *pxProtocolAddress = pxShimCreateGPA((uint8_t *)pcAd, 5);

	ARPCacheRow_t xNullCacheEntry;

	xNullCacheEntry.pxProtocolAddress = pxProtocolAddress;
	xNullCacheEntry.pxMACAddress = pxTmp;
	xNullCacheEntry.ucAge = 0;
	xNullCacheEntry.ucValid = 0;
	int i;
	for (i = 0; i < ARP_CACHE_ENTRIES; i++)
	{
		//( void ) memset( &( xARPCache[ i ] ), 0, sizeof( ARPCacheRow_t ) );
		xARPCache[i] = xNullCacheEntry;
	}

	ESP_LOGI(TAG_ARP, "ARP CACHE Initialized");
}

void vARPPrintCache(void)
{
	BaseType_t x, xCount = 0;

	for (x = 0; x < ARP_CACHE_ENTRIES; x++)
	{
		if ((xARPCache[x].pxProtocolAddress->ucAddress != 0UL) && (xARPCache[x].ucValid != 0))
		{
			ESP_LOGI(TAG_ARP, "Arp Entry %i: %3d - %s - %02x:%02x:%02x:%02x:%02x:%02x\n",
					 x,
					 xARPCache[x].ucAge,
					 xARPCache[x].pxProtocolAddress->ucAddress,
					 xARPCache[x].pxMACAddress->xAddress.ucBytes[0],
					 xARPCache[x].pxMACAddress->xAddress.ucBytes[1],
					 xARPCache[x].pxMACAddress->xAddress.ucBytes[2],
					 xARPCache[x].pxMACAddress->xAddress.ucBytes[3],
					 xARPCache[x].pxMACAddress->xAddress.ucBytes[4],
					 xARPCache[x].pxMACAddress->xAddress.ucBytes[5]);
		}
		xCount++;
	}
	ESP_LOGI(TAG_ARP, "Arp has %d entries\n", xCount);
}

void vARPPrintMACAddress(const gha_t *pxGha)
{

	ESP_LOGI(TAG_ARP, "MACAddress: %02x:%02x:%02x:%02x:%02x:%02x\n", pxGha->xAddress.ucBytes[0],
			 pxGha->xAddress.ucBytes[1],
			 pxGha->xAddress.ucBytes[2],
			 pxGha->xAddress.ucBytes[3],
			 pxGha->xAddress.ucBytes[4],
			 pxGha->xAddress.ucBytes[5]);
}

void vARPAddCacheEntry(struct rinarpHandle_t *pxHandle, uint8_t ucAge)
{
	ARPCacheRow_t xCacheEntry;
	BaseType_t x = 0;

	xCacheEntry.pxProtocolAddress = pxHandle->pxPa;
	xCacheEntry.pxMACAddress = pxHandle->pxHa;
	xCacheEntry.ucAge = ucAge;
	xCacheEntry.ucValid = 1;

	for (x = 0; x < ARP_CACHE_ENTRIES; x++)

	{
		if (xARPCache[x].ucValid == 0)
		{
			xARPCache[x].pxProtocolAddress = xCacheEntry.pxProtocolAddress;
			xARPCache[x].pxMACAddress = xCacheEntry.pxMACAddress;
			xARPCache[x].ucAge = xCacheEntry.ucAge;
			xARPCache[x].ucValid = xCacheEntry.ucValid;
			ESP_LOGD(TAG_ARP, "ARP Entry successful");

			break;
		}
	}
}

void *FreeRTOS_memscan(void *addr, int c, size_t size)
{
	unsigned char *p = (unsigned char *)addr;
	while (size)
	{
		if (*p == c)
			return (void *)p;
		p++;
		size--;
	}
	return (void *)p;
}
