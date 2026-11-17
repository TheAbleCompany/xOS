/*
** table.c
** 静态表控制模块
** TeCGraf - PUC-Rio
** 1993年5月11日
**
** Lua 静态表管理头文件
** 定义了符号表、常量表、字符串表、数组表和文件表的管理接口
** 提供静态表的查找、创建和管理功能
*/

#ifndef table_h
#define table_h

/* 符号表相关变量 */
extern Symbol *lua_table;  /* 符号表数组 */
extern Word    lua_ntable;  /* 符号表项数 */

/* 常量表相关变量 */
extern char  **lua_constant;  /* 常量字符串表 */
extern Word    lua_nconstant;  /* 常量数量 */

/* 字符串表相关变量 */
extern char  **lua_string;  /* 字符串表 */
extern Word    lua_nstring;  /* 字符串数量 */

/* 数组表相关变量 */
extern Hash  **lua_array;  /* 数组表 */
extern Word    lua_narray;  /* 数组数量 */

/* 文件表相关变量 */
extern char   *lua_file[];  /* 文件名数组 */
extern int     lua_nfile;  /* 文件数量 */

/* 字符串标记宏 */
#define lua_markstring(s)	(*((s)-1))  /* 获取字符串的标记字节 */


int   lua_findsymbol           (char *s);
int   lua_findenclosedconstant (char *s);
int   lua_findconstant         (char *s);
void  lua_markobject           (Object *o);
char *lua_createstring         (char *s);
void *lua_createarray          (void *a);
int   lua_addfile              (char *fn);
char *lua_filename             (void);
void  lua_nextvar              (void);

#endif
