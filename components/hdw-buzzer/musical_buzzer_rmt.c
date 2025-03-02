//==============================================================================
// Includes
//==============================================================================

#include <esp_log.h>
#include "esp_timer.h"
#include "musical_buzzer.h"

//==============================================================================
// Structs
//==============================================================================

typedef struct
{
    const song_t* song;
    uint32_t note_index;
    int64_t start_time;
} buzzerTrack_t;

typedef struct
{
    rmt_channel_t channel;
    uint32_t counter_clk_hz;
    buzzerTrack_t bgm;
    buzzerTrack_t sfx;
    const musicalNote_t* playNote;
    bool stopSong;
    bool isMuted;
} rmt_buzzer_t;

//==============================================================================
// Function Prototypes
//==============================================================================

static void play_note(const musicalNote_t* notation);
static bool buzzer_track_check_next_note(buzzerTrack_t* track, bool isActive);

//==============================================================================
// Variables
//==============================================================================

rmt_buzzer_t rmt_buzzer;

//==============================================================================
// Functions
//==============================================================================

/**
 * @brief Initialize a buzzer peripheral
 *
 * @param gpio The GPIO the buzzer is connected to
 * @param rmt  The RMT channel to control the buzzer with
 * @param isMuted true to mute the buzzer, false to make it buzz
 */
void buzzer_init(gpio_num_t gpio, rmt_channel_t rmt, bool isMuted)
{
    // Don't do much if muted
    rmt_buzzer.isMuted = isMuted;
    if(rmt_buzzer.isMuted)
    {
        return;
    }

    // Start with the default RMT configuration
    rmt_config_t dev_config =
    {
        .rmt_mode = RMT_MODE_TX,
        .channel = rmt,
        .gpio_num = gpio,
        .clk_div = 80,
        .mem_block_num = 1,
        .flags = 0,
        .tx_config =
        {
            .carrier_freq_hz = 44100,
            .carrier_level = RMT_CARRIER_LEVEL_HIGH,
            .idle_level = RMT_IDLE_LEVEL_LOW,
            .carrier_duty_percent = 50,
            .carrier_en = false,
            .loop_en = true, // not default
#if SOC_RMT_SUPPORT_TX_LOOP_COUNT
            .loop_count = 0, // not default
#endif
            .idle_output_en = true,
        }
    };

    // Install RMT driver
    ESP_ERROR_CHECK(rmt_config(&dev_config));
    ESP_ERROR_CHECK(rmt_driver_install(rmt, 0, 0));

    // Save the channel and clock frequency
    rmt_buzzer.channel = rmt;
    ESP_ERROR_CHECK(rmt_get_counter_clock(rmt, &rmt_buzzer.counter_clk_hz));
}

/**
 * @brief Start playing a sound effect on the buzzer. This has higher priority
 * than background music
 *
 * @param song The song to play as a sequence of notes
 */
void buzzer_play_sfx(const song_t* song)
{
    // Don't do much if muted
    if(rmt_buzzer.isMuted)
    {
        return;
    }

    // update notation with the new one
    rmt_buzzer.sfx.song = song;
    rmt_buzzer.sfx.note_index = 0;
    rmt_buzzer.sfx.start_time = esp_timer_get_time();

    // Always start playing SFX
    rmt_buzzer.playNote = &(rmt_buzzer.sfx.song->notes[rmt_buzzer.sfx.note_index]);
    rmt_buzzer.stopSong = false;
}

/**
 * @brief Start playing a background music on the buzzer. This has lower priority
 * than sound effects
 *
 * @param song The song to play as a sequence of notes
 */
void buzzer_play_bgm(const song_t* song)
{
    // Don't do much if muted
    if(rmt_buzzer.isMuted)
    {
        return;
    }

    // update notation with the new one
    rmt_buzzer.bgm.song = song;
    rmt_buzzer.bgm.note_index = 0;
    rmt_buzzer.bgm.start_time = esp_timer_get_time();

    // If there is no current SFX
    if(NULL == rmt_buzzer.sfx.song)
    {
        // Start playing BGM
        rmt_buzzer.playNote = &(rmt_buzzer.bgm.song->notes[rmt_buzzer.bgm.note_index]);
        rmt_buzzer.stopSong = false;
    }
}

/**
 * Check a specific track for notes to be played and queue them for playing.
 * This will always advance through notes in a song, even if it's not the active
 * track
 *
 * @param track The track to advance notes in
 * @param isActive true if this is active and should set a note to be played
 *                 false to just advance notes without playing
 * @return true  if this track is playing a note
 *         false if this track is not playing a note
 */
static bool buzzer_track_check_next_note(buzzerTrack_t* track, bool isActive)
{
    // Check if there is a song and there are still notes
    if((NULL != track->song) && (track->note_index < track->song->numNotes))
    {
        // Get the current time
        int64_t cTime = esp_timer_get_time();

        // Check if it's time to play the next note
        if (cTime - track->start_time >= (1000 * track->song->notes[track->note_index].timeMs))
        {
            // Move to the next note
            track->note_index++;
            track->start_time = cTime;

            // Loop if we should
            if(track->song->shouldLoop && (track->note_index == track->song->numNotes))
            {
                track->note_index = 0;
            }

            // If there is a note
            if(track->note_index < track->song->numNotes)
            {
                if(isActive)
                {
                    // Set the note to be played
                    rmt_buzzer.playNote = &(track->song->notes[track->note_index]);
                    rmt_buzzer.stopSong = false;
                }
            }
            else
            {
                if(isActive)
                {
                    // Set the song to stop
                    rmt_buzzer.playNote = NULL;
                    rmt_buzzer.stopSong = true;
                }

                // Clear track data
                track->start_time = 0;
                track->note_index = 0;
                track->song = NULL;
                // Track isn't active
                return false;
            }
        }
        // Track is active
        return true;
    }
    // Track isn't active
    return false;
}

/**
 * @brief Check if there is a new note to play on the buzzer. This must be
 * called periodically
 */
void buzzer_check_next_note(void)
{
    // Don't do much if muted
    if(rmt_buzzer.isMuted)
    {
        return;
    }

    // Try playing SFX first
    bool sfxIsActive = buzzer_track_check_next_note(&rmt_buzzer.sfx, true);
    // Then play BGM if SFX isn't active
    buzzer_track_check_next_note(&rmt_buzzer.bgm, !sfxIsActive);

    // Check if there is a song and there are still notes
    if((NULL != rmt_buzzer.playNote) || rmt_buzzer.stopSong)
    {
        // Make sure RMT is idle
        if(ESP_ERR_TIMEOUT != rmt_wait_tx_done(rmt_buzzer.channel, 0))
        {
            // If there is a note
            if(NULL != rmt_buzzer.playNote)
            {
                // Play the note
                play_note(rmt_buzzer.playNote);
                rmt_buzzer.playNote = NULL;
            }
            else if (rmt_buzzer.stopSong)
            {
                // Song is over
                rmt_tx_stop(rmt_buzzer.channel);
                rmt_buzzer.stopSong = false;
            }
        }
    }
}

/**
 * @brief Play the current note on the buzzer
 * Warning, this MUST only be called when RMT is idle
 */
static void play_note(const musicalNote_t* notation)
{
    if(SILENCE == notation->note)
    {
        rmt_tx_stop(rmt_buzzer.channel);
    }
    else
    {
        static rmt_item32_t notation_code;
        notation_code.level0 = 1;
        // convert frequency to RMT item format
        notation_code.duration0 = rmt_buzzer.counter_clk_hz / notation->note / 2;
        notation_code.level1 = 0,
        // Copy RMT item format
        notation_code.duration1 = notation_code.duration0;

        // loop until manually stopped
        rmt_set_tx_loop_count(rmt_buzzer.channel, 1);

        // start TX
        rmt_write_items(rmt_buzzer.channel, &notation_code, 1, false);
    }
}

/**
 * @brief Stop the buzzer from playing anything
 * This may be called from anywhere
 */
void buzzer_stop(void)
{
    // Don't do much if muted
    if(rmt_buzzer.isMuted)
    {
        return;
    }

    // Spin and wait for any ongoing transmissions to finish
    while (ESP_ERR_TIMEOUT == rmt_wait_tx_done(rmt_buzzer.channel, 0))
    {
        ;
    }

    // Stop transmitting
    rmt_tx_stop(rmt_buzzer.channel);

    // Clear internal variables
    rmt_buzzer.bgm.note_index = 0;
    rmt_buzzer.bgm.song = NULL;
    rmt_buzzer.bgm.start_time = 0;

    rmt_buzzer.sfx.note_index = 0;
    rmt_buzzer.sfx.song = NULL;
    rmt_buzzer.sfx.start_time = 0;

    rmt_buzzer.playNote = NULL;
    rmt_buzzer.stopSong = false;
}
