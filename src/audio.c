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


static SDL_AudioStream *Audio_Input_Stream  = NULL;
static SDL_AudioStream *Audio_Output_Stream = NULL;

static bool            bSoundOutputWorking  = false; /* Is sound output OK */
static bool            bSoundInputWorking   = false; /* Is sound input OK */
static bool            bPlayingBuffer       = false; /* Is playing buffer? */
static bool            bRecordingBuffer     = false; /* Is recording buffer? */
static const uint32_t  REC_BUFFER_SIZE      = 1<<16; /* Recording buffer size in power of two */
static const uint32_t  REC_BUFFER_MASK      = REC_BUFFER_SIZE-1;
static uint8_t         recBuffer[REC_BUFFER_SIZE];
static uint32_t        recBufferWr          = 0;
static uint32_t        recBufferRd          = 0;
static lock_t          recBufferLock;

void Audio_Output_Queue(uint8_t* data, int len) {
	if (len > 0) {
		Grab_Sound(data, len);
		if (bSoundOutputWorking) {
			SDL_PutAudioStreamData(Audio_Output_Stream, data, len);
		}
	}
}

uint32_t Audio_Output_Queue_Size(void) {
	if (bSoundOutputWorking) {
		return SDL_GetAudioStreamQueued(Audio_Output_Stream) / 4;
	} else {
		return 0;
	}
}

void Audio_Output_Queue_Clear(void) {
	if (bSoundOutputWorking) {
		SDL_ClearAudioStream(Audio_Output_Stream);
	}
}

/*-----------------------------------------------------------------------*/
/**
 * SDL audio callback functions - move sound between emulation and audio system.
 * Note: These functions will run in a separate thread.
 */

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

static void Audio_New_Input_CallBack(void *userdata, SDL_AudioStream *stream, int additional_amount, int total_amount) {
	if (additional_amount > 0) {
		uint8_t *data = SDL_stack_alloc(uint8_t, additional_amount);
		if (data) {
			SDL_GetAudioStreamData(stream, data, additional_amount);
			Audio_Input_CallBack(userdata, data, additional_amount);
			SDL_stack_free(data);
		}
	}
}

void Audio_Input_Lock(void) {
	host_lock(&recBufferLock);
}

/* 
 * Initialize recording buffer with silence to compensate for time gap
 * between Audio_Input_Enable and first call of Audio_Input_CallBack.
 */
#define AUDIO_RECBUF_INIT 32 /* 16000 byte = 1 second */

static void Audio_Input_InitBuf(void) {
	recBufferRd = 0;
	Log_Printf(LOG_WARN, "[Audio] Initializing input buffer with %d ms of silence.", AUDIO_RECBUF_INIT>>4);
	for (recBufferWr = 0; recBufferWr < AUDIO_RECBUF_INIT; recBufferWr++) {
		recBuffer[recBufferWr] = 0;
	}
}

int Audio_Input_BufSize(void) {
	if (bSoundInputWorking) {
		if (recBufferRd <= recBufferWr) {
			return recBufferWr - recBufferRd;
		} else {
			return REC_BUFFER_SIZE - (recBufferRd - recBufferWr);
		}
	} else {
		return 0;
	}
}

int Audio_Input_Read(int16_t* sample) {
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
	SDL_AudioSpec request = {SDL_AUDIO_S16BE, 2, SOUND_OUT_FREQUENCY};
	SDL_AudioSpec granted = request;

	/* Init the SDL's audio subsystem: */
	if (SDL_WasInit(SDL_INIT_AUDIO) == 0) {
		if (SDL_InitSubSystem(SDL_INIT_AUDIO) < 0) {
			Log_Printf(LOG_WARN, "[Audio] Could not init audio output: %s\n", SDL_GetError());
			Statusbar_AddMessage("Error: Can't open SDL audio subsystem.", 5000);
			bSoundOutputWorking = false;
			return;
		}
	}

	if (Audio_Output_Stream == NULL) {
		Audio_Output_Stream = SDL_OpenAudioDeviceStream(SDL_AUDIO_DEVICE_DEFAULT_OUTPUT, &request, NULL, NULL);
	}
	if (Audio_Output_Stream == NULL) {
		Log_Printf(LOG_WARN, "[Audio] Could not open audio output device: %s\n", SDL_GetError());
		Statusbar_AddMessage("Error: Can't open audio output device. No sound output.", 5000);
		bSoundOutputWorking = false;
		return;
	}
	bSoundOutputWorking  = true;
	bSoundOutputWorking &= check_audio(request.freq,     granted.freq,     "freq");
	bSoundOutputWorking &= check_audio(request.format,   granted.format,   "format");
	bSoundOutputWorking &= check_audio(request.channels, granted.channels, "channels");

	if (!bSoundOutputWorking) {
		SDL_DestroyAudioStream(Audio_Output_Stream);
		Audio_Output_Stream = NULL;
		Statusbar_AddMessage("Error: Can't open audio output device. No sound output.", 5000);
	}
}

void Audio_Input_Init(void) {
	SDL_AudioSpec request = {SDL_AUDIO_S16BE, 1, SOUND_IN_FREQUENCY};
	SDL_AudioSpec granted = request;

	/* Init the SDL's audio subsystem: */
	if (SDL_WasInit(SDL_INIT_AUDIO) == 0) {
		if (SDL_InitSubSystem(SDL_INIT_AUDIO) < 0) {
			Log_Printf(LOG_WARN, "[Audio] Could not init audio input: %s\n", SDL_GetError());
			Statusbar_AddMessage("Error: Can't open SDL audio subsystem.", 5000);
			bSoundInputWorking = false;
			return;
		}
	}

	if (Audio_Input_Stream == NULL) {
		Audio_Input_Stream = SDL_OpenAudioDeviceStream(SDL_AUDIO_DEVICE_DEFAULT_CAPTURE, &request, Audio_New_Input_CallBack, NULL);
	}
	if (Audio_Input_Stream == NULL) {
		Log_Printf(LOG_WARN, "[Audio] Could not open audio input device: %s\n", SDL_GetError());
		Statusbar_AddMessage("Error: Can't open audio input device. Recording silence.", 5000);
		bSoundInputWorking = false;
		return;
	}

	bSoundInputWorking  = true;
	bSoundInputWorking &= check_audio(request.freq,     granted.freq,     "freq");
	bSoundInputWorking &= check_audio(request.format,   granted.format,   "format");
	bSoundInputWorking &= check_audio(request.channels, granted.channels, "channels");

	if (!bSoundInputWorking) {
		SDL_DestroyAudioStream(Audio_Input_Stream);
		Audio_Input_Stream = NULL;
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

		SDL_DestroyAudioStream(Audio_Output_Stream);
		Audio_Output_Stream = NULL;

		bSoundOutputWorking = false;
	}
}

void Audio_Input_UnInit(void) {
	if (bSoundInputWorking) {
		/* Stop */
		Audio_Input_Enable(false);

		SDL_DestroyAudioStream(Audio_Input_Stream);
		Audio_Input_Stream = NULL;

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
		SDL_ResumeAudioDevice(SDL_GetAudioStreamDevice(Audio_Output_Stream));
		bPlayingBuffer = true;
	}
	else if (!bEnable && bPlayingBuffer) {
		/* Stop from playing */
		SDL_PauseAudioDevice(SDL_GetAudioStreamDevice(Audio_Output_Stream));
		bPlayingBuffer = false;
	}
}

void Audio_Input_Enable(bool bEnable) {
	if (bEnable && !bRecordingBuffer) {
		/* Start recording */
		Audio_Input_InitBuf();
		SDL_ResumeAudioDevice(SDL_GetAudioStreamDevice(Audio_Input_Stream));
		bRecordingBuffer = true;
	}
	else if (!bEnable && bRecordingBuffer) {
		/* Stop recording */
		SDL_PauseAudioDevice(SDL_GetAudioStreamDevice(Audio_Input_Stream));
		bRecordingBuffer = false;
	}
}
