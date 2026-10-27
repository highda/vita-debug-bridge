.arch armv7a

.section .vitalink.fstubs.VdbForUser,"ax",%progbits

	.align 4
	.global vdb_usb_start
	.type vdb_usb_start, %function
vdb_usb_start:
.if GEN_WEAK_EXPORTS
	.word 0x00000008
.else
	.word 0x00000000
.endif //GEN_WEAK_EXPORTS
	.word 0x754B48CF
	.word 0xE4D803D5
	.align 4

