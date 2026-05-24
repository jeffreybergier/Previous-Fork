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

static bool bPlayingBuffer   = false; /* Is playback active? */
static bool bRecordingBuffer = false; /* Is recording active? */
static bool bDspRecording    = false; /* Is DSP recording active? */


/*-----------------------------------------------------------------------*/
/**
 * Sound playback functions.
 */
void Audio_Output_Queue_Put(uint8_t* data, int len) {
	if (Audio_Output_Stream && len > 0) {
		SDL_PutAudioStreamData(Audio_Output_Stream, data, len);
	}
}

int Audio_Output_Queue_Size(void) {
	if (Audio_Output_Stream) {
		return SDL_GetAudioStreamQueued(Audio_Output_Stream) / 4;
	} else {
		return 0;
	}
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
				Log_Printf(LOG_WARN, "[Audio] Recording buffer has invalid size (%d).", buf->size);
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
 * Initialise the audio subsystem.
 */
static void Audio_Open(SDL_AudioDeviceID dev, SDL_AudioStream** stream, int channels, int freq) {
	if (*stream == NULL) {
		SDL_AudioSpec request = {SDL_AUDIO_S16BE, channels, freq};
		
		/* Init the SDL's audio subsystem: */
		if (SDL_WasInit(SDL_INIT_AUDIO) == 0) {
			if (SDL_InitSubSystem(SDL_INIT_AUDIO) == false) {
				Log_Printf(LOG_WARN, "[Audio] Could not init audio subsystem: %s\n", SDL_GetError());
				Statusbar_AddMessage("Error: Can't open SDL audio subsystem.", 5000);
				return;
			}
		}
		/* Open streaming device */
		*stream = SDL_OpenAudioDeviceStream(dev, &request, NULL, NULL);
		if (*stream == NULL) {
			Log_Printf(LOG_WARN, "[Audio] Could not open audio device: %s\n", SDL_GetError());
			Statusbar_AddMessage("Error: Can't open audio output device. No sound.", 5000);
		}
	}
}

void Audio_Output_Init(int channels, int freq) {
	Audio_Open(SDL_AUDIO_DEVICE_DEFAULT_PLAYBACK, &Audio_Output_Stream, channels, freq);
}

void Audio_Input_Init(int channels, int freq) {
	Audio_Open(SDL_AUDIO_DEVICE_DEFAULT_RECORDING, &Audio_Input_Stream, channels, freq);
}

void Audio_DSP_Init(int channels, int freq) {
	Audio_Open(SDL_AUDIO_DEVICE_DEFAULT_RECORDING, &Audio_DSP_Stream, channels, freq);
}

/*-----------------------------------------------------------------------*/
/**
 * Free the audio subsystem.
 */
void Audio_Output_UnInit(void) {
	if (Audio_Output_Stream) {
		/* Stop */
		Audio_Output_Enable(false);
		SDL_DestroyAudioStream(Audio_Output_Stream);
		Audio_Output_Stream = NULL;
	}
}

void Audio_Input_UnInit(void) {
	if (Audio_Input_Stream) {
		/* Stop */
		Audio_Input_Enable(false);
		SDL_DestroyAudioStream(Audio_Input_Stream);
		Audio_Input_Stream = NULL;
	}
}

void Audio_DSP_UnInit(void) {
	if (Audio_DSP_Stream) {
		/* Stop */
		Audio_DSP_Enable(false);
		SDL_DestroyAudioStream(Audio_DSP_Stream);
		Audio_DSP_Stream = NULL;
	}
}

/*-----------------------------------------------------------------------*/
/**
 * Start/Stop playback and recording.
 */
void Audio_Output_Enable(bool bEnable) {
	if (bEnable && !bPlayingBuffer) {
		/* Start playing */
		SDL_ResumeAudioStreamDevice(Audio_Output_Stream);
		bPlayingBuffer = true;
	} else if (!bEnable && bPlayingBuffer) {
		/* Stop from playing */
		SDL_PauseAudioStreamDevice(Audio_Output_Stream);
		bPlayingBuffer = false;
	}
}

void Audio_Input_Enable(bool bEnable) {
	if (bEnable && !bRecordingBuffer) {
		/* Start recording */
		Audio_Init_Data(&codec_data, 32);
		SDL_ResumeAudioStreamDevice(Audio_Input_Stream);
		bRecordingBuffer = true;
	} else if (!bEnable && bRecordingBuffer) {
		/* Stop recording */
		SDL_PauseAudioStreamDevice(Audio_Input_Stream);
		bRecordingBuffer = false;
	}
}

void Audio_DSP_Enable(bool bEnable) {
	if (bEnable && !bDspRecording) {
		/* Start recording */
		Audio_Init_Data(&dsp_data, 32);
		SDL_ResumeAudioStreamDevice(Audio_DSP_Stream);
		bDspRecording = true;
	} else if (!bEnable && bDspRecording) {
		/* Stop recording */
		SDL_PauseAudioStreamDevice(Audio_DSP_Stream);
		bDspRecording = false;
	}
}
