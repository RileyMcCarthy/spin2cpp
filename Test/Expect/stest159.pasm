pub main
  coginit(0, @entry, 0)
dat
	org	0
entry

_demo
	wrlong	local01, sp
	add	sp, #4
	wrlong	fp, sp
	add	sp, #4
	mov	fp, sp
	add	sp, #8
	mov	local01, #0
	wrlong	local01, fp
	mov	arg02, #1
	wrbyte	arg02, fp
	rdlong	result1, fp
	mov	sp, fp
	sub	sp, #4
	rdlong	fp, sp
	sub	sp, #4
	rdlong	local01, sp
_demo_ret
	ret

_setone
	wrbyte	arg02, arg01
_setone_ret
	ret

fp
	long	0
result1
	long	0
sp
	long	@@@stackspace
COG_BSS_START
	fit	496
stackspace
	long	0[1]
	org	COG_BSS_START
arg01
	res	1
arg02
	res	1
local01
	res	1
	fit	496
