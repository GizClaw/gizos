# POSIX Components

`libs/pal/providers/posix/` 只保存多个 POSIX OS owner 需要共享、但没有独立 public provider identity 的实现 source。它不是运行平台、target family 或 App-facing capability root。

当前 `libs/pal/providers/posix/serial_host` 由 Linux 与 Darwin Host Serial owner 私下复用。它拥有 termios config、immutable snapshot lifecycle、mutex-protected session、bounded read/write/flush、control line、原始 terminal 恢复和 idempotent close；OS package 实现 private scanner hook并导出自己的 public accessor。

POSIX target 必须保持 private visibility，只允许明确列出的 OS owner 依赖。它不能提供 public include directory、semantic native-catalog entry、Runtime composition、unsupported fallback 或 `h2_posix_*` public provider accessor。Desktop、App、board 与 project launcher 不得直接依赖该 target。

共享 test support 接受 OS-owned API object，由每个 OS package 自己实例化测试。新增另一项共享实现前，必须证明至少两个真实 OS owner 需要完全相同的 lifecycle；仅因为代码使用 pthread 或 libc，不足以把它移动到 POSIX root。
