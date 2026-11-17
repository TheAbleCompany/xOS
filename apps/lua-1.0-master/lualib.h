/*
** Lua程序中使用的库
** 图形计算技术组
** TeCGraf - PUC-Rio
** 1993年5月19日
**
** Lua 标准库头文件
** 声明了Lua标准库的初始化函数
** 包含I/O库、字符串库和数学库的接口
*/

#ifndef lualib_h
#define lualib_h

void iolib_open   (void);  /* 初始化I/O库 */
void strlib_open  (void);  /* 初始化字符串库 */
void mathlib_open (void);  /* 初始化数学库 */

#endif
