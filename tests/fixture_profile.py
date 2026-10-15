"""Explicit Linux ELF bootstrap profiles for the native C workload fixtures."""
import struct


def startup_boundary(binary):
    """Declare the fixture's Linux ELF prelude; tracer never guesses a runtime."""
    with binary.open("rb") as image:
        header = image.read(64)
        if header[:6] != b"\x7fELF\x02\x01":
            raise ValueError("fixture requires Linux ELF64 little-endian executable")
        offset = struct.unpack_from("<Q", header, 32)[0]
        size, count = struct.unpack_from("<HH", header, 54)
        for index in range(count):
            image.seek(offset + index * size)
            if struct.unpack("<I", image.read(4))[0] == 3:  # PT_INTERP
                return 11  # SYS_munmap: supported dynamic-loader fixture profile
    return 10  # SYS_mprotect: supported static-libc fixture profile
