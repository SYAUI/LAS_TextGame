#include "TextCore.h"
#include "PythonRuntime.h"

#include <cstdio>
#include <cstring>
#include <fstream>
#include <iostream>

namespace textgame {

    // ------------------------- 全局活动实例 -------------------------

    static TextGameCore* g_activeCore = nullptr;

    void SetActiveCore(TextGameCore* c) { g_activeCore = c; }
    TextGameCore* GetActiveCore() { return g_activeCore; }

    // ------------------------- Impl 内存 -------------------------

    void TextGameCoreImpl::MemInit(int bytes) {
        if (bytes <= 0) return;
        dataSize = bytes;
        data = new char[static_cast<size_t>(bytes) + TLSFAllocator::ALIGN];
        std::memset(data, 0, static_cast<size_t>(bytes) + TLSFAllocator::ALIGN);

        tlsf.init(data, static_cast<size_t>(bytes) + TLSFAllocator::ALIGN);
        if (!tlsf.valid()) {
            std::cerr << "[Core] TLSF 初始化失败\n";
            delete[] data; data = nullptr; dataSize = 0;
        }
    }

    void TextGameCoreImpl::MemClose() {
        delete[] data;
        data = nullptr;
        dataSize = 0;
    }

    void* TextGameCoreImpl::Calloc(size_t n, size_t sz) {
        size_t total = n * sz;
        void* p = Malloc(total);
        if (p) std::memset(p, 0, total);
        return p;
    }

    // TextGameCore

    TextGameCore::TextGameCore() : impl_(std::make_unique<TextGameCoreImpl>()) {}
    TextGameCore::~TextGameCore() { Shutdown(); }

    bool TextGameCore::Init(const GameConfig& cfg) {
        if (impl_->initialized) return true;

        impl_->cfg = cfg;

        // 1) 沙盒内存
        if (cfg.useSandbox) {
            impl_->MemInit(cfg.sandboxMB * 1024 * 1024);

            // 沙盒自检
            if (!impl_->MemReady()) {
                std::cerr << "[Core] 沙盒初始化失败\n";
                return false;
            }
            void* t = impl_->Malloc(16);
            if (!t) {
                std::cerr << "[Core] 沙盒自检失败（首块分配返回 nullptr）\n";
                impl_->MemClose();
                return false;
            }
            impl_->Free(t);
        }

        // 2) Python
        if (!InitializePythonRuntime(this)) {
            std::cerr << "[Core] Python 初始化失败\n";
            if (cfg.useSandbox) impl_->MemClose();
            return false;
        }
        impl_->pyInitialized = true;

        // 3) 注册绑定和加载 wrapper 脚本
        if (!RegisterCoreModule(this)) {
            std::cerr << "[Core] 注册 core 模块失败\n";
            Shutdown();
            return false;
        }
        if (!LoadRuntimeScripts(this)) {
            std::cerr << "[Core] 加载运行时脚本失败\n";
            Shutdown();
            return false;
        }

        SetActiveCore(this);
        impl_->initialized = true;

        // 初始化完成，让出 GIL，交给游戏线程按需 PyGILState_Ensure
        impl_->mainThreadState = ReleasePythonGIL();

        return true;
    }

    void TextGameCore::LaunchGame(const std::string& startLabel) {
        GILGuard gil;
        RunGameLoop(startLabel);
    }

    void TextGameCore::Shutdown() {
        if (!impl_) return;

        if (impl_->pyInitialized) {
            // 先恢复主线程的 GIL，才能安全调用 Py_FinalizeEx
            if (impl_->mainThreadState) {
                RestorePythonGIL(impl_->mainThreadState);
                impl_->mainThreadState = nullptr;
            }

            SetActiveCore(nullptr);
            ShutdownPythonRuntime();   // Py_FinalizeEx
            impl_->pyInitialized = false;
        }
        if (impl_->data) {
            impl_->MemClose();
        }
        impl_->initialized = false;
    }

    bool TextGameCore::IsInitialized() const { return impl_->initialized; }

    // ------------------------- 脚本 -------------------------

    ScriptResult TextGameCore::RunScriptFile(const std::string& path) {
        std::ifstream f(path, std::ios::binary);
        if (!f) return ScriptResult::Failure("无法打开脚本: " + path);

        std::string src((std::istreambuf_iterator<char>(f)),
            std::istreambuf_iterator<char>());
        return RunScript(path, src);
    }

    ScriptResult TextGameCore::RunScript(const std::string& path,
        const std::string& src) {
        if (!impl_->initialized)
            return ScriptResult::Failure("引擎未初始化");
        GILGuard gil;
        return ExecuteScriptInternal(this, path, src);
    }

    int TextGameCore::LoadGameLevel(const std::string& path) {
        std::string full = impl_->cfg.levelRoot + path;
        std::ifstream ifs(full);
        if (!ifs.is_open()) return -1;

        impl_->levelSrc.clear();
        std::string line;
        while (std::getline(ifs, line)) {
            impl_->levelSrc.push_back(std::move(line));
        }
        return 0;
    }

    // ------------------------- 脚本 API -------------------------

    bool TextGameCore::Speak(const std::string& text) {
        if (impl_->cfg.onSpeak) { impl_->cfg.onSpeak(text); return true; }
        std::printf("%s", text.c_str());
        return true;
    }

    bool TextGameCore::TextPrint(const std::vector<std::string>& lines) {
        if (impl_->cfg.onTextPrint) { impl_->cfg.onTextPrint(lines); return true; }
        for (const auto& s : lines) std::printf("%s", s.c_str());
        return true;
    }

    bool TextGameCore::Load(const std::string& path) {
        if (impl_->cfg.onLoad) { impl_->cfg.onLoad(path); return true; }
        std::printf("[load] %s\n", path.c_str());
        return true;
    }

    bool TextGameCore::SpeakCharacter(const std::string& name,
        const std::string& color,
        const std::string& text,
        const std::string& sprite,
        const std::string& avatar,
        const std::string& side) {
        if (impl_->cfg.onCharacterSpeak) {
            impl_->cfg.onCharacterSpeak(name, color, text, sprite, avatar, side);
            return true;
        }
        std::printf("<%s> %s\n", name.c_str(), text.c_str());
        return true;
    }

    bool TextGameCore::LoadScene(const std::string& path) {
        if (impl_->cfg.onLoadScene) {
            impl_->cfg.onLoadScene(path);
            return true;
        }
        return true;
    }

    bool TextGameCore::ClearCharacters() {
        if (impl_->cfg.onClearCharacters) {
            impl_->cfg.onClearCharacters();
            return true;
        }
        return true;
    }

    int TextGameCore::Menu(const std::string& title,
        const std::vector<std::string>& choices) {
        if (impl_->cfg.onMenu) return impl_->cfg.onMenu(title, choices);
        return 0; 
    }

    // ------------------------- 调试 -------------------------

    void TextGameCore::VMemoryDump(const std::string& outFile) const {
        if (!impl_->data || impl_->dataSize <= 0) return;
        FILE* fp = nullptr;
        fopen_s(&fp, outFile.c_str(), "wb");
        if (!fp) return;
        std::fwrite(impl_->data, 1, static_cast<size_t>(impl_->dataSize), fp);
        std::fclose(fp);
    }

    const GameConfig& TextGameCore::GetConfig() const { return impl_->cfg; }

} // namespace textgame