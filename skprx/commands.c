// vdb: the host protocol, command by command
#include "vdb.h"

//-----------------------------------------------------------------------------
// Files, on the kernel's own IO: every mounted device is reachable
//-----------------------------------------------------------------------------
static SceUID s_PutFd = -1;			// file being written by PUT_OPEN .. PUT_CLOSE
static int s_nPutError;
static unsigned long long s_nPutBytes;

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
		if ( nUsed + n > ReplyCapacity() )
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
	int n = FormatEntry( (char *)ReplyPayload(), ReplyCapacity(), &st, pszPath );
	SendReply( VDB_CMD_STAT, 0, n > 0 ? n : 0 );
}

// GET: 8-byte size, then the file in reply-sized frames, then an empty frame
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
		int n = ksceIoRead( fd, ReplyPayload(), ReplyCapacity() );
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

void PutAbort( void )
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

//-----------------------------------------------------------------------------
// Apps. Requests for the user-side companion (vdb_shell.suprx inside
// SceShell): launching through ksceAppMgrLaunchAppByPath from this kernel
// thread took the whole system down twice (kernel panic, Vita off), so apps
// start the way the shell starts them, sceAppMgrLaunchAppByUri, called by
// the companion. The kernel only queues the request; the companion polls it
// through the vdb_shell_poll syscall and reports the result
//-----------------------------------------------------------------------------
static char s_ShellReq[128];
static int s_bShellReqPending;
static unsigned long long s_nShellLastPoll;

static int ShellCompanionAlive( void )
{
	return s_nShellLastPoll && ksceKernelGetSystemTimeWide() - s_nShellLastPoll < 5 * 1000 * 1000ULL;
}

// LAUNCH: payload is a title id ("ABCD12345") or a full uri ("psgm:play?titleid=...")
static void CmdLaunch( const char *pszWhat )
{
	if ( !ShellCompanionAlive() )
	{
		SendReply( VDB_CMD_LAUNCH, -5, 0 );
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

SceUID ScreenOwnerPid( void )
{
	SceDisplayFrameBufInfo info;
	return ScreenOwner( &info );
}

// PEEK: a process's memory, streamed like a file
static void CmdPeek( SceUID pid, const unsigned char *pPayload, unsigned nPayload )
{
	if ( nPayload < 8 )
	{
		SendReply( VDB_CMD_PEEK, -2, 0 );
		return;
	}
	unsigned nAddr, nLen;
	memcpy( &nAddr, pPayload, 4 );
	memcpy( &nLen, pPayload + 4, 4 );
	if ( pid == 0 )
		pid = ScreenOwnerPid();
	if ( pid < 0 || nLen > 64 * 1024 * 1024 )
	{
		SendReply( VDB_CMD_PEEK, pid < 0 ? pid : -2, 0 );
		return;
	}
	unsigned nDone = 0;
	int nErr = 0;
	while ( nDone < nLen )
	{
		unsigned n = nLen - nDone;
		if ( n > ReplyCapacity() )
			n = ReplyCapacity();
		int r = ksceKernelCopyFromUserProc( pid, ReplyPayload(), (const void *)( nAddr + nDone ), n );
		if ( r < 0 )
		{
			nErr = r;
			break;
		}
		if ( SendReply( VDB_CMD_PEEK, 0, n ) < 0 )
			return;
		nDone += n;
	}
	SendReply( VDB_CMD_PEEK, nErr, 0 );
}

// POKE: bytes into a process's memory
static void CmdPoke( SceUID pid, const unsigned char *pPayload, unsigned nPayload )
{
	if ( nPayload < 5 )
	{
		SendReply( VDB_CMD_POKE, -2, 0 );
		return;
	}
	unsigned nAddr;
	memcpy( &nAddr, pPayload, 4 );
	if ( pid == 0 )
		pid = ScreenOwnerPid();
	if ( pid < 0 )
	{
		SendReply( VDB_CMD_POKE, pid, 0 );
		return;
	}
	unsigned nLen = nPayload - 4;
	int r = ksceKernelCopyToUserProc( pid, (void *)nAddr, pPayload + 4, nLen );
	unsigned nWritten = r < 0 ? 0 : nLen;
	Log( "vdb: poke pid 0x%08x addr 0x%08x %u bytes: 0x%08x", pid, nAddr, nLen, r );
	memcpy( ReplyPayload(), &nWritten, 4 );
	SendReply( VDB_CMD_POKE, r < 0 ? r : 0, 4 );
}

static void CmdKill( int pid )
{
	SceDisplayFrameBufInfo info;
	if ( pid == 0 && ShellCompanionAlive() )
	{
		// the shell closes the running app the way the user would
		snprintf( s_ShellReq, sizeof( s_ShellReq ), "destroy" );
		s_bShellReqPending = 1;
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
		if ( n > ReplyCapacity() )
			n = ReplyCapacity();
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

// ScePower's charging status: 0 none, 1 charging (charger), 3 charging over
// USB (Syscon's USB charge flags); kscePowerIsBatteryCharging is false on a
// host port whatever happens, so it is not shown
static const char *ChargingText( void )
{
	static char s_Text[32];
	int st = kscePowerGetBatteryChargingStatus();
	snprintf( s_Text, sizeof( s_Text ), st == 3 ? "charging over usb" : st == 1 ? "charging" : st == 0 ? "not charging" : "charging status %d", st );
	return s_Text;
}

static void CmdStatus( void )
{
	// "rx N" is read by the host to size its command frames
	int n = snprintf( (char *)ReplyPayload(), ReplyCapacity(),
		"vdb " VDB_VERSION ": ring %u used of %u, dropped %u, uptime %u s, put fd %d, rx %u, battery %d%% (%d mAh) %s, usb state 0x%x, awake %d, kprintf %s\n",
		RingUsed(), RING_SIZE, RingDropped(), Uptime(), (int)s_PutFd, (unsigned)RX_SIZE,
		kscePowerGetBatteryLifePercent(), kscePowerGetBatteryRemainCapacity(), ChargingText(),
		(unsigned)ksceUdcdGetDeviceState(), g_nAwakeMode, KprintfCapturing() ? "on" : "off" );
	SendReply( VDB_CMD_STATUS, 0, n > 0 ? n : 0 );
}

static void CmdPower( unsigned nAction )
{
	// reply first (the host waits for it), then act once the reply is on
	// the wire: a reboot from the Mac closes the edit-build-install loop
	// without touching the Vita, and the plugin can replace itself
	static const char *s_pszActions[] = { "cold reboot", "soft reboot", "power off", "suspend" };
	if ( nAction > 3 )
	{
		SendReply( VDB_CMD_POWER, -2, 0 );
		return;
	}
	PutAbort();
	Log( "vdb: %s requested by the host", s_pszActions[nAction] );
	SendReply( VDB_CMD_POWER, 0, 0 );
	ksceKernelDelayThread( 300 * 1000 );
	int ret = nAction == 0 ? kscePowerRequestColdReset() : nAction == 1 ? kscePowerRequestSoftReset()
		: nAction == 2 ? kscePowerRequestStandby() : kscePowerRequestSuspend();
	Log( "vdb: %s request returned 0x%08x", s_pszActions[nAction], ret );
}

//-----------------------------------------------------------------------------
// Dispatcher. A frame is VdbCmdHdr_t + payload, or one of the 0.1 single
// bytes 'P' ping and 'S' status, which only log
//-----------------------------------------------------------------------------
void HandleCommand( unsigned char *pCmd, int nLen )
{
	if ( nLen < 1 )
		return;
	if ( nLen < (int)sizeof( VdbCmdHdr_t ) )
	{
		if ( pCmd[0] == 'P' )
			Log( "vdb: pong" );
		else if ( pCmd[0] == 'S' )
			Log( "vdb: status: ring %u used of %u, dropped %u, uptime %u s", RingUsed(), RING_SIZE, RingDropped(), Uptime() );
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
		CmdStatus();
		break;
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
	case VDB_CMD_POWER:
		CmdPower( hdr.arg );
		break;
	case VDB_CMD_LAUNCH:
		CmdLaunch( pszPath );
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
		InputSet( (const VdbInput_t *)pPayload );
		SendReply( VDB_CMD_INPUT, 0, 0 );
		break;
	case VDB_CMD_TOUCH:
		if ( nPayload < 12 || !TouchAvailable() )
		{
			SendReply( VDB_CMD_TOUCH, nPayload < 12 ? -2 : -6, 0 );
			break;
		}
		TouchSet( (const VdbTouch_t *)pPayload, nPayload );
		SendReply( VDB_CMD_TOUCH, 0, 0 );
		break;
	case VDB_CMD_OSD:
		if ( !OsdAvailable() )
		{
			SendReply( VDB_CMD_OSD, -6, 0 );
			break;
		}
		// "@status" turns the status overlay on (the hotkey L+R+Start+Up
		// does the same); any other text replaces it, empty turns all off
		g_bStatusOsd = strcmp( pszPath, "@status" ) == 0;
		if ( !g_bStatusOsd )
			OsdSet( pszPath );
		SendReply( VDB_CMD_OSD, 0, 0 );
		break;
	case VDB_CMD_PEEK:
		CmdPeek( (SceUID)hdr.arg, pPayload, nPayload );
		break;
	case VDB_CMD_POKE:
		CmdPoke( (SceUID)hdr.arg, pPayload, nPayload );
		break;
	case VDB_CMD_PROCINFO:
		CmdProcInfo( hdr.arg ? (SceUID)hdr.arg : ScreenOwnerPid() );
		break;
	case VDB_CMD_PROFILE:
		CmdProfile( hdr.arg, pPayload, nPayload );
		break;
	case VDB_CMD_AWAKE:
		g_nAwakeMode = hdr.arg > 2 ? 2 : (int)hdr.arg;
		Log( "vdb: keep-awake mode %d (%s)", g_nAwakeMode, g_nAwakeMode == 0 ? "off" : g_nAwakeMode == 1 ? "no standby" : "no standby, screen on" );
		SendReply( VDB_CMD_AWAKE, 0, 0 );
		break;
	case VDB_CMD_KPRINTF:
		KprintfCapture( hdr.arg != 0 );
		Log( "vdb: kernel printf capture %s", hdr.arg ? "on" : "off" );
		SendReply( VDB_CMD_KPRINTF, 0, 0 );
		break;
	default:
		Log( "vdb: unknown command %u (%d bytes)", hdr.cmd, nLen );
		SendReply( hdr.cmd, -2, 0 );
		break;
	}
}

//-----------------------------------------------------------------------------
// Syscalls for the shell companion. poll: copies a pending request to the
// caller and returns its length, 0 when there is none
//-----------------------------------------------------------------------------
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
