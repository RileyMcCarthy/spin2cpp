PUB main
  coginit(0, @entry, 0)
DAT
	org	0
entry

_fibo
	wrlong	_fibo_x, sp
	add	sp, #4
	wrlong	fibo_tmp004_, sp
	add	sp, #4
	wrlong	_fibo_ret, sp
	add	sp, #4
	mov	_fibo_x, arg1
	cmps	_fibo_x, #2 wc,wz
 if_b	mov	result1, _fibo_x
 if_b	jmp	#L__0001
	mov	arg1, _fibo_x
	sub	arg1, #1
	call	#_fibo
	mov	fibo_tmp004_, result1
	sub	_fibo_x, #2
	mov	arg1, _fibo_x
	call	#_fibo
	add	fibo_tmp004_, result1
	mov	result1, fibo_tmp004_
L__0001
	sub	sp, #4
	rdlong	_fibo_ret, sp
	sub	sp, #4
	rdlong	fibo_tmp004_, sp
	sub	sp, #4
	rdlong	_fibo_x, sp
_fibo_ret
	ret

_fibo_x
	long	0
arg1
	long	0
arg2
	long	0
arg3
	long	0
arg4
	long	0
fibo_tmp004_
	long	0
result1
	long	0
sp
	long	@@@stackspace
	fit	496
stackspace
	res	1
