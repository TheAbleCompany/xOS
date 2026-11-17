/*
** table.c
** Module to control static tables
** TeCGraf - PUC-Rio
** 11 May 93
**
** Lua 符号表管理模块
** 负责管理 Lua 语言中的各种静态表：
** - 符号表：存储变量名和函数名
** - 常量表：存储字符串常量
** - 字符串表：存储动态创建的字符串
** - 数组表：存储哈希表（关联数组）
** - 文件表：存储文件名信息
*/

#include <stdlib.h>
#include <string.h>

#include "opcode.h"
#include "hash.h"
#include "inout.h"
#include "table.h"
#include "lua.h"

#define streq(s1,s2)	(strcmp(s1,s2)==0)  /* 字符串相等比较宏 */

#ifndef MAXSYMBOL
#define MAXSYMBOL	512  /* 符号表的最大容量 */
#endif
/* 预定义的符号表缓冲区，包含内置函数 */
static Symbol  		tablebuffer[MAXSYMBOL] = {
                                    {"type",{T_CFUNCTION,{lua_type}}},           /* 类型检查函数 */
                                    {"tonumber",{T_CFUNCTION,{lua_obj2number}}}, /* 转换为数值函数 */
                                    {"next",{T_CFUNCTION,{lua_next}}},           /* 哈希表迭代函数 */
                                    {"nextvar",{T_CFUNCTION,{lua_nextvar}}},     /* 全局变量迭代函数 */
                                    {"print",{T_CFUNCTION,{lua_print}}}          /* 打印函数 */
                                                 };
Symbol	       	       *lua_table=tablebuffer;  /* 符号表指针 */
Word   			lua_ntable=5;  /* 当前符号表中的符号数量 */

#ifndef MAXCONSTANT
#define MAXCONSTANT	256
#endif
static char  	       *constantbuffer[MAXCONSTANT] = {"mark","nil","number",
                                                       "string","table",
                                                       "function","cfunction"
                                                      };
char  	      	      **lua_constant = constantbuffer;
Word    		lua_nconstant=T_CFUNCTION+1;

#ifndef MAXSTRING
#define MAXSTRING	512
#endif
static char 	       *stringbuffer[MAXSTRING];
char  		      **lua_string = stringbuffer;
Word    		lua_nstring=0;

#ifndef MAXARRAY
#define MAXARRAY	512
#endif
static Hash             *arraybuffer[MAXARRAY];
Hash  	              **lua_array = arraybuffer;
Word    		lua_narray=0;

#define MAXFILE 	20
char  		       *lua_file[MAXFILE];
int      		lua_nfile;


/*
** 给定一个名称，在符号表中搜索并返回其索引。如果未找到，
** 在表末尾分配，检查溢出并返回其索引。出错时返回-1
*/
int lua_findsymbol (char *s)
{
 int i;
 for (i=0; i<lua_ntable; i++)
  if (streq(s,s_name(i)))
   return i;
 if (lua_ntable >= MAXSYMBOL-1)
 {
  lua_error ("symbol table overflow");
  return -1;
 }
 s_name(lua_ntable) = strdup(s);
 if (s_name(lua_ntable) == NULL)
 {
  lua_error ("not enough memory");
  return -1;
 }
 s_tag(lua_ntable++) = T_NIL;
 
 return (lua_ntable-1);
}

/*
** 给定一个常量字符串，去除其分隔符("或')，在常量表中搜索
** 并返回其索引。如果未找到，在表末尾分配，检查溢出并返回其索引
**
** 对于每次分配，函数分配一个额外的字符用于标记使用的字符串
** （这对于统一处理常量和字符串是必要的）。函数将分配的第二个位置
** 存储在表中，代表实际字符串的开始。出错时返回-1
*/
int lua_findenclosedconstant (char *s)
{
 int i, j, l=strlen(s);
 char *c = calloc (l, sizeof(char)); 	/* make a copy */
 
 c++;		/* create mark space */

 /* introduce scape characters */ 
 for (i=1,j=0; i<l-1; i++)
 {
  if (s[i] == '\\')
  {
   switch (s[++i])
   {
    case 'n': c[j++] = '\n'; break;
    case 't': c[j++] = '\t'; break;
    case 'r': c[j++] = '\r'; break;
    default : c[j++] = '\\'; c[j++] = c[i]; break;
   }
  }
  else
   c[j++] = s[i];
 }
 c[j++] = 0;
 
 for (i=0; i<lua_nconstant; i++)
  if (streq(c,lua_constant[i]))
  {
   free (c-1);
   return i;
  }
 if (lua_nconstant >= MAXCONSTANT-1)
 {
  lua_error ("lua: constant string table overflow"); 
  return -1;
 }
 lua_constant[lua_nconstant++] = c;
 return (lua_nconstant-1);
}

/*
** 给定一个常量字符串，在常量表中搜索并返回其索引
** 如果未找到，在表末尾分配，检查溢出并返回其索引
**
** 对于每次分配，函数分配一个额外的字符用于标记使用的字符串
** （这对于统一处理常量和字符串是必要的）。函数将分配的第二个位置
** 存储在表中，代表实际字符串的开始。出错时返回-1
*/
int lua_findconstant (char *s)
{
 int i;
 for (i=0; i<lua_nconstant; i++)
  if (streq(s,lua_constant[i]))
   return i;
 if (lua_nconstant >= MAXCONSTANT-1)
 {
  lua_error ("lua: constant string table overflow"); 
  return -1;
 }
 {
  char *c = calloc(strlen(s)+2,sizeof(char));
  c++;		/* create mark space */
  lua_constant[lua_nconstant++] = strcpy(c,s);
 }
 return (lua_nconstant-1);
}


/*
** 如果对象是字符串或未标记的数组，则标记该对象
*/
void lua_markobject (Object *o)
{
 if (tag(o) == T_STRING)
  lua_markstring (svalue(o)) = 1;
 else if (tag(o) == T_ARRAY && markarray(avalue(o)) == 0)
   lua_hashmark (avalue(o));
}

/*
** 标记符号表中存储的任何对象使用的所有字符串和数组
*/
static void lua_marktable (void)
{
 int i;
 for (i=0; i<lua_ntable; i++)
  lua_markobject (&s_object(i));
} 

/*
** 模拟垃圾收集。当字符串表或数组表溢出时，
** 此函数检查所有分配的字符串和数组是否都在使用中。
** 如果有未使用的，则打包（压缩）表
*/
static void lua_pack (void)
{
 lua_markstack ();
 lua_marktable ();
 
 { /* pack string */
  int i, j;
  for (i=j=0; i<lua_nstring; i++)
   if (lua_markstring(lua_string[i]) == 1)
   {
    lua_string[j++] = lua_string[i];
    lua_markstring(lua_string[i]) = 0;
   }
   else
   {
    free (lua_string[i]-1);
   }
  lua_nstring = j;
 }

 { /* pack array */
  int i, j;
  for (i=j=0; i<lua_narray; i++)
   if (markarray(lua_array[i]) == 1)
   {
    lua_array[j++] = lua_array[i];
    markarray(lua_array[i]) = 0;
   }
   else
   {
    lua_hashdelete (lua_array[i]);
   }
  lua_narray = j;
 }
}

/*
** 在字符串表中分配一个新字符串。给定的字符串已经
** 分配了标记空间，函数将其放在表的末尾，检查溢出，
** 并返回其自身指针，出错时返回NULL
*/
char *lua_createstring (char *s)
{
 if (s == NULL) return NULL;
 
 if (lua_nstring >= MAXSTRING-1)
 {
  lua_pack ();
  if (lua_nstring >= MAXSTRING-1)
  {
   lua_error ("string table overflow");
   return NULL;
  }
 } 
 lua_string[lua_nstring++] = s;
 return s;
}

/*
** 在数组表中分配一个已创建的新数组。函数将其放在表的末尾，
** 检查溢出，并返回其自身指针，出错时返回NULL
*/
void *lua_createarray (void *a)
{
 if (a == NULL) return NULL;
 
 if (lua_narray >= MAXARRAY-1)
 {
  lua_pack ();
  if (lua_narray >= MAXARRAY-1)
  {
   lua_error ("array table overflow");
   return NULL;
  }
 } 
 lua_array[lua_narray++] = a;
 return a;
}


/*
** 在文件表中添加文件名，检查溢出。此函数还将外部变量
** "lua_filename"设置为函数文件名集。成功返回0，出错返回1
*/
int lua_addfile (char *fn)
{
 if (lua_nfile >= MAXFILE-1)
 {
  lua_error ("too many files");
  return 1;
 }
 if ((lua_file[lua_nfile++] = strdup (fn)) == NULL)
 {
  lua_error ("not enough memory");
  return 1;
 }
 return 0;
}

/*
** 返回最后设置的文件名
*/
char *lua_filename (void)
{
 return lua_file[lua_nfile-1];
}

/*
** 内部函数：返回下一个全局变量
*/
void lua_nextvar (void)
{
 int index;
 Object *o = lua_getparam (1);
 if (o == NULL)
 { lua_error ("too few arguments to function `nextvar'"); return; }
 if (lua_getparam (2) != NULL)
 { lua_error ("too many arguments to function `nextvar'"); return; }
 if (tag(o) == T_NIL)
 {
  index = 0;
 }
 else if (tag(o) != T_STRING) 
 { lua_error ("incorrect arguments to function `nextvar'"); return; }
 else
 {
  for (index=0; index<lua_ntable; index++)
   if (streq(s_name(index),svalue(o))) break;
  if (index == lua_ntable) 
  {
   lua_error ("name not found in function `nextvar'");
   return;
  }
  index++;
  while (index < lua_ntable-1 && tag(&s_object(index)) == T_NIL) index++;
  
  if (index == lua_ntable-1)
  {
   lua_pushnil();
   lua_pushnil();
   return;
  }
 }
 {
  Object name;
  tag(&name) = T_STRING;
  svalue(&name) = lua_createstring(lua_strdup(s_name(index)));
  if (lua_pushobject (&name)) return;
  if (lua_pushobject (&s_object(index))) return;
 }
}
