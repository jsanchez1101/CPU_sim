# vector_add.asm -- c[i] = a[i] + b[i] over 16 elements, then sum c.
# Sequential access, so it shows off spatial locality: one cache miss per line
# rather than one per element. Expected output: 272.

.data
a:  .word 1, 2, 3, 4, 5, 6, 7, 8, 9, 10, 11, 12, 13, 14, 15, 16
b:  .word 16, 15, 14, 13, 12, 11, 10, 9, 8, 7, 6, 5, 4, 3, 2, 1
c:  .space 64

.text
main:
    li   r1, 0          # i
    li   r2, 16         # n
    la   r3, a
    la   r4, b
    la   r5, c
    li   r6, 0          # running sum
loop:
    bge  r1, r2, done
    lw   r7, 0(r3)
    lw   r8, 0(r4)
    add  r9, r7, r8
    sw   r9, 0(r5)
    add  r6, r6, r9
    addi r3, r3, 4
    addi r4, r4, 4
    addi r5, r5, 4
    addi r1, r1, 1
    j    loop
done:
    print r6            # 16 * 17 = 272
    halt
