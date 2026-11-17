/*
** strlib.c
** String library to LUA
**
** Waldemar Celes Filho
** TeCGraf - PUC-Rio
** 19 May 93
**
** Lua 字符串库
** 提供 Lua 语言的基本字符串处理功能
** 包括字符串查找、长度计算、子串提取、大小写转换等操作
*/

#include <stdlib.h>
#include <string.h>
#include <ctype.h>


#include "lua.h"

/*
** 字符串查找函数
** 返回子串在字符串中第一次出现的位置
** Lua 接口：n = strfind (string, substring)
** 参数：string - 要搜索的字符串，substring - 要查找的子串
** 返回：子串的起始位置（1-based），如果未找到返回0
*/
static void str_find (void)
{
 int n;
 char *s1, *s2;
 lua_Object o1 = lua_getparam (1);  /* 获取第一个参数（要搜索的字符串） */
 lua_Object o2 = lua_getparam (2);  /* 获取第二个参数（要查找的子串） */
 if (!lua_isstring(o1) || !lua_isstring(o2))  /* 检查参数类型是否为字符串 */
 { lua_error ("incorrect arguments to function `strfind'"); return; }
 s1 = lua_getstring(o1);  /* 获取字符串指针 */
 s2 = lua_getstring(o2);  /* 获取子串指针 */
 n = strstr(s1,s2) - s1 + 1;  /* 计算子串位置（转换为1-based索引） */
 lua_pushnumber (n);  /* 返回位置值 */
}

/*
** Return the string length
** LUA interface:
**			n = strlen (string)
*/
static void str_len (void)
{
 lua_Object o = lua_getparam (1);
 if (!lua_isstring(o))
 { lua_error ("incorrect arguments to function `strlen'"); return; }
 lua_pushnumber(strlen(lua_getstring(o)));
}


/*
** Return the substring of a string, from start to end
** LUA interface:
**			substring = strsub (string, start, end)
*/
static void str_sub (void)
{
 int start, end;
 char *s;
 lua_Object o1 = lua_getparam (1);
 lua_Object o2 = lua_getparam (2);
 lua_Object o3 = lua_getparam (3);
 if (!lua_isstring(o1) || !lua_isnumber(o2) || !lua_isnumber(o3))
 { lua_error ("incorrect arguments to function `strsub'"); return; }
 s = strdup (lua_getstring(o1));
 start = lua_getnumber (o2);
 end = lua_getnumber (o3);
 if (end < start || start < 1 || end > strlen(s))
  lua_pushstring ("");
 else
 {
  s[end] = 0;
  lua_pushstring (&s[start-1]);
 }
 free (s);
}

/*
** Convert a string to lower case.
** LUA interface:
**			lowercase = strlower (string)
*/
static void str_lower (void)
{
 char *s, *c;
 lua_Object o = lua_getparam (1);
 if (!lua_isstring(o))
 { lua_error ("incorrect arguments to function `strlower'"); return; }
 c = s = strdup(lua_getstring(o));
 while (*c != 0)
 {
  *c = tolower(*c);
  c++;
 }
 lua_pushstring(s);
 free(s);
} 


/*
** Convert a string to upper case.
** LUA interface:
**			uppercase = strupper (string)
*/
static void str_upper (void)
{
 char *s, *c;
 lua_Object o = lua_getparam (1);
 if (!lua_isstring(o))
 { lua_error ("incorrect arguments to function `strupper'"); return; }
 c = s = strdup(lua_getstring(o));
 while (*c != 0)
 {
  *c = toupper(*c);
  c++;
 }
 lua_pushstring(s);
 free(s);
} 


/*
** Open string library
*/
void strlib_open (void)
{
 lua_register ("strfind", str_find);
 lua_register ("strlen", str_len);
 lua_register ("strsub", str_sub);
 lua_register ("strlower", str_lower);
 lua_register ("strupper", str_upper);
}
