// vdb_shell: user-side companion of the vdb kernel plugin, loaded into
// SceShell (*main in config.txt). It starts and closes apps on the host's
// behalf the way the shell itself does, because launching from the kernel
// plugin (ksceAppMgrLaunchAppByPath) brought the system down.
//
// Every 300 ms it asks the kernel plugin for a pending request:
//   "psgm:play?titleid=XXXXXXXXX"  -> sceAppMgrLaunchAppByUri
//   "destroy"                      -> sceAppMgrDestroyOtherApp
// and reports the result into the log ring.
#include <psp2/kernel/modulemgr.h>
#include <psp2/kernel/threadmgr.h>
#include <psp2/kernel/clib.h>
#include <psp2/appmgr.h>
#include "../user/vdb_user.h"

static int ShellThread( SceSize args, void *argp )
{
	char req[128];
	for ( ;; )
	{
		sceKernelDelayThread( 300 * 1000 );
		int n = vdb_shell_poll( req, sizeof( req ) );
		if ( n <= 0 )
			continue;
		req[sizeof( req ) - 1] = 0;
		int r;
		if ( sceClibStrcmp( req, "destroy" ) == 0 )
			r = sceAppMgrDestroyOtherApp();
		else
			r = sceAppMgrLaunchAppByUri( 0x20000, req );
		vdb_shell_result( r );
	}
	return 0;
}

void _start() __attribute__(( weak, alias( "module_start" ) ));
int module_start( SceSize argc, const void *args )
{
	SceUID th = sceKernelCreateThread( "vdb_shell", ShellThread, 0x10000100, 0x1000, 0, 0, NULL );
	if ( th < 0 )
		return SCE_KERNEL_START_FAILED;
	sceKernelStartThread( th, 0, NULL );
	return SCE_KERNEL_START_SUCCESS;
}

int module_stop( SceSize argc, const void *args )
{
	return SCE_KERNEL_STOP_SUCCESS;
}
