.section .text
.globl _start
.type _start, @function
_start:
    call __lona_main__
    mov %eax, %edi
    mov $60, %eax
    syscall

.size _start, .-_start
