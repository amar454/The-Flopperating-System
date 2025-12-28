global context_switch
section .text
context_switch:
	mov eax, [esp + 4]
	mov edx, [esp + 8]

	; save gprs
	push ebp
	push ebx
	push esi
	push edi

	mov eax, esp
	mov esp, edx

	; restore gprs
	pop edi
	pop esi
	pop ebx
	pop ebp

	; fuck the fpu :)

	ret
