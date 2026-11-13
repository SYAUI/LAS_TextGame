#pragma once
#include <string>
#include <vector>
#include "TLSFAllocator.h"
#include "TextGameAPI.h"

namespace textgame {

	class TextGameCoreImpl {
	public:
		GameConfig cfg;
		bool       pyInitialized = false;
		bool       initialized = false;

		// 主线程的 PyThreadState*，由 PythonRuntime 管理，Init 后保存，Shutdown 前恢复
		void* mainThreadState = nullptr;

		// 沙盒内存
		TLSFAllocator tlsf;
		char* data = nullptr;   // 底层大块内存
		int   dataSize = 0;         // 字节

		// 关卡
		std::vector<std::string> levelSrc;

		// 内存接口
		void  MemInit(int bytes);
		void  MemClose();

		void* Malloc(size_t n) { return tlsf.valid() ? tlsf.alloc(n) : nullptr; }
		void* Calloc(size_t n, size_t sz);
		void* Realloc(void* p, size_t sz) { return tlsf.valid() ? tlsf.realloc(p, sz) : nullptr; }
		void  Free(void* p) { if (p && tlsf.valid()) tlsf.dealloc(p); }

		bool  MemReady() const { return data != nullptr && tlsf.valid(); }
	};

} // namespace textgame