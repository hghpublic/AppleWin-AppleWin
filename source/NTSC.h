#pragma once

#include "Video.h"	// NB. needed by GCC (for fwd enum declaration)

// Globals (Public)
extern uint32_t g_nChromaSize;

// Prototypes (Public) ________________________________________________
void NTSC_SetVideoMode(uint32_t uVideoModeFlags, bool bDelay=false);
void NTSC_SetVideoStyle(void);
void NTSC_SetVideoTextMode(int cols);
uint32_t* NTSC_VideoGetChromaTable(bool bHueTypeMonochrome, bool bMonitorTypeColorTV);
void NTSC_VideoClockResync(const uint32_t dwCyclesThisFrame);
uint16_t NTSC_VideoGetScannerAddress(const ULONG uExecutedCycles, const bool fullSpeed);
void NTSC_GetVideoVertHorzForDebugger(uint16_t& vert, uint16_t& horz);
uint16_t NTSC_GetVideoVertForDebugger(void);
void NTSC_Destroy(void);
void NTSC_VideoInit(uint8_t *pFramebuffer);
void NTSC_VideoReinitialize(uint32_t cyclesThisFrame, bool bInitVideoScannerAddress);
void NTSC_VideoInitAppleType(void);
void NTSC_VideoInitChroma(void);
void NTSC_VideoUpdateCycles(UINT cycles6502);
void NTSC_VideoRedrawWholeScreen(void);

void NTSC_SetRefreshRate(VideoRefreshRate_e rate);
UINT NTSC_GetCyclesPerFrame(void);
UINT NTSC_GetCyclesPerLine(void);
UINT NTSC_GetVideoLines(void);
UINT NTSC_GetCyclesUntilVBlank(int cycles);
bool NTSC_GetVblBar(void);
bool NTSC_IsVisible(void);
uint16_t NTSC_GetScannerAddressAndData(uint32_t& data, int& dataSize);
