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


struct audio_t {
	uint8_t data[4096];
	int read;
	int size;
	int freq;
	int chan;
	SDL_AudioDeviceID* device;
	bool enabled;
};

static struct audio_t audio_playback;
static struct audio_t audio_recording;
static struct audio_t audio_dsp;

/*-----------------------------------------------------------------------*/
/**
 * Sound playback functions.
 */
void Audio_Output_Queue_Put(uint8_t* data, int len) {
	if (audio_playback.device && len > 0) {
		SDL_QueueAudio(audio_playback.device, data, len);
	}
}

int Audio_Output_Queue_Size(void) {
	if (audio_playback.device) {
		int size = SDL_GetQueuedAudioSize(audio_playback.device);
		if (size > 512 * 4) { /* Matches requested buffer size (request is in frames) */
			return size;
		}
	}
	return 0;
}

void Audio_Output_Queue_Flush(void) {
}

void Audio_Output_Queue_Clear(void) {
	if (audio_playback.device) {
		SDL_ClearQueuedAudio(audio_playback.device);
	}
}

/*-----------------------------------------------------------------------*/
/**
 * Sound recording functions.
 */
static int Audio_Data_Get(struct audio_t* audio, int16_t* sample) {
	if (audio->device) {
		if (audio->read >= audio->size) { /* Try to re-fill buffer in case it is empty. */
			audio->read = 0;
			audio->size = SDL_DequeueAudio(audio->device, audio->data, sizeof(audio->data));
			if (audio->size & 1) {
				Log_Printf(LOG_WARN, "[Audio] Recorded data has invalid size (%d).", audio->size);
				audio->size--;
			}
		}
		if (audio->read < audio->size) {
			*sample = (((uint16_t)audio->data[audio->read] << 8) | audio->data[audio->read + 1]);
			audio->read += 2;
		} else {
			return -1;
		}
	} else {
		*sample = 0; /* silence */
	}
	return 0;
}

int Audio_Input_Buffer_Size(void) {
	if (audio_recording.device) {
		return SDL_GetQueuedAudioSize(audio_recording.device);
	}
	return 0;
}

int Audio_Input_Buffer_Get(int16_t* sample) {
	return Audio_Data_Get(&audio_recording, sample);
}

int Audio_DSP_Buffer_Get(int16_t* sample) {
	return Audio_Data_Get(&audio_dsp, sample);
}

/*-----------------------------------------------------------------------*/
/**
 * Start/Stop playback and recording.
 */
static void Audio_Init_Data(struct audio_t* audio, int init) {
	Log_Printf(LOG_WARN, "[Audio] Initialising buffer with %d samples of silence.", init / (2 * audio->chan));
	/* Initialise buffer with silence to compensate for time gap between
	 * Audio_Input_Enable() and first availability of recorded data. */
	audio->read = 0;
	audio->size = init;
	memset(audio->data, 0, audio->size);
}

static void Audio_Enable(struct audio_t* audio, bool bEnable) {
	if (audio->device) {
		if (bEnable && SDL_GetAudioDeviceStatus(audio->device) == SDL_AUDIO_PAUSED) {
			/* Start */
			Audio_Init_Data(audio, 32);
			SDL_PauseAudioDevice(audio->device, 0);
		} else if (!bEnable && SDL_GetAudioDeviceStatus(audio->device) == SDL_AUDIO_PLAYING) {
			/* Stop */
			SDL_PauseAudioDevice(audio->device, 1);
		}
	}
	audio->enabled = bEnable;
}

void Audio_Output_Enable(bool bEnable) {
	Audio_Enable(&audio_playback, bEnable);
}

/*-----------------------------------------------------------------------*/
/**
 * Initialise the audio subsystem.
 */
static void Audio_Open(struct audio_t* audio, int iscapture, int channels, int freq) {
	if (audio->device == 0) {
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
		audio->device = SDL_OpenAudioDevice(NULL, iscapture, &request, NULL, 0);
		if (audio->device == 0) {
			Log_Printf(LOG_WARN, "[Audio] Could not open audio device: %s", SDL_GetError());
			Statusbar_AddMessage("Error: Can't open audio output device. No sound.", 5000);
		}
	}
	audio->chan = channels;
	audio->freq = freq;
}

void Audio_Output_Init(int channels, int freq) {
	Audio_Open(&audio_playback, 0, channels, freq);
}

void Audio_Input_InitAndEnable(int channels, int freq) {
	Audio_Open(&audio_recording, 1, channels, freq);
	Audio_Enable(&audio_recording, true);
}

void Audio_DSP_InitAndEnable(int channels, int freq) {
	Audio_Open(&audio_dsp, 1, channels, freq);
	Audio_Enable(&audio_dsp, true);
}

/*-----------------------------------------------------------------------*/
/**
 * Free the audio subsystem.
 */
static void Audio_Close(struct audio_t* audio) {
	if (audio->device) {
		/* Stop and close audio stream */
		SDL_CloseAudioDevice(audio->device);
	}
	memset(audio, 0, sizeof(struct audio_t));
}

void Audio_Output_UnInit(void) {
	Audio_Close(&audio_playback);
}

void Audio_Input_UnInit(void) {
	Audio_Close(&audio_recording);
}

void Audio_DSP_UnInit(void) {
	Audio_Close(&audio_dsp);
}

/*-----------------------------------------------------------------------*/
/**
 * Handle audio device connect and disconnect.
 */
static void Audio_Handle_Connect(struct audio_t* audio, SDL_AudioDeviceID dev) {
	if (audio->freq > 0 && audio->device == 0) {
		Audio_Open(audio, dev, audio->chan, audio->freq);
		Audio_Enable(audio, audio->enabled);
	}
}

static void Audio_Handle_Disconnect(struct audio_t* audio) {
	if (audio->freq > 0 && SDL_GetAudioDeviceStatus(audio->device) == SDL_AUDIO_STOPPED) {
		SDL_CloseAudioDevice(audio->device);
		audio->device = 0;
	}
}

void Audio_DeviceConnected(bool recording) {
	if (recording) {
		Audio_Handle_Connect(&audio_recording, 1);
		Audio_Handle_Connect(&audio_dsp, 1);
	} else {
		Audio_Handle_Connect(&audio_playback, 0);
	}
}

void Audio_DeviceDisconnected(bool recording) {
	if (recording) {
		Audio_Handle_Disconnect(&audio_recording);
		Audio_Handle_Disconnect(&audio_dsp);
	} else {
		Audio_Handle_Disconnect(&audio_playback);
	}
}
