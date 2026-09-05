#ifndef BOARDS_JIELI_AC791N_DEVKIT_AC791N_LAYOUTS_H2LOADER_APP_CONFIG_H_
#define BOARDS_JIELI_AC791N_DEVKIT_AC791N_LAYOUTS_H2LOADER_APP_CONFIG_H_

#define __SDRAM_SIZE__ (8 * 1024 * 1024)
#define CONFIG_JLFAT_ENABLE 1
/* TIMER5 is exclusively owned by the shared PAL monotonic clock. */
#define H2_JIELI_CLOCK_TIMER5 1

/* One console for all roles: adapter RX <- PB3, adapter TX -> PA6, 8N1. */
#define CONFIG_H2_UART1_DEBUG_ENABLE
#define H2_JIELI_CONSOLE_BAUD 460800
/* Keep USB update code linked for the vendor dual-bank library, but do not
 * auto-start CDC when an OTG cable is detected. UART1 owns both Loader
 * transport and logs in this layout. */
#define USB_PC_NO_APP_MODE 0

#define CONFIG_IIC_ENABLE 1
#define CONFIG_AUDIO_ENABLE 1
#define CONFIG_AUDIO_MIX_ENABLE 1
#define CONFIG_AEC_ENC_ENABLE 1
#define CONFIG_PCM_DEC_ENABLE 1
#define CONFIG_PCM_ENC_ENABLE 1
#define AUDIO_ENC_SAMPLE_SOURCE_MIC 0
#define CONFIG_AUDIO_ENC_SAMPLE_SOURCE AUDIO_ENC_SAMPLE_SOURCE_MIC
#define CONFIG_AUDIO_DEC_PLAY_SOURCE "dac"
#define CONFIG_AUDIO_ADC_CHANNEL_L 1
#define CONFIG_AUDIO_ADC_GAIN 100
#define TCFG_DAC_MUTE_PORT IO_PORTB_02
#define TCFG_DAC_MUTE_VALUE 0

#include "../../../include/h2_jieli_ac791n_devkit_sdk_config.h"

#endif
