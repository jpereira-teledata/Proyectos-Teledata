/*
 * ESPRESSIF MIT License
 *
 * Copyright (c) 2020 <ESPRESSIF SYSTEMS (SHANGHAI) CO., LTD>
 *
 * Permission is hereby granted for use on all ESPRESSIF SYSTEMS products, in which case,
 * it is free of charge, to any person obtaining a copy of this software and associated
 * documentation files (the "Software"), to deal in the Software without restriction, including
 * without limitation the rights to use, copy, modify, merge, publish, distribute, sublicense,
 * and/or sell copies of the Software, and to permit persons to whom the Software is furnished
 * to do so, subject to the following conditions:
 *
 * The above copyright notice and this permission notice shall be included in all copies or
 * substantial portions of the Software.
 *
 * THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
 * IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY, FITNESS
 * FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL THE AUTHORS OR
 * COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER LIABILITY, WHETHER
 * IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM, OUT OF OR IN
 * CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN THE SOFTWARE.
 *
 */

#ifndef _AUDIO_BOARD_DEFINITION_H_
#define _AUDIO_BOARD_DEFINITION_H_

#define I2S_STREAM_INTERNAL_ADC_CFG() {                                 		    \
    .type = AUDIO_STREAM_READER,                                                \
    .i2s_config = {                                                             \
        .mode = I2S_MODE_MASTER | I2S_MODE_RX | I2S_MODE_ADC_BUILT_IN,          \
        .sample_rate = 48000,                                                   \
        .bits_per_sample = I2S_BITS_PER_SAMPLE_16BIT,                           \
        .channel_format = I2S_CHANNEL_FMT_ONLY_LEFT,                           \
        .communication_format = I2S_COMM_FORMAT_I2S_MSB,                        \
        .intr_alloc_flags = ESP_INTR_FLAG_LEVEL2 | ESP_INTR_FLAG_IRAM,          \
        .dma_buf_count = 3,                                                     \
        .dma_buf_len = 300,                                                     \
        .use_apll = false,                                                      \
        .tx_desc_auto_clear = true,                                             \
        .fixed_mclk = 0                                                         \
    },                                                                          \
    .i2s_port = I2S_NUM_0,                                                      \
    .use_alc = false,                                                           \
    .volume = 0,                                                                \
    .out_rb_size = I2S_STREAM_RINGBUFFER_SIZE,                                  \
    .task_stack = I2S_STREAM_TASK_STACK,                                        \
    .task_core = I2S_STREAM_TASK_CORE,                                          \
    .task_prio = I2S_STREAM_TASK_PRIO,                                          \
    .stack_in_ext = false,                                                      \
    .multi_out_num = 0,                                                         \
    .uninstall_drv = true,                                                   	  \
}

#define ADC_UNIT		  ADC_UNIT_1
#define ADC_CHANNEL 	ADC1_CHANNEL_0 	// ADC1 channel 0 is GPIO36
#define ADC_ATTEN 		ADC_ATTEN_DB_11

/*
 *        When VDD_A is 3.3 V:
 *
 *        - 0 dB attenuation (ADC_ATTEN_DB_0) gives full-scale voltage 1.1 V
 *        - 2.5 dB attenuation (ADC_ATTEN_DB_2_5) gives full-scale voltage 1.5 V
 *        - 6 dB attenuation (ADC_ATTEN_DB_6) gives full-scale voltage 2.2 V
 *        - 11 dB attenuation (ADC_ATTEN_DB_11) gives full-scale voltage 3.9 V
 */

#define I2S_STREAM_CFG() {                                              		    \
    .type = AUDIO_STREAM_WRITER,                                                \
    .i2s_config = {                                                             \
        .mode = I2S_MODE_MASTER | I2S_MODE_TX,                    				      \
        .sample_rate = 48000,                                                   \
        .bits_per_sample = I2S_BITS_PER_SAMPLE_16BIT,                           \
        .channel_format = I2S_CHANNEL_FMT_RIGHT_LEFT,                           \
        .communication_format = I2S_COMM_FORMAT_I2S_MSB,                        \
        .intr_alloc_flags = ESP_INTR_FLAG_LEVEL2 | ESP_INTR_FLAG_IRAM,          \
        .dma_buf_count = 3,                                                     \
        .dma_buf_len = 300,                                                     \
        .use_apll = false,                                                      \
        .tx_desc_auto_clear = true,                                             \
        .fixed_mclk = 0                                                         \
    },                                                                          \
    .i2s_port = I2S_NUM_1,                                                      \
    .use_alc = false,                                                           \
    .volume = 0,                                                                \
    .out_rb_size = I2S_STREAM_RINGBUFFER_SIZE,                                  \
    .task_stack = I2S_STREAM_TASK_STACK,                                        \
    .task_core = I2S_STREAM_TASK_CORE,                                          \
    .task_prio = I2S_STREAM_TASK_PRIO,                                          \
    .stack_in_ext = false,                                                      \
    .multi_out_num = 0,                                                         \
    .uninstall_drv = true,                                                      \
}

#endif
