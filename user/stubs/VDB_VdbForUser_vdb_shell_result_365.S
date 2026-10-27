.arch armv7a

.section .vitalink.fstubs.VdbForUser,"ax",%progbits

	.align 4
	.global vdb_shell_result
	.type vdb_shell_result, %function
vdb_shell_result:
.if GEN_WEAK_EXPORTS
	.word 0x00000008
.else
	.word 0x00000000
.endif //GEN_WEAK_EXPORTS
	.word 0x754B48CF
	.word 0x7D90E5B4
	.align 4

