# hazard_chain.asm -- deliberately terrible instruction scheduling.
#
# Every load is used by the very next instruction (a load-use hazard that costs
# one stall even with full forwarding), and the arithmetic forms a serial
# dependency chain so that every operand has to come off a bypass path. Compare
# a run with and without --no-forwarding to see what the bypass network buys.
# Expected output: 288.

.data
v: .word 1, 2, 3, 4, 5, 6, 7, 8

.text
main:
    la   r1, v
    li   r2, 0          # i
    li   r3, 8          # n
    li   r4, 0          # total
loop:
    bge  r2, r3, done
    slli r5, r2, 2
    add  r6, r1, r5
    lw   r7, 0(r6)      # load ...
    add  r8, r7, r7     # ... used immediately: load-use stall
    add  r9, r8, r8
    add  r10, r9, r9
    add  r4, r4, r10
    addi r2, r2, 1
    j    loop
done:
    print r4            # 8 * (1+2+...+8) = 288
    halt
