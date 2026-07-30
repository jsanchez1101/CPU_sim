# pointer_chase.asm -- four passes of a 64-byte stride walk over 8 KiB.
#
# The stride is twice the default 32-byte line size, so every store touches a
# fresh line and the following load hits it: about a 50% D-cache miss rate, and
# a run dominated by memory stalls. Because the buffer is re-walked four times,
# it also thrashes under a small physical memory -- compare:
#
#   ./cpusim programs/pointer_chase.asm
#   ./cpusim --frames 2 programs/pointer_chase.asm     # eviction storm
#   ./cpusim --line 128 programs/pointer_chase.asm     # stride now fits a line
#
# Expected output: 3584.

.data
buf: .space 8192

.text
main:
    la   r1, buf
    li   r5, 0          # accumulator
    li   r8, 7
    li   r9, 0          # pass
    li   r10, 4         # passes
pass:
    bge  r9, r10, done
    li   r2, 0          # i
    li   r3, 128        # elements
loop:
    bge  r2, r3, next_pass
    slli r4, r2, 6      # stride of 64 bytes
    add  r6, r1, r4
    sw   r8, 0(r6)      # dirty the line so eviction costs a writeback
    lw   r7, 0(r6)
    add  r5, r5, r7
    addi r2, r2, 1
    j    loop
next_pass:
    addi r9, r9, 1
    j    pass
done:
    print r5            # 4 * 128 * 7 = 3584
    halt
