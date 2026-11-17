/*
** inout.c
** 提供实现输入/输出功能和调试器支持的函数
**
** Waldemar Celes Filho
** TeCGraf - PUC-Rio
** 1993年5月11日
**
** 输入输出处理模块
** 提供 Lua 语言的输入输出功能和调试器支持
** 处理文件输入、字符串输入、错误报告等功能
*/

#include <stdio.h>
#include <string.h>

#include "opcode.h"
#include "hash.h"
#include "inout.h"
#include "table.h"

/* 导出的全局变量 */
int lua_linenumber;   /* 当前处理的行号 */
int lua_debug;        /* 调试模式标志 */
int lua_debugline;    /* 调试行号 */

/* 内部变量 */
#ifndef MAXFUNCSTACK
#define MAXFUNCSTACK 32  /* 函数调用栈的最大深度 */
#endif
static struct { int file; int function; } funcstack[MAXFUNCSTACK];  /* 函数调用栈 */
static int nfuncstack=0;  /* 当前函数调用栈的深度 */

static FILE *fp;           /* 当前输入文件指针 */
static char *st;           /* 当前输入字符串指针 */
static void (*usererror) (char *s);  /* 用户自定义错误处理函数指针 */

/*
** 设置用户函数来处理错误的函数
*/
void lua_errorfunction (void (*fn) (char *s))
{
 usererror = fn;
}

/*
** 从输入文件中获取下一个字符的函数
*/
static int fileinput (void)
{
 int c = fgetc (fp);
 return (c == EOF ? 0 : c);
}

/*
** 将下一个字符退回到输入文件的函数
*/
static void fileunput (int c)
{
 ungetc (c, fp);
}

/*
** 从输入字符串中获取下一个字符的函数
*/
static int stringinput (void)
{
 st++;
 return (*(st-1));
}

/*
** 将下一个字符退回到输入字符串的函数
*/
static void stringunput (int c)
{
 st--;
}

/*
** 打开文件作为输入单元的函数
** 成功返回0，出错返回1
*/
int lua_openfile (char *fn)
{
 lua_linenumber = 1;
 lua_setinput (fileinput);
 lua_setunput (fileunput);
 fp = fopen (fn, "r");
 if (fp == NULL) return 1;
 if (lua_addfile (fn)) return 1;
 return 0;
}

/*
** 关闭已打开文件的函数
*/
void lua_closefile (void)
{
 if (fp != NULL)
 {
  fclose (fp);
  fp = NULL;
 }
}

/*
** 打开字符串作为输入单元的函数
*/
int lua_openstring (char *s)
{
 lua_linenumber = 1;
 lua_setinput (stringinput);
 lua_setunput (stringunput);
 st = s;
 {
  char sn[64];
  sprintf (sn, "String: %10.10s...", s);
  if (lua_addfile (sn)) return 1;
 }
 return 0;
}

/*
** 如果已注册，调用用户函数处理错误消息。否则使用标准函数(fprintf)报告错误
*/
void lua_error (char *s)
{
 if (usererror != NULL) usererror (s);
 else				    fprintf (stderr, "lua: %s\n", s);
}

/*
** 执行SETFUNCTION操作码时调用，此函数将函数压入函数栈
** 成功返回0，出错返回1
*/
int lua_pushfunction (int file, int function)
{
 if (nfuncstack >= MAXFUNCSTACK-1)
 {
  lua_error ("function stack overflow");
  return 1;
 }
 funcstack[nfuncstack].file = file;
 funcstack[nfuncstack].function = function;
 nfuncstack++;
 return 0;
}

/*
** 执行RESETFUNCTION操作码时调用，此函数从函数栈弹出函数
** 成功返回0，出错返回1
*/
void lua_popfunction (void)
{
 nfuncstack--;
}

/*
** 构建错误消息并将其发送给lua_error函数的bug报告
*/
void lua_reportbug (char *s)
{
 char msg[1024];
 strcpy (msg, s);
 if (lua_debugline != 0)
 {
  int i;
  if (nfuncstack > 0)
  {
   sprintf (strchr(msg,0), 
         "\n\tin statement begining at line %d in function \"%s\" of file \"%s\"",
         lua_debugline, s_name(funcstack[nfuncstack-1].function),
  	 lua_file[funcstack[nfuncstack-1].file]);
   sprintf (strchr(msg,0), "\n\tactive stack\n");
   for (i=nfuncstack-1; i>=0; i--)
    sprintf (strchr(msg,0), "\t-> function \"%s\" of file \"%s\"\n", 
                            s_name(funcstack[i].function),
			    lua_file[funcstack[i].file]);
  }
  else
  {
   sprintf (strchr(msg,0),
         "\n\tin statement begining at line %d of file \"%s\"", 
         lua_debugline, lua_filename());
  }
 }
 lua_error (msg);
}

