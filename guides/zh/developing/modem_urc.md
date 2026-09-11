# Modem URC 接收与并发合同

公共 owner 是 `libs/pal/providers/modem/common` 和 `quectel`；板级 UART、CMUX、PPP 生命周期仍由 consumer 持有。队列容量保持 16，不以扩容替代过滤或并发控制。

## 接收

`h2_modem_rx_feed` 持有有界尾片段，只在 CR/LF 后交付完整行。物理流绝对 offset 区分新字节与累计前缀，不比较字符串进行去重。重复的 RING 是独立 occurrence；重放的相同 offset 不是。超长行或 NUL 污染行丢弃至下一个分隔符，不把截断尾巴当作新行。返回首个错误但继续消费当前输入，不能通过重放整个 batch 重试 FULL。

`h2_quectel_rx_feed` 在入队前分类。普通 OK、echo、IMEI/IMSI 等不进入 URC worker。注册查询的 `<n>,<stat>` 与通知的 `<stat>[,"lac",...]` 按语法区分，即使查询期间也保留真实通知。CPIN、CGATT、CSQ、CLCC、QSIMSTAT 在对应查询期间存在同格式歧义，必须提供命令上下文；确定为独立通知的行可由 transport 直接调用 `h2_quectel_post_urc_line`。不支持凭内容推断两次同文本是同一 occurrence。

receiver 为每个 AT 通道单独配置，生产者串行调用，不能输入 PPP 数据通道。命令上下文必须覆盖完整行，不得在尾片段中途切换。停止并 join 所有生产者之后才能重置 receiver 或销毁 modem。

## esp_modem 1.4.3 consumer 接线

`DTE::command_cb::process_line` 的旧 URC hook 在命令 parser 之前调用，参数为 `data, consumed + len`，包括普通应答。非 CMUX 使用累计 buffer；CMUX 开启 inflatable 时也可能累计，关闭时则直接提供片段。旧 hook 没有 consumed、buffer epoch 或物理 offset，不能仅靠长度、指针或相同前缀无歧义恢复物理流。

consumer 必须在只交付新字节的边界调用 `h2_quectel_rx_feed`，物理 offset 每次增加实际读取长度，零长度输入不推进 offset。命令 parser 仍取得原始完整输入，不能被 framer 消费或修改。

- 非 CMUX：观察 UART 终端实际读出的字节。Firmwares 以 `-Wl,--wrap=uart_read_bytes` 包装读取，只处理 modem UART 端口；这正是 DTE 追加到累计 buffer 的字节，因此不受 buffer 重置、command 成功/超时影响。
- CMUX（未开启 inflatable）：旧 hook 收到的就是 AT 通道的单个 payload 片段，可以直接作为来源；UART 字节此时是帧，不能再输入 framer。开启 inflatable 后 hook 又会累计，consumer 必须拒绝该配置。
- 切换来源或 CMUX 进出数据模式（AT 通道换 DLCI）时重置 receiver；切到 CMUX 前先切来源，最多重复框出 `AT+CMUX` 的回显/OK，不会把帧当文本。

不能修改共享 SDK/cache。私有 `tiga_modem_urc_line(data,total_len)` 的切行循环必须删除。transport 在命令发布、完成时同步 RX 命令上下文。禁止同时从 command response 再交付已经由 RX worker 负责的真实 URC。启用异步 worker 后，公共 command-response 路径假定 transport 负责全部真实 URC；同步 read/write 模式由公共 AT parser 负责。

## 锁和生命周期

锁顺序为 operation lock → state lock。operation lock 保持完整公共操作和 AT 事务串行；嵌套操作只保留一次 state lock acquisition。阻塞 command/read/write 期间释放 state lock，保留 operation lock。URC worker 只取 state lock，所以不等待 identity 或注册查询的 AT timeout。纯缓存 getter 同样只取 state lock。

URC 的系统事件、SIM invalidation 和 sleep gate 回调不得重入 modem API，不能等待 AT/RX/URC 任务。state lock 的短临界区不意味着任意 consumer 回调都自动无阻塞，板级实现必须遵守合同。close 的 transport teardown 在 state lock 外执行，deinit 在生产者停止后于所有 provider lock 外 join worker。join 失败保留实例供重试。SIM/reset generation 用于拒绝跨失效边界的 command response。

## 事件与诊断

注册、packet、signal 按语义字段变化发布，保留 A → B → A；call/READY 不作全局去重。close/reset 清除观察状态。系统事件采用 timeout 0，失败累计 event_drop_count；观察状态不等价于 Runtime 已收到事件。

`h2_modem_urc_get_stats` 提供 accepted、handled、full、truncated 计数，不输出行文本、身份或 secret。并发快照各字段独立读取，不是事务一致的队列长度。FULL 证明此队列入队失败，不能据此证明 Runtime event queue 满或组合键失效。

## 验证边界

公共回归包括分片/累计重放、重复真实通知、普通应答风暴、长事务期间消费者进度、状态边沿和生命周期失败重试。固件构建及真实按键、注册、SIM、PPP 验收由 consumer 在配对接线后完成；host 测试不能代替设备结论。
