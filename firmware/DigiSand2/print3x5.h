#pragma once
#include <GyverGFX.h>

void printDig(GyverGFX* mx, int X, int Y, uint8_t dig);
void printIcon(GyverGFX* mx, int X, int Y, uint8_t icon);
// side: 0 — буква A, 1 — буква B. Знак 5×3, младший бит столбца — верхний ряд.
void printSide(GyverGFX* mx, int X, int Y, uint8_t side);