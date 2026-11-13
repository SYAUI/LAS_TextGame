#pragma once

#include "TextGameAPI.h"
#include <string>

namespace textgame {

    class TextGameCore;

    // 初始化 / 关闭 Python（含沙盒分配器）
    bool InitializePythonRuntime(TextGameCore* core);
    void ShutdownPythonRuntime();

    // 注册 core 模块 + WRAPPER
    bool RegisterCoreModule(TextGameCore* core);
    bool LoadRuntimeScripts(TextGameCore* core);

    // 启动游戏主循环
    void RunGameLoop(const std::string& startLabel);

    // 执行脚本
    ScriptResult ExecuteScriptInternal(TextGameCore* core,
        const std::string& filepath,
        const std::string& scriptContent);

    // ---- GIL 辅助 ----

    // RAII：保证进入作用域后持有 GIL（内部 PyGILState_Ensure / Release）。
    // 只能在任意线程使用；如当前线程已持有 GIL 则幂等。
    class GILGuard {
    public:
        GILGuard();
        ~GILGuard();
        GILGuard(const GILGuard&) = delete;
        GILGuard& operator=(const GILGuard&) = delete;
    private:
        int state_ = 0;   // PyGILState_STATE
    };

    // 生命周期用：
    //   Init 成功后：state = ReleasePythonGIL();     // 让出 GIL，主线程不再持有
    //   Shutdown 前：RestorePythonGIL(state);        // 拿回 GIL，再 Py_FinalizeEx
    // 两者必须在同一线程调用。
    void* ReleasePythonGIL();
    void  RestorePythonGIL(void* state);

} // namespace textgame