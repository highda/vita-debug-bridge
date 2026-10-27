.arch armv7a

.section .vitalink.fstubs.VdbForUser,"ax",%progbits

	.align 4
	.global vdb_shell_poll
	.type vdb_shell_poll, %function
vdb_shell_poll:
.if GEN_WEAK_EXPORTS
	.word 0x00000008
.else
	.word 0x00000000
.endif //GEN_WEAK_EXPORTS
	.word 0x754B48CF
	.word 0x3A1F6C02
	.align 4

