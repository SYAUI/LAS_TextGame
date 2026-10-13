#include "TextCore.h"

//初始化虚拟内存
void TextGameCore::IE_malloc_init(int data_size)
{
	//避免存在未释放内容
	if (data) { 
		delete[] data; 
		data = nullptr; 
	}

	if (data_size <= 0) 
		return;

	const size_t overhead = 4 * TLSFAllocator::ALIGN + 256;
	size_t total = (size_t)data_size + overhead;

	//虚拟数据段空间开辟
	data = new char[total];
	tlsf.init(data, total);

	return;
}

//虚拟内存分配
void* TextGameCore::IE_malloc(size_t size)
{
	return tlsf.alloc(size);
}

void* TextGameCore::IE_calloc(size_t nmemb, size_t size)
{
	// 1) 乘法溢出检查
	if (nmemb == 0 || size == 0) return nullptr;
	if (nmemb > SIZE_MAX / size) return nullptr;

	size_t total = nmemb * size;

	// 2) 分配
	void* p = tlsf.alloc(total);
	if (!p) return nullptr;

	// 3) 清零
	std::memset(p, 0, total);
	return p;
}

// 虚拟内存重分配
void* TextGameCore::IE_realloc(void* p, size_t new_size)
{
	// 等价于 IE_malloc(new_size)
	if (!p)
		return tlsf.alloc(new_size);
	
	// 等价于 IE_free(p)，返回 nullptr
	if (new_size == 0) 
	{ 
		tlsf.dealloc(p); 
		return nullptr; 
	}

	return tlsf.realloc(p, new_size);
}

//虚拟内存释放
void TextGameCore::IE_free(void* p)
{
	tlsf.dealloc(p);
}