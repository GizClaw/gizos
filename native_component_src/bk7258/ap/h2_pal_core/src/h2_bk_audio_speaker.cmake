# Pause must transfer ring ownership from the worker before the control task
# primes a stopped DMA ring. A retained write offset can leave resumed DMA at
# its read-pause address, with no completion available to wake the producer.
file(READ "${H2_BK_SPEAKER_INPUT}" h2_speaker_source)
string(REPLACE "\r\n" "\n" h2_speaker_source "${h2_speaker_source}")
string(SHA256 h2_speaker_sha "${h2_speaker_source}")
file(READ "${H2_BK_AUDIO_RING_INPUT}" h2_audio_ring_source)
string(REPLACE "\r\n" "\n" h2_audio_ring_source "${h2_audio_ring_source}")
string(SHA256 h2_audio_ring_sha "${h2_audio_ring_source}")
if(NOT h2_speaker_sha STREQUAL "e2da48a7bcd6abc8b44d0d40a8e4f232707ff461d64ce2da2bb1143489e5f9f9"
    OR NOT h2_audio_ring_sha STREQUAL "06e4e63d15a04c22b56b630c466aee6c00b9d3fb5d426ebb4a9fcc8649afaedc")
  message(FATAL_ERROR "Pinned BK speaker/ring sources changed; revalidate restart ownership")
endif()
function(h2_bk_speaker_replace original replacement)
  string(REPLACE "${original}" "" without "${h2_speaker_source}")
  string(LENGTH "${h2_speaker_source}" before)
  string(LENGTH "${without}" after)
  string(LENGTH "${original}" width)
  math(EXPR removed "${before} - ${after}")
  if(NOT removed EQUAL width)
    message(FATAL_ERROR "Expected one pinned BK speaker restart signature")
  endif()
  string(REPLACE "${original}" "${replacement}" corrected "${h2_speaker_source}")
  set(h2_speaker_source "${corrected}" PARENT_SCOPE)
endfunction()

set(original [=[                    spk_data_read_handle->running = false;
                    wait_time = BEKEN_WAIT_FOREVER;
                    break;]=])
set(replacement [=[                    spk_data_read_handle->running = false;
                    wait_time = BEKEN_WAIT_FOREVER;
                    /* All preceding ring writes have finished before the ACK. */
                    rtos_set_semaphore(&spk_data_read_handle->sem);
                    break;]=])
h2_bk_speaker_replace("${original}" "${replacement}")

set(original [=[    /* fill two frame silence data */
    os_memset(onboard_spk->read_buff, 0, onboard_spk->frame_size);
    ring_buffer_write(&onboard_spk->dma_rb, onboard_spk->read_buff, onboard_spk->frame_size);
    ring_buffer_write(&onboard_spk->dma_rb, onboard_spk->read_buff, onboard_spk->frame_size);

    ret = spk_data_read_send_msg(onboard_spk->spk_data_read_msg_que, SPK_DATA_READ_START, NULL);

    ret = bk_dma_start(onboard_spk->dac_dma_id);]=])
set(replacement [=[    /* The initial worker is parked; resume follows the pause ACK.
     * Rebuild a complete DMA bootstrap while no producer owns the ring. */
    ret = bk_dma_set_src_start_addr(onboard_spk->dac_dma_id, (uint32_t)onboard_spk->dma_rb.address);
    if (ret != BK_OK) return ret;
    ret = bk_dma_set_transfer_len(onboard_spk->dac_dma_id, onboard_spk->frame_size);
    if (ret != BK_OK) return ret;
    ring_buffer_clear(&onboard_spk->dma_rb);
    os_memset(onboard_spk->read_buff, 0, onboard_spk->frame_size);
    if (ring_buffer_write(&onboard_spk->dma_rb, onboard_spk->read_buff, onboard_spk->frame_size) != onboard_spk->frame_size ||
        ring_buffer_write(&onboard_spk->dma_rb, onboard_spk->read_buff, onboard_spk->frame_size) != onboard_spk->frame_size)
        return BK_FAIL;

    ret = spk_data_read_send_msg(onboard_spk->spk_data_read_msg_que, SPK_DATA_READ_START, NULL);
    if (ret != BK_OK) return ret;
    ret = bk_dma_start(onboard_spk->dac_dma_id);]=])
h2_bk_speaker_replace("${original}" "${replacement}")

set(original [=[        LOGE("%s, %d, dac dma stop fail\n", __func__, __LINE__);
    }

    ret = bk_aud_dac_start();]=])
set(replacement [=[        LOGE("%s, %d, dac dma stop fail\n", __func__, __LINE__);
        return ret;
    }

    ret = bk_aud_dac_start();]=])
h2_bk_speaker_replace("${original}" "${replacement}")

set(original [=[    bk_err_t ret = bk_dma_stop(onboard_spk->dac_dma_id);
    if (ret != BK_OK)
    {
        LOGE("%s, %d, dac dma stop fail\n", __func__, __LINE__);
    }

    ret = bk_aud_dac_stop();
    if (ret != BK_OK)
    {
        LOGE("%s, %d, dac stop fail\n", __func__, __LINE__);
    }

    ret = spk_data_read_send_msg(onboard_spk->spk_data_read_msg_que, SPK_DATA_READ_IDLE, NULL);]=])
set(replacement [=[    /* Quiesce the sole running ring producer before stopping hardware.
     * The same semaphore already acknowledges worker init and exit. */
    bk_err_t ret = spk_data_read_send_msg(onboard_spk->spk_data_read_msg_que, SPK_DATA_READ_IDLE, NULL);
    if (ret != BK_OK) return ret;
    ret = rtos_get_semaphore(&onboard_spk->sem, BEKEN_WAIT_FOREVER);
    if (ret != BK_OK) return ret;
    ret = bk_dma_stop(onboard_spk->dac_dma_id);
    if (ret != BK_OK) return ret;
    ret = bk_aud_dac_stop();]=])
h2_bk_speaker_replace("${original}" "${replacement}")

foreach(transition pause start)
  if(transition STREQUAL "pause")
    set(state PAUSED)
  else()
    set(state RUNNING)
  endif()
  set(original "        onboard_speaker_state_set(priv, AUDIO_PLAY_STA_${state});")
  set(replacement "        if (ret != BK_OK) return ret;\n${original}")
  h2_bk_speaker_replace("${original}" "${replacement}")
endforeach()
file(WRITE "${H2_BK_SPEAKER_OUTPUT}" "${h2_speaker_source}")
