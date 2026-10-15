# Ogg Opus

`libs/ogg_opus` 拥有原 GizClaw 流式播放器使用的 Ogg/Opus parser 和 decoder。GizClaw 与其它 consumer 均直接使用公共 `h2_ogg_opus.h` 和 `h2_ogg_opus_*` 方法，不保留旧内部名称的转接层，不得复制解析源码。它只依赖 C11、Opus 和调用方提供的 PAL allocator／读取 callback，不拥有文件、网络、Task 或 Audio Track。

## 解码与有界读取

Memory decoder 借用完整不可变输入；reader decoder 由调用方同步提供字节。标准解码保持 16 kHz mono PCM16LE 输出，应用 OpusHead gain、pre-skip 与 EOS granule trimming，并验证 page CRC、serial／sequence、continuation 和 chained stream。原有 80 ms seek preroll、resync 与输出位置合同保持；GizClaw 播放器继续使用这些入口。

Encoded-packet reader 复用相同的 page、packet 与 RFC 7845 timing 检查，但不创建 Opus decoder、不生成 PCM。调用方选择 282–65,307 bytes 的最大 page 和 512–65,536 bytes 的最大 packet；超过限制的 page／audio packet 返回 `NO_SPACE`。超大 OpusTags 保持有界的 magic 校验与随读随弃策略。返回的 packet 借用 reader buffer，直到下一次 reader 调用；音频时长、起始丢弃量和有效样本数均采用 16 kHz 输出网格，gain 保留 OpusHead signed Q8 dB。头包或尚未完成的跨页 packet 返回空 packet；`stream_start` 标识 OpusHead，允许 consumer 限制自身支持的逻辑流数。

每次读取最多处理一个 page 或 packet；读取 callback 必须同步返回 1..capacity 个字节，真正 EOF 使用 `EXIT`。完整流只有在验证 EOS 后才能返回 `EXIT`，截断、CRC 或 granule 错误不能冒充正常结束。调用方决定在哪个 Task 调用，不从 ISR 调用阻塞 reader。同一 instance 的调用必须串行。

## Checkpoint 与重播

Packet reader 的 checkpoint 使用调用方拥有的固定容量不透明 buffer，大小只由 reader 的 page／packet 上限决定，与曲长无关。Save 返回对应的输入字节位置；调用方先把输入 seek 到该位置，再 restore parser 状态。Checkpoint 仅对原 reader、同一进程有效，不能修改、持久化或当作 wire format。Restore 不分配内存、不调用 I/O，不转移 allocator／输入所有权；过期的 packet pointer 不能继续使用。

Reset 在调用方将输入移回 byte 0 后清除 parser 状态，复用既有有界 buffer。Codec 的重置、连续循环、预热、暂停、淡出、混音和 Audio Track 生命周期属于 consumer；公共 packet reader 不注入产品播放策略。读取失败可以通过有效 checkpoint 与输入 seek 恢复；所有 callback／allocator 必须存活至 destroy。

## 验证

`//libs/ogg_opus:packet_reader_test` 覆盖短读、packet timing、正常 EOS、CRC、checkpoint 输入位置、I/O 失败恢复和重播的固定内存边界。原有 `//libs/gizclaw:h2_gizclaw_ogg_opus_test` 与 `//libs/gizclaw:h2_gizclaw_audio_decoder_test` 继续运行同一 decoder，覆盖 pre-skip、gain、end trimming、continued／chained pages、seek 与 resync。Consumer 还需验证自身的文件生命周期、工作线程、背压和音频策略；library host test 不替代真机播放验收。
