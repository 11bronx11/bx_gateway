#include "sync.h"

// 同步原语现全部 header-only(标准库实现):
//   BxSemaphore  -> std::counting_semaphore
//   BxSpinLock   -> std::atomic + 指数退避 + PAUSE
//   BxTicketLock -> std::atomic 取号/叫号
//   BxMutex      -> std::mutex
//   BxRwMutex    -> std::shared_mutex
// 本翻译单元无需实现,保留以兼容 aux_source_directory 的源码收集。
