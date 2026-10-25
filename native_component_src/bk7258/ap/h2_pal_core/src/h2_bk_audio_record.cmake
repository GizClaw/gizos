# Release the pinned backend's retained record_ctx at destroy, after successful
# close and after its owner has waited for all readers to leave.
# Transform only the invocation-local SDK input; the installed SDK is read-only.
file(READ "${H2_BK_AUDIO_RECORD_INPUT}" h2_record_source)
string(REPLACE "\r\n" "\n" h2_record_source "${h2_record_source}")
string(SHA256 h2_record_sha256 "${h2_record_source}")
file(READ "${H2_BK_AUDIO_MIC_INPUT}" h2_mic_source)
string(REPLACE "\r\n" "\n" h2_mic_source "${h2_mic_source}")
string(SHA256 h2_mic_sha256 "${h2_mic_source}")
if(NOT h2_record_sha256 STREQUAL "d2591276f1c0905e1c563dec7e1de98acfd764dcc86a2563d11f35bab0d3482f"
    OR NOT h2_mic_sha256 STREQUAL "462027de2487b6dc52f65454dd217b2900970ae1e7be285785612de506857082")
  message(FATAL_ERROR "Pinned BK audio record sources changed; revalidate context ownership")
endif()

set(h2_record_original [=[    ret = audio_record_close(record);
    if (ret != BK_OK)
    {
        LOGE("%s, %d, audio_record_close fail, ret: %d\n", __func__, __LINE__, ret);
    }

    os_free(record);]=])
set(h2_record_replacement [=[    ret = record->record_ctx != NULL ? audio_record_close(record) : BK_OK;
    if (ret != BK_OK)
    {
        LOGE("%s, %d, audio_record_close fail, ret: %d\n", __func__, __LINE__, ret);
        return ret;
    }

    os_free(record->record_ctx);
    record->record_ctx = NULL;
    os_free(record);]=])
string(REPLACE "${h2_record_original}" "" h2_record_without "${h2_record_source}")
string(LENGTH "${h2_record_source}" h2_record_before)
string(LENGTH "${h2_record_without}" h2_record_after)
string(LENGTH "${h2_record_original}" h2_record_width)
math(EXPR h2_record_removed "${h2_record_before} - ${h2_record_after}")
if(NOT h2_record_removed EQUAL h2_record_width)
  message(FATAL_ERROR "Expected one pinned BK record destructor")
endif()
string(REPLACE "${h2_record_original}" "${h2_record_replacement}" h2_record_source "${h2_record_source}")
file(WRITE "${H2_BK_AUDIO_RECORD_OUTPUT}" "${h2_record_source}")

# Wake an admitted blocking read while all of its buffers remain owned. The
# BSP can acknowledge that reader before close releases those buffers.
set(h2_mic_original [=[        ret = onboard_mic_stop(priv);
        if (ret != BK_OK)
        {
            LOGE("%s, onboard mic record pause fail, ret: %d, %d \n", __func__, ret, __LINE__);
        }
        onboard_mic_state_set(priv, AUDIO_RECORD_STA_PAUSED);]=])
set(h2_mic_replacement [=[        ret = onboard_mic_stop(priv);
        if (ret != BK_OK)
        {
            LOGE("%s, onboard mic record pause fail, ret: %d, %d \n", __func__, ret, __LINE__);
            return ret;
        }
        ret = rb_abort(priv->rb);
        if (ret != BK_OK) return ret;
        onboard_mic_state_set(priv, AUDIO_RECORD_STA_PAUSED);]=])
string(REPLACE "${h2_mic_original}" "" h2_mic_without "${h2_mic_source}")
string(LENGTH "${h2_mic_source}" h2_mic_before)
string(LENGTH "${h2_mic_without}" h2_mic_after)
string(LENGTH "${h2_mic_original}" h2_mic_width)
math(EXPR h2_mic_removed "${h2_mic_before} - ${h2_mic_after}")
if(NOT h2_mic_removed EQUAL h2_mic_width)
  message(FATAL_ERROR "Expected one pinned BK microphone pause")
endif()
string(REPLACE "${h2_mic_original}" "${h2_mic_replacement}" h2_mic_source "${h2_mic_source}")

set(h2_mic_original [=[        ret = onboard_mic_start(priv);
        if (ret != BK_OK)
        {
            LOGE("%s, onboard mic record start fail, ret: %d, %d \n", __func__, ret, __LINE__);
        }
        onboard_mic_state_set(priv, AUDIO_RECORD_STA_RUNNING);]=])
set(h2_mic_replacement [=[        ret = rb_reset(priv->rb);
        if (ret != BK_OK) return ret;
        ret = onboard_mic_start(priv);
        if (ret != BK_OK)
        {
            LOGE("%s, onboard mic record start fail, ret: %d, %d \n", __func__, ret, __LINE__);
            return ret;
        }
        onboard_mic_state_set(priv, AUDIO_RECORD_STA_RUNNING);]=])
string(REPLACE "${h2_mic_original}" "" h2_mic_without "${h2_mic_source}")
string(LENGTH "${h2_mic_source}" h2_mic_before)
string(LENGTH "${h2_mic_without}" h2_mic_after)
string(LENGTH "${h2_mic_original}" h2_mic_width)
math(EXPR h2_mic_removed "${h2_mic_before} - ${h2_mic_after}")
if(NOT h2_mic_removed EQUAL h2_mic_width)
  message(FATAL_ERROR "Expected one pinned BK microphone resume")
endif()
string(REPLACE "${h2_mic_original}" "${h2_mic_replacement}" h2_mic_source "${h2_mic_source}")

# Keep each ADC beat independent across repeat/pause boundaries instead of
# assembling multiple mono FIFO requests into one SRAM word.
set(h2_mic_original "\tdma_config.dst.width = DMA_DATA_WIDTH_32BITS;")
set(h2_mic_replacement "\tdma_config.dst.width = dma_config.src.width;")
string(REPLACE "${h2_mic_original}" "" h2_mic_without "${h2_mic_source}")
string(LENGTH "${h2_mic_source}" h2_mic_before)
string(LENGTH "${h2_mic_without}" h2_mic_after)
string(LENGTH "${h2_mic_original}" h2_mic_width)
math(EXPR h2_mic_removed "${h2_mic_before} - ${h2_mic_after}")
if(NOT h2_mic_removed EQUAL h2_mic_width)
  message(FATAL_ERROR "Expected one pinned BK ADC DMA destination width")
endif()
string(REPLACE "${h2_mic_original}" "${h2_mic_replacement}" h2_mic_source "${h2_mic_source}")
file(WRITE "${H2_BK_AUDIO_MIC_OUTPUT}" "${h2_mic_source}")
