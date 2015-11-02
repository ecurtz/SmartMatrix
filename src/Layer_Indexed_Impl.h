/*
 * SmartMatrix Library - Indexed Layer Class
 *
 * Copyright (c) 2015 Louis Beaudoin (Pixelmatix)
 *
 * Permission is hereby granted, free of charge, to any person obtaining a copy of
 * this software and associated documentation files (the "Software"), to deal in
 * the Software without restriction, including without limitation the rights to
 * use, copy, modify, merge, publish, distribute, sublicense, and/or sell copies of
 * the Software, and to permit persons to whom the Software is furnished to do so,
 * subject to the following conditions:
 *
 * The above copyright notice and this permission notice shall be included in all
 * copies or substantial portions of the Software.
 *
 * THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
 * IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY, FITNESS
 * FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL THE AUTHORS OR
 * COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER LIABILITY, WHETHER
 * IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM, OUT OF OR IN
 * CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN THE SOFTWARE.
 */

#include <string.h>

const unsigned char indexedDrawBuffer = 0;
const unsigned char indexedRefreshBuffer = 1;

static rgb24 color = rgb24(0,0,0);

#define INDEXED_BUFFER_ROW_SIZE     (this->localWidth * entryBits / 8)
#define INDEXED_BUFFER_SIZE         (INDEXED_BUFFER_ROW_SIZE * this->localHeight)

template <typename RGB, unsigned int optionFlags>
SMLayerIndexed<RGB, optionFlags>::SMLayerIndexed(uint8_t * bitmap, RGB* colors, uint16_t width, uint16_t height) {
    indexedBitmap = bitmap;
    palette = colors;
    paletteSize = PALETTE_SIZE_FROM_OPTIONS(optionFlags);
    entryBits = BIT_COUNT_FROM_OPTIONS(optionFlags);
    entryMask = 0xFF >> (8 - entryBits);
    entryPack = 8 / entryBits;
    matrixWidth = width;
    matrixHeight = height;
}

template <typename RGB, unsigned int optionFlags>
void SMLayerIndexed<RGB, optionFlags>::frameRefreshCallback(void) {
    handleBufferCopy();
}

// returns true and copies color to xyPixel if pixel is opaque, returns false if not
template<typename RGB, unsigned int optionFlags> template <typename RGB_OUT>
bool SMLayerIndexed<RGB, optionFlags>::getPixel(uint16_t hardwareX, uint16_t hardwareY, RGB_OUT &xyPixel) {
    uint16_t localScreenX, localScreenY;

    // convert hardware x/y to the pixel in the local screen
    switch( this->rotation ) {
      case rotation0 :
        localScreenX = hardwareX;
        localScreenY = hardwareY;
        break;
      case rotation180 :
        localScreenX = (this->matrixWidth - 1) - hardwareX;
        localScreenY = (this->matrixHeight - 1) - hardwareY;
        break;
      case  rotation90 :
        localScreenX = hardwareY;
        localScreenY = (this->matrixWidth - 1) - hardwareX;
        break;
      case  rotation270 :
        localScreenX = (this->matrixHeight - 1) - hardwareY;
        localScreenY = hardwareX;
        break;
      default:
        // TODO: Should throw an error
        return false;
    };

    // check for out of bounds coordinates
    if (localScreenX < 0 || localScreenY < 0 || localScreenX >= this->localWidth || localScreenY >= this->localHeight)
        return false;

    uint8_t entry;
    if (entryBits == 8) {
        entry = indexedBitmap[(indexedRefreshBuffer * INDEXED_BUFFER_SIZE) + (localScreenY * INDEXED_BUFFER_ROW_SIZE) + localScreenX];
    } else {
        uint8_t entryBlock = indexedBitmap[(indexedRefreshBuffer * INDEXED_BUFFER_SIZE) + (localScreenY * INDEXED_BUFFER_ROW_SIZE) + (localScreenX * entryBits / 8)];
        uint8_t entryIndex = localScreenX % entryPack;
        uint8_t entryShift = (entryPack - entryIndex - 1) * entryBits;
        entry = (entryBlock >> entryShift) & entryMask; 
    }
    
    if (entry || !zeroTransparent) {
        xyPixel = palette[entry];
        return true;
    }

    return false;
}

template <typename RGB, unsigned int optionFlags>
void SMLayerIndexed<RGB, optionFlags>::fillRefreshRow(uint16_t hardwareY, rgb48 refreshRow[]) {
    RGB currentPixel;
    int i;

    for (i = 0; i < this->matrixWidth; i++) {
        if (!getPixel(i, hardwareY, currentPixel) && zeroTransparent)
            continue;

        // load background pixel
        refreshRow[i] = currentPixel;
    }
}

template <typename RGB, unsigned int optionFlags>
void SMLayerIndexed<RGB, optionFlags>::fillRefreshRow(uint16_t hardwareY, rgb24 refreshRow[]) {
    RGB currentPixel;
    int i;

     for (i = 0; i < this->matrixWidth; i++) {
        if (!getPixel(i, hardwareY, currentPixel) && zeroTransparent)
            continue;

        // load background pixel without color correction
        refreshRow[i] = currentPixel;
    }
}

template<typename RGB, unsigned int optionFlags>
void SMLayerIndexed<RGB, optionFlags>::setIndexedColor(uint8_t index, const RGB & newColor) {
    if (index < paletteSize) {
        palette[index] = newColor;
        if (this->ccEnabled)
           colorCorrection(palette[index], palette[index]);
    }
}

template<typename RGB, unsigned int optionFlags>
void SMLayerIndexed<RGB, optionFlags>::enableColorCorrection(bool enabled) {
    this->ccEnabled = sizeof(RGB) <= 3 ? enabled : false;
}

template <typename RGB, unsigned int optionFlags>
void SMLayerIndexed<RGB, optionFlags>::fillScreen(uint8_t index) {
    uint8_t fillValue = index;
    for (uint8_t shift = entryBits; shift < 8; shift += entryBits) {
        fillValue <<= entryBits;
        fillValue |= index;
    }

    memset(&indexedBitmap[indexedRefreshBuffer*INDEXED_BUFFER_SIZE], fillValue, INDEXED_BUFFER_SIZE);
}

template <typename RGB, unsigned int optionFlags>
void SMLayerIndexed<RGB, optionFlags>::swapBuffers(bool copy) {
    while (copyPending);

    copyPending = true;

    while (copy && copyPending);
}

template <typename RGB, unsigned int optionFlags>
void SMLayerIndexed<RGB, optionFlags>::handleBufferCopy(void) {
    if (!copyPending)
        return;

    memcpy(&indexedBitmap[indexedDrawBuffer*INDEXED_BUFFER_SIZE], &indexedBitmap[indexedRefreshBuffer*INDEXED_BUFFER_SIZE], INDEXED_BUFFER_SIZE);
    copyPending = false;
}

template <typename RGB, unsigned int optionFlags>
void SMLayerIndexed<RGB, optionFlags>::drawPixel(int16_t x, int16_t y, uint8_t index) {
    uint8_t tempBitmask;
    int hwx, hwy;

    // check for bad index
    if (index > paletteSize)
        return;

    // map pixel into hardware buffer before writing
    if (this->rotation == rotation0) {
        hwx = x;
        hwy = y;
    } else if (this->rotation == rotation180) {
        hwx = (this->matrixWidth - 1) - x;
        hwy = (this->matrixHeight - 1) - y;
    } else if (this->rotation == rotation90) {
        hwx = (this->matrixWidth - 1) - y;
        hwy = x;
    } else { /* if (rotation == rotation270)*/
        hwx = y;
        hwy = (this->matrixHeight - 1) - x;
    }

    // check for out of bounds coordinates
    if (hwx < 0 || hwy < 0 || hwx >= this->localWidth || hwy >= this->localHeight)
        return;

    if (entryBits == 8) {
        indexedBitmap[(indexedRefreshBuffer * INDEXED_BUFFER_SIZE) + (hwy * INDEXED_BUFFER_ROW_SIZE) + hwx] = index;
    } else {
        uint8_t * entryPtr = &indexedBitmap[(indexedRefreshBuffer * INDEXED_BUFFER_SIZE) + (hwy * INDEXED_BUFFER_ROW_SIZE) + (hwx * entryBits / 8)];
        uint8_t entryIndex = hwx % entryPack;
        uint8_t entryShift = (entryPack - entryIndex - 1) * entryBits;
        *entryPtr &= ~(entryMask << entryShift); 
        *entryPtr |= (index << entryShift); 
    }
}

template <typename RGB, unsigned int optionFlags>
void SMLayerIndexed<RGB, optionFlags>::setFont(fontChoices newFont) {
    layerFont = (bitmap_font *)fontLookup(newFont);
    majorScrollFontChange = true;
}

template <typename RGB, unsigned int optionFlags>
void SMLayerIndexed<RGB, optionFlags>::drawChar(int16_t x, int16_t y, uint8_t index, char character) {
    uint8_t tempBitmask, shiftMask;
    int i, j;

    // only draw if character is on the screen
    if (x + scrollFont->Width < 0 || x >= this->localWidth) {
        return;
    }

    for (i = y; i < y+layerFont->Height; i++) {
        // ignore rows that are not on the screen
        if(i < 0) continue;
        if (i >= this->localHeight) return;

        tempBitmask = getBitmapFontRowAtXY(character, i - y, layerFont);
        shiftMask = 0x80;
        for (j = 0; j < layerFont->Width; j++) {
            if (tempBitmask & shiftMask)
                drawPixel(x + j, i, index);
            shiftMask >>= 1;
        }
    }
}

template <typename RGB, unsigned int optionFlags>
void SMLayerIndexed<RGB, optionFlags>::drawString(int16_t x, int16_t y, uint8_t index, const char text []) {
    uint8_t maxChars = (this->localWidth / layerFont->Width) + 1;
    
    for (int i = 0; i < maxChars; i++) {
        char character = text[i];
        if (character == '\0')
            return;

        drawChar(i * layerFont->Width + x, y, index, character);
    }
}

template <typename RGB, unsigned int optionFlags>
void SMLayerIndexed<RGB, optionFlags>::drawMonoBitmap(int16_t x, int16_t y, uint8_t width, uint8_t height, uint8_t index, uint8_t *bitmap) {
    int xcnt, ycnt;

    for (ycnt = 0; ycnt < height; ycnt++) {
        for (xcnt = 0; xcnt < width; xcnt++) {
            if (getBitmapPixelAtXY(xcnt, ycnt, width, height, bitmap)) {
                drawPixel(x + xcnt, y + ycnt, index);
            }
        }
    }
}

template <typename RGB, unsigned int optionFlags>
bool SMLayerIndexed<RGB, optionFlags>::getBitmapPixelAtXY(uint8_t x, uint8_t y, uint8_t width, uint8_t height, const uint8_t *bitmap) {
    int cell = (y * ((width / 8) + 1)) + (x / 8);

    uint8_t mask = 0x80 >> (x % 8);
    return (mask & bitmap[cell]);
}

