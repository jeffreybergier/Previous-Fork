/*
  Previous - snd.c

  This file is distributed under the GNU General Public License, version 2
  or at your option any later version. Read the file gpl.txt for details.

  This file contains a simulation of Soundbox sound processing and I/O. 
*/
const char Snd_fileid[] = "Previous snd.c";

#include "main.h"
#include "configuration.h"
#include "m68000.h"
#include "cycInt.h"
#include "audio.h"
#include "grab.h"
#include "snd.h"
#include "kms.h"
#include "dsp.h"

#define LOG_SND_LEVEL   LOG_DEBUG
#define LOG_VOL_LEVEL   LOG_DEBUG


uint8_t snd_buffer[SND_BUFFER_SIZE];
int     snd_buffer_len = 0;

static struct {
    double volume[2]; /* 0 = left, 1 = right */
    
    uint8_t mode;
    uint8_t mute;
    uint8_t deemph;
    uint8_t attenuation[2];
} sndout_state;

/* Maximum volume (really is attenuation) */
#define SND_MAX_VOL 43


/* This functions generates 8-bit ulaw samples from 16 bit pcm audio */
#define BIAS 0x84       /* define the add-in bias for 16 bit samples */
#define CLIP 32635

static const int16_t exp_lut[256] = {
    0, 0, 1, 1, 2, 2, 2, 2, 3, 3, 3, 3, 3, 3, 3, 3,
    4, 4, 4, 4, 4, 4, 4, 4, 4, 4, 4, 4, 4, 4, 4, 4,
    5, 5, 5, 5, 5, 5, 5, 5, 5, 5, 5, 5, 5, 5, 5, 5,
    5, 5, 5, 5, 5, 5, 5, 5, 5, 5, 5, 5, 5, 5, 5, 5,
    6, 6, 6, 6, 6, 6, 6, 6, 6, 6, 6, 6, 6, 6, 6, 6,
    6, 6, 6, 6, 6, 6, 6, 6, 6, 6, 6, 6, 6, 6, 6, 6,
    6, 6, 6, 6, 6, 6, 6, 6, 6, 6, 6, 6, 6, 6, 6, 6,
    6, 6, 6, 6, 6, 6, 6, 6, 6, 6, 6, 6, 6, 6, 6, 6,
    7, 7, 7, 7, 7, 7, 7, 7, 7, 7, 7, 7, 7, 7, 7, 7,
    7, 7, 7, 7, 7, 7, 7, 7, 7, 7, 7, 7, 7, 7, 7, 7,
    7, 7, 7, 7, 7, 7, 7, 7, 7, 7, 7, 7, 7, 7, 7, 7,
    7, 7, 7, 7, 7, 7, 7, 7, 7, 7, 7, 7, 7, 7, 7, 7,
    7, 7, 7, 7, 7, 7, 7, 7, 7, 7, 7, 7, 7, 7, 7, 7,
    7, 7, 7, 7, 7, 7, 7, 7, 7, 7, 7, 7, 7, 7, 7, 7,
    7, 7, 7, 7, 7, 7, 7, 7, 7, 7, 7, 7, 7, 7, 7, 7,
    7, 7, 7, 7, 7, 7, 7, 7, 7, 7, 7, 7, 7, 7, 7, 7
};

static uint8_t snd_make_ulaw(int16_t sample) {
    int16_t sign, exponent, mantissa;
    uint8_t ulawbyte;
    
    /** get the sample into sign-magnitude **/
    sign = (sample >> 8) & 0x80;        /* set aside the sign */
    if (sign != 0) {
        sample = -sample;         /* get magnitude */
    }
    /* sample can be zero because we can overflow in the inversion,
     * checking against the unsigned version solves this */
    if (((uint16_t) sample) > CLIP)
        sample = CLIP;            /* clip the magnitude */
    
    /** convert from 16 bit linear to ulaw **/
    sample = sample + BIAS;
    exponent = exp_lut[(sample >> 7) & 0xFF];
    mantissa = (sample >> (exponent + 3)) & 0x0F;
    ulawbyte = ~(sign | (exponent << 4) | mantissa);
    
    return ulawbyte;
}

/* This function performs two times upsampling using repeat or zero-fill */
static void snd_make_double_samples(uint8_t *buffer, int len, bool repeat) {
    uint8_t* src = buffer + len;
    uint8_t* dst = buffer + len * 2;
    assert((len & 3) == 0);
    for (;;) {
        src -= 4;
        dst -= 4;
        if (repeat) { /* repeat */
            memcpy(dst, src, 4);
        } else {   /* zero-fill */
            memset(dst, 0, 4);
        }
        dst -= 4;
        if (src >= dst) {
            break;
        }
        memcpy(dst, src, 4);
    }
}

/* This is a de-emphasis filter for 44.1 kHz pre-emphasised CD audio input */
static struct deemph_t {
    double li[2];
    double lo[2];
} deemph;

static void snd_deemphasis_start(void) {
    deemph.li[0] = deemph.li[1] = 0.0;
    deemph.lo[0] = deemph.lo[1] = 0.0;
}

static double snd_deemphasis_filter(double i, int c) {
    /* Coefficients have been calculated as follows:
     *   T = 1./44100.
     *   V0 = 0.3365
     *   OmegaU = 1./19E-6
     *   B = V0*tan(OmegaU*T/2.)
     *   a1 = (B-1.)/(B+1.)
     *   b0 = (1.+(1.-a1)*(V0-1.)/2.)
     *   b1 = (a1+(a1-1.)*(V0-1.)/2.)
     */
    static const double a1 = -0.62786881719628784282;
    static const double b0 =  0.45995451989513153057;
    static const double b1 = -0.08782333709141937339;
    
    double o = i * b0 + deemph.li[c] * b1 - deemph.lo[c] * a1;
    
    deemph.li[c] = i;
    deemph.lo[c] = o;
    
    return o;
}

/* This function returns a factor for adding volume adjustment to samples */
static double snd_get_volume_factor(uint8_t vol_data) {
    double gain = (double)vol_data * -2.0;
    
    switch (vol_data) {
        case 0:           return 1.0;
        case SND_MAX_VOL: return 0.0;
        default:          return pow(10.0, gain*0.05);
    }
}

/* This function adjusts sound output volume */
static void snd_adjust_volume_and_lowpass(uint8_t *buf, int len) {
    if (sndout_state.mute) {
        memset(buf, 0, len);
    } else if (sndout_state.attenuation[0] || sndout_state.attenuation[1] || sndout_state.deemph) {
        int i;
        long lsample, rsample;
        double ldata, rdata;
        
        for (i = 0; i < len; i += 4) {
            ldata = (double)(int16_t)((buf[i + 0] << 8) | buf[i + 1]);
            rdata = (double)(int16_t)((buf[i + 2] << 8) | buf[i + 3]);
            if (sndout_state.deemph) {
                ldata = snd_deemphasis_filter(ldata, 0);
                rdata = snd_deemphasis_filter(rdata, 1);
            }
            ldata *= sndout_state.volume[0];
            rdata *= sndout_state.volume[1];
            
            lsample = (ldata < 0.0) ? (long)(ldata - 0.5) : (long)(ldata + 0.5);
            rsample = (rdata < 0.0) ? (long)(rdata - 0.5) : (long)(rdata + 0.5);

            if      (lsample > INT16_MAX) lsample = INT16_MAX;
            else if (lsample < INT16_MIN) lsample = INT16_MIN;
            if      (rsample > INT16_MAX) rsample = INT16_MAX;
            else if (rsample < INT16_MIN) rsample = INT16_MIN;

            buf[i + 0] = lsample >> 8;
            buf[i + 1] = lsample;
            buf[i + 2] = rsample >> 8;
            buf[i + 3] = rsample;
        }
    }
}

/* This function processes and sends multiple samples */

/* Valid modes */
#define SND_MODE_NORMAL 0x00
#define SND_MODE_DBL_RP 0x10
#define SND_MODE_DBL_ZF 0x30

static int snd_send_samples(uint8_t* buffer, int len) {
    switch (sndout_state.mode) {
        case SND_MODE_NORMAL:
            break;
        case SND_MODE_DBL_RP:
            snd_make_double_samples(buffer, len, true);
            len *= 2;
            break;
        case SND_MODE_DBL_ZF:
            snd_make_double_samples(buffer, len, false);
            len *= 2;
            break;
        default:
            Log_Printf(LOG_WARN, "[Sound] Error: Unknown sound output mode!");
            return 0;
    }
    snd_adjust_volume_and_lowpass(buffer, len);
    Grab_Sound(buffer, len);
    Audio_Output_Queue_Put(buffer, len);
    return len;
}

/* This function processes and sends one single sample */
void snd_send_sample(uint32_t data) {
    uint8_t buf[8];
    
    if (!snd_output_active())
        return;
    
    buf[0] = data << 24;
    buf[1] = data << 16;
    buf[2] = data << 8;
    buf[3] = data;
    
    snd_send_samples(buf, 4);
}


/* Internal volume control register access (shifted in left to right)
 *
 * xxx ---- ----  start bits (all 1)
 * --- xx-- ----  channel (0x80 = right, 0x40 = left)
 * --- --xx xxxx  volume
 */
static uint16_t tmp_vol = 0;
static int      bit_num = 0;

static void snd_shift_volume_reg(uint8_t databit) {
    Log_Printf(LOG_VOL_LEVEL, "[Sound] Interface shift bit %i (%i).",bit_num,databit?1:0);
    
    tmp_vol <<= 1;
    tmp_vol |= (databit?1:0);
    
    bit_num++;
}

static void snd_volume_interface_reset(void) {
    Log_Printf(LOG_VOL_LEVEL, "[Sound] Interface reset.");
    
    bit_num = 0;
    tmp_vol = 0;
}

static void snd_save_volume_reg(void) {
    uint8_t chan_lr, vol_data;
    
    if (bit_num!=11 || ((tmp_vol>>8)&7)!=7) {
        Log_Printf(LOG_WARN, "[Sound] Bad volume transfer (%i bits, start %i).",bit_num,(tmp_vol>>8)&7);
        return;
    }
    chan_lr = (tmp_vol&0xC0)>>6;
    vol_data = tmp_vol&0x3F;
    
    if (vol_data > SND_MAX_VOL) {
        Log_Printf(LOG_WARN, "[Sound] Volume data limit exceeded (%d).", vol_data);
        vol_data = SND_MAX_VOL;
    }
    if (chan_lr & 1) {
        Log_Printf(LOG_WARN, "[Sound] Setting gain of left channel to %d dB", vol_data * -2);
        sndout_state.attenuation[0] = vol_data;
        sndout_state.volume[0] = snd_get_volume_factor(vol_data);
    }
    if (chan_lr & 2) {
        Log_Printf(LOG_WARN, "[Sound] Setting gain of right channel to %d dB", vol_data * -2);
        sndout_state.attenuation[1] = vol_data;
        sndout_state.volume[1] = snd_get_volume_factor(vol_data);
    }
}

void snd_vol_access(uint8_t data) {
    Log_Printf(LOG_VOL_LEVEL, "[Sound] Volume access: %02X",data);
    
    bit_num = 11;
    tmp_vol = 0x700 | data;
    snd_save_volume_reg();
    snd_volume_interface_reset();
}

/* This function fills the internal volume register */
#define SND_SPEAKER_ENABLE  0x10
#define SND_LOWPASS_ENABLE  0x08

#define SND_INTFC_CLOCK     0x04
#define SND_INTFC_DATA      0x02
#define SND_INTFC_STROBE    0x01

void snd_gpo_access(uint8_t data) {
    static uint8_t old_data = 0;
    
    Log_Printf(LOG_VOL_LEVEL, "[Sound] Control logic access: %02X",data);
    
    sndout_state.mute = data&SND_SPEAKER_ENABLE;
    sndout_state.deemph = data&SND_LOWPASS_ENABLE;
    
    if (data&SND_INTFC_STROBE) {
        snd_save_volume_reg();
    } else if (old_data&SND_INTFC_STROBE) {
        snd_volume_interface_reset();
    } else if ((data&SND_INTFC_CLOCK) && !(old_data&SND_INTFC_CLOCK)) {
        snd_shift_volume_reg(data&SND_INTFC_DATA);
    }
    old_data = data;
}


/* Initialise and uninitialise the audio system */
static bool sndout_inited       = false;
static bool sndin_inited        = false;
static bool snddsp_inited       = false;
static bool sound_output_active = false;
static bool sound_input_active  = false;
static bool sound_dsp_active    = false;

static void sound_unpause(void) {
    if (!sndout_inited && ConfigureParams.Sound.bEnableSound) {
        Log_Printf(LOG_WARN, "[Sound] Initialising output device.");
        Audio_Output_Init(2, SOUND_OUT_FREQUENCY);
        sndout_inited = true;
    }
    if (sound_input_active && !sndin_inited && ConfigureParams.Sound.bEnableSound) {
        Log_Printf(LOG_WARN, "[Sound] Initialising input device.");
        Audio_Input_Init(1, SOUND_IN_FREQUENCY);
        sndin_inited = true;
    }
    if (sound_dsp_active && !snddsp_inited && ConfigureParams.Sound.bEnableSound) {
        Log_Printf(LOG_WARN, "[Sound] Initialising DSP input device.");
        Audio_DSP_Init(2, SOUND_OUT_FREQUENCY);
        snddsp_inited = true;
    }
    if (sound_output_active && sndout_inited) {
        Log_Printf(LOG_WARN, "[Sound] Starting output.");
        Audio_Output_Enable(true);
    }
    if (sound_input_active && sndin_inited) {
        Log_Printf(LOG_WARN, "[Sound] Starting input.");
        Audio_Input_Enable(true);
    }
    if (sound_dsp_active && snddsp_inited) {
        Log_Printf(LOG_WARN, "[Sound] Starting DSP input.");
        Audio_DSP_Enable(true);
    }
}

static void sound_pause(void) {
    if (sndout_inited) {
        Log_Printf(LOG_WARN, "[Sound] Uninitializing output device.");
        sndout_inited = false;
        Audio_Output_UnInit();
    }
    if (sndin_inited) {
        Log_Printf(LOG_WARN, "[Sound] Uninitializing input device.");
        sndin_inited = false;
        Audio_Input_UnInit();
    }
    if (snddsp_inited) {
        Log_Printf(LOG_WARN, "[Sound] Uninitializing DSP input device.");
        snddsp_inited = false;
        Audio_DSP_UnInit();
    }
}


/* Start and stop sound output */
void snd_start_output(uint8_t mode) {
    sndout_state.mode = mode;
    /* Starting host audio playback */
    if (sndout_inited) {
        Audio_Output_Enable(true);
    } else {
        Log_Printf(LOG_SND_LEVEL, "[Sound] Not starting. Sound output device not initialised.");
    }
    /* Starting sound output loop */
    if (!sound_output_active) {
        Log_Printf(LOG_SND_LEVEL, "[Sound] Starting output loop.");
        snd_deemphasis_start();
        sound_output_active = true;
        Audio_Output_Queue_Clear();
    } else { /* Even re-enable loop if we are already active. This lowers the delay. */
        Log_Printf(LOG_DEBUG, "[Sound] Restarting output loop.");
    }
    CycInt_AddTimeEvent(1, 0, EVENT_SND_OUTPUT);
}

void snd_stop_output(void) {
    sound_output_active=false;
}

/* Start and stop sound input */
void snd_start_input(void) {
    /* Starting host audio recording */
    if (sndin_inited) {
        Audio_Input_Enable(true);
    } else if (ConfigureParams.Sound.bEnableSound) {
        sndin_inited = true;
        Audio_Input_Init(1, SOUND_IN_FREQUENCY);
        Audio_Input_Enable(true);
    }
    /* Starting sound input loop */
    if (!sound_input_active) {
        Log_Printf(LOG_SND_LEVEL, "[Sound] Starting input loop.");
        sound_input_active = true;
    } else { /* Even re-enable loop if we are already active. This lowers the delay. */
        Log_Printf(LOG_DEBUG, "[Sound] Restarting input loop.");
    }
    CycInt_AddTimeEvent(1, 0, EVENT_SND_INPUT);
}

void snd_stop_input(void) {
    sound_input_active = false;
    sndin_inited = false;
    Audio_Input_UnInit();
}

/* Start and stop sound input throgh DSP */
void snd_dsp_start(void) {
    if (!snddsp_inited) {
        snddsp_inited = true;
        sound_dsp_active = true;
        Audio_DSP_Init(2, SOUND_OUT_FREQUENCY);
        Audio_DSP_Enable(true);
        CycInt_AddCycleTimeEvent(5, 0, EVENT_SND_DSP_INPUT);
    }
}

void snd_dsp_stop(void) {
    snddsp_inited = false;
    sound_dsp_active = false;
    Audio_DSP_Enable(false);
    Audio_DSP_UnInit();
}


/* Return sound state */
bool snd_output_active(void) {
    return sound_output_active;
}

bool snd_input_active(void) {
    return sound_input_active;
}


/* Reset and pause sound */
void Sound_Reset(void) {
    sndout_state.volume[0] = snd_get_volume_factor(sndout_state.attenuation[0]);
    sndout_state.volume[1] = snd_get_volume_factor(sndout_state.attenuation[1]);
    snd_buffer_len = 0;
}

void Sound_Pause(bool pause) {
    if (pause) {
        sound_pause();
    } else {
        sound_unpause();
    }
}


/* Sound IO loops */

/*
  At a playback rate of 44.1kHz a sample takes about 23 microseconds.
  Assuming that the emulation runs at least 1/3 as fast as a real m68k
  checking the sound queue every 8 microseconds should be ok.
 */
#define SND_CHECK_DELAY 8

void SND_Out_Handler(void) {
    if (!sound_output_active) {
        Audio_Output_Queue_Flush();
        return;
    }
    
    if (sndout_inited && Audio_Output_Queue_Size() > SOUND_BUFFER_SAMPLES * 2) {
        CycInt_UpdateTimeEvent(SND_CHECK_DELAY * SOUND_BUFFER_SAMPLES, 0, EVENT_SND_OUTPUT);
        return;
    }
    
    kms_send_sndout_request();
    
    if (snd_buffer_len) {
        snd_buffer_len = snd_send_samples(snd_buffer, snd_buffer_len);
        CycInt_UpdateTimeEvent(SND_CHECK_DELAY * (snd_buffer_len >> 2), 0, EVENT_SND_OUTPUT);
    } else {
        kms_send_sndout_underrun();
        /* Call do_dma_sndout_intr() a little bit later */
        CycInt_UpdateTimeEvent(100, 0, EVENT_SND_OUTPUT);
    }
}

/*
  Sound is recorded at 8012 Hz. One sample (byte) takes about 124 microseconds.
 */
#define SNDIN_SAMPLE_TIME 124

void SND_In_Handler(void) {
    int16_t sample;
    uint32_t foursamples = 0;
    int size = 0;
    
    if (!sound_input_active) {
        return;
    }
    if (!kms_can_receive_codec()) {
        return;
    }
    
    /* Process 256 samples at a time and then sync */
    while (size<256) {
        if (Audio_Input_Buffer_Get(&sample) < 0) {
            Log_Printf(LOG_WARN, "[Sound] Waiting for sound input data");
            size = 256;
            break;
        }
        
        /* Shift in sample (oldest first) */
        foursamples = (foursamples<<8) | snd_make_ulaw(sample);
        
        size++;
        
        /* After accumulating 4 samples, send them to KMS */
        if ((size&3)==0) {
            if (kms_send_codec_receive(foursamples)) {
                break;
            }
        }
    }
    
    /* If we accumulated too much data write it fast */
    if (Audio_Input_Buffer_Size() > 8192) { /* this is 4096 ulaw samples equaling about 0.5 seconds */
        Log_Printf(LOG_WARN, "[Sound] Writing input data fast");
        size = 16; /* Short delay */
    }
    
    if (kms_can_receive_codec()) {
        CycInt_UpdateTimeEvent(size*SNDIN_SAMPLE_TIME, 0, EVENT_SND_INPUT);
    }
}

/*
 Sound can also be recorded at 44,1 kHz through the SSI port of the DSP.
 */
void SND_DSP_Handler(void) {
    int16_t sample;
    
    if (!sound_dsp_active) {
        return;
    }
    if (Audio_DSP_Buffer_Get(&sample) == 0) {
        DSP_SsiWriteRxValue(sample);
        DSP_SsiReceive_SC0();
    }
    CycInt_UpdateCycleTimeEvent(5, 0, EVENT_SND_DSP_INPUT);
}
