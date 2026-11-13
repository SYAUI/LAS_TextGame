#pragma once

#include <string>
#include <vector>
#include <functional>
#include <memory>
#include <cstdint>

namespace textgame {

    class TextGameCoreImpl;

    // 通用返回值
    struct ScriptResult {
        bool        ok = false;
        std::string error;

        static ScriptResult Success() { return { true, {} }; }
        static ScriptResult Failure(std::string msg) { return { false, std::move(msg) }; }
    };

    // 配置
    struct GameConfig {
        // 路径
        std::string assetRoot = "core/";      // 存放 py/ 资源的根目录
        std::string levelRoot = "Levels/";      // 关卡根目录
        std::string pythonHome = "Libraries/python/";     // Python 运行时 home

        // 沙盒
        bool useSandbox = true;
        int  sandboxMB = 64;

        // 模式
        bool devMode = true;        // true: 优先读源工程；false: 优先读 .pyc / 发布包


        std::function<void(const std::string&)> onSpeak;
        std::function<void(const std::string& name,
            const std::string& color,
            const std::string& text,
            const std::string& sprite,
            const std::string& avatar,
            const std::string& side)> onCharacterSpeak;
        std::function<void(const std::vector<std::string>&)> onTextPrint;
        std::function<void(const std::string& path)> onLoad;

        // 清空所有立绘 + 头像
        std::function<void()> onClearCharacters;

        std::function<void(const std::string& path)> onLoadScene;

        std::function<int(const std::string& title, const std::vector<std::string>& choices)> onMenu;
    };

    // 主类
    class TextGameCore {
    public:
        TextGameCore();
        ~TextGameCore();

        TextGameCore(const TextGameCore&) = delete;
        TextGameCore& operator=(const TextGameCore&) = delete;

        // 生命周期
        bool Init(const GameConfig& cfg);   // 建 TLSF + 初始化 Python + 加载 wrapper
        void Shutdown();                    // Py_Finalize + 释放 TLSF
        bool IsInitialized() const;

        // 脚本
        ScriptResult RunScriptFile(const std::string& path);
        ScriptResult RunScript(const std::string& path, const std::string& src);

        // 游戏流程
        void LaunchGame(const std::string& startLabel = "start");
        int  LoadGameLevel(const std::string& path);   // 0 = 成功

        // 脚本层 API
        bool Speak(const std::string& text);
        bool TextPrint(const std::vector<std::string>& lines);
        bool Load(const std::string& path);
        bool SpeakCharacter(const std::string& name, const std::string& color,
            const std::string& text,
            const std::string& sprite = "",
            const std::string& avatar = "",
            const std::string& side = "");
        bool LoadScene(const std::string& path);
        bool ClearCharacters();
        int  Menu(const std::string& title,
            const std::vector<std::string>& choices);

        // 调试
        void VMemoryDump(const std::string& outFile = "IE.bin") const;

        const GameConfig& GetConfig() const;

        // 供 core 内部模块使用的不透明访问器
        TextGameCoreImpl* GetImpl() noexcept { return impl_.get(); }
        const TextGameCoreImpl* GetImpl() const noexcept { return impl_.get(); }

    private:
        std::unique_ptr<TextGameCoreImpl> impl_;
    };

    // 供 C 风格绑定层访问当前活动实例（仅 core 内部使用）
    void          SetActiveCore(TextGameCore* core);
    TextGameCore* GetActiveCore();

    // 在 Python 回调中临时让出 GIL。
    class GILRelease {
    public:
        GILRelease();
        ~GILRelease();
        GILRelease(const GILRelease&) = delete;
        GILRelease& operator=(const GILRelease&) = delete;
    private:
        void* saved_ = nullptr;   // 实际是 PyThreadState*
    };

    // 在脚本回调中请求终止当前脚本执行, 会向 Python 层抛 SystemExit，使 run_game 立即退出。
    void RaiseScriptTermination(const char* reason = nullptr);

} // namespace textgame