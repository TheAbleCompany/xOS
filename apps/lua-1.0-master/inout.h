/*
** inout.h
**
** Waldemar Celes Filho
** TeCGraf - PUC-Rio
** 1993年5月11日
**
** Lua 输入输出管理头文件
** 定义了文件操作、调试信息和错误报告的接口
** 提供源代码文件管理和调试支持
*/


#ifndef inout_h
#define inout_h

/* 调试和行号信息 */
extern int lua_linenumber;  /* 当前行号 */
extern int lua_debug;  /* 调试模式标志 */
extern int lua_debugline;  /* 调试行号 */

/* 文件操作函数 */
int  lua_openfile     (char *fn);  /* 打开文件 */
void lua_closefile    (void);  /* 关闭文件 */
int  lua_openstring   (char *s);  /* 打开字符串 */
int  lua_pushfunction (int file, int function);  /* 压入函数 */
void lua_popfunction  (void);  /* 弹出函数 */
void lua_reportbug    (char *s);  /* 报告错误 */

#endif
