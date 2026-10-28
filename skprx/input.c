// vdb: pad and touch injection
#include "vdb.h"

//-----------------------------------------------------------------------------
// Pad: the kernel's own controller emulation, no hooks. The state is
// re-applied from the USB thread every 200 ms until the deadline (an
// emulation lasts a given number of samples, so a long hold needs
// refreshing), then released once
//-----------------------------------------------------------------------------
static VdbInput_t s_Input;
static unsigned long long s_nInputUntil;	// system time, 0 = nothing held

static void ApplyInput( int bRelease )
{
	if ( bRelease )
	{
		ksceCtrlSetButtonEmulation( 0, 0, 0, 0, 1 );
		ksceCtrlSetAnalogEmulation( 0, 0, 0x80, 0x80, 0x80, 0x80, 0x80, 0x80, 0x80, 0x80, 1 );
		return;
	}
	// 64 samples outlast the 200 ms refresh at any sampling rate
	ksceCtrlSetButtonEmulation( 0, 0, s_Input.buttons, s_Input.buttons, 64 );
	ksceCtrlSetAnalogEmulation( 0, 0, s_Input.lx, s_Input.ly, s_Input.rx, s_Input.ry, s_Input.lx, s_Input.ly, s_Input.rx, s_Input.ry, 64 );
}

void InputSet( const VdbInput_t *pInput )
{
	s_Input = *pInput;
	if ( s_Input.hold_ms > 10000 )
		s_Input.hold_ms = 10000;
	s_nInputUntil = ksceKernelGetSystemTimeWide() + (unsigned long long)s_Input.hold_ms * 1000;
	ApplyInput( 0 );
}

void InputTick( void )
{
	if ( !s_nInputUntil )
		return;
	if ( ksceKernelGetSystemTimeWide() >= s_nInputUntil )
	{
		s_nInputUntil = 0;
		ApplyInput( 1 );
	}
	else
		ApplyInput( 0 );
}

void InputRelease( void )
{
	if ( !s_nInputUntil )
		return;
	s_nInputUntil = 0;
	ApplyInput( 1 );
}

//-----------------------------------------------------------------------------
// Touch: one finger seen by every reader of a touch panel, through taiHEN
// hooks on the kernel touch functions (ds4vita's method and NIDs); the
// position moves linearly from (x0,y0) to (x1,y1) over the duration
//-----------------------------------------------------------------------------
static VdbTouch_t s_Touch;
static unsigned long long s_nTouchStart, s_nTouchUntil;
static tai_hook_ref_t s_TouchRef[4];
static SceUID s_TouchHook[4] = { -1, -1, -1, -1 };

static void PatchTouch( SceUInt32 port, SceTouchData *pData, SceUInt32 nBufs )
{
	if ( port != s_Touch.port || !s_nTouchUntil || !pData )
		return;
	unsigned long long now = ksceKernelGetSystemTimeWide();
	if ( now >= s_nTouchUntil )
	{
		s_nTouchUntil = 0;	// the finger lifts: the next reads carry no report
		return;
	}
	unsigned nElapsedMs = (unsigned)( ( now - s_nTouchStart ) / 1000 ), nTotalMs = s_Touch.ms ? s_Touch.ms : 1;
	int x = (int)s_Touch.x0 + ( (int)s_Touch.x1 - (int)s_Touch.x0 ) * (int)nElapsedMs / (int)nTotalMs;
	int y = (int)s_Touch.y0 + ( (int)s_Touch.y1 - (int)s_Touch.y0 ) * (int)nElapsedMs / (int)nTotalMs;
	for ( SceUInt32 i = 0; i < nBufs; i++ )
	{
		pData[i].report[0].id = 0;
		pData[i].report[0].force = 128;
		pData[i].report[0].x = (unsigned short)( x * 2 );	// the panels report 1920x1088 for a 960x544 screen
		pData[i].report[0].y = (unsigned short)( y * 2 );
		pData[i].reportNum = 1;
	}
}

static int Hook_ksceTouchPeek( SceUInt32 port, SceTouchData *pData, SceUInt32 nBufs )
{
	int ret = TAI_CONTINUE( int, s_TouchRef[0], port, pData, nBufs );
	if ( ret >= 0 )
		PatchTouch( port, pData, nBufs );
	return ret;
}
static int Hook_ksceTouchPeekRegion( SceUInt32 port, SceTouchData *pData, SceUInt32 nBufs, int region )
{
	int ret = TAI_CONTINUE( int, s_TouchRef[1], port, pData, nBufs, region );
	if ( ret >= 0 )
		PatchTouch( port, pData, nBufs );
	return ret;
}
static int Hook_ksceTouchRead( SceUInt32 port, SceTouchData *pData, SceUInt32 nBufs )
{
	int ret = TAI_CONTINUE( int, s_TouchRef[2], port, pData, nBufs );
	if ( ret >= 0 )
		PatchTouch( port, pData, nBufs );
	return ret;
}
static int Hook_ksceTouchReadRegion( SceUInt32 port, SceTouchData *pData, SceUInt32 nBufs, int region )
{
	int ret = TAI_CONTINUE( int, s_TouchRef[3], port, pData, nBufs, region );
	if ( ret >= 0 )
		PatchTouch( port, pData, nBufs );
	return ret;
}

// NIDs as in ds4vita (3.60/3.65 SceTouch): Peek, PeekRegion, Read, ReadRegion
void InstallInputHooks( void )
{
	static const unsigned s_Nids[4] = { 0xBAD1960B, 0x9B3F7207, 0x70C8AACE, 0x9A91F624 };
	static const void *s_Fns[4] = { Hook_ksceTouchPeek, Hook_ksceTouchPeekRegion, Hook_ksceTouchRead, Hook_ksceTouchReadRegion };
	for ( int i = 0; i < 4; i++ )
	{
		s_TouchHook[i] = taiHookFunctionExportForKernel( KERNEL_PID, &s_TouchRef[i], "SceTouch", TAI_ANY_LIBRARY, s_Nids[i], s_Fns[i] );
		FileNotef( "vdb: touch hook %d: 0x%08x", i, (unsigned)s_TouchHook[i] );
	}
}

void RemoveInputHooks( void )
{
	for ( int i = 0; i < 4; i++ )
		if ( s_TouchHook[i] >= 0 )
			taiHookReleaseForKernel( s_TouchHook[i], s_TouchRef[i] );
}

int TouchAvailable( void )
{
	return s_TouchHook[0] >= 0;
}

void TouchSet( const VdbTouch_t *pTouch, unsigned nPayload )
{
	memset( &s_Touch, 0, sizeof( s_Touch ) );
	memcpy( &s_Touch, pTouch, nPayload < sizeof( s_Touch ) ? nPayload : sizeof( s_Touch ) );
	if ( s_Touch.port > SCE_TOUCH_PORT_BACK )
		s_Touch.port = SCE_TOUCH_PORT_FRONT;
	if ( s_Touch.ms > 10000 )
		s_Touch.ms = 10000;
	if ( s_Touch.ms < 16 )
		s_Touch.ms = 16;
	s_nTouchStart = ksceKernelGetSystemTimeWide();
	s_nTouchUntil = s_nTouchStart + (unsigned long long)s_Touch.ms * 1000;
}
