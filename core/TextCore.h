#pragma once
#include <string>
#include <vector>

class TextGameCore
{
public:
	//构造函数
	TextGameCore(int data_size);
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
	int InitGame(int data_size);//初始化游戏
	void LaunchGame();//启动游戏
	int LoadGameLevel(std::string path);//载入关卡数据

	/* 公开内存操作 */
	void VMemoryDump();
	void IE_malloc_init(int data_size);
	void* IE_malloc(size_t size);
	void IE_free(void* p);
private:
	char* data = nullptr;
	std::vector<std::string> map_src;
	uintptr_t IE_memory_start = 0, IE_memory_end = 0, last_loc = 0;

	/* 内部内存分配函数 */
	void* _GetFreeBlock(size_t size);
	void _MergeFreeBlock(mem_block* current_block);
	void _MergeFreeBlockForEach(uintptr_t start, uintptr_t end);
	void MakeBlockTail(mem_block* block_head);

};