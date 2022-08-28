#ifndef _SETTINGS_MANAGER_H_
#define _SETTINGS_MANAGER_H_

#include "swadgeMode.h"
#include "mode_colorchord.h"

//==============================================================================
// Function Prototypes
//==============================================================================

bool getIsMuted(void);
bool setIsMuted(bool);

int32_t getTftBrightness(void);
bool incTftBrightness(void);
bool decTftBrightness(void);

int32_t getLedBrightness(void);
bool incLedBrightness(void);
bool decLedBrightness(void);

int32_t getMicGain(void);
uint8_t getMicAmplitude(void);
bool decMicGain(void);
bool incMicGain(void);

colorchordMode_t getColorchordMode(void);
bool setColorchordMode(colorchordMode_t);

#endif