# The sealed SDK remains read-only. Correct its generated AP build input.
# Callback bodies derive from Beken's Apache-2.0 SPI driver (2020-2021).
# A woken task on the other AP core can enter its next SDK transfer before
# this ISR returns. Complete the old state before publishing its wakeup.
file(READ "${H2_BK_SPI_INPUT}" h2_spi_source)
string(REPLACE "\r\n" "\n" h2_spi_source "${h2_spi_source}")
string(SHA256 h2_spi_sha256 "${h2_spi_source}")
if(NOT h2_spi_sha256 STREQUAL "2772e38ac5c7838219c9e4cbc9115114ed74860e85be1b14a6d8a77072ae5772")
  message(FATAL_ERROR "Pinned BK SPI source changed; revalidate the SDK correction")
endif()

function(h2_bk_spi_replace_count signature replacement count)
  string(REPLACE "${signature}" "" without "${h2_spi_source}")
  string(LENGTH "${h2_spi_source}" before)
  string(LENGTH "${without}" after)
  string(LENGTH "${signature}" width)
  math(EXPR removed "${before} - ${after}")
  math(EXPR expected "${width} * ${count}")
  if(NOT removed EQUAL expected)
    message(FATAL_ERROR "Pinned BK SPI completion changed; revalidate the SDK correction")
  endif()
  string(REPLACE "${signature}" "${replacement}" corrected "${h2_spi_source}")
  set(h2_spi_source "${corrected}" PARENT_SCOPE)
endfunction()

set(h2_spi_signature [=[static void spi_dma_tx_finish_handler(dma_id_t id)
{
	SPI_LOGV("[%s] spi_id:%d\r\n", __func__, s_current_spi_dma_wr_id);

	if (s_spi_tx_finish_isr[s_current_spi_dma_wr_id].callback){
		s_spi_tx_finish_isr[s_current_spi_dma_wr_id].callback(s_current_spi_dma_wr_id,s_spi_tx_finish_isr[s_current_spi_dma_wr_id].param);
	}
	if (s_spi[s_current_spi_dma_wr_id].is_tx_blocked) {
		rtos_set_semaphore(&s_spi[s_current_spi_dma_wr_id].tx_sema);
		s_spi[s_current_spi_dma_wr_id].is_tx_blocked = false;
	}
}]=])
set(h2_spi_replacement [=[static void spi_dma_tx_finish_handler(dma_id_t id)
{
	const uint32_t int_level = spi_enter_critical();
	const spi_id_t spi_id = s_current_spi_dma_wr_id;
	const bool was_blocked = s_spi[spi_id].h2_tx_dma_active && s_spi[spi_id].is_tx_blocked;
	const spi_callback_t callback = s_spi_tx_finish_isr[spi_id];
	if (was_blocked) {
		s_spi[spi_id].is_tx_blocked = false;
	}
	spi_exit_critical(int_level);
	SPI_LOGV("[%s] spi_id:%d\r\n", __func__, spi_id);
	if (was_blocked && callback.callback) {
		callback.callback(spi_id, callback.param);
	}
	if (was_blocked) {
		rtos_set_semaphore(&s_spi[spi_id].tx_sema);
	}
}]=])
h2_bk_spi_replace_count("${h2_spi_signature}" "${h2_spi_replacement}" 1)

set(h2_spi_signature [=[static void spi_dma_rx_finish_handler(dma_id_t id)
{
	SPI_LOGV("[%s] spi_id:%d\r\n", __func__, s_current_spi_dma_rd_id);
	if (s_spi_rx_finish_isr[s_current_spi_dma_rd_id].callback){
		s_spi_rx_finish_isr[s_current_spi_dma_rd_id].callback(s_current_spi_dma_rd_id,s_spi_rx_finish_isr[s_current_spi_dma_rd_id].param);
	}
	if (s_spi[s_current_spi_dma_rd_id].is_rx_blocked) {
		rtos_set_semaphore(&s_spi[s_current_spi_dma_rd_id].rx_sema);
		s_spi[s_current_spi_dma_rd_id].is_rx_blocked = false;
	}
}]=])
set(h2_spi_replacement [=[static void spi_dma_rx_finish_handler(dma_id_t id)
{
	const uint32_t int_level = spi_enter_critical();
	const spi_id_t spi_id = s_current_spi_dma_rd_id;
	const bool was_blocked = s_spi[spi_id].h2_rx_dma_active && s_spi[spi_id].is_rx_blocked;
	const spi_callback_t callback = s_spi_rx_finish_isr[spi_id];
	if (was_blocked) {
		s_spi[spi_id].is_rx_blocked = false;
	}
	spi_exit_critical(int_level);
	SPI_LOGV("[%s] spi_id:%d\r\n", __func__, spi_id);
	if (was_blocked && callback.callback) {
		callback.callback(spi_id, callback.param);
	}
	if (was_blocked) {
		rtos_set_semaphore(&s_spi[spi_id].rx_sema);
	}
}]=])
h2_bk_spi_replace_count("${h2_spi_signature}" "${h2_spi_replacement}" 1)

set(h2_spi_signature [=[static void spi_isr_common(spi_id_t id)
{]=])
set(h2_spi_replacement [=[static void spi_isr_common(spi_id_t id)
{
	bool h2_wake_tx = false;
	bool h2_wake_rx = false;]=])
h2_bk_spi_replace_count("${h2_spi_signature}" "${h2_spi_replacement}" 1)

set(h2_spi_signature [=[				rtos_set_semaphore(&s_spi[id].tx_sema);
				s_spi[id].is_tx_blocked = false;]=])
set(h2_spi_replacement [=[				s_spi[id].is_tx_blocked = false;
				h2_wake_tx = true;]=])
h2_bk_spi_replace_count("${h2_spi_signature}" "${h2_spi_replacement}" 2)

set(h2_spi_signature [=[			rtos_set_semaphore(&s_spi[id].rx_sema);
			s_spi[id].is_rx_blocked = false;]=])
set(h2_spi_replacement [=[			s_spi[id].is_rx_blocked = false;
			h2_wake_rx = true;]=])
h2_bk_spi_replace_count("${h2_spi_signature}" "${h2_spi_replacement}" 2)

set(h2_spi_signature [=[s_spi_rx_finish_isr[id].callback(s_current_spi_dma_rd_id,s_spi_rx_finish_isr[id].param);]=])
set(h2_spi_replacement [=[s_spi_rx_finish_isr[id].callback(id,s_spi_rx_finish_isr[id].param);]=])
h2_bk_spi_replace_count("${h2_spi_signature}" "${h2_spi_replacement}" 1)

set(h2_spi_signature [=[}

static void spi_isr(void)]=])
set(h2_spi_replacement [=[	/* Finish FIFO/register cleanup before a woken core can reuse SPI. */
	if (h2_wake_tx) {
		rtos_set_semaphore(&s_spi[id].tx_sema);
	}
	if (h2_wake_rx) {
		rtos_set_semaphore(&s_spi[id].rx_sema);
	}
}

static void spi_isr(void)]=])
h2_bk_spi_replace_count("${h2_spi_signature}" "${h2_spi_replacement}" 1)

# A stale ordinary SPI IRQ must not drain DMA FIFOs, disable its transmitter,
# or consume the DMA waiter's semaphore. Both kinds of IRQ serialize their
# state transition with the SDK's existing SMP critical section. Callbacks
# and semaphore wakeups run only after that section and register cleanup.
set(h2_spi_signature [=[	bool is_tx_blocked;
	bool is_sw_tx_finished;]=])
set(h2_spi_replacement [=[	bool is_tx_blocked;
	bool h2_tx_dma_active;
	bool h2_rx_dma_active;
	bool is_sw_tx_finished;]=])
h2_bk_spi_replace_count("${h2_spi_signature}" "${h2_spi_replacement}" 1)

set(h2_spi_signature [=[		uint32_t int_level = spi_enter_critical();
		s_spi[id].is_tx_blocked = true;
		s_current_spi_dma_wr_id = id;]=])
set(h2_spi_replacement [=[		uint32_t int_level = spi_enter_critical();
		s_spi[id].h2_tx_dma_active = true;
		s_spi[id].is_tx_blocked = true;
		s_current_spi_dma_wr_id = id;]=])
h2_bk_spi_replace_count("${h2_spi_signature}" "${h2_spi_replacement}" 1)

set(h2_spi_signature [=[		uint32_t int_level = spi_enter_critical();
		s_current_spi_dma_rd_id = id;]=])
set(h2_spi_replacement [=[		uint32_t int_level = spi_enter_critical();
		s_spi[id].h2_rx_dma_active = true;
		s_current_spi_dma_rd_id = id;]=])
h2_bk_spi_replace_count("${h2_spi_signature}" "${h2_spi_replacement}" 1)

foreach(h2_direction tx rx)
  # Duplex arms both owners; success, overflow retry, and simplex cleanup
  # release the matching owner only after stopping the DMA channel.
  set(h2_spi_signature "\t\t\t\ts_spi[id].is_${h2_direction}_blocked = true;")
  set(h2_spi_replacement "\t\t\t\ts_spi[id].h2_${h2_direction}_dma_active = true;\n${h2_spi_signature}")
  h2_bk_spi_replace_count("${h2_spi_signature}" "${h2_spi_replacement}" 1)
  foreach(h2_indent 2 3 4)
    string(REPEAT "\t" ${h2_indent} h2_tabs)
    set(h2_spi_signature "\n${h2_tabs}bk_dma_stop(s_spi[id].spi_${h2_direction}_dma_chan);")
    set(h2_spi_replacement "${h2_spi_signature}\n${h2_tabs}s_spi[id].h2_${h2_direction}_dma_active = false;")
    h2_bk_spi_replace_count("${h2_spi_signature}" "${h2_spi_replacement}" 1)
  endforeach()
endforeach()

set(h2_spi_signature [=[	bool h2_wake_rx = false;]=])
set(h2_spi_replacement [=[	bool h2_wake_rx = false;
	spi_callback_t h2_rx_data_callback = {0};
	spi_callback_t h2_rx_finish_callback = {0};
	spi_callback_t h2_tx_finish_callback = {0};
	const uint32_t h2_int_level = spi_enter_critical();]=])
h2_bk_spi_replace_count("${h2_spi_signature}" "${h2_spi_replacement}" 1)

foreach(h2_direction tx rx)
  set(h2_spi_signature "if (spi_hal_is_${h2_direction}_fifo_int_triggered_with_status(hal, int_status)) {")
  set(h2_spi_replacement "if (!s_spi[id].h2_${h2_direction}_dma_active &&\n\t\tspi_hal_is_${h2_direction}_fifo_int_triggered_with_status(hal, int_status)) {")
  h2_bk_spi_replace_count("${h2_spi_signature}" "${h2_spi_replacement}" 1)
  set(h2_spi_signature "if (spi_hal_is_${h2_direction}_finish_int_triggered(hal, int_status)")
  set(h2_spi_replacement "if (!s_spi[id].h2_${h2_direction}_dma_active &&\n\t\tspi_hal_is_${h2_direction}_finish_int_triggered(hal, int_status)")
  h2_bk_spi_replace_count("${h2_spi_signature}" "${h2_spi_replacement}" 1)
endforeach()

set(h2_spi_signature [=[		if (s_spi_rx_isr[id].callback) {
			s_spi_rx_isr[id].callback(id, s_spi_rx_isr[id].param);
		}]=])
set(h2_spi_replacement [=[		h2_rx_data_callback = s_spi_rx_isr[id];]=])
h2_bk_spi_replace_count("${h2_spi_signature}" "${h2_spi_replacement}" 1)
set(h2_spi_signature [=[		if (s_spi_rx_finish_isr[id].callback){
			s_spi_rx_finish_isr[id].callback(id,s_spi_rx_finish_isr[id].param);
		}]=])
set(h2_spi_replacement [=[		h2_rx_finish_callback = s_spi_rx_finish_isr[id];]=])
h2_bk_spi_replace_count("${h2_spi_signature}" "${h2_spi_replacement}" 1)
set(h2_spi_signature [=[			if (s_spi_tx_finish_isr[id].callback) {
				s_spi_tx_finish_isr[id].callback(id, s_spi_tx_finish_isr[id].param);
			}]=])
set(h2_spi_replacement [=[			h2_tx_finish_callback = s_spi_tx_finish_isr[id];]=])
h2_bk_spi_replace_count("${h2_spi_signature}" "${h2_spi_replacement}" 2)

set(h2_spi_signature [=[if (spi_hal_is_slave(hal)) {]=])
set(h2_spi_replacement [=[if (!s_spi[id].h2_tx_dma_active && spi_hal_is_slave(hal)) {]=])
h2_bk_spi_replace_count("${h2_spi_signature}" "${h2_spi_replacement}" 1)
set(h2_spi_signature [=[if (s_spi[id].is_rx_blocked) {]=])
set(h2_spi_replacement [=[if (!s_spi[id].h2_rx_dma_active && s_spi[id].is_rx_blocked) {]=])
h2_bk_spi_replace_count("${h2_spi_signature}" "${h2_spi_replacement}" 2)

set(h2_spi_signature [=[	/* Finish FIFO/register cleanup before a woken core can reuse SPI. */]=])
set(h2_spi_replacement [=[	spi_exit_critical(h2_int_level);
	if (h2_rx_data_callback.callback) {
		h2_rx_data_callback.callback(id, h2_rx_data_callback.param);
	}
	if (h2_rx_finish_callback.callback) {
		h2_rx_finish_callback.callback(id, h2_rx_finish_callback.param);
	}
	if (h2_tx_finish_callback.callback) {
		h2_tx_finish_callback.callback(id, h2_tx_finish_callback.param);
	}
	/* Finish FIFO/register cleanup before a woken core can reuse SPI. */]=])
h2_bk_spi_replace_count("${h2_spi_signature}" "${h2_spi_replacement}" 1)

file(WRITE "${H2_BK_SPI_OUTPUT}" "${h2_spi_source}")
