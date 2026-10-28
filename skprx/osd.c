// vdb: on-screen display, text drawn into every framebuffer the app
// presents, through a hook on the display driver's SetFrameBuf (PSVshell's
// method). The hook runs in the presenting process, so the rows go in with
// ksceKernelMemcpyKernelToUser. Font: font8x8 by Daniel Hepper (public domain)
#include "vdb.h"
#include "font8x8.h"

// 8 px glyphs (110 columns of the 960), a 2 px padded box blended over
// the frame at about 60 percent black, 8 lines; the box is read back from
// the framebuffer row by row, blended and written again
#define OSD_LINES		8
#define OSD_COLS		110
#define OSD_X			12
#define OSD_Y			12
#define OSD_PAD			3
static char s_OsdText[OSD_LINES][OSD_COLS + 1];
static volatile int s_nOsdLines;
static unsigned s_OsdRow[OSD_COLS * 8 + 2 * OSD_PAD] __attribute__(( aligned( 64 ) ));
static tai_hook_ref_t s_DisplayRef;
static SceUID s_DisplayHook = -1;

// text -> lines; an empty text switches the display off
void OsdSet( const char *pszText )
{
	int nLines = 0, nCol = 0;
	s_nOsdLines = 0;
	memset( s_OsdText, 0, sizeof( s_OsdText ) );
	for ( const char *p = pszText; *p && nLines < OSD_LINES; p++ )
	{
		if ( *p == '\n' )
		{
			nLines++;
			nCol = 0;
			continue;
		}
		if ( nCol < OSD_COLS )
			s_OsdText[nLines][nCol++] = ( *p >= 32 && *p < 127 ) ? *p : '?';
	}
	if ( nCol )
		nLines++;
	s_nOsdLines = nLines;
}

static void OsdDraw( const SceDisplayFrameBuf *pFb )
{
	if ( !pFb || !pFb->base || pFb->pixelformat != 0 || pFb->width < 64 || pFb->pitch < pFb->width )
		return;
	// the box: the longest line decides the width
	int nMax = 0;
	for ( int l = 0; l < s_nOsdLines; l++ )
	{
		int n = strlen( s_OsdText[l] );
		if ( n > nMax )
			nMax = n;
	}
	if ( !nMax )
		return;
	unsigned nWidth = nMax * 8 + 2 * OSD_PAD, nX = OSD_X - OSD_PAD;
	if ( nX + nWidth > pFb->width )
		nWidth = pFb->width - nX;
	unsigned nRows = s_nOsdLines * 8 + 2 * OSD_PAD;
	for ( unsigned row = 0; row < nRows; row++ )
	{
		unsigned y = OSD_Y - OSD_PAD + row;
		if ( y >= pFb->height )
			return;
		unsigned char *pLine = (unsigned char *)pFb->base + ( y * pFb->pitch + nX ) * 4;
		if ( ksceKernelMemcpyUserToKernel( s_OsdRow, pLine, nWidth * 4 ) < 0 )
			return;
		// darken: 40 percent of the frame remains
		for ( unsigned x = 0; x < nWidth; x++ )
		{
			unsigned px = s_OsdRow[x];
			s_OsdRow[x] = ( px & 0xFF000000 ) | ( ( ( px & 0x00FF00FF ) * 102 >> 8 ) & 0x00FF00FF ) | ( ( ( px & 0x0000FF00 ) * 102 >> 8 ) & 0x0000FF00 );
		}
		int l = ( (int)row - OSD_PAD ) >> 3, r = ( (int)row - OSD_PAD ) & 7;
		if ( row >= OSD_PAD && l < s_nOsdLines )
		{
			const char *pszLine = s_OsdText[l];
			for ( int c = 0; pszLine[c]; c++ )
			{
				unsigned char bits = s_Font8x8[pszLine[c] - 32][r];
				for ( int b = 0; b < 8; b++ )
					if ( ( bits >> b ) & 1 )
						s_OsdRow[OSD_PAD + c * 8 + b] = 0xFFFFFFFF;
			}
		}
		ksceKernelMemcpyKernelToUser( pLine, s_OsdRow, nWidth * 4 );
	}
}

// Every presented frame of the main head, whatever its index. (0.4.0 asked
// ksceAppMgrIsExclusiveProcessRunning with a NULL title id here to spare
// the shell's overlays, as PSVshell does with an older header; the first
// index-1 frame, the game's, dereferenced that NULL and powered the Vita
// off.) The copy routine validates the destination itself, so an unexpected
// framebuffer costs nothing more than a skipped row
static int Hook_ksceDisplaySetFrameBufInternal( int head, int index, const SceDisplayFrameBuf *pParam, int sync )
{
	if ( head && pParam && s_nOsdLines )
		OsdDraw( pParam );
	return TAI_CONTINUE( int, s_DisplayRef, head, index, pParam, sync );
}

void InstallDisplayHook( void )
{
	s_DisplayHook = taiHookFunctionExportForKernel( KERNEL_PID, &s_DisplayRef, "SceDisplay", 0x9FED47AC, 0x16466675, Hook_ksceDisplaySetFrameBufInternal );
	FileNotef( "vdb: display hook: 0x%08x", (unsigned)s_DisplayHook );
}

void RemoveDisplayHook( void )
{
	if ( s_DisplayHook >= 0 )
		taiHookReleaseForKernel( s_DisplayHook, s_DisplayRef );
	s_DisplayHook = -1;
}

int OsdAvailable( void )
{
	return s_DisplayHook >= 0;
}

// Syscall: apps set the text themselves (lines split at \n, empty = off)
int vdb_osd( const char *pUser, unsigned nLen )
{
	char buf[OSD_LINES * ( OSD_COLS + 1 ) + 1];
	if ( s_DisplayHook < 0 )
		return -6;
	if ( nLen >= sizeof( buf ) )
		nLen = sizeof( buf ) - 1;
	if ( nLen && ksceKernelMemcpyFromUser( buf, pUser, nLen ) < 0 )
		return -1;
	buf[nLen] = 0;
	OsdSet( buf );
	return 0;
}
