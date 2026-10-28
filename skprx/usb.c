// vdb: the USB device (UDCD driver), its transfers and the takeover of the port
#include "vdb.h"
#include "usb_descriptors.h"

SceUID g_Evf = -1;
int g_bActive;
int g_bHostAttached;
static int s_bRegistered;

// Buffers. The receive buffers (two, one request outstanding at a time:
// when frame N has arrived in one, the next receive is queued into the
// other before frame N is written to the card, so USB fills N+1 while the
// card takes N) and the reply buffer come from a physically contiguous,
// uncached kernel block, as udcd_uvc allocates its USB frame: the USB
// controller addresses them by physical address. A second buffer in the
// module's own .bss (0.3.4 to 0.3.6) was never filled by the controller
// and the host's writes timed out; the single static buffer of 0.3.3 only
// worked by placement. Two requests queued at once hung the endpoint, hence
// one request object, reused
#define RX_BUFS 2
static unsigned char s_TxBuf[CHUNK_SIZE] __attribute__(( aligned( 64 ) ));		// log chunks, EP1 (small, static has worked throughout)
static unsigned char s_RxBufStatic[RX_SIZE] __attribute__(( aligned( 64 ) ));	// fallback when the block cannot be allocated
static unsigned char s_DataBufStatic[sizeof( VdbRspHdr_t ) + 4096] __attribute__(( aligned( 64 ) ));
static unsigned char *s_pRxBuf[RX_BUFS] = { s_RxBufStatic, s_RxBufStatic };
static int s_nRxBufs = 1;
static unsigned char *s_DataBuf = s_DataBufStatic;
static unsigned s_nDataChunk = 4096;
static SceUID s_Block = -1;

static SceUdcdDeviceRequest s_TxReq, s_DataReq;
// one request object per receive buffer: re-queuing the object that has just
// completed, from its own completion, was refused now and then
// (0x80243007), which left no receive outstanding and the host's writes
// failing until it re-attached
static SceUdcdDeviceRequest s_RxReq[RX_BUFS];
static int s_nRxQueuedIdx;
static volatile int s_bRxOutstanding;
static volatile unsigned s_nRxCompleted;	// receives finished (callback)
static unsigned s_nRxProcessed;				// receives handled by the thread

//-----------------------------------------------------------------------------
// Transfer completions (UDCD callbacks) and the transfers
//-----------------------------------------------------------------------------
static void OnSent( SceUdcdDeviceRequest *req )
{
	ksceKernelSetEventFlag( g_Evf, EVF_SENT );
}

static void OnReceived( SceUdcdDeviceRequest *req )
{
	s_bRxOutstanding = 0;
	s_nRxCompleted++;
	ksceKernelSetEventFlag( g_Evf, EVF_RECEIVED );
}

static void OnDataSent( SceUdcdDeviceRequest *req )
{
	ksceKernelSetEventFlag( g_Evf, EVF_DSENT );
}

static int SendChunk( unsigned nLen )
{
	ksceKernelDcacheCleanRange( s_TxBuf, sizeof( s_TxBuf ) );
	memset( &s_TxReq, 0, sizeof( s_TxReq ) );
	s_TxReq.endpoint = &endpoints[VDB_EP_IN];
	s_TxReq.data = s_TxBuf;
	s_TxReq.size = nLen;
	s_TxReq.onComplete = OnSent;
	return ksceUdcdReqSend( &s_TxReq );
}

static int QueueReceive( int idx )
{
	// USB DMA writes the frame behind the CPU's back: drop any cached copy
	// of the buffer before the transfer (and again after it), or the CPU
	// re-reads the previous command. Seen on hardware: the first command
	// worked, every later one re-ran the old one with a garbage path
	ksceKernelDcacheInvalidateRange( s_pRxBuf[idx], RX_SIZE );
	SceUdcdDeviceRequest *pReq = &s_RxReq[idx];
	int r = -1;
	for ( int nTry = 0; nTry < 5 && r < 0; nTry++ )
	{
		if ( nTry )
			ksceKernelDelayThread( 200 );
		memset( pReq, 0, sizeof( *pReq ) );
		pReq->endpoint = &endpoints[VDB_EP_OUT];
		pReq->data = s_pRxBuf[idx];
		pReq->size = RX_SIZE;
		pReq->onComplete = OnReceived;
		s_nRxQueuedIdx = idx;
		s_bRxOutstanding = 1;
		r = ksceUdcdReqRecv( pReq );
		if ( r < 0 )
		{
			s_bRxOutstanding = 0;
			FileNotef( "vdb: ReqRecv(%d) failed 0x%08x (try %d)", idx, r, nTry + 1 );
		}
	}
	return r;
}

void UsbStartReceiving( void )
{
	s_nRxCompleted = 0;
	s_nRxProcessed = 0;
	QueueReceive( 0 );
}

int UsbTakeFrame( unsigned char **ppFrame )
{
	if ( s_nRxProcessed == s_nRxCompleted )
		return 0;
	// the one outstanding request finished into buffer idx: take its
	// length, queue the other buffer, hand idx out while USB fills the other
	int idx = s_nRxQueuedIdx;
	int nLen = s_RxReq[idx].transmitted;
	s_nRxProcessed = s_nRxCompleted;
	QueueReceive( ( idx + 1 ) % s_nRxBufs );
	ksceKernelDcacheInvalidateRange( s_pRxBuf[idx], RX_SIZE );
	*ppFrame = s_pRxBuf[idx];
	return nLen;
}

// From the thread tick: with a host attached there must always be a receive
// outstanding, or the host's next command times out
void UsbWatchdog( void )
{
	if ( g_bHostAttached && g_bActive && !s_bRxOutstanding && s_nRxProcessed == s_nRxCompleted )
	{
		FileNotef( "vdb: no receive outstanding, queued again (uptime %u s)", Uptime() );
		QueueReceive( ( s_nRxQueuedIdx + 1 ) % s_nRxBufs );
	}
}

int UsbStreamLog( void )
{
	unsigned n = RingTake( s_TxBuf, CHUNK_SIZE );
	if ( !n )
		return 0;
	return SendChunk( n ) >= 0;
}

//-----------------------------------------------------------------------------
// Replies on EP3. Sent synchronously from the USB thread: one frame in
// flight, wait for its completion (3 s), so a file streams as a sequence of
// frames with USB flow control and nothing else to buffer. The log ring
// pauses meanwhile and catches up afterwards (nothing is lost under 64 KB)
//-----------------------------------------------------------------------------
unsigned char *ReplyPayload( void )
{
	return s_DataBuf + sizeof( VdbRspHdr_t );
}

unsigned ReplyCapacity( void )
{
	return s_nDataChunk;
}

int SendReply( unsigned nCmd, int nStatus, unsigned nLen )
{
	VdbRspHdr_t *pHdr = (VdbRspHdr_t *)s_DataBuf;
	if ( nLen > s_nDataChunk )
		nLen = s_nDataChunk;
	pHdr->magic = VDB_RSP_MAGIC;
	pHdr->cmd = nCmd;
	pHdr->status = nStatus;
	pHdr->len = nLen;
	ksceKernelClearEventFlag( g_Evf, ~EVF_DSENT );
	ksceKernelDcacheCleanRange( s_DataBuf, sizeof( VdbRspHdr_t ) + nLen );
	memset( &s_DataReq, 0, sizeof( s_DataReq ) );
	s_DataReq.endpoint = &endpoints[VDB_EP_DATA];
	s_DataReq.data = s_DataBuf;
	s_DataReq.size = sizeof( VdbRspHdr_t ) + nLen;
	s_DataReq.onComplete = OnDataSent;
	int ret = ksceUdcdReqSend( &s_DataReq );
	if ( ret < 0 )
		return ret;
	unsigned bits = 0;
	SceUInt to = 3 * 1000 * 1000;
	ret = ksceKernelWaitEventFlag( g_Evf, EVF_DSENT, SCE_EVENT_WAITOR | SCE_EVENT_WAITCLEAR_PAT, &bits, &to );
	if ( ret < 0 )
	{
		ksceUdcdReqCancelAll( &endpoints[VDB_EP_DATA] );
		return ret;
	}
	return s_DataReq.returnCode < 0 ? s_DataReq.returnCode : 0;
}

//-----------------------------------------------------------------------------
// Charging. The system's USB stack asks ScePower for USB charging for its
// own devices; with ours in place nobody did, and a night on the bridge
// drained the battery from 92 to 24 percent. ScePower's own charging flag
// stays off on the bridge, the capacity nevertheless rises
//-----------------------------------------------------------------------------
void UsbChargeNudge( void )
{
	if ( g_bHostAttached && !kscePowerIsBatteryCharging() )
	{
		kscePowerBatterySetUsbStatus( SCE_POWER_BATTERY_USB_STATUS_CONFIGURED );
		kscePowerBatteryEnableUsbCharging();
	}
}

//-----------------------------------------------------------------------------
// UDCD driver callbacks
//-----------------------------------------------------------------------------
static int ProcessRequest( int recipient, int arg, SceUdcdEP0DeviceRequest *req, void *user_data )
{
	if ( arg < 0 )
		return -1;
	// GET_DESCRIPTOR(STRING): answer with our one string
	if ( ( req->bmRequestType & 0x80 ) && req->bRequest == USB_REQ_GET_DESCRIPTOR && ( ( req->wValue >> 8 ) & 0xFF ) == USB_DT_STRING )
	{
		static SceUdcdDeviceRequest sreq;
		memset( &sreq, 0, sizeof( sreq ) );
		sreq.endpoint = &endpoints[0];
		sreq.data = &string_descriptors[0];
		sreq.size = sizeof( string_descriptors[0] );
		return ksceUdcdReqSend( &sreq );
	}
	return 0;
}

static int ChangeSetting( int interfaceNumber, int alternateSetting, int bus )
{
	return 0;
}

static int Attach( int usb_version, void *user_data )
{
	// the host configured us
	kscePowerBatterySetUsbStatus( SCE_POWER_BATTERY_USB_STATUS_CONFIGURED );
	kscePowerBatteryEnableUsbCharging();
	kscePowerBatteryPermitCharging();	// in case the system's USB teardown left a forbid count behind
	ksceUdcdReqCancelAll( &endpoints[VDB_EP_IN] );
	ksceUdcdClearFIFO( &endpoints[VDB_EP_IN] );
	ksceUdcdReqCancelAll( &endpoints[VDB_EP_OUT] );
	ksceUdcdClearFIFO( &endpoints[VDB_EP_OUT] );
	ksceUdcdReqCancelAll( &endpoints[VDB_EP_DATA] );
	ksceUdcdClearFIFO( &endpoints[VDB_EP_DATA] );
	ksceKernelSetEventFlag( g_Evf, EVF_CONNECTED );
	return 0;
}

static void Detach( void *user_data )
{
	kscePowerBatterySetUsbStatus( SCE_POWER_BATTERY_USB_STATUS_ACTIVE );
	ksceKernelSetEventFlag( g_Evf, EVF_DISCONNECTED );
}

static void Configure( int usb_version, int desc_count, SceUdcdInterfaceSettings *settings, void *user_data )
{
}

static int DriverStart( int size, void *args, void *user_data )
{
	return 0;
}

static int DriverStop( int size, void *args, void *user_data )
{
	return 0;
}

static SceUdcdDriver s_Driver = {
	VDB_DRIVER_NAME,
	4,
	&endpoints[0],
	&interfaces[0],
	&devdesc_hi,
	&config_hi,
	&devdesc_full,
	&config_full,
	&string_descriptors[0],
	&string_descriptors[0],
	&string_descriptors[0],
	&ProcessRequest,
	&ChangeSetting,
	&Attach,
	&Detach,
	&Configure,
	&DriverStart,
	&DriverStop,
	NULL,
	0,
	NULL
};

//-----------------------------------------------------------------------------
// Registration, buffers, takeover of the port
//-----------------------------------------------------------------------------
int UsbRegister( void )
{
	int r = ksceUdcdRegister( &s_Driver );
	if ( r < 0 )
		return r;
	s_bRegistered = 1;

	SceKernelAllocMemBlockKernelOpt opt;
	memset( &opt, 0, sizeof( opt ) );
	opt.size = sizeof( opt );
	opt.attr = SCE_KERNEL_ALLOC_MEMBLOCK_ATTR_PHYCONT | SCE_KERNEL_ALLOC_MEMBLOCK_ATTR_HAS_ALIGNMENT;
	opt.alignment = 4 * 1024;
	const unsigned nDataBuf = ( sizeof( VdbRspHdr_t ) + DATA_CHUNK + 4095 ) & ~4095u;
	s_Block = ksceKernelAllocMemBlock( "vdb_usb", 0x10208006, RX_BUFS * RX_SIZE + nDataBuf, &opt );
	void *pBase = NULL;
	if ( s_Block >= 0 && ksceKernelGetMemBlockBase( s_Block, &pBase ) >= 0 && pBase )
	{
		for ( int i = 0; i < RX_BUFS; i++ )
			s_pRxBuf[i] = (unsigned char *)pBase + i * RX_SIZE;
		s_nRxBufs = RX_BUFS;
		s_DataBuf = (unsigned char *)pBase + RX_BUFS * RX_SIZE;
		s_nDataChunk = DATA_CHUNK;
	}
	FileNotef( "vdb: usb buffers: block 0x%08x base %p, %d receive buffers", (unsigned)s_Block, pBase, s_nRxBufs );
	return 0;
}

void UsbUnregister( void )
{
	if ( s_Block >= 0 )
		ksceKernelFreeMemBlock( s_Block );
	if ( s_bRegistered )
		ksceUdcdUnregister( &s_Driver );
	s_bRegistered = 0;
}

int vdb_usb_start( void )
{
	int ret;
	if ( !s_bRegistered )
		return -1;
	if ( g_bActive )
		return 0;
	ret = ksceUdcdDeactivate();
	FileNotef( "vdb: deactivate 0x%08x", ret );
	if ( ret < 0 && ret != SCE_UDCD_ERROR_INVALID_ARGUMENT )
		return ret;
	ret = ksceUdcdStop( "USB_MTP_Driver", 0, NULL );
	FileNotef( "vdb: stop mtp 0x%08x", ret );
	ksceUdcdStop( "USBPSPCommunicationDriver", 0, NULL );
	ksceUdcdStop( "USBSerDriver", 0, NULL );
	ret = ksceUdcdStop( "USBDeviceControllerDriver", 0, NULL );
	FileNotef( "vdb: stop controller 0x%08x", ret );
	ret = ksceUdcdStart( "USBDeviceControllerDriver", 0, NULL );
	FileNotef( "vdb: start controller 0x%08x", ret );
	if ( ret < 0 )
		return ret;
	ret = ksceUdcdStart( VDB_DRIVER_NAME, 0, NULL );
	FileNotef( "vdb: start vdb driver 0x%08x", ret );
	if ( ret < 0 )
	{
		ksceUdcdStop( "USBDeviceControllerDriver", 0, NULL );
		return ret;
	}
	ksceKernelClearEventFlag( g_Evf, ~EVF_ALL );
	ret = ksceUdcdActivate( VDB_USB_PID );
	FileNotef( "vdb: activate 0x%08x", ret );
	if ( ret < 0 )
	{
		ksceUdcdStop( VDB_DRIVER_NAME, 0, NULL );
		ksceUdcdStop( "USBDeviceControllerDriver", 0, NULL );
		return ret;
	}
	g_bActive = 1;
	kscePowerBatteryEnableUsbCharging();
	Log( "vdb: usb device active (pid 0x%04x)", VDB_USB_PID );
	return 0;
}

int vdb_usb_stop( void )
{
	if ( !g_bActive )
		return 0;
	ksceKernelSetEventFlag( g_Evf, EVF_DISCONNECTED );
	ksceUdcdDeactivate();
	ksceUdcdStop( VDB_DRIVER_NAME, 0, NULL );
	ksceUdcdStop( "USBDeviceControllerDriver", 0, NULL );
	// back to the system's MTP device, as vitastick does
	ksceUdcdStart( "USBDeviceControllerDriver", 0, NULL );
	ksceUdcdStart( "USB_MTP_Driver", 0, NULL );
	ksceUdcdActivate( 0x4E4 );
	g_bActive = 0;
	return 0;
}

// Starting an app makes the system re-activate its own USB device (0x4E4,
// Content Manager) and ours silently goes away: the first launch from the
// host left the Vita unreachable. Called once a second while we believe we
// own the port. (Requiring our own driver to report ACTIVATED as well, as
// 0.4.2 did, retook the port every second: that flag is never set for it)
void UsbRetakeIfLost( void )
{
	if ( !g_bActive )
		return;
	int mtp = ksceUdcdGetDrvState( "USB_MTP_Driver" );
	if ( !( mtp & SCE_UDCD_STATUS_ACTIVATED ) )
		return;
	g_bActive = 0;
	g_bHostAttached = 0;
	PutAbort();
	int r = vdb_usb_start();
	Log( "vdb: the system took the USB port (mtp state 0x%x), retaken: 0x%08x", mtp, r );
	FileNotef( "vdb: port retaken after the system took it: 0x%08x (uptime %u s)", r, Uptime() );
}
