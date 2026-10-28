// vdb: the USB thread and the module entry points (see vdb.h for the map)
#include "vdb.h"

static SceUID s_Thread = -1;
static int s_bAutoStart;	// ux0:vdbplugin/usb_on existed at boot

//-----------------------------------------------------------------------------
// USB thread: streams the ring while a host is attached, runs the commands,
// polls the hotkeys, keeps the Vita awake and the port ours
//-----------------------------------------------------------------------------
static void Hotkeys( unsigned *pnPrevButtons )
{
	// L+R+Start+Select toggles the bridge, L+R+Start+Up writes a status
	// line to boot.txt; edge-triggered, fires once when all keys are held
	SceCtrlData pad;
	memset( &pad, 0, sizeof( pad ) );
	if ( ksceCtrlPeekBufferPositive( 0, &pad, 1 ) <= 0 )
		return;
	const unsigned nCombo = SCE_CTRL_LTRIGGER | SCE_CTRL_RTRIGGER | SCE_CTRL_START;
	const unsigned nToggle = nCombo | SCE_CTRL_SELECT, nStatus = nCombo | SCE_CTRL_UP;
	unsigned nNow = pad.buttons, nPrev = *pnPrevButtons;
	*pnPrevButtons = nNow;
	if ( ( nNow & nToggle ) == nToggle && ( nPrev & nToggle ) != nToggle )
	{
		int r = g_bActive ? vdb_usb_stop() : vdb_usb_start();
		FileNotef( "vdb: hotkey %s: 0x%08x (uptime %u s)", g_bActive ? "bridge on" : "bridge off", r, Uptime() );
		Log( "vdb: hotkey %s: 0x%08x", g_bActive ? "bridge on" : "bridge off", r );
	}
	else if ( ( nNow & nStatus ) == nStatus && ( nPrev & nStatus ) != nStatus )
	{
		FileNotef( "vdb: status: usb %s, host %s, ring %u used of %u, dropped %u, uptime %u s",
			g_bActive ? "bridge" : "system", g_bHostAttached ? "attached" : "none", RingUsed(), RING_SIZE, RingDropped(), Uptime() );
	}
}

static int UsbThread( SceSize args, void *argp )
{
	int bSending = 0;
	unsigned nBeats = 0, nTicks = 0, nPrevButtons = 0;

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
		FileNotef( "vdb: mtp wait returned 0x%08x at uptime %u s, pausing 3 s", w, Uptime() );
		ksceKernelDelayThread( 3 * 1000 * 1000 );
		int ret = vdb_usb_start();
		Log( "vdb: usb start after boot: 0x%08x (mtp wait 0x%08x)", ret, w );
		FileNotef( "vdb " VDB_VERSION ": usb start after boot 0x%08x (mtp wait 0x%08x, uptime %u s)", ret, w, Uptime() );
	}

	for ( ;; )
	{
		unsigned int bits = 0;
		SceUInt to = 200 * 1000;
		int ret = ksceKernelWaitEventFlag( g_Evf, EVF_ALL, SCE_EVENT_WAITOR | SCE_EVENT_WAITCLEAR_PAT, &bits, &to );
		if ( ret < 0 && ret != (int)0x80028005 /* SCE_KERNEL_ERROR_WAIT_TIMEOUT */ )
			break;
		if ( ret < 0 )
		{
			// the 200 ms tick
			bits = 0;
			nTicks++;
			if ( nTicks % 25 == 0 )
			{
				Log( "vdb: alive %u (uptime %u s, ring %u used, dropped %u, host %s)", ++nBeats, Uptime(), RingUsed(), RingDropped(),
					g_bHostAttached ? "attached" : "none" );
				if ( g_bActive )
				{
					// Keep the screen on and the Vita awake while the bridge
					// is up: a Vita that locked itself went unreachable over
					// USB more than once in the middle of a test session
					ksceKernelPowerTick( SCE_KERNEL_POWER_TICK_DEFAULT );
					UsbChargeNudge();
				}
			}
			InputTick();
			if ( nTicks % 5 == 0 )
				UsbRetakeIfLost();
			Hotkeys( &nPrevButtons );
		}
		if ( bits & EVF_EXIT )
			break;
		if ( bits & EVF_CONNECTED )
		{
			g_bHostAttached = 1;
			bSending = 0;
			UsbStartReceiving();
			Log( "vdb: host attached (vdb " VDB_VERSION ")" );
		}
		if ( bits & EVF_DISCONNECTED )
		{
			g_bHostAttached = 0;
			bSending = 0;
			PutAbort();		// a half-written file stays, truncated; the host reports the failure
			InputRelease();
		}
		if ( ( bits & EVF_RECEIVED ) && g_bHostAttached )
		{
			unsigned char *pFrame;
			int nLen = UsbTakeFrame( &pFrame );
			if ( nLen > 0 )
				HandleCommand( pFrame, nLen );
		}
		if ( bits & EVF_SENT )
			bSending = 0;
		if ( g_bHostAttached && !bSending )
			bSending = UsbStreamLog();
	}
	return 0;
}

//-----------------------------------------------------------------------------
// Module entry
//-----------------------------------------------------------------------------
void _start() __attribute__(( weak, alias( "module_start" ) ));
int module_start( SceSize argc, const void *args )
{
	Log( "vdb " VDB_VERSION " starting" );
	g_Evf = ksceKernelCreateEventFlag( "vdb_evf", 0, 0, NULL );
	if ( g_Evf < 0 )
		return SCE_KERNEL_START_FAILED;
	s_Thread = ksceKernelCreateThread( "vdb_usb", UsbThread, 0x3C, 0x2000, 0, 0x10000, 0 );
	if ( s_Thread < 0 )
	{
		ksceKernelDeleteEventFlag( g_Evf );
		return SCE_KERNEL_START_FAILED;
	}
	if ( UsbRegister() < 0 )
	{
		ksceKernelDeleteThread( s_Thread );
		ksceKernelDeleteEventFlag( g_Evf );
		return SCE_KERNEL_START_FAILED;
	}
	InstallInputHooks();
	InstallDisplayHook();
	InstallKprintfCapture();

	// decide the auto start before the thread runs (it once read the flag
	// before module_start set it: the takeover never happened)
	SceUID fdFlag = ksceIoOpen( VDB_FLAG_FILE, SCE_O_RDONLY, 0 );
	if ( fdFlag >= 0 )
	{
		ksceIoClose( fdFlag );
		s_bAutoStart = 1;
	}
	int st = ksceKernelStartThread( s_Thread, 0, NULL );
	FileNotef( "vdb " VDB_VERSION ": loaded, thread 0x%08x start 0x%08x, usb %s", (unsigned)s_Thread, st,
		s_bAutoStart ? "start scheduled after the shell is up" : "idle (no usb_on flag)" );
	Log( s_bAutoStart ? "vdb: usb start scheduled for after boot" : "vdb: usb idle (no " VDB_FLAG_FILE ")" );
	return SCE_KERNEL_START_SUCCESS;
}

int module_stop( SceSize argc, const void *args )
{
	KprintfCapture( 0 );
	RemoveDisplayHook();
	RemoveInputHooks();
	if ( g_bActive )
		vdb_usb_stop();
	if ( g_Evf >= 0 )
		ksceKernelSetEventFlag( g_Evf, EVF_EXIT );
	if ( s_Thread >= 0 )
	{
		ksceKernelWaitThreadEnd( s_Thread, NULL, NULL );
		ksceKernelDeleteThread( s_Thread );
	}
	UsbUnregister();
	if ( g_Evf >= 0 )
		ksceKernelDeleteEventFlag( g_Evf );
	return SCE_KERNEL_STOP_SUCCESS;
}
