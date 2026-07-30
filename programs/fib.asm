# fib.asm -- writes the first 20 Fibonacci numbers to memory and prints F(19).
# Branch-heavy with a very predictable loop, so the 2-bit predictor should reach
# a mispredict rate close to 1/trip-count. Expected output: 4181.

.data
result: .space 80

.text
main:
    la   r1, result     # write pointer
    li   r2, 0          # a
    li   r3, 1          # b
    li   r4, 0          # i
    li   r5, 20         # n
loop:
    bge  r4, r5, done
    sw   r2, 0(r1)
    add  r6, r2, r3
    mv   r2, r3
    mv   r3, r6
    addi r1, r1, 4
    addi r4, r4, 1
    j    loop
done:
    la   r1, result
    lw   r7, 76(r1)     # result[19]
    print r7            # 4181
    halt
