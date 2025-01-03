#include "TextCore.h"

// 内存对齐
#define ALIGNMENT 4 
// 内存块合并最大阈值
#define SIZE_THRESHOLD 1024

//尾块生成
void TextGameCore::MakeBlockTail(mem_block *block_head)
{
	mem_block* block_tail = (mem_block*)(block_head->next_block - block_size);
	block_tail->is_free = block_head->is_free;
	block_tail->last_is_free = block_head->last_is_free;
	block_tail->blocksize = block_head->blocksize;

	//注意，尾块的next指针指向头块
	block_tail->next_block = (uintptr_t)block_head;
	return;
}

// 空闲块融合
void TextGameCore::_MergeFreeBlock(mem_block * current_block)
{
	//检查上一个块是否空闲
	if (current_block->last_is_free)
	{
		mem_block* temp_block = (mem_block*)((uintptr_t)(current_block - block_size));
		//如果空闲，读取尾块，由尾块推算出头块
		temp_block = (mem_block*)temp_block->next_block;

		//将当前块大小合并,并继承下一块地址
		temp_block->blocksize += current_block->blocksize + block_size;//大小还需要加上被合并的头块
		temp_block->next_block = current_block->next_block;

		//新尾块生成
		MakeBlockTail(temp_block);

		//读取下一块,并标记上一块为空闲。
		temp_block = (mem_block*)temp_block->next_block;
		temp_block->last_is_free = true;
	}
	else //上一块不为空，检查下一块是否空闲
	{
		mem_block* next_block = (mem_block*)current_block->next_block;
		if (next_block->is_free)
		{
			//下一块空闲，由当前块与它合并
			current_block->blocksize += next_block->blocksize + block_size;//大小还需要加上被合并的头块
			current_block->next_block = next_block->next_block;

			//新尾块生成
			MakeBlockTail(current_block);
			//无需读取下一块，下一块对上一块标记肯定是空闲
		}
	}
	return;
}

// 空闲块遍历融合
void TextGameCore::_MergeFreeBlockForEach(uintptr_t start, uintptr_t end)
{
	mem_block* current_block = (mem_block*)start, * next_block = nullptr;
	uintptr_t mem_loc = current_block->next_block, tmp_i = 0;
	
	while (mem_loc < end)
	{
		next_block = (mem_block*)current_block->next_block;
		if (next_block->is_free)
		{
			MERGELOOP:// <----------------------------------------------------
			//下一块空闲，由当前块与它合并,大小还需要加上被合并的头块		 |
			current_block->blocksize += next_block->blocksize + block_size;//|
			current_block->next_block = next_block->next_block;//			 |
			next_block = (mem_block*)mem_loc;//								 |
			mem_loc = next_block->next_block;//定位下一块					 |
			if (next_block->is_free)//										 |
				goto MERGELOOP;//---------------------------------------------
			else //新尾块生成
			{
				MakeBlockTail(current_block);
				current_block = next_block;
			}
		}
		else
		{
			current_block = next_block;
			mem_loc = next_block->next_block;
		}
	}

}

// 空闲块查找
void* TextGameCore::_GetFreeBlock(size_t size)
{
	
	mem_block* current_block = nullptr;
	uintptr_t mem_loc = 0, tmp_i = 0;
	void* mem_ptr = nullptr;
	
	//根据申请大小判断用FF还是NF
	bool Is_FF = size < SIZE_THRESHOLD;
	if (Is_FF)
		mem_loc = IE_memory_start;//小于阈值都使用首次适应算法(First Fix),进行顺序搜索
	else
		mem_loc = last_loc;//大于阈值使用循环适应算法(Next Fix)


	LOOP:while (mem_loc < IE_memory_end)
	{
		current_block = (mem_block*)mem_loc;
		if (current_block->is_free == true && current_block->blocksize >= size)//空闲块大小是否符合要求
		{
			if (current_block->blocksize == size)
			{
				current_block->is_free = false;
				mem_ptr = (void*)(mem_loc + block_size);//将块的头部排除
				last_loc = mem_loc;
				break;
			}
			else //可用空间大于需求
			{
				//获取新块地址
				tmp_i = current_block->blocksize + size + block_size;
				//确认新块地址没有溢出
				if (tmp_i + block_size < IE_memory_end)
				{

					// |		当  前  块		  | 后块 |
					// | 当前块	| 新块头 | 新块尾 | 后块 |

					//拆分当前块，创建新块并设置索引
					current_block->next_block = tmp_i;
					mem_block* new_block = (mem_block*)tmp_i;

					//设置新块参数
					new_block->is_free = true;
					new_block->last_is_free = false;
					new_block->blocksize = current_block->blocksize - size - block_size;
					new_block->next_block = current_block->next_block;

					//为新块末端添加尾块
					MakeBlockTail(new_block);

					//设置当前块参数
					current_block->is_free = false;
					current_block->blocksize = size;
					mem_ptr = (void*)(mem_loc + block_size);//将块的头部排除
					last_loc = mem_loc;
					break;
				}
				else
				{
					break; //新块内存溢出，分配失败
				}
			}
		}
		else
		{
			mem_loc = current_block->next_block;
		}

	}

	//内存分配失败
	if (!mem_ptr)
	{
		if (!Is_FF)//如果之前尝试的是循环适应则改为首次适应
		{
			//进行空闲块遍历融合
			_MergeFreeBlockForEach(IE_memory_start, IE_memory_end);
			Is_FF = true;
			mem_loc = IE_memory_start;
			goto LOOP;
		}
	}

	return mem_ptr;
}

//初始化虚拟内存
void TextGameCore::IE_malloc_init(int data_size)
{
	//避免存在未释放内容
	if (!data)
		delete[]data;
	//虚拟数据段空间开辟
	data = new char[data_size];
	block_size = sizeof(mem_block);

	//设置开头和结尾实际物理地址
	last_loc = IE_memory_start = (uintptr_t)data;
	IE_memory_end = IE_memory_start + data_size;

	mem_block* mem_head = (mem_block*)IE_memory_start;
	mem_block* mem_tail = (mem_block*)(IE_memory_end - block_size);

	//头部块数据
	mem_head->is_free = true;
	mem_tail->last_is_free = false;
	mem_head->blocksize = data_size - block_size;//排除自身后所有大小
	mem_head->next_block = IE_memory_end;


	//尾部块数据
	MakeBlockTail(mem_head);
	mem_tail->last_is_free = true;


	return;

}

//虚拟内存分配
void* TextGameCore::IE_malloc(size_t size)
{
	void* ptr = nullptr;
	if (!size)	return ptr; //不合法大小，直接返回空指针。
	//进行内存对齐
	size = (size % ALIGNMENT == 0) ? size : ((size / ALIGNMENT) + 1) * size;

	ptr = _GetFreeBlock(size);


	return ptr;
}

//虚拟内存释放
void TextGameCore::IE_free(void* p)
{
	//检查是为空指针
	if (p) 
	{
		mem_block* current_block = (mem_block*)p;
		//标记空闲
		current_block->is_free = true;
		//空闲块融合
		if (current_block->blocksize < SIZE_THRESHOLD)
			_MergeFreeBlock(current_block);

		current_block = nullptr;
	}
	return;
}