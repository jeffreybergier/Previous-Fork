/*
  Previous - audio.c

  This file is distributed under the GNU General Public License, version 2
  or at your option any later version. Read the file gpl.txt for details.

  This file contains the SDL interface for sound input and sound output.
*/
const char Audio_fileid[] = "Previous audio.c";

#include "main.h"
#include "statusbar.h"
#include "configuration.h"
#include "m68000.h"
#include "sysdeps.h"
#include "audio.h"
#include "dma.h"
#include "snd.h"
#include "host.h"
#include "grab.h"

#include <SDL.h>


static SDL_AudioDeviceID Audio_Input_Device  = 0;
static SDL_AudioDeviceID Audio_Output_Device = 0;

static bool bSoundOutputWorking = false; /* Is sound output OK */
static bool bSoundInputWorking  = false; /* Is sound input OK */
static bool bPlayingBuffer      = false; /* Is playing buffer? */
static bool bRecordingBuffer    = false; /* Is recording buffer? */


/*-----------------------------------------------------------------------*/
/**
 * Sound output functions.
 */
void Audio_Output_Queue_Put(uint8_t* data, int len) {
	if (len > 0) {
		Grab_Sound(data, len);
		if (bSoundOutputWorking) {
			int chunkSize = SOUND_BUFFER_SAMPLES;
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
	if (bSoundOutputWorking) {
		return SDL_GetQueuedAudioSize(Audio_Output_Device) / 4;
	} else {
		return 0;
	}
}

void Audio_Output_Queue_Clear(void) {
	if (bSoundOutputWorking) {
		SDL_ClearQueuedAudio(Audio_Output_Device);
	}
}

/*-----------------------------------------------------------------------*/
/**
 * Sound input functions.
 *
 * Initialize recording buffer with silence to compensate for time gap
 * between Audio_Input_Enable and first availability of recorded data.
 */
#define AUDIO_RECBUF_INIT 32 /* 16000 byte = 1 second */

#define REC_BUFFER_SIZE (1<<16)
#define REC_BUFFER_MASK (REC_BUFFER_SIZE-1)
static uint8_t  recBuffer[REC_BUFFER_SIZE];
static uint32_t recBufferWr = 0;
static uint32_t recBufferRd = 0;
static lock_t   recBufferLock;

static void Audio_Input_InitBuf(void) {
	Log_Printf(LOG_WARN, "[Audio] Initializing input buffer with %d ms of silence.", AUDIO_RECBUF_INIT>>4);
	recBufferRd = 0;
	for (recBufferWr = 0; recBufferWr < AUDIO_RECBUF_INIT; recBufferWr++) {
		recBuffer[recBufferWr] = 0;
	}
}

int Audio_Input_Buffer_Size(void) {
	if (bSoundInputWorking) {
		if (recBufferRd <= recBufferWr) {
			return recBufferWr - recBufferRd;
		} else {
			return REC_BUFFER_SIZE - (recBufferRd - recBufferWr);
		}
	}
	return 0;
}

int Audio_Input_Buffer_Get(int16_t* sample) {
	if (bSoundInputWorking) {
		if ((recBufferRd&REC_BUFFER_MASK)==(recBufferWr&REC_BUFFER_MASK)) {
			return -1;
		} else {
			*sample = ((recBuffer[recBufferRd&REC_BUFFER_MASK]<<8)|recBuffer[(recBufferRd&REC_BUFFER_MASK)+1]);
			recBufferRd += 2;
			recBufferRd &= REC_BUFFER_MASK;
		}
	} else {
		*sample = 0; /* silence */
	}
	return 0;
}

static void Audio_Input_CallBack(void *userdata, uint8_t *stream, int len) {
	Log_Printf(LOG_WARN, "Audio_Input_CallBack %d", len);
	if(len == 0) return;
	Audio_Input_Lock();
	while(len--) {
		recBuffer[recBufferWr++&REC_BUFFER_MASK] = *stream++;
	}
	recBufferWr &= REC_BUFFER_MASK;
	recBufferWr &= ~1; /* Just to be sure */
	Audio_Input_Unlock();
}

void Audio_Input_Lock(void) {
	host_lock(&recBufferLock);
}

void Audio_Input_Unlock(void) {
	host_unlock(&recBufferLock);
}

static bool check_audio(int requested, int granted, const char* attribute) {
	if(requested != granted)
		Log_Printf(LOG_WARN, "[Audio] Device %s mismatch: requested: %d, granted: %d.", attribute, requested, granted);
	return requested == granted;
}

/*-----------------------------------------------------------------------*/
/**
 * Initialize the audio subsystem. Return true if all OK.
 */
void Audio_Output_Init(void)
{
	SDL_AudioSpec request;    /* We fill in the desired SDL audio options here */
	SDL_AudioSpec granted;

	bSoundOutputWorking = false;

	/* Init the SDL's audio subsystem: */
	if (SDL_WasInit(SDL_INIT_AUDIO) == 0) {
		if (SDL_InitSubSystem(SDL_INIT_AUDIO) < 0) {
			Log_Printf(LOG_WARN, "[Audio] Could not init audio output: %s\n", SDL_GetError());
			Statusbar_AddMessage("Error: Can't open SDL audio subsystem.", 5000);
			return;
		}
	}

	/* Set up SDL audio: */
	request.freq     = SOUND_OUT_FREQUENCY; /* 44,1 kHz */
	request.format   = AUDIO_S16MSB;        /* 16-Bit signed, big endian */
	request.channels = 2;                   /* stereo */
	request.callback = NULL;
	request.userdata = NULL;
	request.samples  = SOUND_BUFFER_SAMPLES; /* buffer size in samples */

	if (Audio_Output_Device == 0) {
		Audio_Output_Device = SDL_OpenAudioDevice(NULL, 0, &request, &granted, 0);
	}
	if (Audio_Output_Device == 0) {
		Log_Printf(LOG_WARN, "[Audio] Could not open audio output device: %s\n", SDL_GetError());
		Statusbar_AddMessage("Error: Can't open audio output device. No sound output.", 5000);
		return;
	}

	bSoundOutputWorking  = true;
	bSoundOutputWorking &= check_audio(request.freq,     granted.freq,     "freq");
	bSoundOutputWorking &= check_audio(request.format,   granted.format,   "format");
	bSoundOutputWorking &= check_audio(request.channels, granted.channels, "channels");
	bSoundOutputWorking &= check_audio(request.samples,  granted.samples,  "samples");

	if (!bSoundOutputWorking) {
		SDL_CloseAudioDevice(Audio_Output_Device);
		Audio_Output_Device = 0;
		Statusbar_AddMessage("Error: Can't open audio output device. No sound output.", 5000);
	}
}

void Audio_Input_Init(void) {
	SDL_AudioSpec request;    /* We fill in the desired SDL audio options here */
	SDL_AudioSpec granted;

	bSoundInputWorking = false;

	/* Init the SDL's audio subsystem: */
	if (SDL_WasInit(SDL_INIT_AUDIO) == 0) {
		if (SDL_InitSubSystem(SDL_INIT_AUDIO) < 0) {
			Log_Printf(LOG_WARN, "[Audio] Could not init audio input: %s\n", SDL_GetError());
			Statusbar_AddMessage("Error: Can't open SDL audio subsystem.", 5000);
			return;
		}
	}

	/* Set up SDL audio: */
	request.freq     = SOUND_IN_FREQUENCY; /* 8kHz */
	request.format   = AUDIO_S16MSB;       /* 16-Bit signed, big endian */
	request.channels = 1;                  /* mono */
	request.callback = Audio_Input_CallBack;
	request.userdata = NULL;
	request.samples  = SOUND_BUFFER_SAMPLES; /* buffer size in samples */

	if (Audio_Input_Device == 0) {
		Audio_Input_Device = SDL_OpenAudioDevice(NULL, 1, &request, &granted, 0); /* Open audio device */
	}
	if (Audio_Input_Device == 0) {
		Log_Printf(LOG_WARN, "[Audio] Could not open audio input device: %s\n", SDL_GetError());
		Statusbar_AddMessage("Error: Can't open audio input device. Recording silence.", 5000);
		return;
	}

	bSoundInputWorking  = true;
	bSoundInputWorking &= check_audio(request.freq,     granted.freq,     "freq");
	bSoundInputWorking &= check_audio(request.format,   granted.format,   "format");
	bSoundInputWorking &= check_audio(request.channels, granted.channels, "channels");
	bSoundInputWorking &= check_audio(request.samples,  granted.samples,  "samples");

	if (!bSoundInputWorking) {
		SDL_CloseAudioDevice(Audio_Input_Device);
		Audio_Input_Device = 0;
		Statusbar_AddMessage("Error: Can't open audio input device. Recording silence.", 5000);
	}
}

/*-----------------------------------------------------------------------*/
/**
 * Free audio subsystem
 */
void Audio_Output_UnInit(void) {
	if (bSoundOutputWorking) {
		/* Stop */
		Audio_Output_Enable(false);

		SDL_CloseAudioDevice(Audio_Output_Device);
		Audio_Output_Device = 0;

		bSoundOutputWorking = false;
	}
}

void Audio_Input_UnInit(void) {
	if (bSoundInputWorking) {
		/* Stop */
		Audio_Input_Enable(false);

		SDL_CloseAudioDevice(Audio_Input_Device);
		Audio_Input_Device = 0;

		bSoundInputWorking = false;
	}
}

/*-----------------------------------------------------------------------*/
/**
 * Start/Stop sound buffer
 */
void Audio_Output_Enable(bool bEnable) {
	if (bEnable && !bPlayingBuffer) {
		/* Start playing */
		SDL_PauseAudioDevice(Audio_Output_Device, false);
		bPlayingBuffer = true;
	}
	else if (!bEnable && bPlayingBuffer) {
		/* Stop from playing */
		SDL_PauseAudioDevice(Audio_Output_Device, true);
		bPlayingBuffer = false;
	}
}

void Audio_Input_Enable(bool bEnable) {
	if (bEnable && !bRecordingBuffer) {
		/* Start recording */
		Audio_Input_InitBuf();
		SDL_PauseAudioDevice(Audio_Input_Device, false);
		bRecordingBuffer = true;
	}
	else if (!bEnable && bRecordingBuffer) {
		/* Stop recording */
		SDL_PauseAudioDevice(Audio_Input_Device, true);
		bRecordingBuffer = false;
	}
}
