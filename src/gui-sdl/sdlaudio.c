/*
  Previous - sdlaudio.c

  This file is distributed under the GNU General Public License, version 2
  or at your option any later version. Read the file gpl.txt for details.

  This file contains the SDL interface for sound input and sound output.
*/
const char SDLaudio_fileid[] = "Previous sdlaudio.c";

#include <SDL3/SDL.h>

#include "main.h"
#include "audio.h"
#include "log.h"
#include "statusbar.h"


static SDL_AudioStream* Audio_Output_Stream = NULL;
static SDL_AudioStream* Audio_Input_Stream  = NULL;
static SDL_AudioStream* Audio_DSP_Stream    = NULL;

/*-----------------------------------------------------------------------*/
/**
 * Sound playback functions.
 */
static int Audio_Buffer_Size;

void Audio_Output_Queue_Put(uint8_t* data, int len) {
	if (Audio_Output_Stream && len > 0) {
		SDL_PutAudioStreamData(Audio_Output_Stream, data, len);
	}
}

int Audio_Output_Queue_Size(void) {
	if (Audio_Output_Stream) {
		if (SDL_GetAudioStreamAvailable(Audio_Output_Stream) > Audio_Buffer_Size) {
			return SDL_GetAudioStreamQueued(Audio_Output_Stream);
		}
	}
	return 0;
}

void Audio_Output_Queue_Flush(void) {
	if (Audio_Output_Stream) {
		SDL_FlushAudioStream(Audio_Output_Stream);
	}
}

void Audio_Output_Queue_Clear(void) {
	if (Audio_Output_Stream) {
		SDL_ClearAudioStream(Audio_Output_Stream);
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
	Log_Printf(LOG_WARN, "[Audio] Initializing input buffer with %d samples of silence.", init);
	buf->read = 0;
	/* Initialise buffer with silence to compensate for time gap between
	 * Audio_Input_Enable() and first availability of recorded data. */
	for (buf->size = 0; buf->size < init; buf->size++) {
		buf->data[buf->size] = 0;
	}
}

static int Audio_Data_Get(SDL_AudioStream* stream, struct rec_data* buf, int16_t* sample) {
	if (stream) {
		if (buf->read >= buf->size) { /* Try to re-fill buffer in case it is empty. */
			buf->read = 0;
			buf->size = SDL_GetAudioStreamData(stream, buf->data, sizeof(buf->data));
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
		return SDL_GetAudioStreamAvailable(Audio_Input_Stream);
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
static void Audio_Enable(SDL_AudioStream* stream, bool bEnable) {
	if (stream) {
		if (bEnable && SDL_AudioStreamDevicePaused(stream)) {
			/* Start */
			SDL_ResumeAudioStreamDevice(stream);
		} else if (!bEnable && !SDL_AudioStreamDevicePaused(stream)) {
			/* Stop */
			SDL_PauseAudioStreamDevice(stream);
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
static void Audio_Open(SDL_AudioStream** stream, SDL_AudioDeviceID dev, int channels, int freq) {
	if (*stream == NULL) {
		SDL_AudioSpec request = {SDL_AUDIO_S16BE, channels, freq};
		
		/* Init the SDL's audio subsystem: */
		if (SDL_WasInit(SDL_INIT_AUDIO) == 0) {
			if (SDL_InitSubSystem(SDL_INIT_AUDIO) == false) {
				Log_Printf(LOG_WARN, "[Audio] Could not init audio subsystem: %s", SDL_GetError());
				Statusbar_AddMessage("Error: Can't open SDL audio subsystem.", 5000);
				return;
			}
		}
		/* Open streaming device */
		*stream = SDL_OpenAudioDeviceStream(dev, &request, NULL, NULL);
		if (*stream == NULL) {
			Log_Printf(LOG_WARN, "[Audio] Could not open audio device: %s", SDL_GetError());
			Statusbar_AddMessage("Error: Can't open audio output device. No sound.", 5000);
		}
	}
}

void Audio_Output_Init(int channels, int freq) {
	SDL_AudioSpec spec = { 0 };
	int frames = 0;
	Audio_Open(&Audio_Output_Stream, SDL_AUDIO_DEVICE_DEFAULT_PLAYBACK, channels, freq);
	SDL_GetAudioDeviceFormat(SDL_GetAudioStreamDevice(Audio_Output_Stream), &spec, &frames);
	Audio_Buffer_Size = frames * SDL_AUDIO_FRAMESIZE(spec);
	Log_Printf(LOG_WARN, "[Audio] Output buffer size: %d byte", Audio_Buffer_Size);
}

void Audio_Input_InitAndEnable(int channels, int freq) {
	Audio_Open(&Audio_Input_Stream, SDL_AUDIO_DEVICE_DEFAULT_RECORDING, channels, freq);
	Audio_Init_Data(&codec_data, 32);
	Audio_Enable(Audio_Input_Stream, true);
}

void Audio_DSP_InitAndEnable(int channels, int freq) {
	Audio_Open(&Audio_DSP_Stream, SDL_AUDIO_DEVICE_DEFAULT_RECORDING, channels, freq);
	Audio_Init_Data(&dsp_data, 32);
	Audio_Enable(Audio_Input_Stream, true);
}

/*-----------------------------------------------------------------------*/
/**
 * Free the audio subsystem.
 */
static void Audio_Close(SDL_AudioStream** stream) {
	if (*stream) {
		/* Stop and close audio stream */
		SDL_DestroyAudioStream(*stream);
		*stream = NULL;
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
