// vdb: Vita debug bridge, kernel side.
//
// A vendor USB device with three bulk pipes: EP1 IN streams a 64 KB log
// ring to the host, EP2 OUT carries framed commands from the host, EP3 IN
// carries the replies and file data. Log writers: the vdb_write syscall
// (any user app that mirrors its debug log into it) and this module's
// own heartbeat. File commands (list, stat, get, put, mkdir, remove, rmdir,
// rename) run on the kernel's own IO, so every mounted device is reachable
// (ux0:, ur0:, uma0:, vs0:, os0: ...) with no app involved. Activated at
// boot only when the flag file ux0:vdbplugin/usb_on exists (VitaShell's
// USB mass storage keeps working otherwise), by the hotkey, or from user
// space with vdb_usb_start().
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
#include <psp2kern/io/dirent.h>
#include <psp2kern/io/stat.h>
#include <psp2kern/udcd.h>
#include <psp2kern/ctrl.h>
#include <psp2kern/power.h>
#include <psp2kern/kernel/suspend.h>
#include <psp2kern/kernel/sysroot.h>
#include <psp2kern/kernel/sysmem/data_transfers.h>
#include <psp2kern/appmgr.h>
#include <psp2kern/display.h>
#include <stdarg.h>
#include "usb_descriptors.h"

#define VDB_DRIVER_NAME	"VDB"
#define VDB_USB_PID		0x0DB6
#define VDB_VERSION		"0.3.2"
#define VDB_FLAG_FILE	"ux0:vdbplugin/usb_on"

#define RING_SIZE		( 64 * 1024 )
#define CHUNK_SIZE		4096
#define RX_SIZE			( 64 * 1024 )	// one command frame: header + payload; 64 KB so a PUT costs one transfer and one card write per 64 KB (4 KB frames: 0.7 MB/s)
#define DATA_CHUNK		( 32 * 1024 )	// reply payload per frame on EP3

enum
{
	EVF_CONNECTED		= 1 << 0,
	EVF_DISCONNECTED	= 1 << 1,
	EVF_DATA			= 1 << 2,
	EVF_SENT			= 1 << 3,
	EVF_RECEIVED		= 1 << 4,
	EVF_EXIT			= 1 << 5,
	EVF_ALL				= 0x3F,		// what the main loop waits on and clears
	EVF_DSENT			= 1 << 6	// EP3 reply sent (waited for synchronously by SendReply)
};

//-----------------------------------------------------------------------------
// Command protocol (little endian, host/vdb.py mirrors it)
//
// Host -> EP2: VdbCmdHdr_t + payload (paths are NUL-terminated, two for
// RENAME). A frame is one USB transfer; the host pads a frame whose size is
// a multiple of the packet size so the transfer terminates (short packet).
// Device -> EP3: VdbRspHdr_t + payload. GET streams the file as data frames
// and ends with an empty one; LIST streams text lines the same way.
//-----------------------------------------------------------------------------
#define VDB_CMD_MAGIC	0x31424456	// "VDB1"
#define VDB_RSP_MAGIC	0x52424456	// "VDBR"

enum
{
	VDB_CMD_PING		= 1,
	VDB_CMD_STATUS		= 2,	// reply payload: status text
	VDB_CMD_LIST		= 3,	// payload: dir path; reply frames: "d|f <size> <yyyy-mm-dd hh:mm:ss> <name>\n", final status = entry count
	VDB_CMD_STAT		= 4,	// payload: path; reply: one such line
	VDB_CMD_GET			= 5,	// payload: path; reply: 8-byte size, data frames, empty frame
	VDB_CMD_PUT_OPEN	= 6,	// payload: path (truncate/create); reply: status
	VDB_CMD_PUT_DATA	= 7,	// payload: file data (no reply)
	VDB_CMD_PUT_CLOSE	= 8,	// reply: status, payload 8-byte bytes written
	VDB_CMD_MKDIR		= 9,
	VDB_CMD_REMOVE		= 10,
	VDB_CMD_RMDIR		= 11,
	VDB_CMD_RENAME		= 12,	// payload: "old\0new\0"
	VDB_CMD_LAUNCH		= 14,	// payload: "path\0args\0..."; reply payload: 4-byte pid
	VDB_CMD_KILL		= 15,	// arg: pid (0 = the process that owns the screen); reply payload: 4-byte pid
	VDB_CMD_SCREEN		= 16,	// reply: VdbScreenHdr_t, then the raw framebuffer in frames, then an empty frame
	VDB_CMD_INPUT		= 17,	// payload: VdbInput_t; held for hold_ms, then released
	VDB_CMD_POWER		= 13,	// arg: 0 cold reboot, 1 soft reboot, 2 power off, 3 suspend; replies, then acts 300 ms later
};

typedef struct
{
	unsigned magic, cmd, len, arg;
} VdbCmdHdr_t;

typedef struct
{
	unsigned magic, cmd;
	int status;
	unsigned len;
} VdbRspHdr_t;

typedef struct
{
	unsigned width, height, pitch, pixelformat, pid, bytes;
} VdbScreenHdr_t;

typedef struct
{
	unsigned buttons;			// SCE_CTRL_* bits
	unsigned char lx, ly, rx, ry;	// sticks, 0x80 = centre
	unsigned hold_ms;			// how long the state is held before the release
} VdbInput_t;

// Injected pad state, re-applied from the USB thread every 200 ms until the
// deadline (the emulation lasts a given number of samples, so a long hold
// needs refreshing), then released once
static VdbInput_t s_Input;
static unsigned long long s_nInputUntil;	// system time, 0 = nothing held

static char s_Ring[RING_SIZE];
static unsigned s_nRingWrite, s_nRingRead;	// monotonic byte counters; valid = write - read
static unsigned s_nDropped;
static unsigned char s_TxBuf[CHUNK_SIZE] __attribute__(( aligned( 64 ) ));
static unsigned char s_RxBuf[RX_SIZE] __attribute__(( aligned( 64 ) ));
static unsigned char s_DataBuf[sizeof( VdbRspHdr_t ) + DATA_CHUNK] __attribute__(( aligned( 64 ) ));
static SceUID s_Evf = -1;
static SceUID s_Thread = -1;
static int s_bRegistered, s_bActive;
static SceUID s_PutFd = -1;			// file being written by PUT_OPEN .. PUT_CLOSE
static int s_nPutError;
static unsigned long long s_nPutBytes;

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

static void OnDataSent( SceUdcdDeviceRequest *req )
{
	ksceKernelSetEventFlag( s_Evf, EVF_DSENT );
}

static SceUdcdDeviceRequest s_TxReq;
static SceUdcdDeviceRequest s_RxReq;
static SceUdcdDeviceRequest s_DataReq;

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
	// USB DMA writes the frame behind the CPU's back: drop any cached copy
	// of the buffer before the transfer (and again after it, see the
	// receive handling), or the CPU re-reads the previous command. Seen
	// on hardware: the first command worked, every later one re-ran the
	// old one with a garbage path
	ksceKernelDcacheInvalidateRange( s_RxBuf, RX_SIZE );
	memset( &s_RxReq, 0, sizeof( s_RxReq ) );
	s_RxReq.endpoint = &endpoints[VDB_EP_OUT];
	s_RxReq.data = s_RxBuf;
	s_RxReq.size = RX_SIZE;
	s_RxReq.onComplete = OnReceived;
	return ksceUdcdReqRecv( &s_RxReq );
}

//-----------------------------------------------------------------------------
// Replies on EP3. Sent synchronously from the USB thread: one frame in
// flight, wait for its completion (3 s), so a file streams as a sequence of
// frames with USB flow control and nothing else to buffer. The log ring
// pauses meanwhile and catches up afterwards (nothing is lost under 64 KB)
//-----------------------------------------------------------------------------
static unsigned char *ReplyPayload( void )
{
	return s_DataBuf + sizeof( VdbRspHdr_t );
}

static int SendReply( unsigned nCmd, int nStatus, unsigned nLen )
{
	VdbRspHdr_t *pHdr = (VdbRspHdr_t *)s_DataBuf;
	if ( nLen > DATA_CHUNK )
		nLen = DATA_CHUNK;
	pHdr->magic = VDB_RSP_MAGIC;
	pHdr->cmd = nCmd;
	pHdr->status = nStatus;
	pHdr->len = nLen;
	ksceKernelClearEventFlag( s_Evf, ~EVF_DSENT );
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
	ret = ksceKernelWaitEventFlag( s_Evf, EVF_DSENT, SCE_EVENT_WAITOR | SCE_EVENT_WAITCLEAR_PAT, &bits, &to );
	if ( ret < 0 )
	{
		ksceUdcdReqCancelAll( &endpoints[VDB_EP_DATA] );
		return ret;
	}
	return s_DataReq.returnCode < 0 ? s_DataReq.returnCode : 0;
}

// A text line for one directory entry or stat result
static int FormatEntry( char *pOut, unsigned nMax, const SceIoStat *pStat, const char *pszName )
{
	int bDir = SCE_S_ISDIR( pStat->st_mode );
	return snprintf( pOut, nMax, "%c %llu %04u-%02u-%02u %02u:%02u:%02u %s\n", bDir ? 'd' : 'f', (unsigned long long)pStat->st_size,
		pStat->st_mtime.year, pStat->st_mtime.month, pStat->st_mtime.day, pStat->st_mtime.hour, pStat->st_mtime.minute, pStat->st_mtime.second,
		pszName );
}

// LIST: stream the entries as text, final empty frame carries the count
static void CmdList( const char *pszPath )
{
	SceUID dfd = ksceIoDopen( pszPath );
	if ( dfd < 0 )
	{
		SendReply( VDB_CMD_LIST, dfd, 0 );
		return;
	}
	static SceIoDirent ent;	// 0x160 bytes, keep it off the kernel stack
	char *pText = (char *)ReplyPayload();
	unsigned nUsed = 0;
	int nCount = 0, nErr = 0;
	for ( ;; )
	{
		memset( &ent, 0, sizeof( ent ) );
		int r = ksceIoDread( dfd, &ent );
		if ( r <= 0 )
		{
			if ( r < 0 )
				nErr = r;
			break;
		}
		char line[320];
		int n = FormatEntry( line, sizeof( line ), &ent.d_stat, ent.d_name );
		if ( n < 0 || n >= (int)sizeof( line ) )
			continue;
		if ( nUsed + n > DATA_CHUNK )
		{
			if ( SendReply( VDB_CMD_LIST, 0, nUsed ) < 0 )
			{
				nErr = -1;
				break;
			}
			nUsed = 0;
		}
		memcpy( pText + nUsed, line, n );
		nUsed += n;
		nCount++;
	}
	ksceIoDclose( dfd );
	if ( nUsed && SendReply( VDB_CMD_LIST, 0, nUsed ) < 0 )
		return;
	SendReply( VDB_CMD_LIST, nErr < 0 ? nErr : nCount, 0 );
}

static void CmdStat( const char *pszPath )
{
	SceIoStat st;
	memset( &st, 0, sizeof( st ) );
	int r = ksceIoGetstat( pszPath, &st );
	if ( r < 0 )
	{
		SendReply( VDB_CMD_STAT, r, 0 );
		return;
	}
	int n = FormatEntry( (char *)ReplyPayload(), DATA_CHUNK, &st, pszPath );
	SendReply( VDB_CMD_STAT, 0, n > 0 ? n : 0 );
}

// GET: 8-byte size, then the file in DATA_CHUNK frames, then an empty frame
static void CmdGet( const char *pszPath )
{
	SceIoStat st;
	memset( &st, 0, sizeof( st ) );
	int r = ksceIoGetstat( pszPath, &st );
	SceUID fd = r < 0 ? r : ksceIoOpen( pszPath, SCE_O_RDONLY, 0 );
	if ( fd < 0 )
	{
		SendReply( VDB_CMD_GET, fd, 0 );
		return;
	}
	unsigned long long nSize = (unsigned long long)st.st_size;
	memcpy( ReplyPayload(), &nSize, 8 );
	if ( SendReply( VDB_CMD_GET, 0, 8 ) < 0 )
	{
		ksceIoClose( fd );
		return;
	}
	int nErr = 0;
	for ( ;; )
	{
		int n = ksceIoRead( fd, ReplyPayload(), DATA_CHUNK );
		if ( n < 0 )
		{
			nErr = n;
			break;
		}
		if ( n == 0 )
			break;
		if ( SendReply( VDB_CMD_GET, 0, n ) < 0 )
		{
			ksceIoClose( fd );
			return;
		}
	}
	ksceIoClose( fd );
	SendReply( VDB_CMD_GET, nErr, 0 );
}

static void PutAbort( void )
{
	if ( s_PutFd >= 0 )
		ksceIoClose( s_PutFd );
	s_PutFd = -1;
}

static void CmdPutOpen( const char *pszPath )
{
	PutAbort();
	s_nPutError = 0;
	s_nPutBytes = 0;
	s_PutFd = ksceIoOpen( pszPath, SCE_O_WRONLY | SCE_O_CREAT | SCE_O_TRUNC, 0777 );
	SendReply( VDB_CMD_PUT_OPEN, s_PutFd < 0 ? s_PutFd : 0, 0 );
}

static void CmdPutData( const unsigned char *pData, unsigned nLen )
{
	if ( s_PutFd < 0 )
	{
		s_nPutError = -1;
		return;
	}
	if ( s_nPutError )
		return;
	while ( nLen )
	{
		int n = ksceIoWrite( s_PutFd, pData, nLen );
		if ( n <= 0 )
		{
			s_nPutError = n < 0 ? n : -1;
			return;
		}
		pData += n;
		nLen -= n;
		s_nPutBytes += n;
	}
}

static void CmdPutClose( void )
{
	int nStatus = s_nPutError;
	if ( s_PutFd >= 0 )
	{
		int r = ksceIoClose( s_PutFd );
		if ( r < 0 && !nStatus )
			nStatus = r;
	}
	else if ( !nStatus )
		nStatus = -1;
	s_PutFd = -1;
	memcpy( ReplyPayload(), &s_nPutBytes, 8 );
	SendReply( VDB_CMD_PUT_CLOSE, nStatus, 8 );
}

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

// Called from the thread loop every 200 ms
static void InputTick( void )
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

// The process whose framebuffer is on the screen (the foreground app, or the shell)
static SceUID ScreenOwner( SceDisplayFrameBufInfo *pInfo )
{
	memset( pInfo, 0, sizeof( *pInfo ) );
	pInfo->size = sizeof( *pInfo );
	int head = ksceDisplayGetPrimaryHead();
	int r = ksceDisplayGetProcFrameBufInternal( -1, head, 0, pInfo );
	if ( r < 0 || pInfo->paddr == 0 )
		r = ksceDisplayGetProcFrameBufInternal( -1, head, 1, pInfo );
	if ( r < 0 )
		return r;
	return pInfo->pid;
}

// Requests for the user-side companion (vdb_shell.suprx inside SceShell):
// launching through ksceAppMgrLaunchAppByPath from this kernel thread took
// the whole system down twice (kernel panic, Vita off), so apps start the
// way the shell starts them, sceAppMgrLaunchAppByUri, called by the
// companion. The kernel only queues the request; the companion polls it
// through the vdb_shell_poll syscall and reports the result
static char s_ShellReq[128];
static int s_bShellReqPending;
static unsigned long long s_nShellLastPoll;

static int ShellCompanionAlive( void )
{
	return s_nShellLastPoll && ksceKernelGetSystemTimeWide() - s_nShellLastPoll < 5 * 1000 * 1000ULL;
}

// LAUNCH: payload is a title id ("ABCD12345") or a full uri ("psgm:play?titleid=...")
static void CmdLaunch( const char *pszWhat, unsigned nPayload )
{
	if ( !ShellCompanionAlive() )
	{
		SendReply( VDB_CMD_LAUNCH, -5, 0 );	// no companion in the shell
		return;
	}
	if ( strchr( pszWhat, ':' ) )
		snprintf( s_ShellReq, sizeof( s_ShellReq ), "%s", pszWhat );
	else
		snprintf( s_ShellReq, sizeof( s_ShellReq ), "psgm:play?titleid=%s", pszWhat );
	s_bShellReqPending = 1;
	Log( "vdb: launch request queued for the shell: %s", s_ShellReq );
	SendReply( VDB_CMD_LAUNCH, 0, 0 );
}

static void QueueShellDestroy( void )
{
	snprintf( s_ShellReq, sizeof( s_ShellReq ), "destroy" );
	s_bShellReqPending = 1;
}

static void CmdKill( int pid )
{
	SceDisplayFrameBufInfo info;
	if ( pid == 0 && ShellCompanionAlive() )
	{
		// the shell closes the running app the way the user would
		QueueShellDestroy();
		memset( ReplyPayload(), 0, 4 );
		SendReply( VDB_CMD_KILL, 0, 4 );
		return;
	}
	if ( pid == 0 )
		pid = ScreenOwner( &info );
	SceUID shell = ksceKernelSysrootGetShellPid();
	if ( pid <= 0 || pid == shell )
	{
		// never the shell: a dead SceShell means a reboot
		memcpy( ReplyPayload(), &pid, 4 );
		SendReply( VDB_CMD_KILL, pid <= 0 ? pid : -3, 4 );
		return;
	}
	int r = ksceAppMgrKillProcess( pid );
	Log( "vdb: kill pid 0x%08x: 0x%08x", pid, r );
	memcpy( ReplyPayload(), &pid, 4 );
	SendReply( VDB_CMD_KILL, r, 4 );
}

// SCREEN: header, then the framebuffer rows copied out of the owning process
static void CmdScreen( void )
{
	SceDisplayFrameBufInfo info;
	SceUID pid = ScreenOwner( &info );
	if ( pid < 0 )
	{
		SendReply( VDB_CMD_SCREEN, pid, 0 );
		return;
	}
	VdbScreenHdr_t hdr;
	hdr.width = info.framebuf.width;
	hdr.height = info.framebuf.height;
	hdr.pitch = info.framebuf.pitch;
	hdr.pixelformat = info.framebuf.pixelformat;
	hdr.pid = pid;
	hdr.bytes = hdr.pitch * hdr.height * 4;
	if ( hdr.width == 0 || hdr.height == 0 || hdr.pitch < hdr.width || hdr.bytes > 8 * 1024 * 1024 )
	{
		SendReply( VDB_CMD_SCREEN, -4, 0 );
		return;
	}
	memcpy( ReplyPayload(), &hdr, sizeof( hdr ) );
	if ( SendReply( VDB_CMD_SCREEN, 0, sizeof( hdr ) ) < 0 )
		return;
	const unsigned char *pSrc = (const unsigned char *)info.framebuf.base;
	unsigned nDone = 0;
	int nErr = 0;
	while ( nDone < hdr.bytes )
	{
		unsigned n = hdr.bytes - nDone;
		if ( n > DATA_CHUNK )
			n = DATA_CHUNK;
		int r = ksceKernelCopyFromUserProc( pid, ReplyPayload(), pSrc + nDone, n );
		if ( r < 0 )
		{
			nErr = r;
			break;
		}
		if ( SendReply( VDB_CMD_SCREEN, 0, n ) < 0 )
			return;
		nDone += n;
	}
	SendReply( VDB_CMD_SCREEN, nErr, 0 );
}

// Commands from the host. Framed (VdbCmdHdr_t + payload), or the 0.1
// single bytes 'P' ping and 'S' status, which only log
static void HandleCommand( unsigned char *pCmd, int nLen )
{
	if ( nLen < 1 )
		return;
	if ( nLen < (int)sizeof( VdbCmdHdr_t ) )
	{
		if ( pCmd[0] == 'P' )
			Log( "vdb: pong" );
		else if ( pCmd[0] == 'S' )
			Log( "vdb: status: ring %u used of %u, dropped %u, uptime %u s", s_nRingWrite - s_nRingRead, RING_SIZE, s_nDropped,
				(unsigned)( ksceKernelGetSystemTimeWide() / 1000000 ) );
		// anything else short is the host's frame padding: ignore
		return;
	}
	VdbCmdHdr_t hdr;
	memcpy( &hdr, pCmd, sizeof( hdr ) );
	if ( hdr.magic != VDB_CMD_MAGIC )
	{
		Log( "vdb: bad command frame (magic 0x%08x, %d bytes)", hdr.magic, nLen );
		return;
	}
	unsigned char *pPayload = pCmd + sizeof( hdr );
	unsigned nPayload = (unsigned)nLen - sizeof( hdr );
	if ( hdr.len < nPayload )
		nPayload = hdr.len;	// the rest is padding
	// paths: force termination inside the buffer
	if ( hdr.cmd != VDB_CMD_PUT_DATA && nPayload < RX_SIZE - sizeof( hdr ) )
		pPayload[nPayload] = 0;
	else if ( hdr.cmd != VDB_CMD_PUT_DATA )
		pPayload[RX_SIZE - sizeof( hdr ) - 1] = 0;
	const char *pszPath = (const char *)pPayload;

	switch ( hdr.cmd )
	{
	case VDB_CMD_PING:
		SendReply( VDB_CMD_PING, 0, 0 );
		break;
	case VDB_CMD_STATUS:
	{
		// "rx N" is read by the host to size its command frames
		int n = snprintf( (char *)ReplyPayload(), DATA_CHUNK, "vdb " VDB_VERSION ": ring %u used of %u, dropped %u, uptime %u s, put fd %d, rx %u\n",
			s_nRingWrite - s_nRingRead, RING_SIZE, s_nDropped, (unsigned)( ksceKernelGetSystemTimeWide() / 1000000 ), (int)s_PutFd, (unsigned)RX_SIZE );
		SendReply( VDB_CMD_STATUS, 0, n > 0 ? n : 0 );
		break;
	}
	case VDB_CMD_LIST:
		CmdList( pszPath );
		break;
	case VDB_CMD_STAT:
		CmdStat( pszPath );
		break;
	case VDB_CMD_GET:
		CmdGet( pszPath );
		break;
	case VDB_CMD_PUT_OPEN:
		CmdPutOpen( pszPath );
		break;
	case VDB_CMD_PUT_DATA:
		CmdPutData( pPayload, nPayload );
		break;
	case VDB_CMD_PUT_CLOSE:
		CmdPutClose();
		break;
	case VDB_CMD_MKDIR:
		SendReply( VDB_CMD_MKDIR, ksceIoMkdir( pszPath, 0777 ), 0 );
		break;
	case VDB_CMD_REMOVE:
		SendReply( VDB_CMD_REMOVE, ksceIoRemove( pszPath ), 0 );
		break;
	case VDB_CMD_RMDIR:
		SendReply( VDB_CMD_RMDIR, ksceIoRmdir( pszPath ), 0 );
		break;
	case VDB_CMD_RENAME:
	{
		unsigned nFirst = strlen( pszPath );
		const char *pszNew = nFirst + 1 < nPayload ? pszPath + nFirst + 1 : "";
		SendReply( VDB_CMD_RENAME, ksceIoRename( pszPath, pszNew ), 0 );
		break;
	}
	case VDB_CMD_LAUNCH:
		CmdLaunch( pszPath, nPayload );
		break;
	case VDB_CMD_KILL:
		CmdKill( (int)hdr.arg );
		break;
	case VDB_CMD_SCREEN:
		CmdScreen();
		break;
	case VDB_CMD_INPUT:
		if ( nPayload < sizeof( VdbInput_t ) )
		{
			SendReply( VDB_CMD_INPUT, -2, 0 );
			break;
		}
		memcpy( &s_Input, pPayload, sizeof( s_Input ) );
		if ( s_Input.hold_ms > 10000 )
			s_Input.hold_ms = 10000;
		s_nInputUntil = ksceKernelGetSystemTimeWide() + (unsigned long long)s_Input.hold_ms * 1000;
		ApplyInput( 0 );
		SendReply( VDB_CMD_INPUT, 0, 0 );
		break;
	case VDB_CMD_POWER:
	{
		// reply first (the host waits for it), then act once the reply is on
		// the wire: a reboot from the Mac closes the edit-build-install loop
		// without touching the Vita, and the plugin can replace itself
		static const char *s_pszActions[] = { "cold reboot", "soft reboot", "power off", "suspend" };
		if ( hdr.arg > 3 )
		{
			SendReply( VDB_CMD_POWER, -2, 0 );
			break;
		}
		PutAbort();
		Log( "vdb: %s requested by the host", s_pszActions[hdr.arg] );
		SendReply( VDB_CMD_POWER, 0, 0 );
		ksceKernelDelayThread( 300 * 1000 );
		int ret = hdr.arg == 0 ? kscePowerRequestColdReset() : hdr.arg == 1 ? kscePowerRequestSoftReset()
			: hdr.arg == 2 ? kscePowerRequestStandby() : kscePowerRequestSuspend();
		Log( "vdb: %s request returned 0x%08x", s_pszActions[hdr.arg], ret );
		break;
	}
	default:
		Log( "vdb: unknown command %u (%d bytes)", hdr.cmd, nLen );
		SendReply( hdr.cmd, -2, 0 );
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
	ksceUdcdReqCancelAll( &endpoints[VDB_EP_DATA] );
	ksceUdcdClearFIFO( &endpoints[VDB_EP_DATA] );
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
			{
				Log( "vdb: alive %u (uptime %u s, ring %u used, dropped %u, host %s)", ++nBeats,
					(unsigned)( ksceKernelGetSystemTimeWide() / 1000000 ), s_nRingWrite - s_nRingRead, s_nDropped, bConnected ? "attached" : "none" );
				// Keep the screen on and the Vita awake while the bridge is
				// up: a Vita that locked itself went unreachable over USB
				// more than once in the middle of a test session
				if ( s_bActive )
					ksceKernelPowerTick( SCE_KERNEL_POWER_TICK_DEFAULT );
			}
			InputTick();
			// Starting an app makes the system re-activate its own USB
			// device (0x4E4, Content Manager) and ours silently goes away:
			// the first `launch` from the host left the Vita unreachable.
			// Once a second, while we believe we own the port, check the
			// system driver and take the port back if it came up
			if ( s_bActive && nTicks % 5 == 0 )
			{
				int mtp = ksceUdcdGetDrvState( "USB_MTP_Driver" );
				if ( mtp & SCE_UDCD_STATUS_ACTIVATED )
				{
					s_bActive = 0;
					bConnected = 0;
					bSending = 0;
					PutAbort();
					int r = vdb_usb_start();
					Log( "vdb: the system took the USB port (mtp state 0x%x), retaken: 0x%08x", mtp, r );
					char note[96];
					snprintf( note, sizeof( note ), "vdb: port retaken after the system took it: 0x%08x (uptime %u s)\n", r,
						(unsigned)( ksceKernelGetSystemTimeWide() / 1000000 ) );
					FileNote( note );
				}
			}
			SceCtrlData pad;
			memset( &pad, 0, sizeof( pad ) );
			if ( ksceCtrlPeekBufferPositive( 0, &pad, 1 ) > 0 )
			{
				const unsigned nCombo = SCE_CTRL_LTRIGGER | SCE_CTRL_RTRIGGER | SCE_CTRL_START;
				const unsigned nToggle = nCombo | SCE_CTRL_SELECT, nStatus = nCombo | SCE_CTRL_UP;
				unsigned nNow = pad.buttons;
				// fires once when all keys of a combo are held, in any order
				int bToggle = ( nNow & nToggle ) == nToggle && ( nPrevButtons & nToggle ) != nToggle;
				int bStatus = ( nNow & nStatus ) == nStatus && ( nPrevButtons & nStatus ) != nStatus;
				nPrevButtons = nNow;
				if ( bToggle )
				{
					int r = s_bActive ? vdb_usb_stop() : vdb_usb_start();
					char note[96];
					snprintf( note, sizeof( note ), "vdb: hotkey %s: 0x%08x (uptime %u s)\n", s_bActive ? "bridge on" : "bridge off", r,
						(unsigned)( ksceKernelGetSystemTimeWide() / 1000000 ) );
					FileNote( note );
					Log( "%s", note );
				}
				else if ( bStatus )
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
			PutAbort();	// a half-written file stays, truncated; the host reports the failure
			if ( s_nInputUntil )
			{
				s_nInputUntil = 0;
				ApplyInput( 1 );
			}
		}
		if ( bits & EVF_RECEIVED )
		{
			if ( bConnected )
			{
				ksceKernelDcacheInvalidateRange( s_RxBuf, RX_SIZE );
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
// Companion syscalls (vdb_shell.suprx). poll: copies a pending request to
// the caller and returns its length, 0 when there is none
int vdb_shell_poll( char *pUserOut, unsigned nSize )
{
	s_nShellLastPoll = ksceKernelGetSystemTimeWide();
	if ( !s_bShellReqPending || !pUserOut || nSize < 2 )
		return 0;
	unsigned n = strlen( s_ShellReq ) + 1;
	if ( n > nSize )
		n = nSize;
	s_ShellReq[n - 1] = 0;
	if ( ksceKernelMemcpyKernelToUser( pUserOut, s_ShellReq, n ) < 0 )
		return -1;
	s_bShellReqPending = 0;
	return (int)n;
}

int vdb_shell_result( int nCode )
{
	Log( "vdb: shell companion: request done, result 0x%08x", nCode );
	return 0;
}

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
	// decide the auto start before the thread runs (it read the flag
	// before module_start set it: the takeover never happened)
	{
		SceUID fdFlag = ksceIoOpen( VDB_FLAG_FILE, SCE_O_RDONLY, 0 );
		if ( fdFlag >= 0 )
		{
			ksceIoClose( fdFlag );
			s_bAutoStart = 1;
		}
	}
	int st = ksceKernelStartThread( s_Thread, 0, NULL );
	{
		char note[96];
		snprintf( note, sizeof( note ), "vdb: thread 0x%08x start 0x%08x\n", (unsigned)s_Thread, st );
		FileNote( note );
	}

	// the flag file decides whether we take the USB port at boot
	if ( s_bAutoStart )
	{
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
