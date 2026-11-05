#ifndef ZLM_ENV_H
#define ZLM_ENV_H

// ============================================================================
// zlm_env — ZLMediaKit 进程级运行时的一次性初始化门
// ----------------------------------------------------------------------------
// 问题：mk_env_init1 是进程级单例，调第二次是未定义行为；而工程里现在有两个
// 服务都依赖 ZLM 运行时（HttpFileService 的 http 静态文件、RtspLiveService 的
// rtsp 直播分发）。两者的开关互相独立（alarm_uplink 关、pad_rtsp 开 是合法组合），
// 谁先起来谁就得负责 mk_env_init1，后起来的必须能识别"已经初始化过了"。
//
// 解法：把"是否已经调过 mk_env_init1"这道判据从 HttpFileService 的私有字段里抽出来，
// 放到这里。两个服务的 start() 都先调 zlm::ensureEnvInit()，由它内部用一个
// 进程级 static 标志保证只调一次。
//
// 约束（与 HttpFileService 原有一致，这里只是换了个归属）：
//   - mk_env_init1 的参数（IO 线程数、日志级别）由这里统一决定，两个服务共用；
//   - stop() 之后本进程不支持再 ensureEnvInit()（库的硬约束，不是本层偷懒）。
// ============================================================================

namespace zlm {

/// 进程级一次性初始化 ZLM 运行时。幂等：第二次直接返回 true。
/// @return true = 运行时已就绪（含"之前已经初始化过"）
bool ensureEnvInit();

/// ZLM 运行时是否已就绪（ensureEnvInit 调过之后为 true）
bool isEnvReady();

}  // namespace zlm

#endif  // ZLM_ENV_H
