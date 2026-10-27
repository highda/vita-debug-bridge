// vdb: Vita debug bridge, kernel side (MVP).
//
// A vendor USB device with one bulk IN pipe that streams a 64 KB log ring
// to the host, and one bulk OUT pipe for commands. Writers: the vdb_write
// syscall (any user app) and this
// module's own heartbeat. Activated at boot only when the flag file
// ux0:vdbplugin/usb_on exists (VitaShell's USB mass storage keeps working
// otherwise), or from user space with vdb_usb_start().
//
// Host: host/vdb.py (pyusb), Sony VID 0x054C, PID VDB_USB_PID.
//
// Structure after xerpi's vitastick (UDCD driver skeleton), rewritten.

#include <psp2kern/kernel/modulemgr.h>
#include <psp2kern/kernel/threadmgr.h>
#include <psp2kern/kernel/sysmem.h>
#include <psp2kern/kernel/sysclib.h>
#include <psp2kern/kernel/cpu.h>
#include <psp2kern/io/fcntl.h>
#include <psp2kern/udcd.h>
#include <psp2kern/ctrl.h>
#include <stdarg.h>
#include "usb_descriptors.h"

#define VDB_DRIVER_NAME	"VDB"
#define VDB_USB_PID		0x0DB6
#define VDB_VERSION		"0.1"
#define VDB_FLAG_FILE	"ux0:vdbplugin/usb_on"

#define RING_SIZE		( 64 * 1024 )
#define CHUNK_SIZE		4096
#define RX_SIZE			64

enum
{
	EVF_CONNECTED		= 1 << 0,
	EVF_DISCONNECTED	= 1 << 1,
	EVF_DATA			= 1 << 2,
	EVF_SENT			= 1 << 3,
	EVF_RECEIVED		= 1 << 4,
	EVF_EXIT			= 1 << 5,
	EVF_ALL				= 0x3F
};

static char s_Ring[RING_SIZE];
static unsigned s_nRingWrite, s_nRingRead;	// monotonic byte counters; valid = write - read
static unsigned s_nDropped;
static unsigned char s_TxBuf[CHUNK_SIZE] __attribute__(( aligned( 64 ) ));
static unsigned char s_RxBuf[RX_SIZE] __attribute__(( aligned( 64 ) ));
static SceUID s_Evf = -1;
static SceUID s_Thread = -1;
static int s_bRegistered, s_bActive;

//-----------------------------------------------------------------------------
// Log ring
//-----------------------------------------------------------------------------
static void RingWrite( const char *pData, unsigned nLen )
{
	int intr = ksceKernelCpuSuspendIntr();
	unsigned nUsed = s_nRingWrite - s_nRingRead;
	if ( nLen > RING_SIZE - nUsed )
	{
		s_nDropped += nLen;
		ksceKernelCpuResumeIntr( intr );
		return;
	}
	for ( unsigned i = 0; i < nLen; i++ )
		s_Ring[( s_nRingWrite + i ) % RING_SIZE] = pData[i];
	s_nRingWrite += nLen;
	ksceKernelCpuResumeIntr( intr );
	if ( s_Evf >= 0 )
		ksceKernelSetEventFlag( s_Evf, EVF_DATA );
}

static void Log( const char *fmt, ... )
{
	char line[256];
	va_list args;
	va_start( args, fmt );
	int n = vsnprintf( line, sizeof( line ) - 1, fmt, args );
	va_end( args );
	if ( n < 0 )
		return;
	if ( n > (int)sizeof( line ) - 2 )
		n = sizeof( line ) - 2;
	line[n++] = '\n';
	RingWrite( line, n );
}

// bytes ready, copied into the transmit buffer (interrupts off for the copy)
static unsigned RingTake( unsigned char *pDest, unsigned nMax )
{
	int intr = ksceKernelCpuSuspendIntr();
	unsigned nUsed = s_nRingWrite - s_nRingRead;
	unsigned n = nUsed < nMax ? nUsed : nMax;
	for ( unsigned i = 0; i < n; i++ )
		pDest[i] = s_Ring[( s_nRingRead + i ) % RING_SIZE];
	s_nRingRead += n;
	ksceKernelCpuResumeIntr( intr );
	return n;
}

// A line in ux0:vdbplugin/boot.txt: the only trace when USB doesn't come up
static void FileNote( const char *pszText )
{
	SceUID fd = ksceIoOpen( "ux0:vdbplugin/boot.txt", SCE_O_WRONLY | SCE_O_CREAT | SCE_O_APPEND, 0777 );
	if ( fd < 0 )
		return;
	ksceIoWrite( fd, pszText, strlen( pszText ) );
	ksceIoClose( fd );
}

//-----------------------------------------------------------------------------
// USB requests
//-----------------------------------------------------------------------------
static void OnSent( SceUdcdDeviceRequest *req )
{
	ksceKernelSetEventFlag( s_Evf, EVF_SENT );
}

static void OnReceived( SceUdcdDeviceRequest *req )
{
	ksceKernelSetEventFlag( s_Evf, EVF_RECEIVED );
}

static SceUdcdDeviceRequest s_TxReq;
static SceUdcdDeviceRequest s_RxReq;

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

static int QueueReceive( void )
{
	memset( &s_RxReq, 0, sizeof( s_RxReq ) );
	s_RxReq.endpoint = &endpoints[VDB_EP_OUT];
	s_RxReq.data = s_RxBuf;
	s_RxReq.size = RX_SIZE;
	s_RxReq.onComplete = OnReceived;
	return ksceUdcdReqRecv( &s_RxReq );
}

// Commands from the host, one byte: 'P' ping, 'S' status
static void HandleCommand( const unsigned char *pCmd, int nLen )
{
	if ( nLen < 1 )
		return;
	switch ( pCmd[0] )
	{
	case 'P':
		Log( "vdb: pong" );
		break;
	case 'S':
		Log( "vdb: status: ring %u used of %u, dropped %u, uptime %u s", s_nRingWrite - s_nRingRead, RING_SIZE, s_nDropped,
			(unsigned)( ksceKernelGetSystemTimeWide() / 1000000 ) );
		break;
	default:
		Log( "vdb: unknown command 0x%02x (%d bytes)", pCmd[0], nLen );
		break;
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
	ksceUdcdReqCancelAll( &endpoints[VDB_EP_IN] );
	ksceUdcdClearFIFO( &endpoints[VDB_EP_IN] );
	ksceUdcdReqCancelAll( &endpoints[VDB_EP_OUT] );
	ksceUdcdClearFIFO( &endpoints[VDB_EP_OUT] );
	ksceKernelSetEventFlag( s_Evf, EVF_CONNECTED );
	return 0;
}

static void Detach( void *user_data )
{
	ksceKernelSetEventFlag( s_Evf, EVF_DISCONNECTED );
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
	3,
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
// USB thread: streams the ring while a host is attached, heartbeat every 5 s
//-----------------------------------------------------------------------------
static int s_bAutoStart;	// ux0:vdbplugin/usb_on existed at boot
int vdb_usb_start( void );
int vdb_usb_stop( void );

static int UsbThread( SceSize args, void *argp )
{
	int bConnected = 0, bSending = 0;
	unsigned nBeats = 0;
	SceUInt nTimeout = 5 * 1000 * 1000;

	// Taking the USB port during boot (at 13 s uptime) killed the boot
	// three times: SceShell brings up its own USB services (MTP) after
	// that and the two collided. As udcd_uvc does, wait until the system's
	// MTP driver is activated (the shell is up), then take over
	if ( s_bAutoStart )
	{
		SceUdcdWaitParam wp;
		memset( &wp, 0, sizeof( wp ) );
		wp.status = SCE_UDCD_STATUS_ACTIVATED;
		wp.driverName = "USB_MTP_Driver";
		FileNote( "vdb: thread up, waiting for the MTP driver (30 s max)\n" );
		int w = ksceUdcdWaitState( &wp, 30 * 1000 * 1000 );
		{
			char n2[96];
			snprintf( n2, sizeof( n2 ), "vdb: mtp wait returned 0x%08x at uptime %u s, pausing 3 s\n", w, (unsigned)( ksceKernelGetSystemTimeWide() / 1000000 ) );
			FileNote( n2 );
		}
		ksceKernelDelayThread( 3 * 1000 * 1000 );
		int ret = vdb_usb_start();
		Log( "vdb: usb start after boot: 0x%08x (mtp wait 0x%08x)", ret, w );
		char note[96];
		snprintf( note, sizeof( note ), "vdb " VDB_VERSION ": usb start after boot 0x%08x (mtp wait 0x%08x, uptime %u s)\n", ret, w,
			(unsigned)( ksceKernelGetSystemTimeWide() / 1000000 ) );
		FileNote( note );
	}
	// Hotkeys, polled every 200 ms: L+R+Start+Select toggles the bridge,
	// L+R+Start+Up writes a status line to boot.txt. Edge-triggered
	unsigned nPrevButtons = 0, nTicks = 0;
	nTimeout = 200 * 1000;
	for ( ;; )
	{
		unsigned int bits = 0;
		SceUInt to = nTimeout;
		int ret = ksceKernelWaitEventFlag( s_Evf, EVF_ALL, SCE_EVENT_WAITOR | SCE_EVENT_WAITCLEAR_PAT, &bits, &to );
		if ( ret < 0 && ret != (int)0x80028005 /* SCE_KERNEL_ERROR_WAIT_TIMEOUT */ )
			break;
		if ( ret < 0 )
		{
			bits = 0;
			if ( ++nTicks % 25 == 0 )
				Log( "vdb: alive %u (uptime %u s, ring %u used, dropped %u, host %s)", ++nBeats,
					(unsigned)( ksceKernelGetSystemTimeWide() / 1000000 ), s_nRingWrite - s_nRingRead, s_nDropped, bConnected ? "attached" : "none" );
			SceCtrlData pad;
			memset( &pad, 0, sizeof( pad ) );
			if ( ksceCtrlPeekBufferPositive( 0, &pad, 1 ) > 0 )
			{
				const unsigned nCombo = SCE_CTRL_LTRIGGER | SCE_CTRL_RTRIGGER | SCE_CTRL_START;
				unsigned nNow = pad.buttons, nPressed = nNow & ~nPrevButtons;
				nPrevButtons = nNow;
				if ( ( nNow & nCombo ) == nCombo && ( nPressed & SCE_CTRL_SELECT ) )
				{
					int r = s_bActive ? vdb_usb_stop() : vdb_usb_start();
					char note[96];
					snprintf( note, sizeof( note ), "vdb: hotkey %s: 0x%08x (uptime %u s)\n", s_bActive ? "bridge on" : "bridge off", r,
						(unsigned)( ksceKernelGetSystemTimeWide() / 1000000 ) );
					FileNote( note );
					Log( "%s", note );
				}
				else if ( ( nNow & nCombo ) == nCombo && ( nPressed & SCE_CTRL_UP ) )
				{
					char note[128];
					snprintf( note, sizeof( note ), "vdb: status: usb %s, host %s, ring %u used of %u, dropped %u, uptime %u s\n",
						s_bActive ? "bridge" : "system", bConnected ? "attached" : "none", s_nRingWrite - s_nRingRead, RING_SIZE, s_nDropped,
						(unsigned)( ksceKernelGetSystemTimeWide() / 1000000 ) );
					FileNote( note );
					Log( "%s", note );
				}
			}
		}
		if ( bits & EVF_EXIT )
			break;
		if ( bits & EVF_CONNECTED )
		{
			bConnected = 1;
			bSending = 0;
			QueueReceive();
			Log( "vdb: host attached (vdb " VDB_VERSION ")" );
		}
		if ( bits & EVF_DISCONNECTED )
		{
			bConnected = 0;
			bSending = 0;
		}
		if ( bits & EVF_RECEIVED )
		{
			if ( bConnected )
			{
				HandleCommand( s_RxBuf, s_RxReq.transmitted );
				QueueReceive();
			}
		}
		if ( bits & EVF_SENT )
			bSending = 0;
		if ( bConnected && !bSending )
		{
			unsigned n = RingTake( s_TxBuf, CHUNK_SIZE );
			if ( n )
			{
				if ( SendChunk( n ) >= 0 )
					bSending = 1;
			}
		}
	}
	return 0;
}

//-----------------------------------------------------------------------------
// Exports to user space (syscalls, see vdb.yml)
//-----------------------------------------------------------------------------
int vdb_write( const char *pUser, unsigned nLen )
{
	char buf[512];
	unsigned nDone = 0;
	if ( !pUser || !nLen )
		return 0;
	if ( nLen > 4096 )
		nLen = 4096;
	while ( nDone < nLen )
	{
		unsigned n = nLen - nDone;
		if ( n > sizeof( buf ) )
			n = sizeof( buf );
		if ( ksceKernelMemcpyFromUser( buf, pUser + nDone, n ) < 0 )
			return -1;
		RingWrite( buf, n );
		nDone += n;
	}
	return (int)nDone;
}

int vdb_usb_start( void )
{
	int ret;
	if ( !s_bRegistered )
		return -1;
	if ( s_bActive )
		return 0;
	char step[96];
	ret = ksceUdcdDeactivate();
	snprintf( step, sizeof( step ), "vdb: deactivate 0x%08x\n", ret ); FileNote( step );
	if ( ret < 0 && ret != SCE_UDCD_ERROR_INVALID_ARGUMENT )
		return ret;
	ret = ksceUdcdStop( "USB_MTP_Driver", 0, NULL );
	snprintf( step, sizeof( step ), "vdb: stop mtp 0x%08x\n", ret ); FileNote( step );
	ksceUdcdStop( "USBPSPCommunicationDriver", 0, NULL );
	ksceUdcdStop( "USBSerDriver", 0, NULL );
	ret = ksceUdcdStop( "USBDeviceControllerDriver", 0, NULL );
	snprintf( step, sizeof( step ), "vdb: stop controller 0x%08x\n", ret ); FileNote( step );
	ret = ksceUdcdStart( "USBDeviceControllerDriver", 0, NULL );
	snprintf( step, sizeof( step ), "vdb: start controller 0x%08x\n", ret ); FileNote( step );
	if ( ret < 0 )
		return ret;
	ret = ksceUdcdStart( VDB_DRIVER_NAME, 0, NULL );
	snprintf( step, sizeof( step ), "vdb: start vdb driver 0x%08x\n", ret ); FileNote( step );
	if ( ret < 0 )
	{
		ksceUdcdStop( "USBDeviceControllerDriver", 0, NULL );
		return ret;
	}
	ksceKernelClearEventFlag( s_Evf, ~EVF_ALL );
	ret = ksceUdcdActivate( VDB_USB_PID );
	snprintf( step, sizeof( step ), "vdb: activate 0x%08x\n", ret ); FileNote( step );
	if ( ret < 0 )
	{
		ksceUdcdStop( VDB_DRIVER_NAME, 0, NULL );
		ksceUdcdStop( "USBDeviceControllerDriver", 0, NULL );
		return ret;
	}
	s_bActive = 1;
	Log( "vdb: usb device active (pid 0x%04x)", VDB_USB_PID );
	return 0;
}

int vdb_usb_stop( void )
{
	if ( !s_bActive )
		return 0;
	ksceKernelSetEventFlag( s_Evf, EVF_DISCONNECTED );
	ksceUdcdDeactivate();
	ksceUdcdStop( VDB_DRIVER_NAME, 0, NULL );
	ksceUdcdStop( "USBDeviceControllerDriver", 0, NULL );
	// back to the system's MTP device, as vitastick does
	ksceUdcdStart( "USBDeviceControllerDriver", 0, NULL );
	ksceUdcdStart( "USB_MTP_Driver", 0, NULL );
	ksceUdcdActivate( 0x4E4 );
	s_bActive = 0;
	return 0;
}

//-----------------------------------------------------------------------------
// Module entry
//-----------------------------------------------------------------------------
void _start() __attribute__(( weak, alias( "module_start" ) ));
int module_start( SceSize argc, const void *args )
{
	Log( "vdb " VDB_VERSION " starting" );
	s_Evf = ksceKernelCreateEventFlag( "vdb_evf", 0, 0, NULL );
	if ( s_Evf < 0 )
		return SCE_KERNEL_START_FAILED;
	s_Thread = ksceKernelCreateThread( "vdb_usb", UsbThread, 0x3C, 0x2000, 0, 0x10000, 0 );
	if ( s_Thread < 0 )
	{
		ksceKernelDeleteEventFlag( s_Evf );
		return SCE_KERNEL_START_FAILED;
	}
	if ( ksceUdcdRegister( &s_Driver ) < 0 )
	{
		ksceKernelDeleteThread( s_Thread );
		ksceKernelDeleteEventFlag( s_Evf );
		return SCE_KERNEL_START_FAILED;
	}
	s_bRegistered = 1;
	int st = ksceKernelStartThread( s_Thread, 0, NULL );
	{
		char note[96];
		snprintf( note, sizeof( note ), "vdb: thread 0x%08x start 0x%08x\n", (unsigned)s_Thread, st );
		FileNote( note );
	}

	// the flag file decides whether we take the USB port at boot
	SceUID fd = ksceIoOpen( VDB_FLAG_FILE, SCE_O_RDONLY, 0 );
	if ( fd >= 0 )
	{
		ksceIoClose( fd );
		s_bAutoStart = 1;
		Log( "vdb: usb start scheduled for after boot" );
		FileNote( "vdb " VDB_VERSION ": loaded, usb start scheduled after the shell is up\n" );
	}
	else
	{
		Log( "vdb: usb idle (no " VDB_FLAG_FILE ")" );
		FileNote( "vdb " VDB_VERSION ": loaded, usb idle (no usb_on flag)\n" );
	}
	return SCE_KERNEL_START_SUCCESS;
}

int module_stop( SceSize argc, const void *args )
{
	if ( s_bActive )
		vdb_usb_stop();
	if ( s_Evf >= 0 )
		ksceKernelSetEventFlag( s_Evf, EVF_EXIT );
	if ( s_Thread >= 0 )
	{
		ksceKernelWaitThreadEnd( s_Thread, NULL, NULL );
		ksceKernelDeleteThread( s_Thread );
	}
	if ( s_bRegistered )
		ksceUdcdUnregister( &s_Driver );
	if ( s_Evf >= 0 )
		ksceKernelDeleteEventFlag( s_Evf );
	return SCE_KERNEL_STOP_SUCCESS;
}
