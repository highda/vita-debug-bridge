// vdb: user-side interface of the Vita debug bridge kernel plugin (VDB,
// library VdbForUser). Link libVdbForUser_stub_weak.a and check that the
// module is loaded (_vshKernelSearchModuleByName( "VDB", ... ) >= 0) before
// calling; the plugin is optional.
#ifndef VDB_USER_H
#define VDB_USER_H

#ifdef __cplusplus
extern "C" {
#endif

// Appends up to 4096 bytes to the bridge's log ring (streamed to the host
// over USB when attached, buffered otherwise). Returns bytes written or < 0
int vdb_write( const char *pData, unsigned nLen );
// Take the USB port as the bridge device / give it back to the system
int vdb_usb_start( void );
int vdb_usb_stop( void );

#ifdef __cplusplus
}
#endif

#endif // VDB_USER_H
