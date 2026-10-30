file(READ "${H2_BK_FLASH_LOCK_INPUT}" h2_flash_lock_source)
string(REPLACE "\r\n" "\n" h2_flash_lock_source "${h2_flash_lock_source}")
string(SHA256 h2_flash_lock_sha256 "${h2_flash_lock_source}")
if(NOT h2_flash_lock_sha256 STREQUAL "5a8f7ceaf8f7e29f35d4f3c8af72fce61b9545ec411886f76f839da89179e292")
  message(FATAL_ERROR "Pinned BK Flash shared-lock source changed; revalidate lock ordering")
endif()

# flash_lock() already owns the shared lock before entering this critical
# section. Taking the AP spinlock first lets another core hold that spinlock
# while waiting for the writer's shared lock, forming a cycle.
set(h2_flash_lock_old [=[	uint32_t flags = rtos_disable_int();
#ifdef CONFIG_FREERTOS_SMP
	spin_lock(&s_flash_aspl_spin_lock);
#endif
	uint32_t cpu_id = bk_flash_shared_get_cpu_id();

	if (bk_flash_shared_acquire(cpu_id) != BK_OK) {
#ifdef CONFIG_FREERTOS_SMP
		spin_unlock(&s_flash_aspl_spin_lock);
#endif
		rtos_enable_int(flags);
		BK_ASSERT(0);
	}

	return flags;]=])
set(h2_flash_lock_new [=[	uint32_t flags = rtos_disable_int();
	uint32_t cpu_id = bk_flash_shared_get_cpu_id();

	if (bk_flash_shared_acquire(cpu_id) != BK_OK) {
		rtos_enable_int(flags);
		BK_ASSERT(0);
	}
#ifdef CONFIG_FREERTOS_SMP
	spin_lock(&s_flash_aspl_spin_lock);
#endif

	return flags;]=])
string(REPLACE "${h2_flash_lock_old}" "${h2_flash_lock_new}"
  h2_flash_lock_corrected "${h2_flash_lock_source}")
if(h2_flash_lock_corrected STREQUAL h2_flash_lock_source)
  message(FATAL_ERROR "Pinned BK Flash critical entry changed")
endif()
set(h2_flash_unlock_old [=[	bk_flash_shared_release(cpu_id);
#ifdef CONFIG_FREERTOS_SMP
	spin_unlock(&s_flash_aspl_spin_lock);
#endif
	rtos_enable_int(flags);]=])
set(h2_flash_unlock_new [=[#ifdef CONFIG_FREERTOS_SMP
	spin_unlock(&s_flash_aspl_spin_lock);
#endif
	bk_flash_shared_release(cpu_id);
	rtos_enable_int(flags);]=])
string(REPLACE "${h2_flash_unlock_old}" "${h2_flash_unlock_new}"
  h2_flash_lock_final "${h2_flash_lock_corrected}")
if(h2_flash_lock_final STREQUAL h2_flash_lock_corrected)
  message(FATAL_ERROR "Pinned BK Flash critical exit changed")
endif()
file(WRITE "${H2_BK_FLASH_LOCK_OUTPUT}" "${h2_flash_lock_final}")
