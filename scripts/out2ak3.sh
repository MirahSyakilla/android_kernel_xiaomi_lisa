#!/bin/bash

ZIP_NAME="update.zip"
AK3_DIR="$HOME/AnyKernel3"
OUT_DIR="./out/arch/arm64/boot"

BLUE='\033[0;34m'
GREEN='\033[0;32m'
RED='\033[0;31m'
NC='\033[0m' # No Color

echo -e "${BLUE}[1/5] Cleaning up old AnyKernel3 directory...${NC}"
rm -rf "$AK3_DIR"

echo -e "${BLUE}[2/5] Cloning AnyKernel3 repository...${NC}"
git clone https://github.com/rawnullbyte/AnyKernel3/ "$AK3_DIR"

echo -e "${BLUE}[3/5] Copying build artifacts (Image & dtbo)...${NC}"
if [[ -f "$OUT_DIR/Image" && -f "$OUT_DIR/dtbo.img" ]]; then
    cp "$OUT_DIR/dtbo.img" "$AK3_DIR/"
    cp "$OUT_DIR/Image" "$AK3_DIR/"
    echo -e "${GREEN} -> Files copied successfully.${NC}"
else
    echo -e "${RED} -> Error: Image or dtbo.img not found in $OUT_DIR!${NC}"
    exit 1
fi

cd "$AK3_DIR" || exit

echo -e "${BLUE}[4/5] Creating flashable zip: $ZIP_NAME...${NC}"
zip -r9 "$ZIP_NAME" . -x "*.git*" "README.md" "LICENSE" "Makefile" "zipsigner*" "modules/placeholder"

if [ -f "$ZIP_NAME" ]; then
    echo -e "${GREEN} -> Zip created successfully.${NC}"
else
    echo -e "${RED} -> Failed to create zip!${NC}"
    exit 1
fi

echo -e "${BLUE}[5/5] Uploading to temp.sh...${NC}"
UPLOAD_URL=$(curl -s -F "file=@$ZIP_NAME" https://temp.sh/upload)

echo -e "----------------------------------------------------"
echo -e "${GREEN}DONE!${NC}"
echo -e "${BLUE}Download Link:${NC} $UPLOAD_URL"
echo -e "----------------------------------------------------"
