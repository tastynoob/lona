.section .text
.globl _start
.type _start, @function
_start:
    call __lona_entry__
    mov %eax, %edi
    mov $60, %eax
    syscall

.size _start, .-_start
