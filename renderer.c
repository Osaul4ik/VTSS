/*
 * This File is Part Of : 
 *      ___                       ___           ___           ___           ___           ___                 
 *     /  /\        ___          /__/\         /  /\         /__/\         /  /\         /  /\          ___   
 *    /  /::\      /  /\         \  \:\       /  /:/         \  \:\       /  /:/_       /  /::\        /  /\  
 *   /  /:/\:\    /  /:/          \  \:\     /  /:/           \__\:\     /  /:/ /\     /  /:/\:\      /  /:/  
 *  /  /:/~/:/   /__/::\      _____\__\:\   /  /:/  ___   ___ /  /::\   /  /:/ /:/_   /  /:/~/::\    /  /:/   
 * /__/:/ /:/___ \__\/\:\__  /__/::::::::\ /__/:/  /  /\ /__/\  /:/\:\ /__/:/ /:/ /\ /__/:/ /:/\:\  /  /::\   
 * \  \:\/:::::/    \  \:\/\ \  \:\~~\~~\/ \  \:\ /  /:/ \  \:\/:/__\/ \  \:\/:/ /:/ \  \:\/:/__\/ /__/:/\:\  
 *  \  \::/~~~~      \__\::/  \  \:\  ~~~   \  \:\  /:/   \  \::/       \  \::/ /:/   \  \::/      \__\/  \:\ 
 *   \  \:\          /__/:/    \  \:\        \  \:\/:/     \  \:\        \  \:\/:/     \  \:\           \  \:\
 *    \  \:\         \__\/      \  \:\        \  \::/       \  \:\        \  \::/       \  \:\           \__\/
 *     \__\/                     \__\/         \__\/         \__\/         \__\/         \__\/                
 *
 * Copyright (c) Rinnegatamante <rinnegatamante@gmail.com>
 *
 */

#include <psp2/types.h>
#include <psp2/display.h>
#include <stdint.h>
#include "font.h"

unsigned int* vram32;
int bufferwidth;
uint32_t color;

void updateFramebuf(const SceDisplayFrameBuf *param) {
	vram32 = param->base;
	bufferwidth = param->pitch;
}

void setTextColor(uint32_t clr) {
	color = clr;
}

void drawCharacter(int character, int x, int y) {
    character &= 0xFF; // font has 256 glyphs
    for (int yy = 0; yy < 10; yy++) {
        uint8_t charPos = font[character * 10 + yy];
        if (!(charPos & 0xFC))
            continue; // empty row of the glyph (bits 7..2 are drawn)
        uint32_t *screenPos = (uint32_t *)vram32 + x + (y + yy) * bufferwidth;
        for (int xx = 7; xx >= 2; xx--) {
			if ((charPos >> xx) & 1) {
				*screenPos = color;
				*(screenPos + 1) = 0;
				*(screenPos + bufferwidth + 1) = 0;
				*(screenPos + bufferwidth) = 0;
			}
			screenPos++;
        }
    }
}

void drawBigCharacter(int character, int x, int y){
    character &= 0xFF;
    for (int yy = 0; yy < 10; yy++) {
        uint8_t charPos = font[character * 10 + yy];
        if (!(charPos & 0xFC))
            continue; // empty row of the glyph
        uint32_t* screenPos = (uint32_t *)vram32 + x + (y + (yy<<1)) * bufferwidth;
        for (int xx = 7; xx >= 2; xx--) {
			if ((charPos >> xx) & 1) {
				*(screenPos) = color;
				*(screenPos + 1) = color;
				*(screenPos + 2) = 0;
				*(screenPos + bufferwidth) = color;
				*(screenPos + bufferwidth + 1) = color;
				*(screenPos + bufferwidth + 2) = 0;
				*(screenPos + bufferwidth * 2) = 0;
				*(screenPos + 1+ bufferwidth * 2) = 0;
			}
			screenPos += 2;
        }
    }
}

void drawString(int x, int y, const char *str) {
	for (; *str; str++, x += 6)
		drawCharacter(*str, x, y);
}

void drawBigString(int x, int y, const char *str) {
	for (; *str; str++, x += 12)
		drawBigCharacter(*str, x, y);
}

/* Integer line with the same pixels as the old float version (truncated
 * steps along the longer axis, end point excluded; a falling segment is drawn
 * from its right end), without its float math: the fraction is carried as a
 * remainder, no division or float per pixel. The charts draw short segments
 * with x0 < x1. */
void drawLine(int x0, int y0, int x1, int y1, uint32_t clr) {
	if (x1 < x0) { int t = x0; x0 = x1; x1 = t; t = y0; y0 = y1; y1 = t; }
	int inv = 0;
	if (y0 > y1) { int t = y0; y0 = y1; y1 = t; inv = 1; }
	const int dx = x1 - x0, dy = y1 - y0, pitch = bufferwidth;
	uint32_t *row = (uint32_t *)vram32 + y0 * pitch;
	int q = 0, r = 0;
	if (dy > dx) {                          /* steep: one pixel per row */
		for (int k = 0; k < dy; k++) {
			row[inv ? x1 - q - (r != 0) : x0 + q] = clr;
			row += pitch;
			r += dx;
			if (r >= dy) { r -= dy; q++; }
		}
	} else {                                /* flat: one pixel per column */
		for (int k = 0; k < dx; k++) {
			row[inv ? x1 - k : x0 + k] = clr;
			r += dy;
			if (r >= dx) { r -= dx; q++; row += pitch; }
		}
	}
}
