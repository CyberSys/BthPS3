#include "Driver.h"
#include "L2CAP.Connect.tmh"

//
// Incoming connection request, prepare and send response
// 
_IRQL_requires_max_(DISPATCH_LEVEL)
NTSTATUS
L2CAP_PS3_HandleRemoteConnect(
	_In_ PBTHPS3_SERVER_CONTEXT DevCtx,
	_In_ PINDICATION_PARAMETERS ConnectParams
)
{
	NTSTATUS status = STATUS_UNSUCCESSFUL;
	struct _BRB_L2CA_OPEN_CHANNEL* brb = NULL;
	PFN_WDF_REQUEST_COMPLETION_ROUTINE completionRoutine = NULL;
	USHORT psm = ConnectParams->Parameters.Connect.Request.PSM;
	PBTHPS3_PDO_CONTEXT pPdoCtx = NULL;
	WDFREQUEST brbAsyncRequest = NULL;
	CHAR remoteName[BTH_MAX_NAME_SIZE];
	DS_DEVICE_TYPE deviceType = DS_DEVICE_TYPE_UNKNOWN;


	FuncEntry(TRACE_L2CAP);

	//
	// (Try to) refresh settings from registry
	// 
	(void)BthPS3_SettingsContextInit(DevCtx);

	//
	// Look for an existing connection object and reuse that
	// 
	status = BthPS3_PDO_RetrieveByBthAddr(
		DevCtx,
		ConnectParams->BtAddress,
		&pPdoCtx
	);

	//
	// This device apparently isn't connected, allocate new object
	// 
	if (status == STATUS_NOT_FOUND)
	{
		//
		// Request remote name from radio for device identification
		// 
		status = BthPS3_GetDeviceName(
			DevCtx->Header.IoTarget,
			ConnectParams->BtAddress,
			remoteName
		);

		if (NT_SUCCESS(status))
		{
			TraceInformation(
				TRACE_L2CAP,
				"Device %012llX name: %s",
				ConnectParams->BtAddress,
				remoteName
			);
		}
		else
		{
			TraceError(
				TRACE_L2CAP,
				"BTHPS3_GET_DEVICE_NAME failed with status %!STATUS!, dropping connection",
				status
			);

			//
			// Name couldn't be resolved, drop connection
			// 
			return L2CAP_PS3_DenyRemoteConnect(DevCtx, ConnectParams);
		}

		//
		// Distinguish device type based on reported remote name
		// 

		//
		// Check if PLAYSTATION(R)3 Controller
		// 
		if (DevCtx->Settings.IsSIXAXISSupported
			&& StringUtil_BthNameIsInCollection(remoteName, DevCtx->Settings.SIXAXISSupportedNames)) {
			deviceType = DS_DEVICE_TYPE_SIXAXIS;

			TraceInformation(
				TRACE_L2CAP,
				"Device %012llX identified as SIXAXIS compatible",
				ConnectParams->BtAddress
			);
		}

		//
		// Check if Navigation Controller
		// 
		if (DevCtx->Settings.IsNAVIGATIONSupported
			&& StringUtil_BthNameIsInCollection(remoteName, DevCtx->Settings.NAVIGATIONSupportedNames)) {
			deviceType = DS_DEVICE_TYPE_NAVIGATION;

			TraceInformation(
				TRACE_L2CAP,
				"Device %012llX identified as NAVIGATION compatible",
				ConnectParams->BtAddress
			);
		}

		//
		// Check if Motion Controller
		// 
		if (DevCtx->Settings.IsMOTIONSupported
			&& StringUtil_BthNameIsInCollection(remoteName, DevCtx->Settings.MOTIONSupportedNames)) {
			deviceType = DS_DEVICE_TYPE_MOTION;

			TraceInformation(
				TRACE_L2CAP,
				"Device %012llX identified as MOTION compatible",
				ConnectParams->BtAddress
			);
		}

		//
		// Check if Wireless Controller
		// 
		if (DevCtx->Settings.IsWIRELESSSupported
			&& StringUtil_BthNameIsInCollection(remoteName, DevCtx->Settings.WIRELESSSupportedNames)) {
			deviceType = DS_DEVICE_TYPE_WIRELESS;

			TraceInformation(
				TRACE_L2CAP,
				"Device %012llX identified as WIRELESS compatible",
				ConnectParams->BtAddress
			);
		}

		//
		// We were not able to identify, drop it
		// 
		if (deviceType == DS_DEVICE_TYPE_UNKNOWN)
		{
			TraceEvents(TRACE_LEVEL_WARNING,
				TRACE_L2CAP,
				"Device %012llX not identified or denied, dropping connection",
				ConnectParams->BtAddress
			);

			//
			// Filter re-routed potentially unsupported device, disable
			// 
			if (DevCtx->Settings.AutoDisableFilter)
			{
				status = BthPS3PSM_DisablePatchSync(
					DevCtx->PsmFilter.IoTarget,
					0
				);
				if (!NT_SUCCESS(status))
				{
					TraceError(
						TRACE_L2CAP,
						"BthPS3PSM_DisablePatchSync failed with status %!STATUS!", status);
				}
				else
				{
					TraceInformation(
						TRACE_L2CAP,
						"Filter disabled"
					);

					//
					// Fire off re-enable timer
					// 
					if (DevCtx->Settings.AutoEnableFilter)
					{
						TraceInformation(
							TRACE_L2CAP,
							"Filter disabled, re-enabling in %d seconds",
							DevCtx->Settings.AutoEnableFilterDelay
						);

						(void)WdfTimerStart(
							DevCtx->PsmFilter.AutoResetTimer,
							WDF_REL_TIMEOUT_IN_SEC(DevCtx->Settings.AutoEnableFilterDelay)
						);
					}
				}
			}

			//
			// Unsupported device, drop connection
			// 
			return L2CAP_PS3_DenyRemoteConnect(DevCtx, ConnectParams);
		}

		//
		// Allocate new connection object
		// 
		status = BthPS3_PDO_Create(
			DevCtx,
			ConnectParams->BtAddress,
			deviceType,
			EvtClientConnectionsDestroyConnection,
			&pPdoCtx
		);

		if (!NT_SUCCESS(status)) {
			TraceError(
				TRACE_L2CAP,
				"ClientConnections_CreateAndInsert failed with status %!STATUS!", status);
			goto exit;
		}

		//
		// Store device type (required to later spawn the right PDO)
		// 
		pPdoCtx->DeviceType = deviceType;

		//
		// Store remote name in connection context
		// 
		status = RtlUnicodeStringPrintf(&pPdoCtx->RemoteName, L"%hs", remoteName);

		if (!NT_SUCCESS(status)) {
			TraceError(
				TRACE_L2CAP,
				"RtlUnicodeStringPrintf failed with status %!STATUS!", status);
			goto exit;
		}
	}

	//
	// Adjust control flow depending on PSM
	// 
	switch (psm)
	{
	case PSM_DS3_HID_CONTROL:
		completionRoutine = L2CAP_PS3_ControlConnectResponseCompleted;
		pPdoCtx->HidControlChannel.ChannelHandle = ConnectParams->ConnectionHandle;
		brbAsyncRequest = pPdoCtx->HidControlChannel.ConnectDisconnectRequest;
		brb = (struct _BRB_L2CA_OPEN_CHANNEL*)&(pPdoCtx->HidControlChannel.ConnectDisconnectBrb);
		break;
	case PSM_DS3_HID_INTERRUPT:
		completionRoutine = L2CAP_PS3_InterruptConnectResponseCompleted;
		pPdoCtx->HidInterruptChannel.ChannelHandle = ConnectParams->ConnectionHandle;
		brbAsyncRequest = pPdoCtx->HidInterruptChannel.ConnectDisconnectRequest;
		brb = (struct _BRB_L2CA_OPEN_CHANNEL*)&(pPdoCtx->HidInterruptChannel.ConnectDisconnectBrb);
		break;
	default:
		// Doesn't happen
		break;
	}

	CLIENT_CONNECTION_REQUEST_REUSE(brbAsyncRequest);
	DevCtx->Header.ProfileDrvInterface.BthReuseBrb((PBRB)brb, BRB_L2CA_OPEN_CHANNEL_RESPONSE);

	//
	// Pass connection object along as context
	// 
	brb->Hdr.ClientContext[0] = pPdoCtx;

	brb->BtAddress = ConnectParams->BtAddress;
	brb->Psm = psm;
	brb->ChannelHandle = ConnectParams->ConnectionHandle;
	brb->Response = CONNECT_RSP_RESULT_SUCCESS;

	brb->ChannelFlags = CF_ROLE_EITHER;

	brb->ConfigOut.Flags = 0;
	brb->ConfigIn.Flags = 0;

	//
	// Set expected and preferred MTU to max value
	// 
	brb->ConfigOut.Flags |= CFG_MTU;
	brb->ConfigOut.Mtu.Max = L2CAP_MAX_MTU;
	brb->ConfigOut.Mtu.Min = L2CAP_MIN_MTU;
	brb->ConfigOut.Mtu.Preferred = L2CAP_MAX_MTU;

	brb->ConfigIn.Flags = CFG_MTU;
	brb->ConfigIn.Mtu.Max = brb->ConfigOut.Mtu.Max;
	brb->ConfigIn.Mtu.Min = brb->ConfigOut.Mtu.Min;
	brb->ConfigIn.Mtu.Preferred = brb->ConfigOut.Mtu.Preferred;

	//
	// Remaining L2CAP defaults
	// 
	brb->ConfigOut.FlushTO.Max = L2CAP_DEFAULT_FLUSHTO;
	brb->ConfigOut.FlushTO.Min = L2CAP_MIN_FLUSHTO;
	brb->ConfigOut.FlushTO.Preferred = L2CAP_DEFAULT_FLUSHTO;
	brb->ConfigOut.ExtraOptions = 0;
	brb->ConfigOut.NumExtraOptions = 0;
	brb->ConfigOut.LinkTO = 0;

	//
	// Max count of MTUs to stay buffered until discarded
	// 
	brb->IncomingQueueDepth = 10;

	//
	// Get notifications about disconnect and QOS
	//
	brb->CallbackFlags = CALLBACK_DISCONNECT | CALLBACK_CONFIG_QOS;
	brb->Callback = &L2CAP_PS3_ConnectionIndicationCallback;
	brb->CallbackContext = pPdoCtx;
	brb->ReferenceObject = (PVOID)WdfDeviceWdmGetDeviceObject(DevCtx->Header.Device);

	//
	// Submit response
	// 
	if (!NT_SUCCESS(status = BthPS3_SendBrbAsync(
		DevCtx->Header.IoTarget,
		brbAsyncRequest,
		(PBRB)brb,
		sizeof(*brb),
		completionRoutine,
		brb
	)))
	{
		TraceError(
			TRACE_L2CAP,
			"BthPS3_SendBrbAsync failed with status %!STATUS!",
			status
		);
	}

exit:

	//
	// TODO: handle intermediate disconnects
	// 
	if (!NT_SUCCESS(status) && pPdoCtx)
	{
		BthPS3_PDO_Destroy(&DevCtx->Header, pPdoCtx);
	}

	FuncExit(TRACE_L2CAP, "status=%!STATUS!", status);

	return status;
}
