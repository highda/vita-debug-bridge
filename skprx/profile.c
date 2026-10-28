// vdb: process information and a sampling profiler. No instrumentation: a
// kernel thread reads the program counter of every thread of the profiled
// process at a fixed interval (the debugger interface the kernel offers) and
// counts the addresses; the host resolves them against the binaries with the
// load bases the app logged
#include "vdb.h"

//-----------------------------------------------------------------------------
// PROCINFO: text lines, "P pid title", "T thid prio status name", "M name base size path"
//-----------------------------------------------------------------------------
static int Emit( unsigned nCmd, unsigned *pnUsed, const char *fmt, ... )
{
	char line[400];
	va_list args;
	va_start( args, fmt );
	int n = vsnprintf( line, sizeof( line ), fmt, args );
	va_end( args );
	if ( n < 0 || n >= (int)sizeof( line ) )
		return 0;
	if ( *pnUsed + n > ReplyCapacity() )
	{
		if ( SendReply( nCmd, 0, *pnUsed ) < 0 )
			return -1;
		*pnUsed = 0;
	}
	memcpy( ReplyPayload() + *pnUsed, line, n );
	*pnUsed += n;
	return 0;
}

static int Flush( unsigned nCmd, unsigned *pnUsed, int nStatus )
{
	if ( *pnUsed && SendReply( nCmd, 0, *pnUsed ) < 0 )
		return -1;
	*pnUsed = 0;
	return SendReply( nCmd, nStatus, 0 );
}

void CmdProcInfo( SceUID pid )
{
	unsigned nUsed = 0;
	if ( pid < 0 )
	{
		SendReply( VDB_CMD_PROCINFO, pid, 0 );
		return;
	}
	char title[32];
	memset( title, 0, sizeof( title ) );
	ksceKernelSysrootGetProcessTitleId( pid, title, sizeof( title ) );
	if ( Emit( VDB_CMD_PROCINFO, &nUsed, "P 0x%08x %s\n", (unsigned)pid, title ) < 0 )
		return;

	static SceUID ids[128];
	int nCopied = 0;
	int r = ksceKernelGetThreadIdList( pid, ids, 128, &nCopied );
	for ( int i = 0; r >= 0 && i < nCopied; i++ )
	{
		static SceKernelThreadInfo info;
		memset( &info, 0, sizeof( info ) );
		info.size = sizeof( info );
		if ( ksceKernelGetThreadInfo( ids[i], &info ) < 0 )
			continue;
		if ( Emit( VDB_CMD_PROCINFO, &nUsed, "T 0x%08x %d %d %s\n", (unsigned)ids[i], info.currentPriority, info.status, info.name ) < 0 )
			return;
	}

	static SceUID mods[64];
	SceSize nMods = 64;
	r = ksceKernelGetModuleList( pid, 0x7FFFFFFF, 1, mods, &nMods );
	for ( SceSize i = 0; r >= 0 && i < nMods; i++ )
	{
		static SceKernelModuleInfo mi;
		memset( &mi, 0, sizeof( mi ) );
		mi.size = sizeof( mi );
		if ( ksceKernelGetModuleInfo( pid, mods[i], &mi ) < 0 )
			continue;
		if ( Emit( VDB_CMD_PROCINFO, &nUsed, "M %s 0x%08x 0x%x %s\n", mi.module_name, (unsigned)mi.segments[0].vaddr, (unsigned)mi.segments[0].memsz, mi.path ) < 0 )
			return;
	}
	Flush( VDB_CMD_PROCINFO, &nUsed, 0 );
}

//-----------------------------------------------------------------------------
// Profiler. Per thread (up to PROF_THREADS of the process), a small open
// hash of program counters. The sampler runs as its own kernel thread at a
// high priority and sleeps for the interval between rounds; a sample of a
// thread that is not running still tells where it waits, so the running
// state is recorded with each thread
//-----------------------------------------------------------------------------
#define PROF_THREADS	12
#define PROF_BUCKETS	4096	// per thread, power of two
typedef struct
{
	unsigned pc, lr, ra, ra2, count;	// lr: the return address, so the host attributes a sample in a syscall stub to its caller;
								// ra: the first return address found on the stack, for when lr is in the stub as well
} ProfBucket_t;

typedef struct
{
	SceUID thid;
	char name[32];
	unsigned nSamples, nRunning, nOverflow;
	ProfBucket_t buckets[PROF_BUCKETS];
} ProfThread_t;

static ProfThread_t s_Prof[PROF_THREADS];
static int s_nProfThreads;
static SceUID s_ProfPid = -1, s_ProfThread = -1;
static volatile int s_bProfRun;
static unsigned s_nProfInterval = 1000, s_nProfRounds;

static ProfThread_t *ProfSlot( SceUID thid )
{
	for ( int i = 0; i < s_nProfThreads; i++ )
		if ( s_Prof[i].thid == thid )
			return &s_Prof[i];
	if ( s_nProfThreads >= PROF_THREADS )
		return NULL;
	ProfThread_t *p = &s_Prof[s_nProfThreads++];
	memset( p, 0, sizeof( *p ) );
	p->thid = thid;
	SceKernelThreadInfo info;
	memset( &info, 0, sizeof( info ) );
	info.size = sizeof( info );
	if ( ksceKernelGetThreadInfo( thid, &info ) >= 0 )
		snprintf( p->name, sizeof( p->name ), "%s", info.name );
	return p;
}

static void ProfCount( ProfThread_t *p, unsigned pc, unsigned lr, unsigned ra, unsigned ra2 )
{
	// keyed on pc and lr only: the stack address is kept from the first
	// sample, keying on it as well overflowed the table on a flat profile
	unsigned h = ( ( pc >> 1 ) ^ ( lr << 7 ) ) * 2654435761u;
	for ( unsigned i = 0; i < 16; i++ )
	{
		ProfBucket_t *b = &p->buckets[( h + i ) & ( PROF_BUCKETS - 1 )];
		if ( b->count == 0 )
		{
			b->pc = pc;
			b->lr = lr;
			b->ra = ra;
			b->ra2 = ra2;
			b->count = 1;
			return;
		}
		if ( b->pc == pc && b->lr == lr )
		{
			if ( !b->ra )
			{
				b->ra = ra;
				b->ra2 = ra2;
			}
			b->count++;
			return;
		}
	}
	p->nOverflow++;
}

static int ProfSampler( SceSize args, void *argp )
{
	static SceUID ids[64];
	unsigned nRound = 0;
	while ( s_bProfRun )
	{
		ksceKernelDelayThread( s_nProfInterval );
		int nCopied = 0;
		if ( ksceKernelGetThreadIdList( s_ProfPid, ids, 64, &nCopied ) < 0 )
			continue;
		for ( int i = 0; i < nCopied; i++ )
		{
			ProfThread_t *p = ProfSlot( ids[i] );
			if ( !p )
				continue;
			// the running state at this instant, before the suspend changes it
			SceKernelThreadInfo info;
			memset( &info, 0, sizeof( info ) );
			info.size = sizeof( info );
			// running, or ready: the sampler itself runs at a high priority
			// and preempts the thread it is about to look at, which then
			// reads as ready though it wanted the core
			int bRunning = ksceKernelGetThreadInfo( ids[i], &info ) >= 0 && ( info.status & ( SCE_THREAD_RUNNING | SCE_THREAD_READY ) );
			// the registers can be read from a debug-suspended thread only:
			// suspend, read, resume, a few microseconds per thread. The
			// status word's meaning is undocumented; the first accepted one
			// of a few candidates is kept
			static int s_nSuspendStatus = 0;
			static const int s_Candidates[] = { 0x1, 0x2, 0x100, 0x1000, 0x10000 };
			SceThreadCpuRegisters regs;
			memset( &regs, 0, sizeof( regs ) );
			int rs = -1, rr = -1;
			if ( s_nSuspendStatus )
				rs = ksceKernelDebugSuspendThread( ids[i], s_nSuspendStatus );
			else
			{
				for ( unsigned c = 0; c < sizeof( s_Candidates ) / sizeof( s_Candidates[0] ); c++ )
				{
					rs = ksceKernelDebugSuspendThread( ids[i], s_Candidates[c] );
					if ( rs >= 0 )
					{
						s_nSuspendStatus = s_Candidates[c];
						Log( "vdb: profiler: suspend status 0x%x accepted", s_nSuspendStatus );
						break;
					}
				}
			}
			if ( rs >= 0 )
			{
				rr = ksceKernelGetThreadCpuRegisters( ids[i], &regs );
				// a thread that was running on another core has no saved
				// context until it has actually stopped: give it a moment
				if ( rr >= 0 && regs.entry[0].pc == 0 && regs.entry[1].pc == 0 )
				{
					ksceKernelDelayThread( 30 );
					rr = ksceKernelGetThreadCpuRegisters( ids[i], &regs );
				}
				ksceKernelDebugResumeThread( ids[i], s_nSuspendStatus );
			}
			if ( rs < 0 || rr < 0 )
			{
				static int s_nLogged;
				if ( s_nLogged < 3 )
				{
					s_nLogged++;
					Log( "vdb: profiler: thread 0x%08x: suspend 0x%08x registers 0x%08x", (unsigned)ids[i], rs, rr );
				}
				continue;
			}
			// User code lives at 0x80000000 and above on the Vita, the
			// kernel below. A thread blocked in a syscall carries its kernel
			// PC in one entry and the app's PC, where it called in, in the
			// other; the app's is the one that profiles the program
			static int s_bDumped;
			if ( !s_bDumped )
			{
				s_bDumped = 1;
				Log( "vdb: profiler: first sample: entry0 pc %08x sp %08x lr %08x cpsr %08x, entry1 pc %08x sp %08x lr %08x cpsr %08x",
					regs.entry[0].pc, regs.entry[0].sp, regs.entry[0].lr, regs.entry[0].cpsr, regs.entry[1].pc, regs.entry[1].sp, regs.entry[1].lr, regs.entry[1].cpsr );
			}
			// A CPU profile: only samples of a thread that was running count
			// towards its histogram; waiting samples are only counted. Of the
			// two register sets the one in user mode (cpsr mode bits 0x10)
			// holds the app's program counter, whether the thread was
			// interrupted by the suspend or sits in a syscall
			static int s_bDumpedRunning;
			if ( !s_bDumpedRunning && bRunning )
			{
				s_bDumpedRunning = 1;
				Log( "vdb: profiler: first running sample: entry0 pc %08x lr %08x cpsr %08x, entry1 pc %08x lr %08x cpsr %08x",
					regs.entry[0].pc, regs.entry[0].lr, regs.entry[0].cpsr, regs.entry[1].pc, regs.entry[1].lr, regs.entry[1].cpsr );
			}
			unsigned pc = 0, lr = 0, sp = 0;
			for ( int e = 0; e < 2; e++ )
				if ( ( regs.entry[e].cpsr & 0x1F ) == 0x10 && regs.entry[e].pc >= 0x80000000u )
				{
					pc = regs.entry[e].pc;
					lr = regs.entry[e].lr;
					sp = regs.entry[e].sp;
				}
			if ( !pc )
			{
				int e = regs.entry[1].pc >= 0x80000000u ? 1 : 0;
				pc = regs.entry[e].pc;
				lr = regs.entry[e].lr;
				sp = regs.entry[e].sp;
			}
			p->nSamples++;
			if ( bRunning )
			{
				// the top of the user stack: the first word that looks like a
				// code address and is not lr is the stub's own return address
				// when pc and lr both sit in a system library's stub (its
				// inner call pushed lr). Cheap: 64 bytes from the process
				// (two of them: the second is the caller of the first's
				// function when that is a small helper such as the pthread
				// TLS lookup). Any user address with the Thumb bit; even
				// words are mostly data pointers, all-ones is a sentinel
				unsigned ra = 0, ra2 = 0, words[24];
				if ( sp >= 0x80000000u && ksceKernelCopyFromUserProc( s_ProfPid, words, (const void *)( sp & ~3u ), sizeof( words ) ) >= 0 )
					for ( int w = 0; w < 24 && !ra2; w++ )
						if ( words[w] >= 0x80000000u && words[w] != 0xFFFFFFFFu && words[w] != lr && ( words[w] & 1 ) )
						{
							if ( !ra )
								ra = words[w];
							else if ( words[w] != ra )
								ra2 = words[w];
						}
				p->nRunning++;
				ProfCount( p, pc, lr, ra, ra2 );
			}
		}
		nRound++;
		s_nProfRounds = nRound;
	}
	return 0;
}

void ProfileStop( void )
{
	if ( s_ProfThread < 0 )
		return;
	s_bProfRun = 0;
	ksceKernelWaitThreadEnd( s_ProfThread, NULL, NULL );
	ksceKernelDeleteThread( s_ProfThread );
	s_ProfThread = -1;
}

void CmdProfile( unsigned nAction, const unsigned char *pPayload, unsigned nPayload )
{
	if ( nAction == 1 )
	{
		if ( nPayload < 8 )
		{
			SendReply( VDB_CMD_PROFILE, -2, 0 );
			return;
		}
		ProfileStop();
		unsigned pid, us;
		memcpy( &pid, pPayload, 4 );
		memcpy( &us, pPayload + 4, 4 );
		s_ProfPid = pid ? (SceUID)pid : ScreenOwnerPid();
		s_nProfInterval = us < 200 ? 200 : us > 100000 ? 100000 : us;
		s_nProfThreads = 0;
		s_nProfRounds = 0;
		s_bProfRun = 1;
		s_ProfThread = ksceKernelCreateThread( "vdb_prof", ProfSampler, 0x20, 0x2000, 0, 0x10000, 0 );
		if ( s_ProfThread < 0 || ksceKernelStartThread( s_ProfThread, 0, NULL ) < 0 )
		{
			s_bProfRun = 0;
			SendReply( VDB_CMD_PROFILE, s_ProfThread < 0 ? s_ProfThread : -1, 0 );
			return;
		}
		Log( "vdb: profiling pid 0x%08x every %u us", (unsigned)s_ProfPid, s_nProfInterval );
		SendReply( VDB_CMD_PROFILE, 0, 0 );
		return;
	}
	if ( nAction == 0 )
	{
		ProfileStop();
		memcpy( ReplyPayload(), &s_nProfRounds, 4 );
		SendReply( VDB_CMD_PROFILE, 0, 4 );
		return;
	}
	// dump: "R rounds interval pid", then per thread "T thid samples running overflow name" and "S pc lr count" lines
	unsigned nUsed = 0;
	if ( Emit( VDB_CMD_PROFILE, &nUsed, "R %u %u 0x%08x\n", s_nProfRounds, s_nProfInterval, (unsigned)s_ProfPid ) < 0 )
		return;
	for ( int t = 0; t < s_nProfThreads; t++ )
	{
		ProfThread_t *p = &s_Prof[t];
		if ( Emit( VDB_CMD_PROFILE, &nUsed, "T 0x%08x %u %u %u %s\n", (unsigned)p->thid, p->nSamples, p->nRunning, p->nOverflow, p->name ) < 0 )
			return;
		for ( int b = 0; b < PROF_BUCKETS; b++ )
			if ( p->buckets[b].count && Emit( VDB_CMD_PROFILE, &nUsed, "S %x %x %x %x %u\n", p->buckets[b].pc, p->buckets[b].lr, p->buckets[b].ra, p->buckets[b].ra2, p->buckets[b].count ) < 0 )
				return;
	}
	Flush( VDB_CMD_PROFILE, &nUsed, 0 );
}
