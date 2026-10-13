#pragma once
#include <Python.h>
#include <string>
#include <iostream>
#include "TextCore.h"

// 调试开关
static const bool USE_SANDBOX = true;

// ========== 沙盒分配器桥接函数 ==========
namespace SandboxAlloc {

    inline void* Malloc(void* ctx, size_t size) {
        if (size == 0) size = 1;   // 不返回 nullptr, CPython 明确要求返回唯一可 free 的非空指针
        return static_cast<TextGameCore*>(ctx)->IE_malloc(size);
    }

    inline void* Calloc(void* ctx, size_t nelem, size_t elsize) {
        return static_cast<TextGameCore*>(ctx)->IE_calloc(nelem, elsize);
    }

    inline void* Realloc(void* ctx, void* p, size_t new_size) {
        if (new_size == 0) {
            if (p) static_cast<TextGameCore*>(ctx)->IE_free(p);
            return nullptr;
        }
        if (p == nullptr) {
            return static_cast<TextGameCore*>(ctx)->IE_malloc(new_size);
        }
        return static_cast<TextGameCore*>(ctx)->IE_realloc(p, new_size);
    }

    inline void Free(void* ctx, void* p) {
        if (p) static_cast<TextGameCore*>(ctx)->IE_free(p);
    }
}

// 初始化 Python（根据 USE_SANDBOX 决定是否使用沙盒）
bool InitializePythonRuntime(TextGameCore* core, int sandboxSizeMB = 64) {
    if (USE_SANDBOX) {
        if (core == nullptr) {
            std::cerr << "[Python] 沙盒模式需要有效的 TextGameCore 指针" << std::endl;
            return false;
        }
        core->IE_malloc_init(sandboxSizeMB * 1024 * 1024);

        // 沙盒自检
        void* test = core->IE_malloc(16);
        if (!test) {
            std::cerr << "[Python] 沙盒自检失败" << std::endl;
            return false;
        }
        core->IE_free(test);

        // 1. 初始化 PreConfig，并调用 Py_PreInitialize
        PyPreConfig preconfig;
        PyPreConfig_InitPythonConfig(&preconfig); // 使用标准 Python 配置初始化
        PyStatus pre_status = Py_PreInitialize(&preconfig);
        if (PyStatus_Exception(pre_status)) {
            std::cerr << "[Python] Py_PreInitialize 失败: " << pre_status.err_msg << std::endl;
            return false;
        }

        // 准备分配器
        static PyMemAllocatorEx sandbox_alloc = {
            core,
            SandboxAlloc::Malloc,
            SandboxAlloc::Calloc,
            SandboxAlloc::Realloc,
            SandboxAlloc::Free
        };

        // RAW/MEM/OBJ 域设置
        PyMem_SetAllocator(PYMEM_DOMAIN_RAW, &sandbox_alloc);
        PyMem_SetAllocator(PYMEM_DOMAIN_MEM, &sandbox_alloc);
        PyMem_SetAllocator(PYMEM_DOMAIN_OBJ, &sandbox_alloc);
        //PyMem_SetupDebugHooks(); // 启用调试钩子

        // 初始化 Python
        PyConfig config;
        PyConfig_InitPythonConfig(&config);
        std::wstring home = L"Libraries/python/";
        PyConfig_SetString(&config, &config.home, home.c_str());

        PyStatus status = Py_InitializeFromConfig(&config);
        PyConfig_Clear(&config);
        if (PyStatus_Exception(status)) {
            std::cerr << "[Python] 初始化失败: " << status.err_msg << std::endl;
            return false;
        }
        std::cout << "[Python] 已启用沙盒内存模式（" << sandboxSizeMB << " MB）" << std::endl;
    }
    else {
        // -------- 系统模式：直接使用默认分配器 --------
        PyConfig config;
        PyConfig_InitPythonConfig(&config);
        std::wstring home = L"Libraries/python/";
        PyConfig_SetString(&config, &config.home, home.c_str());
        PyStatus status = Py_InitializeFromConfig(&config);
        PyConfig_Clear(&config);
        if (PyStatus_Exception(status)) {
            std::cerr << "[Python] 初始化失败: " << status.err_msg << std::endl;
            return false;
        }

        std::cout << "[Python] 已启用系统内存模式" << std::endl;
    }

    return true;
}

// 关闭 Python
void ShutdownPythonRuntime(TextGameCore* core) {
    Py_Finalize();
    // 沙盒的销毁由 TextGameCore 的生命周期决定，此处不主动销毁
}