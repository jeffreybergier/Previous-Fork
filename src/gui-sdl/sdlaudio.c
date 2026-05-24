/*
  Previous - sdlaudio.c

  This file is distributed under the GNU General Public License, version 2
  or at your option any later version. Read the file gpl.txt for details.

  This file contains the SDL interface for sound input and sound output.
*/
const char SDLaudio_fileid[] = "Previous sdlaudio.c";

#include <SDL.h>

#include "main.h"
#include "audio.h"
#include "log.h"
#include "statusbar.h"


static SDL_AudioDeviceID Audio_Output_Device = 0;
static SDL_AudioDeviceID Audio_Input_Device  = 0;
static SDL_AudioDeviceID Audio_DSP_Device    = 0;

static bool bPlayingBuffer   = false; /* Is playback active? */
static bool bRecordingBuffer = false; /* Is recording active? */
static bool bDspRecording    = false; /* Is DSP recording active? */


/*-----------------------------------------------------------------------*/
/**
 * Sound playback functions.
 */
void Audio_Output_Queue_Put(uint8_t* data, int len) {
	if (len > 0) {
		if (Audio_Output_Device) {
			int chunkSize = 512;
			do {
				if (len < chunkSize) chunkSize = len;
				SDL_QueueAudio(Audio_Output_Device, data, chunkSize);
				data += chunkSize;
				len  -= chunkSize;
			} while (len > 0);
		}
	}
}

int Audio_Output_Queue_Size(void) {
	if (Audio_Output_Device) {
		return SDL_GetQueuedAudioSize(Audio_Output_Device) / 4;
	} else {
		return 0;
	}
}

void Audio_Output_Queue_Flush(void) {
}

void Audio_Output_Queue_Clear(void) {
	if (Audio_Output_Device) {
		SDL_ClearQueuedAudio(Audio_Output_Device);
	}
}

/*-----------------------------------------------------------------------*/
/**
 * Sound recording functions.
 */
#define RECDATA_SIZE 4096
#define RECDATA_MASK (RECDATA_SIZE - 1)
struct rec_data {
	uint8_t data[RECDATA_SIZE];
	SDL_SpinLock lock;
	int write;
	int read;
};

static struct rec_data codec_data;
static struct rec_data dsp_data;

static void Audio_Init_Data(struct rec_data* buf, int init) {
	Log_Printf(LOG_WARN, "[Audio] Initializing input buffer with %d samples of silence.", init);
	buf->read = 0;
	/* Initialise buffer with silence to compensate for time gap between
	 * Audio_Input_Enable() and first availability of recorded data. */
	for (buf->write = 0; buf->write < init; buf->write++) {
		buf->data[buf->write] = 0;
	}
}

static int Audio_Data_Get(SDL_AudioDeviceID device, struct rec_data* buf, int16_t* sample) {
	int result = 0;
	if (device) {
		SDL_AtomicLock(&buf->lock);
		if ((buf->read & RECDATA_MASK) != (buf->write & RECDATA_MASK)) {
			*sample = (((uint16_t)buf->data[buf->read & RECDATA_MASK] << 8) | buf->data[(buf->read & RECDATA_MASK) + 1]);
			buf->read += 2;
			buf->read &= RECDATA_MASK;
		} else {
			result = -1;
		}
		SDL_AtomicUnlock(&buf->lock);
	} else {
		*sample = 0; /* silence */
	}
	return result;
}

int Audio_Input_Buffer_Size(void) {
	int result = 0;
	if (Audio_Input_Device) {
		SDL_AtomicLock(&codec_data.lock);
		if (codec_data.read <= codec_data.write) {
			result = codec_data.write - codec_data.read;
		} else {
			result = RECDATA_SIZE - (codec_data.read - codec_data.write);
		}
		SDL_AtomicUnlock(&codec_data.lock);
	}
	return result;
}

int Audio_Input_Buffer_Get(int16_t* sample) {
	return Audio_Data_Get(Audio_Input_Device, &codec_data, sample);
}

int Audio_DSP_Buffer_Get(int16_t* sample) {
	return Audio_Data_Get(Audio_DSP_Device, &dsp_data, sample);
}

static void Audio_Data_Put(struct rec_data* buf, uint8_t* data, int len) {
	if (len > 0) {
		SDL_AtomicLock(&buf->lock);
		while (len--) {
			buf->data[buf->write++ & RECDATA_MASK] = *data++;
		}
		buf->write &= RECDATA_MASK;
		buf->write &= ~1; /* Just to be sure */
		SDL_AtomicUnlock(&buf->lock);
	}
}

static void Audio_Input_CallBack(void *userdata, uint8_t *stream, int len) {
	Log_Printf(LOG_WARN, "Audio_Input_CallBack %d", len);
	Audio_Data_Put(&codec_data, stream, len);
}

static void Audio_DSP_CallBack(void *userdata, uint8_t *stream, int len) {
	Log_Printf(LOG_WARN, "Audio_DSP_CallBack %d", len);
	Audio_Data_Put(&dsp_data, stream, len);
}

/*-----------------------------------------------------------------------*/
/**
 * Initialise the audio subsystem.
 */
static void Audio_Open(SDL_AudioCallback func, SDL_AudioDeviceID *dev, int input, int channels, int freq) {
	if (*dev == 0) {
		SDL_AudioSpec request = {freq, AUDIO_S16MSB, channels, 0, 512, 0, 0, func, NULL};
		
		/* Init the SDL's audio subsystem: */
		if (SDL_WasInit(SDL_INIT_AUDIO) == 0) {
			if (SDL_InitSubSystem(SDL_INIT_AUDIO) < 0) {
				Log_Printf(LOG_WARN, "[Audio] Could not init audio subsystem: %s\n", SDL_GetError());
				Statusbar_AddMessage("Error: Can't open SDL audio subsystem.", 5000);
				return;
			}
		}
		/* Open streaming device */
		*dev = SDL_OpenAudioDevice(NULL, input, &request, NULL, 0);
		if (*dev == 0) {
			Log_Printf(LOG_WARN, "[Audio] Could not open audio device: %s\n", SDL_GetError());
			Statusbar_AddMessage("Error: Can't open audio output device. No sound.", 5000);
		}
	}
}

void Audio_Output_Init(int channels, int freq) {
	Audio_Open(NULL, &Audio_Output_Device, 0, channels, freq);
}

void Audio_Input_Init(int channels, int freq) {
	Audio_Open(Audio_Input_CallBack, &Audio_Input_Device, 1, channels, freq);
}

void Audio_DSP_Init(int channels, int freq) {
	Audio_Open(Audio_DSP_CallBack, &Audio_DSP_Device, 1, channels, freq);
}

/*-----------------------------------------------------------------------*/
/**
 * Free the audio subsystem.
 */
void Audio_Output_UnInit(void) {
	if (Audio_Output_Device) {
		/* Stop */
		Audio_Output_Enable(false);
		SDL_CloseAudioDevice(Audio_Output_Device);
		Audio_Output_Device = 0;
	}
}

void Audio_Input_UnInit(void) {
	if (Audio_Input_Device) {
		/* Stop */
		Audio_Input_Enable(false);
		SDL_CloseAudioDevice(Audio_Input_Device);
		Audio_Input_Device = 0;
	}
}

void Audio_DSP_UnInit(void) {
	if (Audio_DSP_Device) {
		/* Stop */
		Audio_DSP_Enable(false);
		SDL_CloseAudioDevice(Audio_DSP_Device);
		Audio_DSP_Device = 0;
	}
}

/*-----------------------------------------------------------------------*/
/**
 * Start/Stop playback and recording.
 */
void Audio_Output_Enable(bool bEnable) {
	if (bEnable && !bPlayingBuffer) {
		/* Start playing */
		SDL_PauseAudioDevice(Audio_Output_Device, false);
		bPlayingBuffer = true;
	} else if (!bEnable && bPlayingBuffer) {
		/* Stop from playing */
		SDL_PauseAudioDevice(Audio_Output_Device, true);
		bPlayingBuffer = false;
	}
}

void Audio_Input_Enable(bool bEnable) {
	if (bEnable && !bRecordingBuffer) {
		/* Start recording */
		Audio_Init_Data(&codec_data, 32);
		SDL_PauseAudioDevice(Audio_Input_Device, false);
		bRecordingBuffer = true;
	} else if (!bEnable && bRecordingBuffer) {
		/* Stop recording */
		SDL_PauseAudioDevice(Audio_Input_Device, true);
		bRecordingBuffer = false;
	}
}

void Audio_DSP_Enable(bool bEnable) {
	if (bEnable && !bDspRecording) {
		/* Start recording */
		Audio_Init_Data(&dsp_data, 32);
		SDL_PauseAudioDevice(Audio_DSP_Device, false);
		bDspRecording = true;
	} else if (!bEnable && bDspRecording) {
		/* Stop recording */
		SDL_PauseAudioDevice(Audio_DSP_Device, true);
		bDspRecording = false;
	}
}
