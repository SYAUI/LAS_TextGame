#pragma once
#include <string>
#include <vector>
#include "TLSFAllocator.h"

class TextGameCore
{
public:
	//构造函数
	TextGameCore();
	//解析函数
	~TextGameCore();

	static struct mem_block
	{
		bool is_free;
		bool last_is_free;
		uintptr_t blocksize;//块大小
		uintptr_t next_block;
	};
	int block_size;

	/* 脚本函数 */
	void LaunchGame();//启动游戏
	int LoadGameLevel(std::string path);//载入关卡数据

	/* 游戏逻辑 */


	/* 公开内存操作 */
	void VMemoryDump() const;
	void IE_malloc_init(int data_size);
	void* IE_malloc(size_t size);
	void* IE_calloc(size_t nmemb, size_t size);
	void* IE_realloc(void* p, size_t new_size);
	void IE_free(void* p);

private:

	TLSFAllocator tlsf;
	char* data = nullptr; // 底层大块内存

	std::vector<std::string> level_src;

};


// Cpp原型操作

bool _speak(std::string text);
bool _textprint(std::string text[], int size);
bool _load(std::string path);


// Python 脚本相关
bool RunScriptFile(const std::string& filepath);