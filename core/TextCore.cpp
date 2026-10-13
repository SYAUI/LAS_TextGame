#include "TextCore.h"
#include <stdio.h>
#include <sstream>
#include <fstream>



TextGameCore::TextGameCore()
{
}

TextGameCore::~TextGameCore()
{
    if (data) 
    { 
        delete[] data; 
        data = nullptr;
    }
}

//内存转储
void TextGameCore::VMemoryDump() const
{
    FILE* fp;
    fopen_s(&fp, "IE.bin", "wb");
    if (!fp)
        return;
    fwrite(data, sizeof(char), strlen(data), fp);
    fclose(fp);
    return;
}


int TextGameCore::LoadGameLevel(std::string path = "map_main.tgl")
{
    std::ifstream ifs;
    ifs.open("Levels/" + path);
    if (!ifs.is_open())
        return -1;//文件不存在或无法打开
    
    //逐行读取文本
    level_src.clear();
    std::string str_line;
    while (std::getline(ifs, str_line)) {
        level_src.push_back(str_line);
    }
    
    

}


void TextGameCore::LaunchGame()
{
}

bool _speak(std::string text)
{
    printf(text.c_str());
    return true;
}

bool _textprint(std::string text[], int size)
{
    for (size_t i = 0; i < size; i++)
    {
        printf(text[i].c_str());
    }
    return true;
}

bool _load(std::string path)
{
    printf(path.c_str());
    return true;
}
