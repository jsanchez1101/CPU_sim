# spinner.asm -- a CPU-bound job that never gives up the processor voluntarily.
# Pair it with yielder.asm to watch a scheduler preempt it. Expected output: 3000.

.text
main:
    li   r1, 0
    li   r2, 3000
loop:
    bge  r1, r2, done
    addi r1, r1, 1
    j    loop
done:
    print r1
    halt
