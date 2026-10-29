// vdb: the USB thread and the module entry points (see vdb.h for the map)
#include "vdb.h"

static SceUID s_Thread = -1;
static int s_bAutoStart;	// ux0:vdbplugin/usb_on existed at boot
int g_nAwakeMode;		// off by default: the Vita sleeps and charges as it would without the plugin
int g_bStatusOsd;

// The status overlay: every marker that matters when the Mac sees nothing
void StatusOsdTick( void )
{
	static unsigned s_nLast;
	if ( !g_bStatusOsd || Uptime() == s_nLast )
		return;
	s_nLast = Uptime();
	char text[6 * 64];
	int n = snprintf( text, sizeof( text ), "vdb " VDB_VERSION " fps %u up %us batt %d%% %dmAh chg %d awake %d ring %u/%u sysw %us\n",
		FpsBusier(), Uptime(), kscePowerGetBatteryLifePercent(), kscePowerGetBatteryRemainCapacity(), kscePowerGetBatteryChargingStatus(), g_nAwakeMode, RingUsed(), RING_SIZE, SystemWritesSecondsLeft() );
	if ( n > 0 && n < (int)sizeof( text ) )
		UsbStatusLines( text + n, sizeof( text ) - n );
	OsdSet( text );
}

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
	const unsigned nToggle = nCombo | SCE_CTRL_SELECT, nStatus = nCombo | SCE_CTRL_UP, nUnlock = nCombo | SCE_CTRL_DOWN;
	unsigned nNow = pad.buttons, nPrev = *pnPrevButtons;
	*pnPrevButtons = nNow;
	if ( ( nNow & nToggle ) == nToggle && ( nPrev & nToggle ) != nToggle )
	{
		UsbRetakeReset();
		int r = g_bActive ? vdb_usb_stop() : vdb_usb_start();
		FileNotef( "vdb: hotkey %s: 0x%08x (uptime %u s)", g_bActive ? "bridge on" : "bridge off", r, Uptime() );
		Log( "vdb: hotkey %s: 0x%08x", g_bActive ? "bridge on" : "bridge off", r );
	}
	else if ( ( nNow & nUnlock ) == nUnlock && ( nPrev & nUnlock ) != nUnlock )
		SystemWritesUnlock();
	else if ( ( nNow & nStatus ) == nStatus && ( nPrev & nStatus ) != nStatus )
	{
		// toggles the status overlay and leaves a note either way
		g_bStatusOsd = !g_bStatusOsd;
		if ( !g_bStatusOsd )
			OsdSet( "" );
		char lines[3 * 64];
		UsbStatusLines( lines, sizeof( lines ) );
		FileNotef( "vdb: status hotkey (overlay %s): usb %s, host %s, ring %u used of %u, dropped %u, uptime %u s\n%s",
			g_bStatusOsd ? "on" : "off", g_bActive ? "bridge" : "system", g_bHostAttached ? "attached" : "none", RingUsed(), RING_SIZE, RingDropped(), Uptime(), lines );
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
		// the system's own device, for comparison with ours in the notes
		FileNotef( "vdb: before takeover: usb state 0x%x, mtp 0x%x, charging status %d", (unsigned)ksceUdcdGetDeviceState(),
			(unsigned)ksceUdcdGetDrvState( "USB_MTP_Driver" ), kscePowerGetBatteryChargingStatus() );
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
				Log( "vdb: alive %u (uptime %u s, ring %u used, dropped %u, host %s, usb 0x%x, awake %d%s)", ++nBeats, Uptime(), RingUsed(), RingDropped(),
					g_bHostAttached ? "attached" : "none", (unsigned)ksceUdcdGetDeviceState(), g_nAwakeMode, UsbSuspended() ? ", suspended" : "" );
				// Keep-awake is the host's choice (vdb.py awake): off, the
				// Vita dims, sleeps and charges exactly as without the
				// plugin; 1 cancels only the automatic standby (screen off,
				// port alive, 500 mA go into the battery); 2 keeps the
				// screen on as well. 0.2.3 to 0.6.12 forced mode 2 and a
				// hung standby drained the battery to zero overnight
				if ( g_nAwakeMode == 1 )
					ksceKernelPowerTick( SCE_KERNEL_POWER_TICK_DISABLE_AUTO_SUSPEND );
				else if ( g_nAwakeMode >= 2 )
					ksceKernelPowerTick( SCE_KERNEL_POWER_TICK_DEFAULT );
			}
			InputTick();
			UsbSuspendTick();
			SystemWritesTick();
			UsbWatchdog();
			FpsTick();
			StatusOsdTick();
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
//-----------------------------------------------------------------------------
// Configuration: ux0:vdbplugin/vdb.cfg, one setting per line, read at boot
// from top to bottom, so a later line overrides an earlier one. "#" starts
// a comment; unknown or malformed lines are ignored and noted in boot.txt.
//   usb on|off                take the USB port once the shell is up
//   awake off|standby|screen  keep-awake (also 0|1|2)
//   hooks on|off              touch, display and printf hooks at once
//   touch on|off, display on|off, printf on|off   one hook each
// Without vdb.cfg the single flag files of 0.8 and before still count
// (usb_on, awake, nohooks); they go in a later version
//-----------------------------------------------------------------------------
static int ParseOnOff( const char *v, int *pOut )
{
	if ( !strcmp( v, "on" ) || !strcmp( v, "1" ) || !strcmp( v, "yes" ) || !strcmp( v, "true" ) )
		*pOut = 1;
	else if ( !strcmp( v, "off" ) || !strcmp( v, "0" ) || !strcmp( v, "no" ) || !strcmp( v, "false" ) )
		*pOut = 0;
	else
		return 0;
	return 1;
}

static int FileExists( const char *pszPath )
{
	SceUID fd = ksceIoOpen( pszPath, SCE_O_RDONLY, 0 );
	if ( fd < 0 )
		return 0;
	ksceIoClose( fd );
	return 1;
}

static void ReadConfig( VdbConfig_t *c )
{
	c->bUsb = 0;
	c->nAwake = 0;
	c->bTouch = c->bDisplay = c->bPrintf = 1;
	static char s_Text[4096];
	SceUID fd = ksceIoOpen( VDB_CONFIG_FILE, SCE_O_RDONLY, 0 );
	if ( fd < 0 )
	{
		// the 0.8 flag files
		c->bUsb = FileExists( VDB_FLAG_FILE );
		if ( FileExists( VDB_NOHOOKS_FILE ) )
			c->bTouch = c->bDisplay = c->bPrintf = 0;
		SceUID fa = ksceIoOpen( VDB_AWAKE_FILE, SCE_O_RDONLY, 0 );
		if ( fa >= 0 )
		{
			char ch = 0;
			if ( ksceIoRead( fa, &ch, 1 ) == 1 && ch >= '0' && ch <= '2' )
				c->nAwake = ch - '0';
			ksceIoClose( fa );
		}
		FileNotef( "vdb: no " VDB_CONFIG_FILE ", the old flag files: usb %s, awake %d, hooks %s", c->bUsb ? "on" : "off", c->nAwake, c->bTouch ? "on" : "off" );
		return;
	}
	int n = ksceIoRead( fd, s_Text, sizeof( s_Text ) - 1 );
	ksceIoClose( fd );
	if ( n < 0 )
		n = 0;
	s_Text[n] = 0;
	int nLine = 0;
	for ( char *p = s_Text; *p; )
	{
		char *pEnd = p;
		while ( *pEnd && *pEnd != '\n' )
			pEnd++;
		int bMore = *pEnd != 0;
		*pEnd = 0;
		nLine++;
		char *pHash = strchr( p, '#' );
		if ( pHash )
			*pHash = 0;
		// two words: key and value ("key value" or "key=value"), lower-cased
		char key[16], val[16];
		unsigned k = 0, v = 0;
		char *q = p;
		while ( *q == ' ' || *q == '\t' || *q == '\r' )
			q++;
		while ( *q && *q != ' ' && *q != '\t' && *q != '=' && *q != '\r' && k < sizeof( key ) - 1 )
			key[k++] = ( *q >= 'A' && *q <= 'Z' ) ? *q + 32 : *q, q++;
		key[k] = 0;
		while ( *q == ' ' || *q == '\t' || *q == '=' )
			q++;
		while ( *q && *q != ' ' && *q != '\t' && *q != '\r' && v < sizeof( val ) - 1 )
			val[v++] = ( *q >= 'A' && *q <= 'Z' ) ? *q + 32 : *q, q++;
		val[v] = 0;
		while ( *q == ' ' || *q == '\t' || *q == '\r' )
			q++;
		if ( k )
		{
			int b = 0, ok = 1;
			if ( *q )
				ok = 0;	// a third word
			else if ( !strcmp( key, "usb" ) && ParseOnOff( val, &b ) )
				c->bUsb = b;
			else if ( !strcmp( key, "awake" ) )
			{
				if ( !strcmp( val, "off" ) || !strcmp( val, "0" ) )
					c->nAwake = 0;
				else if ( !strcmp( val, "standby" ) || !strcmp( val, "1" ) )
					c->nAwake = 1;
				else if ( !strcmp( val, "screen" ) || !strcmp( val, "2" ) )
					c->nAwake = 2;
				else
					ok = 0;
			}
			else if ( !strcmp( key, "hooks" ) && ParseOnOff( val, &b ) )
				c->bTouch = c->bDisplay = c->bPrintf = b;
			else if ( !strcmp( key, "touch" ) && ParseOnOff( val, &b ) )
				c->bTouch = b;
			else if ( !strcmp( key, "display" ) && ParseOnOff( val, &b ) )
				c->bDisplay = b;
			else if ( !strcmp( key, "printf" ) && ParseOnOff( val, &b ) )
				c->bPrintf = b;
			else
				ok = 0;
			if ( !ok )
				FileNotef( "vdb: " VDB_CONFIG_FILE " line %d ignored: %s", nLine, p );
		}
		p = bMore ? pEnd + 1 : pEnd;
	}
	FileNotef( "vdb: " VDB_CONFIG_FILE ": usb %s, awake %d, touch %s, display %s, printf %s", c->bUsb ? "on" : "off", c->nAwake,
		c->bTouch ? "on" : "off", c->bDisplay ? "on" : "off", c->bPrintf ? "on" : "off" );
}

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
	VdbConfig_t cfg;
	ReadConfig( &cfg );
	if ( cfg.bTouch )
		InstallInputHooks();
	if ( cfg.bDisplay )
		InstallDisplayHook();
	if ( cfg.bPrintf )
		InstallKprintfCapture();
	if ( !cfg.bTouch || !cfg.bDisplay || !cfg.bPrintf )
		FileNotef( "vdb: hooks: touch %s, display %s, printf %s", cfg.bTouch ? "on" : "off", cfg.bDisplay ? "on" : "off", cfg.bPrintf ? "on" : "off" );
	UsbInstallSuspendHandler();
	// decided before the thread runs (it once read the flag before
	// module_start set it: the takeover never happened)
	s_bAutoStart = cfg.bUsb;
	g_nAwakeMode = cfg.nAwake;
	int st = ksceKernelStartThread( s_Thread, 0, NULL );
	FileNotef( "vdb " VDB_VERSION ": loaded, thread 0x%08x start 0x%08x, usb %s", (unsigned)s_Thread, st,
		s_bAutoStart ? "start scheduled after the shell is up" : "idle (usb off in the config)" );
	Log( s_bAutoStart ? "vdb: usb start scheduled for after boot" : "vdb: usb idle (usb off in " VDB_CONFIG_FILE ")" );
	return SCE_KERNEL_START_SUCCESS;
}

int module_stop( SceSize argc, const void *args )
{
	ProfileStop();
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
