import pathlib
import sys

print('.section .rodata')
for item in sys.argv[1:]:
    name, value = item.split('=', 1)
    path = pathlib.Path(value)
    print(f'.balign 16\n.globl {name}\n{name}:')
    print(f'.incbin "{value}"')
    if path.suffix == '.svg':
        print('.byte 0')
print('.section .note.GNU-stack,"",@progbits')
