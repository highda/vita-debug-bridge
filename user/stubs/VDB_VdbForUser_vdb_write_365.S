.arch armv7a

.section .vitalink.fstubs.VdbForUser,"ax",%progbits

	.align 4
	.global vdb_write
	.type vdb_write, %function
vdb_write:
.if GEN_WEAK_EXPORTS
	.word 0x00000008
.else
	.word 0x00000000
.endif //GEN_WEAK_EXPORTS
	.word 0x754B48CF
	.word 0x921BEDED
	.align 4

