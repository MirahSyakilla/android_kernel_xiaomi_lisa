#!/bin/bash

DEFCONFIG="arch/arm64/configs/lisa_defconfig"
OUT_CONFIG="./out/.config"
CUSTOM_CONFIG_DIR="$HOME/custom_kernel_configs"
RESULT_FILE="$CUSTOM_CONFIG_DIR/changes_$(date +%Y%m%d_%H%M%S).config"

mkdir -p "$CUSTOM_CONFIG_DIR"

if [[ ! -f "$DEFCONFIG" ]]; then
    echo "Error: $DEFCONFIG not found!"
    exit 1
fi

if [[ ! -f "$OUT_CONFIG" ]]; then
    echo "Error: $OUT_CONFIG not found! Did you run make O=out nconfig?"
    exit 1
fi

echo "Comparing $DEFCONFIG with $OUT_CONFIG..."

comm -13 <(grep -v '^#' "$DEFCONFIG" | grep -v '^$' | sort) \
         <(grep -v '^#' "$OUT_CONFIG" | grep -v '^$' | sort) > "$RESULT_FILE"

echo "--------------------------------------"
echo "Comparison complete!"
echo "New/Changed lines saved to: $RESULT_FILE"
echo "--------------------------------------"
cat "$RESULT_FILE"
