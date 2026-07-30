# matmul.asm -- 8x8 integer matrix multiply, C = A * B.
#
# A[i][j] = i + j and B is the identity, so C should come out equal to A. The
# inner loop walks B down a column, which is the classic cache-hostile access
# pattern. Expected output: C[3][4] = 7.

.data
A: .space 256
B: .space 256
C: .space 256

.text
main:
    li   r1, 8              # n
    li   r2, 0              # i
fill_i:
    bge  r2, r1, mm_init
    li   r3, 0              # j
fill_j:
    bge  r3, r1, fill_i_next
    mul  r4, r2, r1
    add  r4, r4, r3
    slli r4, r4, 2          # byte offset of [i][j]
    la   r5, A
    add  r5, r5, r4
    add  r6, r2, r3
    sw   r6, 0(r5)          # A[i][j] = i + j
    la   r5, B
    add  r5, r5, r4
    li   r7, 0
    bne  r2, r3, store_b
    li   r7, 1              # B[i][j] = (i == j)
store_b:
    sw   r7, 0(r5)
    addi r3, r3, 1
    j    fill_j
fill_i_next:
    addi r2, r2, 1
    j    fill_i

mm_init:
    li   r2, 0              # i
mm_i:
    bge  r2, r1, mm_done
    li   r3, 0              # j
mm_j:
    bge  r3, r1, mm_i_next
    li   r8, 0              # accumulator
    li   r9, 0              # k
mm_k:
    bge  r9, r1, mm_store
    mul  r4, r2, r1
    add  r4, r4, r9
    slli r4, r4, 2
    la   r5, A
    add  r5, r5, r4
    lw   r10, 0(r5)         # A[i][k]
    mul  r4, r9, r1
    add  r4, r4, r3
    slli r4, r4, 2
    la   r5, B
    add  r5, r5, r4
    lw   r11, 0(r5)         # B[k][j]
    mul  r12, r10, r11
    add  r8, r8, r12
    addi r9, r9, 1
    j    mm_k
mm_store:
    mul  r4, r2, r1
    add  r4, r4, r3
    slli r4, r4, 2
    la   r5, C
    add  r5, r5, r4
    sw   r8, 0(r5)
    addi r3, r3, 1
    j    mm_j
mm_i_next:
    addi r2, r2, 1
    j    mm_i

mm_done:
    la   r5, C
    li   r4, 3
    mul  r4, r4, r1
    addi r4, r4, 4          # [3][4]
    slli r4, r4, 2
    add  r5, r5, r4
    lw   r13, 0(r5)
    print r13               # 7
    halt
