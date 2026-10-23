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

function(h2_bk_spi_replace_once signature replacement)
  string(REPLACE "${signature}" "" without "${h2_spi_source}")
  string(LENGTH "${h2_spi_source}" before)
  string(LENGTH "${without}" after)
  string(LENGTH "${signature}" expected)
  math(EXPR removed "${before} - ${after}")
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
	const spi_id_t spi_id = s_current_spi_dma_wr_id;
	SPI_LOGV("[%s] spi_id:%d\r\n", __func__, spi_id);

	if (s_spi_tx_finish_isr[spi_id].callback){
		s_spi_tx_finish_isr[spi_id].callback(spi_id,s_spi_tx_finish_isr[spi_id].param);
	}
	if (s_spi[spi_id].is_tx_blocked) {
		s_spi[spi_id].is_tx_blocked = false;
		rtos_set_semaphore(&s_spi[spi_id].tx_sema);
	}
}]=])
h2_bk_spi_replace_once("${h2_spi_signature}" "${h2_spi_replacement}")

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
	const spi_id_t spi_id = s_current_spi_dma_rd_id;
	SPI_LOGV("[%s] spi_id:%d\r\n", __func__, spi_id);
	if (s_spi_rx_finish_isr[spi_id].callback){
		s_spi_rx_finish_isr[spi_id].callback(spi_id,s_spi_rx_finish_isr[spi_id].param);
	}
	if (s_spi[spi_id].is_rx_blocked) {
		s_spi[spi_id].is_rx_blocked = false;
		rtos_set_semaphore(&s_spi[spi_id].rx_sema);
	}
}]=])
h2_bk_spi_replace_once("${h2_spi_signature}" "${h2_spi_replacement}")

file(WRITE "${H2_BK_SPI_OUTPUT}" "${h2_spi_source}")
