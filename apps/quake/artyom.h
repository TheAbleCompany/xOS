/*
 * artyom.h -- общие точки соприкосновения платформенного слоя Quake
 * с ArtyomX64: окно, COM1-отладка, монотонное время, насос событий.
 */
#ifndef ARTYOM_QUAKE_H
#define ARTYOM_QUAKE_H

#include <stdint.h>

extern uint32_t *ax_win;
extern int ax_win_w;
extern int ax_win_h;

void AX_InitDebug(void);
void AX_DebugPuts(const char *s);
void AX_DebugHex(uint32_t v);
void AX_DebugDec(int v);
void AX_Info(const char *fmt, ...);

uint32_t AX_NowMS(void);
void AX_PumpEvents(void);

#endif /* ARTYOM_QUAKE_H */
