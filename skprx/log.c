// vdb: the log ring (what the host tails), boot notes, kernel printf capture
#include "vdb.h"
#include <psp2kern/kernel/cpu/spinlock.h>

static char s_Ring[RING_SIZE];
static unsigned s_nRingWrite, s_nRingRead;	// monotonic byte counters; valid = write - read
static unsigned s_nDropped;
// The ring is written from any thread on any of the three cores (and from
// the kernel's printf, whatever context that runs in) and read by the USB
// thread: a spinlock, not only interrupts off on the current core, which
// is all 0.1 to 0.8.3 had
static SceKernelSpinlock s_RingLock;

// No wakeup from here: the kernel's printf calls in with scheduler or
// other kernel locks held, and setting an event flag from such a context
// can deadlock the kernel. The USB thread drains the ring on its 200 ms
// tick anyway
static void RingWriteQuiet( const char *pData, unsigned nLen )
{
	SceKernelIntrStatus intr = ksceKernelSpinlockLowLockCpuSuspendIntr( &s_RingLock );
	unsigned nUsed = s_nRingWrite - s_nRingRead;
	if ( nLen > RING_SIZE - nUsed )
		s_nDropped += nLen;
	else
	{
		for ( unsigned i = 0; i < nLen; i++ )
			s_Ring[( s_nRingWrite + i ) % RING_SIZE] = pData[i];
		s_nRingWrite += nLen;
	}
	ksceKernelSpinlockLowUnlockCpuResumeIntr( &s_RingLock, intr );
}

void RingWrite( const char *pData, unsigned nLen )
{
	RingWriteQuiet( pData, nLen );
	if ( g_Evf >= 0 )
		ksceKernelSetEventFlag( g_Evf, EVF_DATA );
}

// bytes ready, copied into the transmit buffer (interrupts off for the copy)
unsigned RingTake( unsigned char *pDest, unsigned nMax )
{
	SceKernelIntrStatus intr = ksceKernelSpinlockLowLockCpuSuspendIntr( &s_RingLock );
	unsigned nUsed = s_nRingWrite - s_nRingRead;
	unsigned n = nUsed < nMax ? nUsed : nMax;
	for ( unsigned i = 0; i < n; i++ )
		pDest[i] = s_Ring[( s_nRingRead + i ) % RING_SIZE];
	s_nRingRead += n;
	ksceKernelSpinlockLowUnlockCpuResumeIntr( &s_RingLock, intr );
	return n;
}

unsigned RingUsed( void )
{
	return s_nRingWrite - s_nRingRead;
}

unsigned RingDropped( void )
{
	return s_nDropped;
}

void Log( const char *fmt, ... )
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

// The notes file is capped: past 1 MB it starts over (the retake loop of
// 0.6.12 wrote two megabytes of the same four lines in one night)
void FileNote( const char *pszText )
{
	SceUID fd = ksceIoOpen( VDB_NOTE_FILE, SCE_O_WRONLY | SCE_O_CREAT | SCE_O_APPEND, 0777 );
	if ( fd < 0 )
		return;
	if ( ksceIoLseek( fd, 0, SCE_SEEK_END ) > 1024 * 1024 )
	{
		ksceIoClose( fd );
		fd = ksceIoOpen( VDB_NOTE_FILE, SCE_O_WRONLY | SCE_O_CREAT | SCE_O_TRUNC, 0777 );
		if ( fd < 0 )
			return;
		static const char s_szWrap[] = "vdb: notes file wrapped (1 MB)\n";
		ksceIoWrite( fd, s_szWrap, sizeof( s_szWrap ) - 1 );
	}
	ksceIoWrite( fd, pszText, strlen( pszText ) );
	ksceIoClose( fd );
}

void FileNotef( const char *fmt, ... )
{
	char line[400];
	va_list args;
	va_start( args, fmt );
	int n = vsnprintf( line, sizeof( line ) - 2, fmt, args );
	va_end( args );
	if ( n < 0 )
		return;
	if ( n > (int)sizeof( line ) - 2 )
		n = sizeof( line ) - 2;
	line[n++] = '\n';
	line[n] = 0;
	FileNote( line );
}

unsigned Uptime( void )
{
	return (unsigned)( ksceKernelGetSystemTimeWide() / 1000000 );
}

//-----------------------------------------------------------------------------
// Kernel printf capture: the kernel hands every character of ksceKernelPrintf
// (ksceDebugPrintf) to a registered putchar handler, which on a retail unit
// is otherwise nobody. Lines are collected and dropped into the ring with a
// "[kernel]" prefix while capture is on. The handler may run with interrupts
// disabled or from any thread, so it only touches its own line buffer and
// the ring (which locks with interrupts off itself)
//-----------------------------------------------------------------------------
static volatile int s_bKprintf;
static char s_KLine[200];
static unsigned s_nKLine;

static SceKernelSpinlock s_KLineLock;	// the line buffer, shared by every core that prints

static int KPutchar( void *argp, char c )
{
	if ( !s_bKprintf )
		return 0;
	if ( c != '\n' && (unsigned char)c < 0x20 )
		return 0;	// level bytes and carriage returns the kernel puts in front of its lines
	SceKernelIntrStatus intr = ksceKernelSpinlockLowLockCpuSuspendIntr( &s_KLineLock );
	if ( c != '\n' && s_nKLine < sizeof( s_KLine ) - 1 )
	{
		s_KLine[s_nKLine++] = c;
		if ( s_nKLine < sizeof( s_KLine ) - 1 )
		{
			ksceKernelSpinlockLowUnlockCpuResumeIntr( &s_KLineLock, intr );
			return 0;
		}
	}
	// a full line, or the buffer is full: flush (quietly, see RingWriteQuiet)
	static const char s_szPrefix[] = "[kernel] ";
	RingWriteQuiet( s_szPrefix, sizeof( s_szPrefix ) - 1 );
	RingWriteQuiet( s_KLine, s_nKLine );
	RingWriteQuiet( "\n", 1 );
	s_nKLine = 0;
	ksceKernelSpinlockLowUnlockCpuResumeIntr( &s_KLineLock, intr );
	return 0;
}

void InstallKprintfCapture( void )
{
	int r = ksceKernelRegisterDebugPutcharHandler( KPutchar, NULL );
	FileNotef( "vdb: kernel printf handler: 0x%08x", r );
}

void KprintfCapture( int bOn )
{
	s_nKLine = 0;
	s_bKprintf = bOn ? 1 : 0;
	// a line through the kernel's own printf proves the path end to end: a
	// retail kernel otherwise prints little or nothing
	if ( bOn )
		ksceKernelPrintf( "vdb: kernel printf capture is on (this line came through ksceKernelPrintf)\n" );
}

int KprintfCapturing( void )
{
	return s_bKprintf;
}

//-----------------------------------------------------------------------------
// Syscall: user apps append to the ring
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
