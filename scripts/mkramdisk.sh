#!/bin/bash
# mkramdisk.sh - Create a USTAR tar archive for the MontaukOS ramdisk
# Usage: ./scripts/mkramdisk.sh [input_dir] [output_path]

set -e

INPUT_DIR="${1:-programs/bin}"
OUTPUT_PATH="${2:-ramdisk.tar}"

if [ ! -d "$INPUT_DIR" ]; then
    echo "mkramdisk: input directory '$INPUT_DIR' does not exist, creating empty ramdisk"
    mkdir -p "$INPUT_DIR"
    # Create a placeholder file so the tar isn't completely empty
    echo "MontaukOS ramdisk" > "$INPUT_DIR/readme.txt"
fi

# Create USTAR tar archive
tar --format=ustar -cf "$OUTPUT_PATH" -C "$INPUT_DIR" .

echo "mkramdisk: created $OUTPUT_PATH from $INPUT_DIR ($(wc -c < "$OUTPUT_PATH") bytes)"
