PUB main
  coginit(0, @entry, 0)
DAT
	org	0
entry

_val511
	mov	result1, #511
_val511_ret
	ret

_val512
	mov	result1, imm_512_
_val512_ret
	ret

arg1
	long	0
arg2
	long	0
arg3
	long	0
arg4
	long	0
imm_512_
	long	512
result1
	long	0
COG_BSS_START
	fit	496
	org	COG_BSS_START
	fit	496
