// vdb: Vita debug bridge, kernel side. Shared definitions.
//
// A vendor USB device with three bulk pipes: EP1 IN streams a 64 KB log
// ring to the host, EP2 OUT carries framed commands from the host, EP3 IN
// carries the replies and file data. Activated at boot only when the flag
// file ux0:vdbplugin/usb_on exists (VitaShell's USB mass storage keeps
// working otherwise), by the hotkey, or from user space with vdb_usb_start().
//
// Files: main.c (thread, module entry), usb.c (device, transfers),
// commands.c (the host protocol), log.c (ring, boot notes, kernel printf),
// input.c (pad and touch injection), osd.c (on-screen display).
//
// Host: host/vdb.py (pyusb), Sony VID 0x054C, PID VDB_USB_PID.
// Structure after xerpi's vitastick (UDCD driver skeleton), rewritten.
#ifndef VDB_H
#define VDB_H

#include <psp2kern/kernel/modulemgr.h>
#include <psp2kern/kernel/threadmgr.h>
#include <psp2kern/kernel/threadmgr/debugger.h>
#include <psp2kern/kernel/sysmem.h>
#include <psp2kern/kernel/sysclib.h>
#include <psp2kern/kernel/cpu.h>
#include <psp2kern/kernel/suspend.h>
#include <psp2kern/kernel/sysroot.h>
#include <psp2kern/kernel/debug.h>
#include <psp2kern/kernel/sysmem/data_transfers.h>
#include <psp2kern/io/fcntl.h>
#include <psp2kern/io/dirent.h>
#include <psp2kern/io/stat.h>
#include <psp2kern/udcd.h>
#include <psp2kern/ctrl.h>
#include <psp2kern/power.h>
#include <psp2kern/appmgr.h>
#include <psp2kern/display.h>
#include <psp2/touch.h>
#include <taihen.h>
#include <stdarg.h>

#define VDB_DRIVER_NAME	"VDB"
#define VDB_USB_PID		0x0DB6
#define VDB_VERSION		"0.9.0"
#define VDB_DIR			"ux0:vdbplugin/"
#define VDB_FLAG_FILE	VDB_DIR "usb_on"
#define VDB_AWAKE_FILE	VDB_DIR "awake"		// optional: "1" no standby, "2" no standby and no screen off, read at boot
#define VDB_NOHOOKS_FILE	VDB_DIR "nohooks"	// optional: no touch, display or printf hooks (the USB bridge only), read at boot
#define VDB_CONFIG_FILE	VDB_DIR "vdb.cfg"	// the configuration (main.c, ReadConfig); the three flag files above are its predecessors
typedef struct { int bUsb, nAwake, bTouch, bDisplay, bPrintf; } VdbConfig_t;
#define VDB_NOTE_FILE	VDB_DIR "boot.txt"

#define RING_SIZE		( 64 * 1024 )
#define CHUNK_SIZE		4096			// one log transfer on EP1
#define RX_SIZE			( 256 * 1024 )	// one command frame: header + payload. A PUT costs one transfer and one card write per frame: 4 KB frames 0.7 MB/s, 64 KB 2.4, 256 KB 6.0 (SD2Vita)
#define DATA_CHUNK		( 64 * 1024 )	// reply payload per frame on EP3 (GET, SCREEN)

// Event flags the USB thread waits on
enum
{
	EVF_CONNECTED		= 1 << 0,
	EVF_DISCONNECTED	= 1 << 1,
	EVF_DATA			= 1 << 2,	// the log ring has bytes
	EVF_SENT			= 1 << 3,	// EP1 log chunk sent
	EVF_RECEIVED		= 1 << 4,	// EP2 command frame received
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
// Status values: 0 ok, an SCE error when negative, and these of our own:
// -2 bad request, -3 refused (the shell), -4 unusable framebuffer, -5 no
// shell companion, -6 the feature's hook is not installed.
//-----------------------------------------------------------------------------
#define VDB_CMD_MAGIC	0x31424456	// "VDB1"
#define VDB_RSP_MAGIC	0x52424456	// "VDBR"

enum
{
	VDB_CMD_PING		= 1,
	VDB_CMD_STATUS		= 2,	// reply payload: status text ("rx N" in it sizes the host's frames)
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
	VDB_CMD_POWER		= 13,	// arg: 0 cold reboot, 1 soft reboot, 2 power off, 3 suspend; replies, then acts 300 ms later
	VDB_CMD_LAUNCH		= 14,	// payload: title id or psgm: uri, handed to the shell companion
	VDB_CMD_KILL		= 15,	// arg: pid (0 = the running app, through the shell companion); reply payload: 4-byte pid
	VDB_CMD_SCREEN		= 16,	// reply: VdbScreenHdr_t, then the raw framebuffer in frames, then an empty frame
	VDB_CMD_INPUT		= 17,	// payload: VdbInput_t; held for hold_ms, then released
	VDB_CMD_TOUCH		= 18,	// payload: VdbTouch_t; a finger from (x0,y0) to (x1,y1) over ms milliseconds (screen pixels, 960x544)
	VDB_CMD_OSD			= 19,	// payload: text, lines split at \n (empty: off); drawn on every frame the app shows
	VDB_CMD_KPRINTF		= 20,	// arg: 1 mirrors the kernel's printf output into the log ring, 0 stops
	VDB_CMD_PEEK		= 21,	// arg: pid (0 = the process on screen); payload: u32 addr, u32 len; reply: data frames, then an empty frame
	VDB_CMD_POKE		= 22,	// arg: pid; payload: u32 addr, then the bytes; reply: status, payload u32 bytes written
	VDB_CMD_PROCINFO	= 23,	// arg: pid; reply: text lines (process, threads, modules), then an empty frame
	VDB_CMD_PROFILE		= 24,	// arg: 1 start (payload: u32 pid, u32 interval us), 0 stop, 2 dump (text lines, then an empty frame)
	VDB_CMD_AWAKE		= 25,	// arg: 0 let the Vita sleep as it likes, 1 no automatic standby, 2 no standby and no screen off
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
	unsigned buttons;				// SCE_CTRL_* bits
	unsigned char lx, ly, rx, ry;	// sticks, 0x80 = centre
	unsigned hold_ms;				// how long the state is held before the release
} VdbInput_t;

typedef struct
{
	unsigned short x0, y0, x1, y1;	// screen pixels
	unsigned ms;
	unsigned port;					// 0 front, 1 back (optional: a 12-byte payload means front)
} VdbTouch_t;

//-----------------------------------------------------------------------------
// log.c: the ring every log line goes to, boot notes, kernel printf capture
//-----------------------------------------------------------------------------
void RingWrite( const char *pData, unsigned nLen );
unsigned RingTake( unsigned char *pDest, unsigned nMax );
unsigned RingUsed( void );
unsigned RingDropped( void );
void Log( const char *fmt, ... );
void FileNote( const char *pszText );		// a line in ux0:vdbplugin/boot.txt: the only trace when USB does not come up
void FileNotef( const char *fmt, ... );
unsigned Uptime( void );					// seconds since boot
void InstallKprintfCapture( void );
void KprintfCapture( int bOn );
int KprintfCapturing( void );

//-----------------------------------------------------------------------------
// usb.c: the device, its transfers, the takeover of the port
//-----------------------------------------------------------------------------
extern SceUID g_Evf;
extern int g_bActive;				// the bridge owns the USB port
extern int g_bHostAttached;			// a host configured the device (set by the thread)
int UsbRegister( void );
unsigned UsbRxSize( void );				// the command frame size the host may send (RX_SIZE, or 4 KB without the DMA block)				// driver registration and DMA buffers, from module_start
void UsbUnregister( void );
void UsbStartReceiving( void );			// after a host attached
void UsbWatchdog( void );				// from the thread tick: a receive must be outstanding while a host is attached
int UsbTakeFrame( unsigned char **ppFrame );	// a received command frame, 0 when none; queues the next receive
int UsbStreamLog( void );				// sends the next log chunk, 1 when a send is in flight
unsigned char *ReplyPayload( void );
unsigned ReplyCapacity( void );
int SendReply( unsigned nCmd, int nStatus, unsigned nLen );
int kscePowerGetBatteryChargingStatus( void );	// not in this SDK header (NID in ScePowerForDriver)
void UsbRetakeIfLost( void );			// the system took the port (an app launch): take it back
void UsbInstallSuspendHandler( void );	// standby: hands off during the system's suspend, retakes after resume
int UsbSuspended( void );				// the system is between suspend and resume
void UsbSuspendTick( void );			// thread tick: notes the window, clears an abandoned one
const char *UsbSuspendNotes( void );	// the last suspend/resume event ids, for the log
void UsbStatusLines( char *pBuf, unsigned nSize );	// three lines of USB state for the status overlay
void UsbRetakeReset( void );			// an explicit start (hotkey, host) clears the give-up
extern int g_bStatusOsd;				// the status overlay (hotkey L+R+Start+Up, or osd "@status")
void StatusOsdTick( void );				// refreshes the overlay once a second while it is on
extern int g_nAwakeMode;				// VDB_CMD_AWAKE: 0 none, 1 no standby, 2 no standby and no screen off
int vdb_usb_start( void );
int vdb_usb_stop( void );

//-----------------------------------------------------------------------------
// commands.c: the host protocol
//-----------------------------------------------------------------------------
void HandleCommand( unsigned char *pCmd, int nLen );
// Safety. File writes outside the user storage devices (ux0:, uma0:,
// imc0:, xmc0:) are refused (VDB_ERR_LOCKED) unless unlocked on the Vita
// itself, L+R+Start+Down, for VDB_UNLOCK_SECONDS; process commands only
// accept user processes (never the kernel's)
#define VDB_ERR_LOCKED		-7
#define VDB_ERR_NOT_USER	-8
#define VDB_UNLOCK_SECONDS	120
void SystemWritesUnlock( void );			// from the hotkey
void SystemWritesTick( void );				// thread tick: relocks at the deadline
unsigned SystemWritesSecondsLeft( void );	// 0 when locked
int UserPid( SceUID pid );					// a user process, not the kernel
void PutAbort( void );

//-----------------------------------------------------------------------------
// input.c: pad and touch injection
//-----------------------------------------------------------------------------
void InstallInputHooks( void );
void RemoveInputHooks( void );
void InputSet( const VdbInput_t *pInput );
void InputTick( void );				// from the thread loop every 200 ms
void InputRelease( void );
int TouchAvailable( void );
void TouchSet( const VdbTouch_t *pTouch, unsigned nPayload );

//-----------------------------------------------------------------------------
// profile.c: a sampling profiler (thread program counters) and process info
//-----------------------------------------------------------------------------
SceUID ScreenOwnerPid( void );			// the process whose framebuffer is on screen
void CmdProcInfo( SceUID pid );
void CmdProfile( unsigned nAction, const unsigned char *pPayload, unsigned nPayload );
void ProfileStop( void );

//-----------------------------------------------------------------------------
// osd.c: on-screen display
//-----------------------------------------------------------------------------
void InstallDisplayHook( void );
void RemoveDisplayHook( void );
int OsdAvailable( void );
void OsdSet( const char *pszText );

#endif // VDB_H
