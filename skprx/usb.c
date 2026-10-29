// vdb: the USB device (UDCD driver), its transfers and the takeover of the port
#include "vdb.h"
#include "usb_descriptors.h"

SceUID g_Evf = -1;
int g_bActive;
int g_bHostAttached;
static int s_bRegistered;
static int s_bWantPort;	// the bridge should own the port (set by start, cleared by stop)

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
static unsigned s_nLastRecvError;			// for the status overlay
static volatile int s_bLostByRecv;			// the host went away with a failed receive, not with a Detach
static unsigned s_nRetakes, s_nRetakeFailures, s_nLastRetakeResult;

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
	// right after an attach the cancel of the previous request can still
	// be in flight and the endpoint answers busy (0x80243007): wait it out,
	// up to about 100 ms
	for ( int nTry = 0; nTry < 50 && r < 0; nTry++ )
	{
		if ( nTry )
			ksceKernelDelayThread( 2000 );
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
			if ( r == SCE_UDCD_ERROR_BUS_DRIVER_NOT_STARTED )
			{
				// the cable is out, the bus is suspended or the controller
				// is down: the host is gone for now and the next Attach
				// queues again. 0.6.9 to 0.7.0 declared the port lost here
				// and retook it, which deactivated our own device in the
				// middle of the controller's cable-out handling and left
				// the controller stuck "in progress" until a cold boot
				s_nLastRecvError = (unsigned)r;
				g_bHostAttached = 0;
				s_bLostByRecv = 1;
				FileNotef( "vdb: ReqRecv(%d): bus driver not started, waiting for the next attach (usb state 0x%x, uptime %u s)", idx, (unsigned)ksceUdcdGetDeviceState(), Uptime() );
				return r;
			}
			s_nLastRecvError = (unsigned)r;
			if ( nTry == 0 || nTry == 49 )
				FileNotef( "vdb: ReqRecv(%d) failed 0x%08x (try %d)", idx, r, nTry + 1 );
		}
		else if ( nTry )
			FileNotef( "vdb: ReqRecv(%d) ok after %d tries", idx, nTry + 1 );
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
	int nLen = s_RxReq[idx].returnCode < 0 ? 0 : s_RxReq[idx].transmitted;
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
	// A USB suspend (the Mac asleep) fails the receive like a cable-out, but
	// the resume keeps the configuration: no SET_CONFIGURATION, no Attach.
	// 0.7.1 to 0.7.4 then waited for an attach forever and the bridge sat
	// deaf until the host reset it. Back to "established" and not suspended
	// after such a failure: the same path as an attach
	if ( s_bLostByRecv && g_bActive && !g_bHostAttached )
	{
		// (not when the system's own device took the port: an app's exit
		// fails our receive the same way, and the connection that is then
		// established is the system's; the retake handles that case)
		unsigned st = (unsigned)ksceUdcdGetDeviceState();
		int mtp = ksceUdcdGetDrvState( "USB_MTP_Driver" );
		if ( ( st & SCE_UDCD_STATUS_CONNECTION_ESTABLISHED ) && !( st & SCE_UDCD_STATUS_CONNECTION_SUSPENDED ) && ( st & SCE_UDCD_STATUS_CABLE_CONNECTED )
			&& !( mtp & SCE_UDCD_STATUS_ACTIVATED ) )
		{
			s_bLostByRecv = 0;
			FileNotef( "vdb: bus established again after a failed receive (usb state 0x%x, uptime %u s), resuming", st, Uptime() );
			ksceKernelSetEventFlag( g_Evf, EVF_CONNECTED );
			return;
		}
	}
	// (a completion of a cancelled, older request may have bumped the
	// counters: what matters is only that nothing is outstanding now)
	if ( g_bHostAttached && g_bActive && !s_bRxOutstanding )
	{
		FileNotef( "vdb: no receive outstanding, queued again (uptime %u s)", Uptime() );
		s_nRxProcessed = s_nRxCompleted;
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
// Charging: nothing to do here, and 0.4.3 to 0.6.12 did too much. SceUdcd
// owns USB charging for whatever class driver sits behind its controller
// driver: it tells Syscon the USB state itself (2 on connect, 3 when
// configured, 4 on suspend, 0 on teardown), asks ScePower for USB charging
// from the "USB Power Supply" system setting, and rewrites the descriptor's
// power fields from that setting (500 mA bus powered when on, 2 mA self
// powered when off). Our own kscePowerBatterySetUsbStatus calls duplicated
// the controller driver's, out of order with the real bus events. A host
// port never grants more than 500 mA, and kscePowerIsBatteryCharging is
// false on a host port by design (it needs Syscon's "power online"):
// CmdStatus reads kscePowerGetBatteryChargingStatus instead
//-----------------------------------------------------------------------------

//-----------------------------------------------------------------------------
// Standby. The USB service stops every controller when the system suspends
// and restarts the ones that were running at resume; an activate or stop
// from us inside that window fails (0x8024300E) or leaves the bus down, and
// the retake below used to fire exactly then ("controller not started").
// So: a system event handler marks the window, the thread stays off the
// UDCD calls while it is open, and a few seconds after the last resume
// event the normal retake check runs once. The handler itself only records
// (no file I/O: the card is suspended too)
//-----------------------------------------------------------------------------
static volatile int s_bSuspended;
static volatile unsigned s_nLastEventTick;	// Uptime() at the last suspend/resume event
static unsigned s_nSuspendEvent, s_nResumeEvent;	// first suspend id, last resume id, for the notes
static char s_SuspendNotes[96];

static int SysEventHandler( int resume, int eventid, void *args, void *opt )
{
	if ( !resume )
	{
		if ( !s_bSuspended )
			s_nSuspendEvent = (unsigned)eventid;
		s_bSuspended = 1;
	}
	else
		s_nResumeEvent = (unsigned)eventid;
	s_nLastEventTick = Uptime();
	return 0;
}

void UsbInstallSuspendHandler( void )
{
	int r = ksceKernelRegisterSysEventHandler( "vdb_suspend", SysEventHandler, NULL );
	FileNotef( "vdb: suspend handler 0x%08x", r );
}

int UsbSuspended( void )
{
	// resumed: resume events seen and three quiet seconds since the last one
	if ( s_bSuspended && s_nResumeEvent && Uptime() - s_nLastEventTick >= 3 )
	{
		s_bSuspended = 0;
		snprintf( s_SuspendNotes, sizeof( s_SuspendNotes ), "suspend 0x%x resume 0x%x", s_nSuspendEvent, s_nResumeEvent );
		s_nResumeEvent = 0;
		FileNotef( "vdb: resumed (%s, uptime %u s, usb state 0x%x)", s_SuspendNotes, Uptime(), (unsigned)ksceUdcdGetDeviceState() );
		Log( "vdb: resumed from standby (%s), usb state 0x%x", s_SuspendNotes, (unsigned)ksceUdcdGetDeviceState() );
	}
	return s_bSuspended;
}

// From the thread's 200 ms tick. The thread does not run while the system
// is really suspended, so a mark it keeps seeing for 25 of its own ticks
// with no resume event is a suspend the system started and abandoned (a
// standby that a USB event cut short, say): left set, it blocked every
// retake, and the bridge was lost when an app's exit took the port
void UsbSuspendTick( void )
{
	static unsigned s_nTicks;
	if ( !s_bSuspended || s_nResumeEvent )
	{
		s_nTicks = 0;
		return;
	}
	if ( s_nTicks++ == 0 )
		FileNotef( "vdb: suspend window open (event 0x%x, uptime %u s, usb state 0x%x)", s_nSuspendEvent, Uptime(), (unsigned)ksceUdcdGetDeviceState() );
	else if ( s_nTicks > 25 )
	{
		s_bSuspended = 0;
		s_nTicks = 0;
		snprintf( s_SuspendNotes, sizeof( s_SuspendNotes ), "suspend 0x%x abandoned", s_nSuspendEvent );
		FileNotef( "vdb: suspend 0x%x abandoned, no resume event in 5 s of running (uptime %u s, usb state 0x%x)", s_nSuspendEvent, Uptime(), (unsigned)ksceUdcdGetDeviceState() );
		Log( "vdb: suspend 0x%x abandoned by the system, retake check back on", s_nSuspendEvent );
	}
}

const char *UsbSuspendNotes( void )
{
	return s_SuspendNotes;
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
	// the host configured us (SceUdcd has already told Syscon so)
	s_bLostByRecv = 0;
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
	s_bLostByRecv = 0;
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
	s_bWantPort = 1;
	Log( "vdb: usb device active (pid 0x%04x)", VDB_USB_PID );
	return 0;
}

int vdb_usb_stop( void )
{
	s_bWantPort = 0;
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
// (The controller's state is read from the device state, bit 0x2000 =
// controller started: ksceUdcdGetDrvState("USBDeviceControllerDriver")
// returns -1, the controller is not in the class driver list, so the check
// 0.3.1 to 0.6.12 made on it never fired.) Never inside the suspend
// window, and at most once in five seconds
// What counts as lost, learned from the boot notes of 0.6.x and 0.7.0:
//  - the system activated its own device (an app launch re-activates the
//    Content Manager, MTP shows ACTIVATED): the full takeover is needed;
//  - the controller is up and the cable in, but no device is activated
//    (state DEACTIVATED, 0x100) for ten seconds: a plain activate suffices.
// Not lost: the cable out, the bus suspended or the controller stopped.
// Those are the controller's own transitions (cable-out handling, standby,
// resume); a deactivate or stop from us in the middle of them left the
// controller stuck "in progress" (0x80243006 forever, VitaShell's USB mode
// included) until a cold boot. A failed retake backs off (5 to 60 s) and
// after eight failures the thread leaves the port alone until a hotkey or
// host start
void UsbRetakeIfLost( void )
{
	static unsigned s_nLastRetake, s_nWait = 5, s_nDeactivatedSince;
	if ( !s_bWantPort || UsbSuspended() )
		return;
	int mtp = ksceUdcdGetDrvState( "USB_MTP_Driver" );
	unsigned st = (unsigned)ksceUdcdGetDeviceState();
	int bControllerUp = ( st & 0x2000 ) != 0;
	int bSystemTook = bControllerUp && ( mtp & SCE_UDCD_STATUS_ACTIVATED );
	int bNobodyActive = bControllerUp && ( st & SCE_UDCD_STATUS_DEACTIVATED ) && ( st & SCE_UDCD_STATUS_CABLE_CONNECTED ) && !( mtp & SCE_UDCD_STATUS_ACTIVATED );
	if ( !bNobodyActive )
		s_nDeactivatedSince = 0;
	else if ( !s_nDeactivatedSince )
		s_nDeactivatedSince = Uptime();
	int bReactivate = bNobodyActive && Uptime() - s_nDeactivatedSince >= 10;
	if ( !bSystemTook && !bReactivate )
	{
		if ( st & SCE_UDCD_STATUS_ACTIVATED )
		{
			s_nWait = 5;
			s_nRetakeFailures = 0;
		}
		return;
	}
	if ( Uptime() - s_nLastRetake < s_nWait )
		return;
	if ( s_nRetakeFailures >= 8 )
	{
		if ( s_nRetakeFailures == 8 )
			FileNotef( "vdb: port retake given up after 8 failures (usb state 0x%x, uptime %u s)", st, Uptime() );
		s_nRetakeFailures = 9;
		return;
	}
	s_nLastRetake = Uptime();
	s_nRetakes++;
	int r;
	if ( bSystemTook )
	{
		g_bActive = 0;
		g_bHostAttached = 0;
		PutAbort();
		r = vdb_usb_start();
	}
	else
	{
		ksceKernelClearEventFlag( g_Evf, ~EVF_ALL );
		r = ksceUdcdActivate( VDB_USB_PID );
		if ( r >= 0 )
			g_bActive = 1;
	}
	s_nLastRetakeResult = (unsigned)r;
	if ( r < 0 )
	{
		s_nRetakeFailures++;
		s_nWait = s_nWait < 60 ? s_nWait * 2 : 60;
	}
	Log( "vdb: the USB port was %s (mtp state 0x%x, usb state 0x%x), %s: 0x%08x", bSystemTook ? "taken by the system" : "left deactivated", mtp, st, bSystemTook ? "retaken" : "re-activated", r );
	FileNotef( "vdb: port %s (mtp 0x%x, usb state 0x%x): 0x%08x (uptime %u s, failures %u)", bSystemTook ? "retaken" : "re-activated", mtp, st, r, Uptime(), s_nRetakeFailures );
}

void UsbRetakeReset( void )
{
	s_nRetakeFailures = 0;
}

// Three lines for the status overlay (60 columns each)
void UsbStatusLines( char *pBuf, unsigned nSize )
{
	unsigned st = (unsigned)ksceUdcdGetDeviceState();
	int mtp = ksceUdcdGetDrvState( "USB_MTP_Driver" );
	snprintf( pBuf, nSize,
		"usb 0x%04x ctrl %s act %s cable %s conn %s mtp 0x%x\n"
		"bridge %s want %d host %s susp %s rx err 0x%08x\n"
		"retakes %u fail %u last 0x%08x %s\n",
		st, ( st & 0x2000 ) ? "up" : ( st & 0x1000 ) ? "down" : "?", ( st & SCE_UDCD_STATUS_ACTIVATED ) ? "y" : ( st & SCE_UDCD_STATUS_DEACTIVATED ) ? "n" : "?",
		( st & SCE_UDCD_STATUS_CABLE_CONNECTED ) ? "in" : ( st & SCE_UDCD_STATUS_CABLE_DISCONNECTED ) ? "out" : "?",
		( st & SCE_UDCD_STATUS_CONNECTION_ESTABLISHED ) ? "est" : ( st & SCE_UDCD_STATUS_CONNECTION_SUSPENDED ) ? "susp" : ( st & SCE_UDCD_STATUS_CONNECTION_NEW ) ? "new" : "?", (unsigned)mtp,
		g_bActive ? "on" : "off", s_bWantPort, g_bHostAttached ? "attached" : "none", s_bSuspended ? "yes" : "no", s_nLastRecvError,
		s_nRetakes, s_nRetakeFailures, s_nLastRetakeResult, s_SuspendNotes );
}
