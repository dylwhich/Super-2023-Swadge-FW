/*
 * mode_tunernome.c
 *
 *  Created on: September 17th, 2020
 *      Author: bryce
 */

/*============================================================================
 * Includes
 *==========================================================================*/

#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <limits.h>
#include <math.h>

#include "bresenham.h"
#include "display.h"
#include "embeddednf.h"
#include "embeddedout.h"
#include "esp_timer.h"
#include "led_util.h"
#include "linked_list.h"
#include "mode_main_menu.h"
#include "musical_buzzer.h"
#include "swadgeMode.h"
#include "settingsManager.h"

#include "mode_tunernome.h"

/*============================================================================
 * Defines, Structs, Enums
 *==========================================================================*/

#ifndef M_PI
    #define M_PI 3.14159265358979323846
#endif

#define CORNER_OFFSET 9
#define US_TO_QUIT 1048576 // 2^20, makes division easy

#define NUM_GUITAR_STRINGS    6
#define NUM_VIOLIN_STRINGS    4
#define NUM_UKULELE_STRINGS   4
#define GUITAR_OFFSET         0
#define CHROMATIC_OFFSET      6 // adjust start point by quartertones
#define SENSITIVITY           5
#define TONAL_DIFF_IN_TUNE_DEVIATION 10

#define METRONOME_CENTER_X    tunernome->disp->w / 2
#define METRONOME_CENTER_Y    tunernome->disp->h - 16 - CORNER_OFFSET
#define METRONOME_RADIUS      70
#define INITIAL_BPM           60
#define MAX_BPM               400
#define METRONOME_FLASH_MS    35
#define METRONOME_CLICK_MS    35
#define BPM_CHANGE_FIRST_MS   500
#define BPM_CHANGE_FAST_MS    2000
#define BPM_CHANGE_REPEAT_MS  50

/// Helper macro to return an integer clamped within a range (MIN to MAX)
#define CLAMP(X, MIN, MAX) ( ((X) > (MAX)) ? (MAX) : ( ((X) < (MIN)) ? (MIN) : (X)) )
/// Helper macro to return the absolute value of an integer
#define ABS(X) (((X) < 0) ? -(X) : (X))
/// Helper macro to return the highest of two integers
// #define MAX(X, Y) ( ((X) > (Y)) ? (X) : (Y) )

#define NUM_SEMITONES 12

typedef enum
{
    TN_TUNER,
    TN_METRONOME
} tnMode;

typedef enum
{
    GUITAR_TUNER = 0,
    VIOLIN_TUNER,
    UKULELE_TUNER,
    SEMITONE_0,
    SEMITONE_1,
    SEMITONE_2,
    SEMITONE_3,
    SEMITONE_4,
    SEMITONE_5,
    SEMITONE_6,
    SEMITONE_7,
    SEMITONE_8,
    SEMITONE_9,
    SEMITONE_10,
    SEMITONE_11,
    LISTENING,
    MAX_GUITAR_MODES
} tuner_mode_t;

typedef struct
{
    tnMode mode;
    tuner_mode_t curTunerMode;

    display_t* disp;
    font_t tom_thumb;
    font_t ibm_vga8;
    font_t radiostars;

    buttonBit_t lastBpmButton;
    uint32_t bpmButtonCurChangeUs;
    uint32_t bpmButtonStartUs;
    uint32_t bpmButtonAccumulatedUs;

    dft32_data dd;
    embeddednf_data end;
    embeddedout_data eod;
    int audioSamplesProcessed;
    uint32_t intensities_filt[NUM_LEDS];
    int32_t diffs_filt[NUM_LEDS];

    uint8_t tSigIdx;
    uint8_t beatCtr;
    int bpm;
    int32_t tAccumulatedUs;
    bool isClockwise;
    int32_t usPerBeat;

    uint32_t semitone_intensitiy_filt[NUM_SEMITONES];
    int32_t semitone_diff_filt[NUM_SEMITONES];
    int16_t tonalDiff[NUM_SEMITONES];
    int16_t intensity[NUM_SEMITONES];

    wsg_t upArrowWsg;
    wsg_t flatWsg;

    uint32_t exitTimeStartUs;
    uint32_t exitTimeAccumulatedUs;
    bool exitButtonHeld;

    uint32_t blinkStartUs;
    uint32_t blinkAccumulatedUs;
    bool isBlinking;
} tunernome_t;

typedef struct
{
    uint8_t top;
    uint8_t bottom;
} timeSignature;

/*============================================================================
 * Prototypes
 *==========================================================================*/

void tunernomeEnterMode(display_t* disp);
void tunernomeExitMode(void);
void switchToSubmode(tnMode);
void tunernomeButtonCallback(buttonEvt_t* evt);
void modifyBpm(int16_t bpmMod);
void tunernomeSampleHandler(uint16_t* samples, uint32_t sampleCnt);
void recalcMetronome(void);
void plotInstrumentNameAndNotes(const char* instrumentName, const char** instrumentNotes,
                                uint16_t numNotes);
void plotTopSemiCircle(int xm, int ym, int r, paletteColor_t col);
void instrumentTunerMagic(const uint16_t freqBinIdxs[], uint16_t numStrings, led_t colors[],
                          const uint16_t stringIdxToLedIdx[]);
void tunernomeMainLoop(int64_t elapsedUs);
void ledReset(void* timer_arg);
void fasterBpmChange(void* timer_arg);

static inline int16_t getMagnitude(uint16_t idx);
static inline int16_t getDiffAround(uint16_t idx);
static inline int16_t getSemiMagnitude(int16_t idx);
static inline int16_t getSemiDiffAround(uint16_t idx);
void tnExitTimerFn(void* arg);

/*============================================================================
 * Variables
 *==========================================================================*/

swadgeMode modeTunernome =
{
    .modeName = "Tunernome",
    .fnEnterMode = tunernomeEnterMode,
    .fnExitMode = tunernomeExitMode,
    .fnButtonCallback = tunernomeButtonCallback,
    .fnMainLoop = tunernomeMainLoop,
    .wifiMode = NO_WIFI,
    .fnEspNowRecvCb = NULL,
    .fnEspNowSendCb = NULL,
    .fnAccelerometerCallback = NULL,
    .fnAudioCallback = tunernomeSampleHandler
};

tunernome_t* tunernome;

/*============================================================================
 * Const Variables
 *==========================================================================*/

/**
 * Indicies into fuzzed_bins[], a realtime DFT of sorts
 * fuzzed_bins[0] = A ... 1/2 steps are every 2.
 */
const uint16_t freqBinIdxsGuitar[NUM_GUITAR_STRINGS] =
{
    38, // E string needs to skip an octave... Can't read sounds this low.
    24, // A string is exactly at note #24
    34, // D = A + 5 half steps = 34
    44, // G
    52, // B
    62  // e
};

/**
 * Indicies into fuzzed_bins[], a realtime DFT of sorts
 * fuzzed_bins[0] = A ... 1/2 steps are every 2.
 */
const uint16_t freqBinIdxsViolin[NUM_VIOLIN_STRINGS] =
{
    44, // G
    58, // D
    72, // A
    86  // E
};

/**
 * Indicies into fuzzed_bins[], a realtime DFT of sorts
 * fuzzed_bins[0] = A ... 1/2 steps are every 2.
 */
const uint16_t freqBinIdxsUkulele[NUM_UKULELE_STRINGS] =
{
    68, // G
    54, // C
    62, // E
    72  // A
};

const uint16_t fourNoteStringIdxToLedIdx[4] =
{
    0,
    1,
    4,
    5
};

const char* guitarNoteNames[6] =
{
    "E2",
    "A2",
    "D3",
    "G3",
    "B3",
    "E4"
};

const char* violinNoteNames[4] =
{
    "G3",
    "D4",
    "A4",
    "E5"
};

const char* UkuleleNoteNames[4] =
{
    "G4",
    "C4",
    "E4",
    "A4"
};

// End a string with "\1" to draw the flat symbol
const char* semitoneNoteNames[NUM_SEMITONES] =
{
    "C",
    "C#/D\1",
    "D",
    "D#/E\1",
    "E",
    "F",
    "F#/G\1",
    "G",
    "G#/A\1",
    "A",
    "A#/B\1",
    "B"
};

static const char theWordGuitar[] = "Guitar";
static const char theWordViolin[] = "Violin";
static const char theWordUkulele[] = "Ukulele";
static const char leftStr[] = "< Exit";
static const char rightStrTuner[] = "Tuner >";
static const char rightStrMetronome[] = "Metronome >";

// TODO: these should be const after being assigned
static int TUNER_FLAT_THRES_X;
static int TUNER_SHARP_THRES_X;
static int TUNER_THRES_Y;

static const timeSignature tSigs[] =
{
    {.top = 4, .bottom = 4},
    {.top = 3, .bottom = 4},
    {.top = 2, .bottom = 4},
    {.top = 1, .bottom = 4},
    {.top = 8, .bottom = 4},
    {.top = 7, .bottom = 4},
    {.top = 6, .bottom = 4},
    {.top = 5, .bottom = 4},
};

static const song_t metronome_primary =
{
    .notes =
    {
        {A_4, METRONOME_CLICK_MS}
    },
    .numNotes = 1,
    .shouldLoop = false
};

static const song_t metronome_secondary =
{
    .notes =
    {
        {A_3, METRONOME_CLICK_MS}
    },
    .numNotes = 1,
    .shouldLoop = false
};

/*============================================================================
 * Functions
 *==========================================================================*/

/**
 * Initializer for tunernome
 */
void tunernomeEnterMode(display_t* disp)
{
    // Allocate zero'd memory for the mode
    tunernome = calloc(1, sizeof(tunernome_t));

    tunernome->disp = disp;

    loadFont("tom_thumb.font", &tunernome->tom_thumb);
    loadFont("ibm_vga8.font", &tunernome->ibm_vga8);
    loadFont("radiostars.font", &tunernome->radiostars);

    float intermedX = cosf(TONAL_DIFF_IN_TUNE_DEVIATION * M_PI / 17 );
    float intermedY = sinf(TONAL_DIFF_IN_TUNE_DEVIATION * M_PI / 17 );
    TUNER_SHARP_THRES_X = round(METRONOME_CENTER_X - (intermedX * METRONOME_RADIUS));
    TUNER_FLAT_THRES_X = round(METRONOME_CENTER_X + (intermedX * METRONOME_RADIUS));
    TUNER_THRES_Y = round(METRONOME_CENTER_Y - (ABS(intermedY) * METRONOME_RADIUS));

    loadWsg("uparrow.png", &(tunernome->upArrowWsg));
    loadWsg("flat.png", &(tunernome->flatWsg));

    tunernome->tSigIdx = 0;
    tunernome->beatCtr = 0;
    tunernome->bpm = INITIAL_BPM;
    tunernome->curTunerMode = GUITAR_TUNER;

    switchToSubmode(TN_TUNER);

    InitColorChord(&tunernome->end, &tunernome->dd);

    tunernome->exitTimeStartUs = 0;
    tunernome->exitTimeAccumulatedUs = 0;
    tunernome->exitButtonHeld = false;

    tunernome->blinkStartUs = 0;
    tunernome->blinkAccumulatedUs = 0;
    tunernome->isBlinking = false;
}

/**
 * Switch internal mode
 */
void switchToSubmode(tnMode newMode)
{
    switch(newMode)
    {
        case TN_TUNER:
        {
            tunernome->mode = newMode;

            buzzer_stop();

            led_t leds[NUM_LEDS] = {{0}};
            setLeds(leds, sizeof(leds));

            tunernome->disp->clearPx();

            break;
        }
        case TN_METRONOME:
        {
            tunernome-> mode = newMode;

            tunernome->isClockwise = true;
            tunernome->tSigIdx = 0;
            tunernome->beatCtr = 0;
            tunernome->tAccumulatedUs = 0;

            tunernome->lastBpmButton = 0;
            tunernome->bpmButtonCurChangeUs = 0;
            tunernome->bpmButtonStartUs = 0;
            tunernome->bpmButtonAccumulatedUs = 0;

            tunernome->blinkStartUs = 0;
            tunernome->blinkAccumulatedUs = 0;
            tunernome->isBlinking = false;

            recalcMetronome();

            led_t leds[NUM_LEDS] = {{0}};
            setLeds(leds, sizeof(leds));

            tunernome->disp->clearPx();
            break;
        }
        default:
        {
            break;
        }
    }
}

/**
 * Called when tunernome is exited
 */
void tunernomeExitMode(void)
{
    buzzer_stop();

    freeFont(&tunernome->tom_thumb);
    freeFont(&tunernome->ibm_vga8);
    freeFont(&tunernome->radiostars);

    freeWsg(&(tunernome->upArrowWsg));
    freeWsg(&(tunernome->flatWsg));

    free(tunernome);
}

/**
 * Inline helper function to get the magnitude of a frequency bin from fuzzed_bins[]
 *
 * @param idx The index to get a magnitude from
 * @return A signed magnitude, even though fuzzed_bins[] is unsigned
 */
static inline int16_t getMagnitude(uint16_t idx)
{
    return tunernome->end.fuzzed_bins[idx];
}

/**
 * Inline helper function to get the difference in magnitudes around a given
 * frequency bin from fuzzed_bins[]
 *
 * @param idx The index to get a difference in magnitudes around
 * @return The difference in magnitudes of the bins before and after the given index
 */
static inline int16_t getDiffAround(uint16_t idx)
{
    return getMagnitude(idx + 1) - getMagnitude(idx - 1);
}

/**
 * Inline helper function to get the magnitude of a frequency bin from folded_bins[]
 *
 * @param idx The index to get a magnitude from
 * @return A signed magnitude, even though folded_bins[] is unsigned
 */
static inline int16_t getSemiMagnitude(int16_t idx)
{
    if(idx < 0)
    {
        idx += FIXBPERO;
    }
    if(idx > FIXBPERO - 1)
    {
        idx -= FIXBPERO;
    }
    return tunernome->end.folded_bins[idx];
}

/**
 * Inline helper function to get the difference in magnitudes around a given
 * frequency bin from folded_bins[]
 *
 * @param idx The index to get a difference in magnitudes around
 * @return The difference in magnitudes of the bins before and after the given index
 */
static inline int16_t getSemiDiffAround(uint16_t idx)
{
    return getSemiMagnitude(idx + 1) - getSemiMagnitude(idx - 1);
}

/**
 * Recalculate the per-bpm values for the metronome
 */
void recalcMetronome(void)
{
    // Figure out how many microseconds are in one beat
    tunernome->usPerBeat = (60 * 1000000) / tunernome->bpm;

}

// TODO: make this compatible with instruments with an odd number of notes
/**
 * Plot instrument name and the note names of strings, arranged to match LED positions, in middle of display
 * @param instrumentName The name of the instrument to plot to the display
 * @param instrumentNotes The note names of the strings of the instrument to plot to the display
 * @param numNotes The number of notes in instrumentsNotes
 */
void plotInstrumentNameAndNotes(const char* instrumentName, const char** instrumentNotes,
                                uint16_t numNotes)
{
    // Mode name
    drawText(tunernome->disp, &tunernome->ibm_vga8, c555, instrumentName,
             (tunernome->disp->w - textWidth(&tunernome->ibm_vga8, instrumentName)) / 2,
             (tunernome->disp->h - tunernome->ibm_vga8.h) / 2);

    // Note names of strings, arranged to match LED positions
    bool oddNumLedRows = (numNotes / 2) % 2;
    for(int i = 0; i < numNotes / 2; i++)
    {
        int y;
        if(oddNumLedRows)
        {
            y = (tunernome->disp->h - tunernome->ibm_vga8.h) / 2 + (tunernome->ibm_vga8.h + 5) * (1 - i);
        }
        else
        {
            y = tunernome->disp->h / 2 + (tunernome->ibm_vga8.h + 5) * (- i) + 2;
        }

        drawText(tunernome->disp, &tunernome->ibm_vga8, c555, instrumentNotes[i],
                 (tunernome->disp->w - textWidth(&tunernome->ibm_vga8, instrumentName)) / 2 -
                 textWidth(&tunernome->ibm_vga8, /*placeholder for widest note name + ' '*/ "G4 "), y);
    }
    for(int i = numNotes / 2; i < numNotes; i++)
    {
        int y;
        if(oddNumLedRows)
        {
            y = (tunernome->disp->h - tunernome->ibm_vga8.h) / 2 + (tunernome->ibm_vga8.h + 5) * (i - (numNotes / 2) - 1);
        }
        else
        {
            y = tunernome->disp->h / 2 + (tunernome->ibm_vga8.h + 5) * (i - (numNotes / 2) - 1) + 2;
        }

        drawText(tunernome->disp, &tunernome->ibm_vga8, c555, instrumentNotes[i],
                 (tunernome->disp->w + textWidth(&tunernome->ibm_vga8, instrumentName)) / 2 + textWidth(&tunernome->ibm_vga8, " "), y);
    }
}

/**
 * Instrument-agnostic tuner magic. Updates LEDs
 * @param freqBinIdxs An array of the indices of notes for the instrument's strings. See freqBinIdxsGuitar for an example.
 * @param numStrings The number of strings on the instrument, also the number of elements in freqBinIdxs and stringIdxToLedIdx, if applicable
 * @param colors The RGB colors of the LEDs to set
 * @param stringIdxToLedIdx A remapping from each index into freqBinIdxs (same index into stringIdxToLedIdx), to the index of an LED to map that string/freqBinIdx to. Set to NULL to skip remapping.
 */
void instrumentTunerMagic(const uint16_t freqBinIdxs[], uint16_t numStrings, led_t colors[],
                          const uint16_t stringIdxToLedIdx[])
{
    uint32_t i;
    for( i = 0; i < numStrings; i++ )
    {
        // Pick out the current magnitude and filter it
        tunernome->intensities_filt[i] = (getMagnitude(freqBinIdxs[i] + GUITAR_OFFSET) + tunernome->intensities_filt[i]) -
                                         (tunernome->intensities_filt[i] >> 5);

        // Pick out the difference around current magnitude and filter it too
        tunernome->diffs_filt[i] = (getDiffAround(freqBinIdxs[i] + GUITAR_OFFSET) + tunernome->diffs_filt[i]) -
                                   (tunernome->diffs_filt[i] >> 5);

        // This is the magnitude of the target frequency bin, cleaned up
        int16_t intensity = (tunernome->intensities_filt[i] >> SENSITIVITY) - 40; // drop a baseline.
        intensity = CLAMP(intensity, 0, 255);

        // This is the tonal difference. You "calibrate" out the intensity.
        int16_t tonalDiff = (tunernome->diffs_filt[i] >> SENSITIVITY) * 200 / (intensity + 1);

        int32_t red, grn, blu;
        // Is the note in tune, i.e. is the magnitude difference in surrounding bins small?
        if( (ABS(tonalDiff) < TONAL_DIFF_IN_TUNE_DEVIATION) )
        {
            // Note is in tune, make it white
            red = 255;
            grn = 255;
            blu = 255;
        }
        else
        {
            // Check if the note is sharp or flat
            if( tonalDiff > 0 )
            {
                // Note too sharp, make it red
                red = 255;
                grn = blu = 255 - (tonalDiff) * 15;
            }
            else
            {
                // Note too flat, make it blue
                blu = 255;
                grn = red = 255 - (-tonalDiff) * 15;
            }

            // Make sure LED output isn't more than 255
            red = CLAMP(red, INT_MIN, 255);
            grn = CLAMP(grn, INT_MIN, 255);
            blu = CLAMP(blu, INT_MIN, 255);
        }

        // Scale each LED's brightness by the filtered intensity for that bin
        red = (red >> 3 ) * ( intensity >> 3);
        grn = (grn >> 3 ) * ( intensity >> 3);
        blu = (blu >> 3 ) * ( intensity >> 3);

        // Set the LED, ensure each channel is between 0 and 255
        colors[(stringIdxToLedIdx != NULL) ? stringIdxToLedIdx[i] : i].r = CLAMP(red, 0, 255);
        colors[(stringIdxToLedIdx != NULL) ? stringIdxToLedIdx[i] : i].g = CLAMP(grn, 0, 255);
        colors[(stringIdxToLedIdx != NULL) ? stringIdxToLedIdx[i] : i].b = CLAMP(blu, 0, 255);
    }
}

/**
 * This is called periodically to render the OLED image
 *
 * @param elapsedUs The time elapsed since the last time this function was
 *                  called. Use this value to determine when it's time to do
 *                  things
 */
void tunernomeMainLoop(int64_t elapsedUs)
{
    tunernome->disp->clearPx();

    if(tunernome->exitButtonHeld)
    {
        if(tunernome->exitTimeAccumulatedUs == 0)
        {
            tunernome->exitTimeAccumulatedUs = esp_timer_get_time() - tunernome->exitTimeStartUs;
        }
        else
        {
            tunernome->exitTimeAccumulatedUs += elapsedUs;
        }

        if(tunernome->exitTimeAccumulatedUs >= US_TO_QUIT)
        {
            switchToSwadgeMode(&modeMainMenu);
        }
    }

    switch(tunernome->mode)
    {
        default:
        case TN_TUNER:
        {
            // Instructions at top of display
            drawText(tunernome->disp, &tunernome->ibm_vga8, c115, "Blue=Flat", CORNER_OFFSET, CORNER_OFFSET);
            drawText(tunernome->disp, &tunernome->ibm_vga8, c555, "White=OK", (tunernome->disp->w - textWidth(&tunernome->ibm_vga8,
                     "White=OK")) / 2, CORNER_OFFSET);
            drawText(tunernome->disp, &tunernome->ibm_vga8, c500, "Red=Sharp", tunernome->disp->w - textWidth(&tunernome->ibm_vga8,
                     "Red=Sharp") - CORNER_OFFSET, CORNER_OFFSET);

            // Left/Right button functions at bottom of display
            int16_t afterExit = drawText(tunernome->disp, &tunernome->ibm_vga8, c555, leftStr, CORNER_OFFSET,
                                         tunernome->disp->h - tunernome->ibm_vga8.h - CORNER_OFFSET);
            drawText(tunernome->disp, &tunernome->ibm_vga8, c555, rightStrMetronome,
                     tunernome->disp->w - textWidth(&tunernome->ibm_vga8, rightStrMetronome) - CORNER_OFFSET,
                     tunernome->disp->h - tunernome->ibm_vga8.h - CORNER_OFFSET);

            char gainStr[16] = {0};
            snprintf(gainStr, sizeof(gainStr) - 1, "Gain:%d", getMicGain());
            drawText(tunernome->disp, &tunernome->ibm_vga8, c555, gainStr, 30 + afterExit,
                     tunernome->disp->h - tunernome->ibm_vga8.h - CORNER_OFFSET);

            // Up/Down arrows in middle of display around current note/mode
            drawWsg(tunernome->disp, &(tunernome->upArrowWsg),
                    (tunernome->disp->w - tunernome->upArrowWsg.w) / 2 + 1,
                    tunernome->ibm_vga8.h + 4,
                    false, false, 0);
            drawWsg(tunernome->disp, &(tunernome->upArrowWsg),
                    (tunernome->disp->w - tunernome->upArrowWsg.w) / 2 + 1,
                    tunernome->disp->h - tunernome->upArrowWsg.h,
                    false, true, 0);

            // Current note/mode in middle of display
            switch(tunernome->curTunerMode)
            {
                case GUITAR_TUNER:
                {
                    plotInstrumentNameAndNotes(theWordGuitar, guitarNoteNames, NUM_GUITAR_STRINGS);
                    break;
                }
                case VIOLIN_TUNER:
                {
                    plotInstrumentNameAndNotes(theWordViolin, violinNoteNames, NUM_VIOLIN_STRINGS);
                    break;
                }
                case UKULELE_TUNER:
                {
                    plotInstrumentNameAndNotes(theWordUkulele, UkuleleNoteNames, NUM_UKULELE_STRINGS);
                    break;
                }
                case LISTENING:
                {
                    // Find the note that has the highest intensity. Must be larger than 100
                    int16_t maxIntensity = 100;
                    int8_t semitoneNum = -1;
                    for(uint8_t semitone = 0; semitone < NUM_SEMITONES; semitone++)
                    {
                        if(tunernome->intensity[semitone] > maxIntensity)
                        {
                            maxIntensity = tunernome->intensity[semitone];
                            semitoneNum = semitone;
                        }
                    }

                    led_t leds[NUM_LEDS] = {{0}};

                    // If some note is intense
                    if(-1 != semitoneNum)
                    {
                        // Plot text on top of everything else
                        bool shouldDrawFlat = (semitoneNoteNames[semitoneNum][strlen(semitoneNoteNames[semitoneNum]) - 1] == 1);
                        int16_t tWidth = textWidth(&tunernome->ibm_vga8, semitoneNoteNames[semitoneNum]);
                        if(shouldDrawFlat)
                        {
                            tWidth += tunernome->flatWsg.w + 1;
                        }
                        int16_t textEnd = drawText(tunernome->disp, &tunernome->ibm_vga8, c555, semitoneNoteNames[semitoneNum],
                                                   (tunernome->disp->w - tWidth) / 2 + 1,
                                                   (tunernome->disp->h - tunernome->ibm_vga8.h) / 2);

                        // Append the png for a flat
                        if(shouldDrawFlat)
                        {
                            drawWsg(tunernome->disp, &tunernome->flatWsg, textEnd, (tunernome->disp->h - tunernome->ibm_vga8.h) / 2, false, false,
                                    0);
                        }

                        // Set the LEDs to a colorchord-like value
                        uint32_t toneColor = ECCtoHEX((semitoneNum * 256) / NUM_SEMITONES, 0xFF, 0x80);
                        for(uint8_t i = 0; i < NUM_LEDS; i++)
                        {
                            leds[i].r = (toneColor >>  0) & 0xFF;
                            leds[i].g = (toneColor >>  8) & 0xFF;
                            leds[i].b = (toneColor >> 16) & 0xFF;
                        }
                    }

                    // Set LEDs, this may turn them off
                    setLeds(leds, sizeof(leds));
                    break;
                }
                case MAX_GUITAR_MODES:
                    break;
                case SEMITONE_0:
                case SEMITONE_1:
                case SEMITONE_2:
                case SEMITONE_3:
                case SEMITONE_4:
                case SEMITONE_5:
                case SEMITONE_6:
                case SEMITONE_7:
                case SEMITONE_8:
                case SEMITONE_9:
                case SEMITONE_10:
                case SEMITONE_11:
                default:
                {
                    // Draw tuner needle based on the value of tonalDiff, which is at most -32768 to 32767
                    // clamp it to the range -180 -> 180
                    int16_t clampedTonalDiff = CLAMP(tunernome->tonalDiff[tunernome->curTunerMode - SEMITONE_0] / 2, -180, 180);

                    // If the signal isn't intense enough, don't move the needle
                    if(tunernome->semitone_intensitiy_filt[tunernome->curTunerMode - SEMITONE_0] < 1000)
                    {
                        clampedTonalDiff = -180;
                    }

                    // Find the end point of the unit-length needle
                    float intermedX = sinf(clampedTonalDiff * M_PI / 360.0f );
                    float intermedY = cosf(clampedTonalDiff * M_PI / 360.0f );

                    // Find the actual end point of the full-length needle
                    int x = round(METRONOME_CENTER_X + (intermedX * METRONOME_RADIUS));
                    int y = round(METRONOME_CENTER_Y - (intermedY * METRONOME_RADIUS));

                    // Plot the needle
                    plotLine(tunernome->disp, METRONOME_CENTER_X, METRONOME_CENTER_Y, x, y, c555, 0);
                    // Plot dashed lines indicating the 'in tune' range
                    plotLine(tunernome->disp, METRONOME_CENTER_X, METRONOME_CENTER_Y, TUNER_FLAT_THRES_X, TUNER_THRES_Y, c555, 2);
                    plotLine(tunernome->disp, METRONOME_CENTER_X, METRONOME_CENTER_Y, TUNER_SHARP_THRES_X, TUNER_THRES_Y, c555, 2);
                    // Plot a semicircle around it all
                    plotCircleQuadrants(tunernome->disp, METRONOME_CENTER_X, METRONOME_CENTER_Y, METRONOME_RADIUS, false, false, true, true,
                                        c555);

                    // Plot text on top of everything else
                    uint8_t semitoneNum = (tunernome->curTunerMode - SEMITONE_0);
                    bool shouldDrawFlat = (semitoneNoteNames[semitoneNum][strlen(semitoneNoteNames[semitoneNum]) - 1] == 1);
                    int16_t tWidth = textWidth(&tunernome->ibm_vga8, semitoneNoteNames[semitoneNum]);
                    if(shouldDrawFlat)
                    {
                        tWidth += tunernome->flatWsg.w + 1;
                    }
                    fillDisplayArea(tunernome->disp,
                                    (tunernome->disp->w - tWidth) / 2,
                                    (tunernome->disp->h - tunernome->ibm_vga8.h) / 2 - 1,
                                    (tunernome->disp->w - tWidth) / 2 + tWidth,
                                    ((tunernome->disp->h - tunernome->ibm_vga8.h) / 2) + tunernome->ibm_vga8.h,
                                    c000);
                    int16_t textEnd = drawText(tunernome->disp, &tunernome->ibm_vga8, c555, semitoneNoteNames[semitoneNum],
                                               (tunernome->disp->w - tWidth) / 2 + 1,
                                               (tunernome->disp->h - tunernome->ibm_vga8.h) / 2);

                    // Append the png for a flat
                    if(shouldDrawFlat)
                    {
                        drawWsg(tunernome->disp, &tunernome->flatWsg, textEnd, (tunernome->disp->h - tunernome->ibm_vga8.h) / 2, false, false,
                                0);
                    }
                    break;
                }
            }

            break;
        }
        case TN_METRONOME:
        {
            char bpmStr[32];
            sprintf(bpmStr, "%d bpm, %d/%d", tunernome->bpm, tSigs[tunernome->tSigIdx].top, tSigs[tunernome->tSigIdx].bottom);

            drawText(tunernome->disp, &tunernome->ibm_vga8, c555, bpmStr, (tunernome->disp->w - textWidth(&tunernome->ibm_vga8,
                     bpmStr)) / 2, 0);
            drawText(tunernome->disp, &tunernome->ibm_vga8, c555, leftStr, CORNER_OFFSET,
                     tunernome->disp->h - tunernome->ibm_vga8.h - CORNER_OFFSET);
            drawText(tunernome->disp, &tunernome->ibm_vga8, c555, rightStrTuner,
                     tunernome->disp->w - textWidth(&tunernome->ibm_vga8, rightStrTuner) - CORNER_OFFSET,
                     tunernome->disp->h - tunernome->ibm_vga8.h - CORNER_OFFSET);

            if(tunernome->isBlinking)
            {
                if(tunernome->blinkAccumulatedUs == 0)
                {
                    tunernome->blinkAccumulatedUs = esp_timer_get_time() - tunernome->blinkStartUs;
                }
                else
                {
                    tunernome->blinkAccumulatedUs += elapsedUs;
                }

                if(tunernome->blinkAccumulatedUs > METRONOME_FLASH_MS * 1000)
                {
                    led_t leds[NUM_LEDS] = {{0}};
                    setLeds(leds, sizeof(leds));
                }
            }

            bool shouldBlink = false;
            // If the arm is sweeping clockwise
            if(tunernome->isClockwise)
            {
                // Add to tAccumulatedUs
                tunernome->tAccumulatedUs += elapsedUs;
                // If it's crossed the threshold for one beat
                if(tunernome->tAccumulatedUs >= tunernome->usPerBeat)
                {
                    // Flip the metronome arm
                    tunernome->isClockwise = false;
                    // Start counting down by subtacting the excess time from tAccumulatedUs
                    tunernome->tAccumulatedUs = tunernome->usPerBeat - (tunernome->tAccumulatedUs - tunernome->usPerBeat);
                    // Blink LED Tick color
                    shouldBlink = true;
                } // if(tAccumulatedUs >= tunernome->usPerBeat)
            } // if(tunernome->isClockwise)
            else
            {
                // Subtract from tAccumulatedUs
                tunernome->tAccumulatedUs -= elapsedUs;
                // If it's crossed the threshold for one beat
                if(tunernome->tAccumulatedUs <= 0)
                {
                    // Flip the metronome arm
                    tunernome->isClockwise = true;
                    // Start counting up by flipping the excess time from negative to positive
                    tunernome->tAccumulatedUs = -(tunernome->tAccumulatedUs);
                    // Blink LED Tock color
                    shouldBlink = true;
                } // if(tunernome->tAccumulatedUs <= 0)
            } // if(!tunernome->isClockwise)

            if(shouldBlink)
            {
                tunernome->beatCtr = (tunernome->beatCtr + 1) % tSigs[tunernome->tSigIdx].top;

                song_t* song;
                led_t leds[NUM_LEDS] = {{0}};

                if(0 == tunernome->beatCtr)
                {
                    song = &metronome_primary;
                    for(int i = 0; i < NUM_LEDS; i++)
                    {
                        leds[i].r = 0x40;
                        leds[i].g = 0xFF;
                        leds[i].b = 0x00;
                    }
                }
                else
                {
                    song = &metronome_secondary;
                    for(int i = 0; i < NUM_LEDS; i++)
                    {
                        leds[i].r = 0x40;
                        leds[i].g = 0x00;
                        leds[i].b = 0xFF;
                    }
                    leds[2].r = 0x00;
                    leds[2].g = 0x00;
                    leds[2].b = 0x00;
                    leds[3].r = 0x00;
                    leds[3].g = 0x00;
                    leds[3].b = 0x00;
                }

                buzzer_play_sfx(song);
                setLeds(leds, sizeof(leds));
                tunernome->isBlinking = true;
                tunernome->blinkStartUs = esp_timer_get_time();
                tunernome->blinkAccumulatedUs = 0;
            }

            // Runs after the up or down button is held long enough in metronome mode.
            // Repeats the button press while the button is held.
            if(tunernome->lastBpmButton != 0)
            {
                if(tunernome->bpmButtonAccumulatedUs == 0)
                {
                    tunernome->bpmButtonAccumulatedUs = esp_timer_get_time() - tunernome->bpmButtonStartUs;
                    tunernome->bpmButtonCurChangeUs = tunernome->bpmButtonAccumulatedUs;
                }
                else
                {
                    tunernome->bpmButtonAccumulatedUs += elapsedUs;
                    tunernome->bpmButtonCurChangeUs += elapsedUs;
                }

                if(tunernome->bpmButtonAccumulatedUs >= BPM_CHANGE_FIRST_MS * 1000)
                {
                    if(tunernome->bpmButtonCurChangeUs >= BPM_CHANGE_REPEAT_MS * 1000)
                    {
                        int16_t mod = 1;
                        if(tunernome->bpmButtonAccumulatedUs >= BPM_CHANGE_FAST_MS * 1000)
                        {
                            mod = 3;
                        }
                        switch(tunernome->lastBpmButton)
                        {
                            case UP:
                            {
                                modifyBpm(mod);
                                break;
                            }
                            case DOWN:
                            {
                                modifyBpm(-mod);
                                break;
                            }
                            default:
                            {
                                break;
                            }
                        }

                        tunernome->bpmButtonCurChangeUs = 0;
                    }
                }
            }

            // Draw metronome arm based on the value of tAccumulatedUs, which is between (0, usPerBeat)
            float intermedX = -1 * cosf(tunernome->tAccumulatedUs * M_PI / tunernome->usPerBeat );
            float intermedY = -1 * sinf(tunernome->tAccumulatedUs * M_PI / tunernome->usPerBeat );
            int x = round(METRONOME_CENTER_X - (intermedX * METRONOME_RADIUS));
            int y = round(METRONOME_CENTER_Y - (ABS(intermedY) * METRONOME_RADIUS));
            plotLine(tunernome->disp, METRONOME_CENTER_X, METRONOME_CENTER_Y, x, y, c555, 0);
            break;
        } // case TN_METRONOME:
    } // switch(tunernome->mode)

    // If the quit button is being held
    if(tunernome->exitTimeAccumulatedUs > 0)
    {
        // Draw a bar
        fillDisplayArea(tunernome->disp, 0, tunernome->disp->h - CORNER_OFFSET + 2,
                        (tunernome->disp->w * tunernome->exitTimeAccumulatedUs) / US_TO_QUIT, tunernome->disp->h, c333);
    }
}

/**
 * TODO
 *
 * @param evt The button event that occurred
 */
void tunernomeButtonCallback(buttonEvt_t* evt)
{
    if(LEFT == evt->button)
    {
        if(evt->down)
        {
            // Start the timer to exit
            tunernome->exitButtonHeld = true;
            tunernome-> exitTimeStartUs = esp_timer_get_time();
        }
        else
        {
            // Stop the timer to exit
            tunernome->exitTimeStartUs = 0;
            tunernome->exitTimeAccumulatedUs = 0;
            tunernome->exitButtonHeld = false;
        }
        return;
    }

    switch (tunernome->mode)
    {
        default:
        case TN_TUNER:
        {
            if(evt->down)
            {
                switch(evt->button)
                {
                    case UP:
                    {
                        tunernome->curTunerMode = (tunernome->curTunerMode + 1) % MAX_GUITAR_MODES;
                        break;
                    }
                    case DOWN:
                    {
                        if(0 == tunernome->curTunerMode)
                        {
                            tunernome->curTunerMode = MAX_GUITAR_MODES - 1;
                        }
                        else
                        {
                            tunernome->curTunerMode--;
                        }
                        break;
                    }
                    case BTN_A:
                    {
                        // Cycle microphone sensitivity
                        incMicGain();
                        break;
                    }
                    case RIGHT:
                    {
                        switchToSubmode(TN_METRONOME);
                        break;
                    }
                    case LEFT:
                    {
                        // Handled above
                        break;
                    }
                    default:
                    {
                        break;
                    }
                } // switch(button)
            } // if(down)
            break;
        } // case TN_TUNER:
        case TN_METRONOME:
        {
            if(evt->down)
            {
                switch(evt->button)
                {
                    case UP:
                    {
                        modifyBpm(1);
                        tunernome->lastBpmButton = evt->button;
                        tunernome->bpmButtonStartUs = 0;
                        tunernome->bpmButtonCurChangeUs = 0;
                        tunernome->bpmButtonAccumulatedUs = 0;
                        break;
                    }
                    case DOWN:
                    {
                        modifyBpm(-1);
                        tunernome->lastBpmButton = evt->button;
                        tunernome->bpmButtonStartUs = 0;
                        tunernome->bpmButtonCurChangeUs = 0;
                        tunernome->bpmButtonAccumulatedUs = 0;
                        break;
                    }
                    case BTN_A:
                    {
                        // Cycle the time signature
                        tunernome->tSigIdx = (tunernome->tSigIdx + 1) % (sizeof(tSigs));
                        break;
                    }
                    case RIGHT:
                    {
                        switchToSubmode(TN_TUNER);
                        break;
                    }
                    case LEFT:
                    {
                        // Handled above
                        break;
                    }
                    default:
                    {
                        break;
                    }
                } // switch(button)
            } // if(down)
            else
            {
                switch(evt->button)
                {
                    case UP:
                    case DOWN:
                    {
                        if(evt->button == tunernome->lastBpmButton)
                        {
                            tunernome->lastBpmButton = 0;
                            tunernome->bpmButtonStartUs = 0;
                            tunernome->bpmButtonCurChangeUs = 0;
                            tunernome->bpmButtonAccumulatedUs = 0;
                        }
                        break;
                    }
                    default:
                    {
                        break;
                    }
                }
            }
            break;
        } // case TN_METRONOME:
    }
}

/**
 * Increases the bpm by bpmMod, may be negative
 *
 * @param bpmMod The amount to change the BPM
 */
void modifyBpm(int16_t bpmMod)
{
    tunernome->bpm = CLAMP(tunernome->bpm + bpmMod, 1, MAX_BPM);
    recalcMetronome();
}

/**
 * This function is called whenever audio samples are read from the
 * microphone (ADC) and are ready for processing. Samples are read at 8KHz.
 *
 * @param samples A pointer to 12 bit audio samples
 * @param sampleCnt The number of samples read
 */
void tunernomeSampleHandler(uint16_t* samples, uint32_t sampleCnt)
{
    if(tunernome->mode == TN_TUNER)
    {
        for(uint32_t i = 0; i < sampleCnt; i++)
        {
            PushSample32( &tunernome->dd, samples[i] );
        }
        tunernome->audioSamplesProcessed += sampleCnt;

        // If at least 128 samples have been processed
        if( tunernome->audioSamplesProcessed >= 128 )
        {
            // Colorchord magic
            HandleFrameInfo(&tunernome->end, &tunernome->dd);

            led_t colors[NUM_LEDS] = {{0}};

            switch(tunernome->curTunerMode)
            {
                case GUITAR_TUNER:
                {
                    instrumentTunerMagic(freqBinIdxsGuitar, NUM_GUITAR_STRINGS, colors, NULL);
                    break;
                }
                case VIOLIN_TUNER:
                {
                    instrumentTunerMagic(freqBinIdxsViolin, NUM_VIOLIN_STRINGS, colors, fourNoteStringIdxToLedIdx);
                    break;
                }
                case UKULELE_TUNER:
                {
                    instrumentTunerMagic(freqBinIdxsUkulele, NUM_UKULELE_STRINGS, colors, fourNoteStringIdxToLedIdx);
                    break;
                }
                case MAX_GUITAR_MODES:
                    break;
                case SEMITONE_0:
                case SEMITONE_1:
                case SEMITONE_2:
                case SEMITONE_3:
                case SEMITONE_4:
                case SEMITONE_5:
                case SEMITONE_6:
                case SEMITONE_7:
                case SEMITONE_8:
                case SEMITONE_9:
                case SEMITONE_10:
                case SEMITONE_11:
                case LISTENING:
                default:
                {
                    for(uint8_t semitone = 0; semitone < NUM_SEMITONES; semitone++)
                    {
                        // uint8_t semitoneIdx = (tunernome->curTunerMode - SEMITONE_0) * 2;
                        uint8_t semitoneIdx = semitone * 2;
                        // Pick out the current magnitude and filter it
                        tunernome->semitone_intensitiy_filt[semitone] = (getSemiMagnitude(semitoneIdx + CHROMATIC_OFFSET) +
                                tunernome->semitone_intensitiy_filt[semitone]) -
                                (tunernome->semitone_intensitiy_filt[semitone] >> 5);

                        // Pick out the difference around current magnitude and filter it too
                        tunernome->semitone_diff_filt[semitone] = (getSemiDiffAround(semitoneIdx + CHROMATIC_OFFSET) +
                                tunernome->semitone_diff_filt[semitone]) -
                                (tunernome->semitone_diff_filt[semitone] >> 5);


                        // This is the magnitude of the target frequency bin, cleaned up
                        tunernome->intensity[semitone] = (tunernome->semitone_intensitiy_filt[semitone] >> SENSITIVITY) -
                                                         40; // drop a baseline.
                        tunernome->intensity[semitone] = CLAMP(tunernome->intensity[semitone], 0, 255);

                        //This is the tonal difference. You "calibrate" out the intensity.
                        tunernome->tonalDiff[semitone] = (tunernome->semitone_diff_filt[semitone] >> SENSITIVITY) * 200 /
                                                         (tunernome->intensity[semitone] + 1);
                    }

                    // tonal diff is -32768 to 32767. if its within -10 to 10 (now defined as TONAL_DIFF_IN_TUNE_DEVIATION), it's in tune.
                    // positive means too sharp, negative means too flat
                    // intensity is how 'loud' that frequency is, 0 to 255. you'll have to play around with values
                    int32_t red, grn, blu;
                    // Is the note in tune, i.e. is the magnitude difference in surrounding bins small?
                    if( (ABS(tunernome->tonalDiff[tunernome->curTunerMode - SEMITONE_0]) < TONAL_DIFF_IN_TUNE_DEVIATION) )
                    {
                        // Note is in tune, make it white
                        red = 255;
                        grn = 255;
                        blu = 255;
                    }
                    else
                    {
                        // Check if the note is sharp or flat
                        if( tunernome->tonalDiff[tunernome->curTunerMode - SEMITONE_0] > 0 )
                        {
                            // Note too sharp, make it red
                            red = 255;
                            grn = blu = 255 - (tunernome->tonalDiff[tunernome->curTunerMode - SEMITONE_0] - TONAL_DIFF_IN_TUNE_DEVIATION) * 15;
                        }
                        else
                        {
                            // Note too flat, make it blue
                            blu = 255;
                            grn = red = 255 - (-(tunernome->tonalDiff[tunernome->curTunerMode - SEMITONE_0] + TONAL_DIFF_IN_TUNE_DEVIATION)) * 15;
                        }

                        // Make sure LED output isn't more than 255
                        red = CLAMP(red, INT_MIN, 255);
                        grn = CLAMP(grn, INT_MIN, 255);
                        blu = CLAMP(blu, INT_MIN, 255);
                    }

                    // Scale each LED's brightness by the filtered intensity for that bin
                    red = (red >> 3 ) * ( tunernome->intensity[tunernome->curTunerMode - SEMITONE_0] >> 3);
                    grn = (grn >> 3 ) * ( tunernome->intensity[tunernome->curTunerMode - SEMITONE_0] >> 3);
                    blu = (blu >> 3 ) * ( tunernome->intensity[tunernome->curTunerMode - SEMITONE_0] >> 3);

                    // Set the LED, ensure each channel is between 0 and 255
                    uint32_t i;
                    for (i = 0; i < NUM_GUITAR_STRINGS; i++)
                    {
                        colors[i].r = CLAMP(red, 0, 255);
                        colors[i].g = CLAMP(grn, 0, 255);
                        colors[i].b = CLAMP(blu, 0, 255);
                    }

                    break;
                } // default:
            } // switch(tunernome->curTunerMode)

            if(LISTENING != tunernome->curTunerMode)
            {
                // Draw the LEDs
                setLeds( colors, sizeof(colors) );
            }
            // Reset the sample count
            tunernome->audioSamplesProcessed = 0;
        }
    } // if(tunernome-> mode == TN_TUNER)
}
