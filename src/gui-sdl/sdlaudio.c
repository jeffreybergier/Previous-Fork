/*
  Previous - sdlaudio.c

  This file is distributed under the GNU General Public License, version 2
  or at your option any later version. Read the file gpl.txt for details.

  This file contains the SDL interface for sound input and sound output.
*/
const char SDLaudio_fileid[] = "Previous sdlaudio.c";

#include "main.h"
#include "audio.h"
#include "sdlaudio.h"
#include "log.h"
#include "statusbar.h"


static SDL_AudioDeviceID Audio_Output_Stream = 0;
static SDL_AudioDeviceID Audio_Input_Stream  = 0;
static SDL_AudioDeviceID Audio_DSP_Stream    = 0;

/*-----------------------------------------------------------------------*/
/**
 * Sound playback functions.
 */
void Audio_Output_Queue_Put(uint8_t* data, int len) {
	if (Audio_Output_Stream && len > 0) {
		SDL_QueueAudio(Audio_Output_Stream, data, len);
	}
}

int Audio_Output_Queue_Size(void) {
	if (Audio_Output_Stream) {
		int size = SDL_GetQueuedAudioSize(Audio_Output_Stream);
		if (size > 512 * 4) { /* Matches requested buffer size (request is in frames) */
			return size;
		}
	}
	return 0;
}

void Audio_Output_Queue_Flush(void) {
}

void Audio_Output_Queue_Clear(void) {
	if (Audio_Output_Stream) {
		SDL_ClearQueuedAudio(Audio_Output_Stream);
	}
}

/*-----------------------------------------------------------------------*/
/**
 * Sound recording functions.
 */
struct rec_data {
	uint8_t data[4096];
	int read;
	int size;
};

static struct rec_data codec_data;
static struct rec_data dsp_data;

static void Audio_Init_Data(struct rec_data* buf, int init) {
	Log_Printf(LOG_WARN, "[Audio] Initialising input buffer with %d samples of silence.", init);
	buf->read = 0;
	/* Initialise buffer with silence to compensate for time gap between
	 * Audio_Input_Enable() and first availability of recorded data. */
	for (buf->size = 0; buf->size < init; buf->size++) {
		buf->data[buf->size] = 0;
	}
}

static int Audio_Data_Get(SDL_AudioDeviceID device, struct rec_data* buf, int16_t* sample) {
	if (device) {
		if (buf->read >= buf->size) { /* Try to re-fill buffer in case it is empty. */
			buf->read = 0;
			buf->size = SDL_DequeueAudio(device, buf->data, sizeof(buf->data));
			if (buf->size & 1) {
				Log_Printf(LOG_WARN, "[Audio] Recorded data has invalid size (%d).", buf->size);
				buf->size--;
			}
		}
		if (buf->read < buf->size) {
			*sample = (((uint16_t)buf->data[buf->read] << 8) | buf->data[buf->read + 1]);
			buf->read += 2;
		} else {
			return -1;
		}
	} else {
		*sample = 0; /* silence */
	}
	return 0;
}

int Audio_Input_Buffer_Size(void) {
	if (Audio_Input_Stream) {
		return SDL_GetQueuedAudioSize(Audio_Input_Stream);
	}
	return 0;
}

int Audio_Input_Buffer_Get(int16_t* sample) {
	return Audio_Data_Get(Audio_Input_Stream, &codec_data, sample);
}

int Audio_DSP_Buffer_Get(int16_t* sample) {
	return Audio_Data_Get(Audio_DSP_Stream, &dsp_data, sample);
}

/*-----------------------------------------------------------------------*/
/**
 * Start/Stop playback and recording.
 */
static void Audio_Enable(SDL_AudioDeviceID device, bool bEnable) {
	if (device) {
		if (bEnable && SDL_GetAudioDeviceStatus(device) == SDL_AUDIO_PAUSED) {
			/* Start */
			SDL_PauseAudioDevice(device, 0);
		} else if (!bEnable && SDL_GetAudioDeviceStatus(device) == SDL_AUDIO_PLAYING) {
			/* Stop */
			SDL_PauseAudioDevice(device, 1);
		}
	}
}

void Audio_Output_Enable(bool bEnable) {
	Audio_Enable(Audio_Output_Stream, bEnable);
}

/*-----------------------------------------------------------------------*/
/**
 * Initialise the audio subsystem.
 */
static void Audio_Open(SDL_AudioDeviceID* device, int iscapture, int channels, int freq) {
	if (*device == 0) {
		SDL_AudioSpec request = {freq, AUDIO_S16MSB, channels, 0, 512, 0, 0, NULL, NULL};
		
		/* Init the SDL's audio subsystem: */
		if (SDL_WasInit(SDL_INIT_AUDIO) == 0) {
			if (SDL_InitSubSystem(SDL_INIT_AUDIO) < 0) {
				Log_Printf(LOG_WARN, "[Audio] Could not init audio subsystem: %s", SDL_GetError());
				Statusbar_AddMessage("Error: Can't open SDL audio subsystem.", 5000);
				return;
			}
		}
		/* Open streaming device */
		*device = SDL_OpenAudioDevice(NULL, iscapture, &request, NULL, 0);
		if (*device == 0) {
			Log_Printf(LOG_WARN, "[Audio] Could not open audio device: %s", SDL_GetError());
			Statusbar_AddMessage("Error: Can't open audio output device. No sound.", 5000);
		}
	}
}

void Audio_Output_Init(int channels, int freq) {
	Audio_Open(&Audio_Output_Stream, 0, channels, freq);
}

void Audio_Input_InitAndEnable(int channels, int freq) {
	Audio_Open(&Audio_Input_Stream, 1, channels, freq);
	Audio_Init_Data(&codec_data, 32);
	Audio_Enable(Audio_Input_Stream, true);
}

void Audio_DSP_InitAndEnable(int channels, int freq) {
	Audio_Open(&Audio_DSP_Stream, 1, channels, freq);
	Audio_Init_Data(&dsp_data, 32);
	Audio_Enable(Audio_DSP_Stream, true);
}

/*-----------------------------------------------------------------------*/
/**
 * Free the audio subsystem.
 */
static void Audio_Close(SDL_AudioDeviceID* device) {
	if (*device) {
		/* Stop and close audio stream */
		SDL_CloseAudioDevice(*device);
		*device = 0;
	}
}

void Audio_Output_UnInit(void) {
	Audio_Close(&Audio_Output_Stream);
}

void Audio_Input_UnInit(void) {
	Audio_Close(&Audio_Input_Stream);
}

void Audio_DSP_UnInit(void) {
	Audio_Close(&Audio_DSP_Stream);
}
