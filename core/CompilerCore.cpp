#include <fstream>
#include <map>

// 配置
//bool IsAllNumDouble = true;
//bool IsDebugMode = false;


using namespace std;

//  符号类型
enum TokenType {
    NONE, NUM = 128, ID, CHOOSE, ELSE, IF, PRINT,

    STRING, INT, BOOL,

    Assign, Cond, Lor, Lan, Or, Xor, And, Eq, Ne, Lt, Gt, Le, Ge, Shl, Shr, Add, Sub, Mul, Div, Mod, Inc, Dec,

};

struct TokenStruct
{
    TokenType type;
    char *value;
};



map<string, TokenType> sym=
{
    {"choose",TokenType::CHOOSE },
    {"else",TokenType::ELSE },
    {"if",TokenType::IF },
    {"print",TokenType::PRINT },
    {"char",TokenType::STRING },
    {"num",TokenType::INT },
    {"bool",TokenType::BOOL }
    

};

char *CC_src, *CC_data;
int line = 0;

static string idstr; // 当前标识符
double numval;// 符号对应值

void CC_SyntaxError()
{
   
}


//	词法分析

static TokenType GetToken()// 标识符获取
{	
    char* p = CC_src;

    static char tok = ' ';
    

    while (tok = *p)// 跳过空格
    {
        ++p;
        if (tok == '\n') {
          line++;
        }else
          break;
    }

    // 首位为字母
    if (isalpha(tok))
    {
        idstr = tok;
        while (isalnum(tok = *(p++)))
            idstr += tok;
        // 获取token
        map<string, TokenType>::iterator iter = sym.find(idstr);
        if (iter != sym.end()) 
            return iter->second;
        else //找不到，保存新的标识符到符号表中
        {
            sym.insert(pair<string, TokenType>(idstr, TokenType::ID));
            return TokenType::ID;
        }   
    }
    // 首位为数字
    else if (isdigit(tok))
    {
        string valstr;
        if(tok != '0') // 非0，认定为十进制
        {
            while (isdigit(tok) || tok == '.')
            {
                valstr += tok;
                tok = *(p++);
            }
            numval = strtod(valstr.c_str(), nullptr);// 默认为double
        }
        else if (tok = *(p++) == 'x')// 为0，且带有x，认定为十六进制
        {
            while (isalnum(tok = *(p++)))
                numval = numval * 16 + (tok & 15) + (tok >= 'A' ? 9 : 0); // 转化为10进制
        }
        else
        {
            // 异常处理
            printf("error: Invalid value in line %d",line);
            return TokenType::NONE;
        }
        return TokenType::NUM;
    }
    // 处理注释
    else if (tok == '/')
    {
        tok = *p;
        if (tok == '/') // 单行注释
            while (*p != 0 && *p != '\n') ++p;
        else if(tok == '*')// 多行注释
            while (*p != 0) if (*(++p) == '*') if (*(++p) == '/') break; 
        else
            return TokenType::Div; // 为除号
    }
    // 处理字符串
    else if (tok == '\'' || tok == '"') { // 单双引号开头，默认字符串
        char *str_p = CC_data;
        while (*p != 0 && *p != tok) {//直到找到匹配的引号为止
            if ((numval = *p++) == '\\') {	// 解析转义字符
                if ((numval = *p++) == 'n') numval = '\n';// '\n' 认为是'\n' 其他直接忽略'\'转义
            }
            if (tok == '"') *CC_data++ = tok;//如果是双引号,认为是字符串,向data拷贝字符
        }
        ++p;
        if (tok == '"') 
        {    
            numval = (int)str_p; 
            return TokenType::STRING;
        }
        else return TokenType::NUM;// 单引号则认为是数字
        
    }





    return TokenType::NONE;
}





int test(string path)
{
  ifstream ifs;
  ifs.open(path);
  if (!ifs.is_open())
    return -1;
  
  ifs.seekg(0, std::ios::end);    
  int length = ifs.tellg();
  ifs.seekg(0, std::ios::beg);
  CC_src = new char[length];
  ifs.read(CC_src, length);
  ifs.close();                    
  

  // GetToken(); //解析测试



}

int main()
{
    test("Levels/map_main.tgl");

}





int IE_PackAssets(string path)
{
    ifstream ifs;
    ifs.open(path);
    if (!ifs.is_open())
        return -1;//文件不存在或无法打开




    ifs.close();
    return 0;
}