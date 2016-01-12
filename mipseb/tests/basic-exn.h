
utlbexn:
	lui k0, 0xaaaa
	j die
	nop
	nop; nop; nop; nop; nop

	nop; nop; nop; nop; nop; nop; nop; nop
	nop; nop; nop; nop; nop; nop; nop; nop
	nop; nop; nop; nop; nop; nop; nop; nop

genexn:
	lui k0, 0xbbbb

die:
	DUMP(0xdeadbeef)
	POWEROFF
