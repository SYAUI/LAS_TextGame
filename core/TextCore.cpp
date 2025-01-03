#include "TextCore.h"
#include <stdio.h>
#include <sstream>
#include <fstream>



TextGameCore::TextGameCore(int data_size)
{
    IE_malloc_init(data_size);

}

TextGameCore::~TextGameCore()
{
    delete[]data;


}

//内存转储
void TextGameCore::VMemoryDump()
{
    FILE* fp;
    fopen_s(&fp, "IE.dmp", "wb");
    fwrite(data, sizeof(char), strlen(data), fp);
    fclose(fp);
}


int TextGameCore::InitGame(int data_size)
{
    //避免存在未释放内容
    if (!data)
        delete []data;
    //虚拟数据段空间开辟
    data = new char[data_size + 1];



	return 0;
}

int TextGameCore::LoadGameLevel(std::string path = "map_main.tgl")
{
    std::ifstream ifs;
    ifs.open("Levels/" + path);
    if (!ifs.is_open())
        return -1;//文件不存在或无法打开
    
    //逐行读取文本
    map_src.clear();
    std::string str_line;
    while (std::getline(ifs, str_line)) {
        map_src.push_back(str_line);
    }
    
    

}


void TextGameCore::LaunchGame()
{
}




int main()
{
    TextGameCore TGC(1024);
    //IE_malloc_init(1024);
    TGC.VMemoryDump();

    return 0;
}
