# yielder.asm -- an interactive-style job: a little work, then yield.
#
# Under MLFQ this job keeps its high-priority level because it never burns a
# whole quantum, while spinner.asm sinks to the bottom queue. Expected output: 40.

.text
main:
    li   r1, 0
    li   r2, 40
loop:
    bge  r1, r2, done
    addi r1, r1, 1
    addi r3, r1, 1
    addi r4, r3, 1
    yield
    j    loop
done:
    print r1
    halt
