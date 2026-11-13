import sys, os

def main():
    if len(sys.argv) != 4:
        print("Usage: pad_image.py <boot.bin> <kernel.bin> <out.img>")
        sys.exit(1)

    boot_path, kernel_path, out_path = sys.argv[1], sys.argv[2], sys.argv[3]

    with open(boot_path, 'rb') as f:
        boot = f.read()
    with open(kernel_path, 'rb') as f:
        kernel = f.read()

    if len(boot) != 512:
        sys.exit(f"ERROR: boot.bin must be 512 bytes, got {len(boot)}")
    if len(kernel) > 64 * 1024:
        sys.exit(f"ERROR: kernel.bin too large ({len(kernel)} bytes, max 64KB)")

    FLOPPY_SIZE = 1440 * 1024
    img = bytearray(FLOPPY_SIZE)
    img[0:512] = boot
    img[512:512 + len(kernel)] = kernel

    with open(out_path, 'wb') as f:
        f.write(img)

    print(f"[OK] {out_path}: {FLOPPY_SIZE} bytes (boot=512, kernel={len(kernel)})")

if __name__ == '__main__':
    main()