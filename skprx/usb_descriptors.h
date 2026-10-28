// vdb: USB descriptors. A vendor-specific device (class 0xFF) with two
// interfaces and three bulk endpoints:
//   interface 0: EP1 IN   the log ring, streamed to the host (vdb.py logs)
//   interface 1: EP2 OUT  framed commands from the host
//                EP3 IN   command replies and file data
// Two interfaces so that a log tail and a file transfer can run from two
// host processes at once (libusb claims are per interface). Sony's vendor
// ID is filled in by ksceUdcdActivate; the product ID is VDB_USB_PID.
#ifndef VDB_USB_DESCRIPTORS_H
#define VDB_USB_DESCRIPTORS_H

#include <psp2kern/udcd.h>

#define VDB_EP_IN		1
#define VDB_EP_OUT		2
#define VDB_EP_DATA		3

// Endpoint blocks: control, bulk IN, bulk OUT, bulk IN (driverEndpointNumber sequential)
static SceUdcdEndpoint endpoints[4] = {
	{ USB_ENDPOINT_OUT, 0, 0, 0 },
	{ USB_ENDPOINT_IN,  1, 0, 0 },
	{ USB_ENDPOINT_OUT, 2, 0, 0 },
	{ USB_ENDPOINT_IN,  3, 0, 0 },
};

static SceUdcdInterface interfaces[1] = {
	{ -1, 0, 2 },
};

static SceUdcdStringDescriptor string_descriptors[2] = {
	{
		8,
		USB_DT_STRING,
		{ 'V', 'D', 'B' }
	},
	{
		0,
		USB_DT_STRING
	}
};

#define VDB_CONFIG_TOTAL_LENGTH	( USB_DT_CONFIG_SIZE + 2 * USB_DT_INTERFACE_SIZE + 3 * USB_DT_ENDPOINT_SIZE )

// High speed
static SceUdcdDeviceDescriptor devdesc_hi = {
	USB_DT_DEVICE_SIZE,
	USB_DT_DEVICE,
	0x200,						// bcdUSB
	USB_CLASS_VENDOR_SPEC,		// bDeviceClass
	0,							// bDeviceSubClass
	0,							// bDeviceProtocol
	64,							// bMaxPacketSize0
	0,							// idVendor (filled in)
	0,							// idProduct (filled in)
	0x100,						// bcdDevice
	0,							// iManufacturer
	0,							// iProduct
	0,							// iSerialNumber
	1							// bNumConfigurations
};

static SceUdcdEndpointDescriptor endpdesc_hi[4] = {
	{
		USB_DT_ENDPOINT_SIZE,
		USB_DT_ENDPOINT,
		0x80 | VDB_EP_IN,		// bEndpointAddress
		0x02,					// bmAttributes: bulk
		512,					// wMaxPacketSize
		0						// bInterval
	},
	{
		USB_DT_ENDPOINT_SIZE,
		USB_DT_ENDPOINT,
		VDB_EP_OUT,
		0x02,
		512,
		0
	},
	{
		USB_DT_ENDPOINT_SIZE,
		USB_DT_ENDPOINT,
		0x80 | VDB_EP_DATA,
		0x02,
		512,
		0
	},
	{
		0,
	}
};

static SceUdcdInterfaceDescriptor interdesc_hi[3] = {
	{
		USB_DT_INTERFACE_SIZE,
		USB_DT_INTERFACE,
		0,							// bInterfaceNumber: logs
		0,							// bAlternateSetting
		1,							// bNumEndpoints
		USB_CLASS_VENDOR_SPEC,		// bInterfaceClass
		0x00,						// bInterfaceSubClass
		0x00,						// bInterfaceProtocol
		1,							// iInterface
		&endpdesc_hi[0],			// endpoints
		NULL,						// extra
		0							// extraLength
	},
	{
		USB_DT_INTERFACE_SIZE,
		USB_DT_INTERFACE,
		1,							// bInterfaceNumber: commands and data
		0,
		2,
		USB_CLASS_VENDOR_SPEC,
		0x00,
		0x00,
		1,
		&endpdesc_hi[1],
		NULL,
		0
	},
	{
		0
	}
};

static SceUdcdInterfaceSettings settings_hi[2] = {
	{ &interdesc_hi[0], 0, 1 },
	{ &interdesc_hi[1], 0, 1 },
};

static SceUdcdConfigDescriptor confdesc_hi = {
	USB_DT_CONFIG_SIZE,
	USB_DT_CONFIG,
	VDB_CONFIG_TOTAL_LENGTH,	// wTotalLength
	2,							// bNumInterfaces
	1,							// bConfigurationValue
	0,							// iConfiguration
	0x80,						// bmAttributes: bus powered (self powered with 0 mA, as before, left the Vita uncharged all night)
	0xFA,						// bMaxPower: 500 mA
	&settings_hi[0]
};

static SceUdcdConfiguration config_hi = {
	&confdesc_hi,
	&settings_hi[0],
	&interdesc_hi[0],
	&endpdesc_hi[0]
};

// Full speed (the same shape, 64-byte packets)
static SceUdcdDeviceDescriptor devdesc_full = {
	USB_DT_DEVICE_SIZE,
	USB_DT_DEVICE,
	0x200,						// bcdUSB (0x110 freezes the Vita, as in vitastick)
	USB_CLASS_VENDOR_SPEC,
	0,
	0,
	8,
	0,
	0,
	0x200,
	0,
	0,
	0,
	1
};

static SceUdcdEndpointDescriptor endpdesc_full[4] = {
	{
		USB_DT_ENDPOINT_SIZE,
		USB_DT_ENDPOINT,
		0x80 | VDB_EP_IN,
		0x02,
		64,
		0
	},
	{
		USB_DT_ENDPOINT_SIZE,
		USB_DT_ENDPOINT,
		VDB_EP_OUT,
		0x02,
		64,
		0
	},
	{
		USB_DT_ENDPOINT_SIZE,
		USB_DT_ENDPOINT,
		0x80 | VDB_EP_DATA,
		0x02,
		64,
		0
	},
	{
		0
	}
};

static SceUdcdInterfaceDescriptor interdesc_full[3] = {
	{
		USB_DT_INTERFACE_SIZE,
		USB_DT_INTERFACE,
		0,
		0,
		1,
		USB_CLASS_VENDOR_SPEC,
		0x00,
		0x00,
		1,
		&endpdesc_full[0],
		NULL,
		0
	},
	{
		USB_DT_INTERFACE_SIZE,
		USB_DT_INTERFACE,
		1,
		0,
		2,
		USB_CLASS_VENDOR_SPEC,
		0x00,
		0x00,
		1,
		&endpdesc_full[1],
		NULL,
		0
	},
	{
		0
	}
};

static SceUdcdInterfaceSettings settings_full[2] = {
	{ &interdesc_full[0], 0, 1 },
	{ &interdesc_full[1], 0, 1 },
};

static SceUdcdConfigDescriptor confdesc_full = {
	USB_DT_CONFIG_SIZE,
	USB_DT_CONFIG,
	VDB_CONFIG_TOTAL_LENGTH,
	2,
	1,
	0,
	0xC0,
	0,
	&settings_full[0]
};

static SceUdcdConfiguration config_full = {
	&confdesc_full,
	&settings_full[0],
	&interdesc_full[0],
	&endpdesc_full[0]
};

#endif // VDB_USB_DESCRIPTORS_H
