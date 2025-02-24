
/*
 * EFCP
 */

#ifndef EFCP_H__INCLUDED
#define EFCP_H__INCLUDED

#include "Rmt.h"
#include "pci.h"
#include "du.h"
#include "efcpStructures.h"

struct efcp_t;
struct du_t;
struct efcpContainer_t;

BaseType_t xEfcpEnqueue(struct efcp_t * pxEfcp, portId_t xPort, struct du_t * pxDu);
BaseType_t xEfcpContainerReceive( struct efcpContainer_t * pxContainer, cepId_t xCepId, struct du_t * pxDu);
BaseType_t xEfcpReceive(struct efcp_t * pxEfcp,  struct du_t *  pxDu);
struct efcpContainer_t * pxEfcpContainerCreate(void);
BaseType_t xEfcpContainerWrite(struct efcpContainer_t *pxEfcpContainer, cepId_t xCepId, struct du_t *pxDu);

BaseType_t xEfcpConnectionDestroy(struct efcpContainer_t * pxContainer, cepId_t xId);

cepId_t xEfcpConnectionCreate(struct efcpContainer_t * pxContainer,
                                address_t               xSrcAddr,
                                address_t               xDstAddr,
                                portId_t               xPortId,
                                qosId_t                xQosId,
                                cepId_t                xSrcCepId,
                                cepId_t                xDstCepId,
                                dtpConfig_t *          pxDtpCfg,
                                struct dtcpConfig_t *    pxDtcpCfg);
                                








#endif /* EFCP_H__*/


