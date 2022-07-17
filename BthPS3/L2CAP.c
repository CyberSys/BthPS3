/**********************************************************************************
 *                                                                                *
 * BthPS3 - Windows kernel-mode Bluetooth profile and bus driver                  *
 *                                                                                *
 * BSD 3-Clause License                                                           *
 *                                                                                *
 * Copyright (c) 2018-2022, Nefarius Software Solutions e.U.                      *
 * All rights reserved.                                                           *
 *                                                                                *
 * Redistribution and use in source and binary forms, with or without             *
 * modification, are permitted provided that the following conditions are met:    *
 *                                                                                *
 * 1. Redistributions of source code must retain the above copyright notice, this *
 *    list of conditions and the following disclaimer.                            *
 *                                                                                *
 * 2. Redistributions in binary form must reproduce the above copyright notice,   *
 *    this list of conditions and the following disclaimer in the documentation   *
 *    and/or other materials provided with the distribution.                      *
 *                                                                                *
 * 3. Neither the name of the copyright holder nor the names of its               *
 *    contributors may be used to endorse or promote products derived from        *
 *    this software without specific prior written permission.                    *
 *                                                                                *
 * THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDERS AND CONTRIBUTORS "AS IS"    *
 * AND ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE      *
 * IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR PURPOSE ARE *
 * DISCLAIMED. IN NO EVENT SHALL THE COPYRIGHT HOLDER OR CONTRIBUTORS BE LIABLE   *
 * FOR ANY DIRECT, INDIRECT, INCIDENTAL, SPECIAL, EXEMPLARY, OR CONSEQUENTIAL     *
 * DAMAGES (INCLUDING, BUT NOT LIMITED TO, PROCUREMENT OF SUBSTITUTE GOODS OR     *
 * SERVICES; LOSS OF USE, DATA, OR PROFITS; OR BUSINESS INTERRUPTION) HOWEVER     *
 * CAUSED AND ON ANY THEORY OF LIABILITY, WHETHER IN CONTRACT, STRICT LIABILITY,  *
 * OR TORT (INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY OUT OF THE USE  *
 * OF THIS SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF SUCH DAMAGE.           *
 *                                                                                *
 **********************************************************************************/


#include "Driver.h"
#include "L2CAP.tmh"


#pragma region L2CAP remote connection handling

 //
 // Calls L2CAP_PS3_HandleRemoteConnect at PASSIVE_LEVEL
 // 
_IRQL_requires_max_(DISPATCH_LEVEL)
VOID
L2CAP_PS3_HandleRemoteConnectAsync(
	_In_ WDFWORKITEM WorkItem
)
{
	PBTHPS3_REMOTE_CONNECT_CONTEXT connectCtx = NULL;

	FuncEntry(TRACE_L2CAP);

	connectCtx = GetRemoteConnectContext(WorkItem);

	(void)L2CAP_PS3_HandleRemoteConnect(
		connectCtx->ServerContext,
		&connectCtx->IndicationParameters
	);

	WdfObjectDelete(WorkItem);

	FuncExitNoReturn(TRACE_L2CAP);
}

//
// Control channel connection result
// 
void
L2CAP_PS3_ControlConnectResponseCompleted(
	_In_ WDFREQUEST  Request,
	_In_ WDFIOTARGET  Target,
	_In_ PWDF_REQUEST_COMPLETION_PARAMS  Params,
	_In_ WDFCONTEXT  Context
)
{
	NTSTATUS status;
	struct _BRB_L2CA_OPEN_CHANNEL* brb = NULL;
	PBTHPS3_PDO_CONTEXT pPdoCtx = NULL;

	UNREFERENCED_PARAMETER(Request);
	UNREFERENCED_PARAMETER(Target);

	status = Params->IoStatus.Status;

	FuncEntryArguments(TRACE_L2CAP, "status=%!STATUS!", status);


	brb = (struct _BRB_L2CA_OPEN_CHANNEL*)Context;
	pPdoCtx = brb->Hdr.ClientContext[0];

	//
	// Connection acceptance successful, channel ready to operate
	// 
	if (NT_SUCCESS(status))
	{
		WdfSpinLockAcquire(pPdoCtx->HidControlChannel.ConnectionStateLock);

		pPdoCtx->HidControlChannel.ConnectionState = ConnectionStateConnected;

		//
		// This will be set again once disconnect has occurred
		// 
		//KeClearEvent(&clientConnection->HidControlChannel.DisconnectEvent);

		WdfSpinLockRelease(pPdoCtx->HidControlChannel.ConnectionStateLock);

		TraceInformation(
			TRACE_L2CAP,
			"HID Control Channel connection established"
		);

		//
		// Channel connected, queues ready to start processing
		// 

		if (!NT_SUCCESS(status = WdfIoQueueReadyNotify(
			pPdoCtx->Queues.HidControlReadRequests,
			BthPS3_PDO_DispatchHidControlRead,
			pPdoCtx
		)))
		{
			TraceError(
				TRACE_L2CAP,
				"WdfIoQueueReadyNotify (HidControlReadRequests) failed with status %!STATUS!",
				status
			);

			//
			// TODO: better error handling
			// 
		}

		if (!NT_SUCCESS(status = WdfIoQueueReadyNotify(
			pPdoCtx->Queues.HidControlWriteRequests,
			BthPS3_PDO_DispatchHidControlWrite,
			pPdoCtx
		)))
		{
			TraceError(
				TRACE_L2CAP,
				"WdfIoQueueReadyNotify (HidControlWriteRequests) failed with status %!STATUS!",
				status
			);

			//
			// TODO: better error handling
			// 
		}
	}
	else
	{
		TraceError(
			TRACE_L2CAP,
			"HID Control Channel connection failed with status %!STATUS!",
			status
		);

		BthPS3_PDO_Destroy(pPdoCtx->DevCtxHdr, pPdoCtx);
	}

	FuncExitNoReturn(TRACE_L2CAP);
}

//
// Interrupt channel connection result
// 
void
L2CAP_PS3_InterruptConnectResponseCompleted(
	_In_ WDFREQUEST  Request,
	_In_ WDFIOTARGET  Target,
	_In_ PWDF_REQUEST_COMPLETION_PARAMS  Params,
	_In_ WDFCONTEXT  Context
)
{
	NTSTATUS status;
	struct _BRB_L2CA_OPEN_CHANNEL* brb = NULL;
	PBTHPS3_PDO_CONTEXT pPdoCtx = NULL;
	BTHPS3_CONNECTION_STATE controlState;

	UNREFERENCED_PARAMETER(Request);
	UNREFERENCED_PARAMETER(Target);

	status = Params->IoStatus.Status;

	FuncEntryArguments(TRACE_L2CAP, "status=%!STATUS!", status);


	brb = (struct _BRB_L2CA_OPEN_CHANNEL*)Context;
	pPdoCtx = brb->Hdr.ClientContext[0];

	//
	// Connection acceptance successful, channel ready to operate
	// 
	if (NT_SUCCESS(status))
	{
		WdfSpinLockAcquire(pPdoCtx->HidInterruptChannel.ConnectionStateLock);

		pPdoCtx->HidInterruptChannel.ConnectionState = ConnectionStateConnected;

		//
		// This will be set again once disconnect has occurred
		// 
		//KeClearEvent(&clientConnection->HidInterruptChannel.DisconnectEvent);

		WdfSpinLockRelease(pPdoCtx->HidInterruptChannel.ConnectionStateLock);

		TraceInformation(
			TRACE_L2CAP,
			"HID Interrupt Channel connection established"
		);

		//
		// Control channel is expected to be established by now
		// 
		WdfSpinLockAcquire(pPdoCtx->HidControlChannel.ConnectionStateLock);
		controlState = pPdoCtx->HidInterruptChannel.ConnectionState;
		WdfSpinLockRelease(pPdoCtx->HidControlChannel.ConnectionStateLock);

		if (controlState != ConnectionStateConnected)
		{
			TraceError(
				TRACE_L2CAP,
				"HID Control Channel in invalid state (0x%02X), dropping connection",
				controlState
			);

			goto failedDrop;
		}

		//
		// Channel connected, queues ready to start processing
		// 

		if (!NT_SUCCESS(status = WdfIoQueueReadyNotify(
			pPdoCtx->Queues.HidInterruptReadRequests,
			BthPS3_PDO_DispatchHidInterruptRead,
			pPdoCtx
		)))
		{
			TraceError(
				TRACE_L2CAP,
				"WdfIoQueueReadyNotify (HidInterruptReadRequests) failed with status %!STATUS!",
				status
			);

			//
			// TODO: better error handling
			// 
		}

		if (!NT_SUCCESS(status = WdfIoQueueReadyNotify(
			pPdoCtx->Queues.HidInterruptWriteRequests,
			BthPS3_PDO_DispatchHidInterruptWrite,
			pPdoCtx
		)))
		{
			TraceError(
				TRACE_L2CAP,
				"WdfIoQueueReadyNotify (HidInterruptWriteRequests) failed with status %!STATUS!",
				status
			);

			//
			// TODO: better error handling
			// 
		}
	}
	else
	{
		goto failedDrop;
	}

	FuncExitNoReturn(TRACE_L2CAP);

	return;

failedDrop:

	TraceError(
		TRACE_L2CAP,
		"Connection failed with status %!STATUS!",
		status
	);

	BthPS3_PDO_Destroy(pPdoCtx->DevCtxHdr, pPdoCtx);

	FuncExitNoReturn(TRACE_L2CAP);
}

#pragma endregion

#pragma region L2CAP deny connection request

//
// Deny an L2CAP connection request
// 
_IRQL_requires_max_(DISPATCH_LEVEL)
NTSTATUS
L2CAP_PS3_DenyRemoteConnect(
	_In_ PBTHPS3_SERVER_CONTEXT DevCtx,
	_In_ PINDICATION_PARAMETERS ConnectParams
)
{
	NTSTATUS status = STATUS_SUCCESS;
	WDFREQUEST brbAsyncRequest = NULL;
	struct _BRB_L2CA_OPEN_CHANNEL* brb = NULL;


	FuncEntry(TRACE_L2CAP);

	if (!NT_SUCCESS(status = WdfRequestCreate(
		WDF_NO_OBJECT_ATTRIBUTES,
		DevCtx->Header.IoTarget,
		&brbAsyncRequest)))
	{
		TraceError(
			TRACE_L2CAP,
			"WdfRequestCreate failed with status %!STATUS!",
			status
		);

		return status;
	}

	brb = (struct _BRB_L2CA_OPEN_CHANNEL*)
		DevCtx->Header.ProfileDrvInterface.BthAllocateBrb(
			BRB_L2CA_OPEN_CHANNEL_RESPONSE,
			POOLTAG_BTHPS3
		);

	if (brb == NULL)
	{
		status = STATUS_INSUFFICIENT_RESOURCES;

		TraceError(
			TRACE_L2CAP,
			"Failed to allocate brb BRB_L2CA_OPEN_CHANNEL_RESPONSE with status %!STATUS!",
			status
		);

		WdfObjectDelete(brbAsyncRequest);
		return status;
	}

	brb->Hdr.ClientContext[0] = DevCtx;

	brb->BtAddress = ConnectParams->BtAddress;
	brb->Psm = ConnectParams->Parameters.Connect.Request.PSM;
	brb->ChannelHandle = ConnectParams->ConnectionHandle;

	//
	// Drop connection
	// 
	brb->Response = CONNECT_RSP_RESULT_PSM_NEG;

	brb->ChannelFlags = CF_ROLE_EITHER;

	brb->ConfigOut.Flags = 0;
	brb->ConfigIn.Flags = 0;

	//
	// Submit response
	// 
	if (!NT_SUCCESS(status = BthPS3_SendBrbAsync(
		DevCtx->Header.IoTarget,
		brbAsyncRequest,
		(PBRB)brb,
		sizeof(*brb),
		L2CAP_PS3_DenyRemoteConnectCompleted,
		brb
	)))
	{
		TraceError(
			TRACE_L2CAP,
			"BthPS3_SendBrbAsync failed with status %!STATUS!",
			status
		);

		DevCtx->Header.ProfileDrvInterface.BthFreeBrb((PBRB)brb);
		WdfObjectDelete(brbAsyncRequest);
	}

	FuncExit(TRACE_L2CAP, "status=%!STATUS!", status);

	return status;
}

//
// Free resources used by deny connection response
// 
void
L2CAP_PS3_DenyRemoteConnectCompleted(
	_In_ WDFREQUEST  Request,
	_In_ WDFIOTARGET  Target,
	_In_ PWDF_REQUEST_COMPLETION_PARAMS  Params,
	_In_ WDFCONTEXT  Context
)
{
	PBTHPS3_SERVER_CONTEXT deviceCtx = NULL;
	struct _BRB_L2CA_OPEN_CHANNEL* brb = NULL;

	UNREFERENCED_PARAMETER(Target);


	FuncEntryArguments(TRACE_L2CAP, "status=%!STATUS!", Params->IoStatus.Status);

	brb = (struct _BRB_L2CA_OPEN_CHANNEL*)Context;
	deviceCtx = (PBTHPS3_SERVER_CONTEXT)brb->Hdr.ClientContext[0];
	deviceCtx->Header.ProfileDrvInterface.BthFreeBrb((PBRB)brb);
	WdfObjectDelete(Request);

	FuncExitNoReturn(TRACE_L2CAP);
}

#pragma endregion

#pragma region L2CAP remote disconnect

//
// Instructs a channel to disconnect
// 
_IRQL_requires_max_(DISPATCH_LEVEL)
BOOLEAN
L2CAP_PS3_RemoteDisconnect(
	_In_ PBTHPS3_DEVICE_CONTEXT_HEADER CtxHdr,
	_In_ BTH_ADDR RemoteAddress,
	_In_ PBTHPS3_CLIENT_L2CAP_CHANNEL Channel
)
{
	struct _BRB_L2CA_CLOSE_CHANNEL* disconnectBrb = NULL;

	FuncEntry(TRACE_L2CAP);

	WdfSpinLockAcquire(Channel->ConnectionStateLock);

	if (Channel->ConnectionState == ConnectionStateConnecting)
	{
		//
		// If the connection is not completed yet set the state 
		// to disconnecting.
		// In such case we should send CLOSE_CHANNEL Brb down after 
		// we receive connect completion.
		//

		Channel->ConnectionState = ConnectionStateDisconnecting;

		//
		// Clear event to indicate that we are in disconnecting
		// state. It will be set when disconnect is completed
		//
		KeClearEvent(&Channel->DisconnectEvent);

		WdfSpinLockRelease(Channel->ConnectionStateLock);
		return TRUE;
	}

	if (Channel->ConnectionState != ConnectionStateConnected)
	{
		//
		// Do nothing if we are not connected
		//

		WdfSpinLockRelease(Channel->ConnectionStateLock);
		FuncExit(TRACE_L2CAP, "returns=FALSE");
		return FALSE;
	}

	Channel->ConnectionState = ConnectionStateDisconnecting;
	WdfSpinLockRelease(Channel->ConnectionStateLock);

	//
	// We are now sending the disconnect, so clear the event.
	//

	KeClearEvent(&Channel->DisconnectEvent);

	CLIENT_CONNECTION_REQUEST_REUSE(Channel->ConnectDisconnectRequest);
	CtxHdr->ProfileDrvInterface.BthReuseBrb(
		&Channel->ConnectDisconnectBrb,
		BRB_L2CA_CLOSE_CHANNEL
	);

	disconnectBrb = (struct _BRB_L2CA_CLOSE_CHANNEL*)&(Channel->ConnectDisconnectBrb);

	disconnectBrb->BtAddress = RemoteAddress;
	disconnectBrb->ChannelHandle = Channel->ChannelHandle;

	//
	// The BRB can fail with STATUS_DEVICE_DISCONNECT if the device is already
	// disconnected, hence we don't assert for success
	//
	(void)BthPS3_SendBrbAsync(
		CtxHdr->IoTarget,
		Channel->ConnectDisconnectRequest,
		(PBRB)disconnectBrb,
		sizeof(*disconnectBrb),
		L2CAP_PS3_ChannelDisconnectCompleted,
		Channel
	);

	FuncExit(TRACE_L2CAP, "returns=TRUE");

	return TRUE;
}

//
// Gets called once a channel disconnect request has been completed
// 
void
L2CAP_PS3_ChannelDisconnectCompleted(
	_In_ WDFREQUEST Request,
	_In_ WDFIOTARGET Target,
	_In_ PWDF_REQUEST_COMPLETION_PARAMS Params,
	_In_ WDFCONTEXT Context
)
{
	PBTHPS3_CLIENT_L2CAP_CHANNEL channel = (PBTHPS3_CLIENT_L2CAP_CHANNEL)Context;

	UNREFERENCED_PARAMETER(Request);
	UNREFERENCED_PARAMETER(Target);

	FuncEntryArguments(TRACE_L2CAP, "status=%!STATUS!", Params->IoStatus.Status);

	WdfSpinLockAcquire(channel->ConnectionStateLock);
	channel->ConnectionState = ConnectionStateDisconnected;
	WdfSpinLockRelease(channel->ConnectionStateLock);

	//
	// Disconnect complete, set the event
	//
	KeSetEvent(
		&channel->DisconnectEvent,
		0,
		FALSE
	);

	FuncExitNoReturn(TRACE_L2CAP);
}

#pragma endregion
