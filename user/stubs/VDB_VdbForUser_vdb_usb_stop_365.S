.arch armv7a

.section .vitalink.fstubs.VdbForUser,"ax",%progbits

	.align 4
	.global vdb_usb_stop
	.type vdb_usb_stop, %function
vdb_usb_stop:
.if GEN_WEAK_EXPORTS
	.word 0x00000008
.else
	.word 0x00000000
.endif //GEN_WEAK_EXPORTS
	.word 0x754B48CF
	.word 0xB62CD7BF
	.align 4

