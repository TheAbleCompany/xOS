/*
** lua.c
** 面向应用用户的语言
** TeCGraf - PUC-Rio
** 1993年4月28日
**
** Lua 主程序文件
** 这是 Lua 语言的解释器入口，负责初始化环境和执行 Lua 脚本
*/

#include <stdio.h>

#include "lua.h"
#include "lualib.h"


/*
** 测试函数
** 这是一个示例函数，用于演示如何从 C 代码中调用 Lua 函数
** 获取第一个参数并调用名为 "c" 的 Lua 函数
*/
void test (void)
{
  lua_pushobject(lua_getparam(1));  /* 将第一个参数压入栈中 */
  lua_call ("c", 1);                /* 调用 Lua 中的 "c" 函数，参数个数为 1 */
}


/*
** 调用函数
** 从参数中获取函数名并调用对应的 Lua 函数
** 参数必须是字符串类型
*/
static void callfunc (void)
{
 lua_Object obj = lua_getparam (1);     /* 获取第一个参数 */
 if (lua_isstring(obj))                  /* 检查参数是否为字符串 */
   lua_call(lua_getstring(obj),0);       /* 如果是字符串，调用对应的 Lua 函数 */
}

/*
** 执行字符串
** 从参数中获取 Lua 代码字符串并执行
** 参数必须是包含 Lua 代码的字符串
*/
static void execstr (void)
{
 lua_Object obj = lua_getparam (1);       /* 获取第一个参数 */
 if (lua_isstring(obj))                    /* 检查参数是否为字符串 */
   lua_dostring(lua_getstring(obj));     /* 如果是字符串，将其作为 Lua 代码执行 */
}

/*
** 主函数
** Lua 解释器的入口点
** 参数：argc - 命令行参数个数，argv - 命令行参数数组
** 用法：lua 文件名 [函数名1 函数名2 ...]
*/
int main (int argc, char *argv[])
{
 int i;
 /* 检查命令行参数，至少需要提供一个 Lua 文件名 */
 if (argc < 2)
 {
  puts ("usage: lua filename [function1 function2 ...]");  /* 显示用法信息 */
  return 1;                                      /* 参数不足，返回错误码 */
 }
 
 /* 注册 C 函数到 Lua 环境中，使其可以在 Lua 脚本中调用 */
 lua_register ("callfunc", callfunc);  /* 注册 callfunc 函数 */
 lua_register ("execstr", execstr);      /* 注册 execstr 函数 */
 lua_register ("test", test);            /* 注册 test 函数 */
 
 /* 打开标准库，提供基本功能 */
 iolib_open ();    /* 打开 I/O 库，提供输入输出功能 */
 strlib_open ();   /* 打开字符串库，提供字符串处理功能 */
 mathlib_open ();  /* 打开数学库，提供数学运算功能 */
 
 /* 执行指定的 Lua 文件 */
 lua_dofile (argv[1]);  /* 加载并执行第一个参数指定的 Lua 文件 */
 
 /* 如果命令行中指定了函数名，依次调用这些函数 */
 for (i=2; i<argc; i++)
 {
  lua_call (argv[i],0);  /* 调用第 i 个参数指定的 Lua 函数 */
 }
 return 0;  /* 程序正常结束 */
}


