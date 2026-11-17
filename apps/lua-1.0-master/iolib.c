/*
** iolib.c
** Input/output library to LUA
**
** Waldemar Celes Filho
** TeCGraf - PUC-Rio
** 19 May 93
**
** Lua 输入输出库
** 提供 Lua 语言的文件操作和输入输出功能
** 包括文件读写、格式化输入输出、系统命令执行等
*/

#include <stdlib.h>
#include <string.h>
#include <stdio.h>
//#include <ctype.h>
#ifdef __GNUC__
#include "floatingpoint.h"
#endif

#include "lua.h"

static FILE *in=NULL, *out=NULL;  /* 输入和输出文件指针，初始为标准输入输出 */

/*
** 打开文件用于读取
** Lua 接口：status = readfrom (filename)
** 参数：filename - 要打开的文件名，如果为 nil 则恢复标准输入
** 返回：status = 1 表示成功，status = 0 表示错误
*/
static void io_readfrom (void)
{
 lua_Object o = lua_getparam (1);  /* 获取参数 */
 if (o == NULL)  /* 如果参数为 nil，恢复标准输入 */
 {
  if (in != stdin)  /* 如果当前不是标准输入 */
  {
   fclose (in);     /* 关闭当前输入文件 */
   in = stdin;      /* 重置为标准输入 */
  }
  lua_pushnumber (1);  /* 返回成功 */
 }
 else  /* 打开指定文件 */
 {
  if (!lua_isstring (o))  /* 检查参数类型是否为字符串 */
  {
   lua_error ("incorrect arguments to function `readfrom'");  /* 参数错误 */
   lua_pushnumber (0);  /* 返回失败 */
  }
  else
  {
   FILE *fp = fopen (lua_getstring(o),"r");  /* 以读模式打开文件 */
   if (fp == NULL)  /* 文件打开失败 */
   {
    lua_pushnumber (0);  /* 返回失败 */
   }
   else  /* 文件打开成功 */
   {
    if (in != stdin) fclose (in);  /* 关闭之前的输入文件 */
    in = fp;                       /* 设置新的输入文件 */
    lua_pushnumber (1);            /* 返回成功 */
   }
  }
 }
}


/*
** Open a file to write.
** LUA interface:
**			status = writeto (filename)
** where:
**			status = 1 -> success
**			status = 0 -> error
*/
static void io_writeto (void)
{
 lua_Object o = lua_getparam (1);
 if (o == NULL)			/* restore standart output */
 {
  if (out != stdout)
  {
   fclose (out);
   out = stdout;
  }
  lua_pushnumber (1);
 }
 else
 {
  if (!lua_isstring (o))
  {
   lua_error ("incorrect arguments to function `writeto'");
   lua_pushnumber (0);
  }
  else
  {
   FILE *fp = fopen (lua_getstring(o),"w");
   if (fp == NULL)
   {
    lua_pushnumber (0);
   }
   else
   {
    if (out != stdout) fclose (out);
    out = fp;
    lua_pushnumber (1);
   }
  }
 }
}


/*
** Read a variable. On error put nil on stack.
** LUA interface:
**			variable = read ([format])
**
** O formato pode ter um dos seguintes especificadores:
**
**	s ou S -> para string
**	f ou F, g ou G, e ou E -> para reais
**	i ou I -> para inteiros
**
**	Estes especificadores podem vir seguidos de numero que representa
**	o numero de campos a serem lidos.
*/
static void io_read (void)
{
 lua_Object o = lua_getparam (1);
 if (o == NULL)			/* free format */
 {
  int c;
  char s[256];
  while (isspace(c=fgetc(in)))
   ;
  if (c == '\"')
  {
   if (fscanf (in, "%[^\"]\"", s) != 1)
   {
    lua_pushnil ();
    return;
   }
  }
  else if (c == '\'')
  {
   if (fscanf (in, "%[^\']\'", s) != 1)
   {
    lua_pushnil ();
    return;
   }
  }
  else
  {
   char *ptr;
   double d;
   ungetc (c, in);
   if (fscanf (in, "%s", s) != 1)
   {
    lua_pushnil ();
    return;
   }
   d = strtod (s, &ptr);
   if (!(*ptr))
   {
    lua_pushnumber (d);
    return;
   }
  }
  lua_pushstring (s);
  return;
 }
 else				/* formatted */
 {
  char *e = lua_getstring(o);
  char t;
  int  m=0;
  while (isspace(*e)) e++;
  t = *e++;
  while (isdigit(*e))
   m = m*10 + (*e++ - '0');
  
  if (m > 0)
  {
   char f[80];
   char s[256];
   sprintf (f, "%%%ds", m);
   fscanf (in, f, s);
   switch (tolower(t))
   {
    case 'i':
    {
     long int l;
     sscanf (s, "%ld", &l);
     lua_pushnumber(l);
    }
    break;
    case 'f': case 'g': case 'e':
    {
     float f;
     sscanf (s, "%f", &f);
     lua_pushnumber(f);
    }
    break;
    default: 
     lua_pushstring(s); 
    break;
   }
  }
  else
  {
   switch (tolower(t))
   {
    case 'i':
    {
     long int l;
     fscanf (in, "%ld", &l);
     lua_pushnumber(l);
    }
    break;
    case 'f': case 'g': case 'e':
    {
     float f;
     fscanf (in, "%f", &f);
     lua_pushnumber(f);
    }
    break;
    default: 
    {
     char s[256];
     fscanf (in, "%s", s);
     lua_pushstring(s);
    }
    break;
   }
  }
 }
}


/*
** Write a variable. On error put 0 on stack, otherwise put 1.
** LUA interface:
**			status = write (variable [,format])
**
** O formato pode ter um dos seguintes especificadores:
**
**	s ou S -> para string
**	f ou F, g ou G, e ou E -> para reais
**	i ou I -> para inteiros
**
**	Estes especificadores podem vir seguidos de:
**
**		[?][m][.n]
**
**	onde:
**		? -> indica justificacao
**			< = esquerda
**			| = centro
**			> = direita (default)
**		m -> numero maximo de campos (se exceder estoura)
**		n -> indica precisao para
**			reais -> numero de casas decimais
**			inteiros -> numero minimo de digitos
**			string -> nao se aplica
*/
static char *buildformat (char *e, lua_Object o)
{
 static char buffer[512];
 static char f[80];
 char *string = &buffer[255];
 char t, j='r';
 int  m=0, n=0, l;
 while (isspace(*e)) e++;
 t = *e++;
 if (*e == '<' || *e == '|' || *e == '>') j = *e++;
 while (isdigit(*e))
  m = m*10 + (*e++ - '0');
 e++;	/* skip point */
 while (isdigit(*e))
  n = n*10 + (*e++ - '0');

 sprintf(f,"%%");
 if (j == '<' || j == '|') sprintf(strchr(f,0),"-");
 if (m != 0)   sprintf(strchr(f,0),"%d", m);
 if (n != 0)   sprintf(strchr(f,0),".%d", n);
 sprintf(strchr(f,0), "%c", t);
 switch (tolower(t))
 {
  case 'i': t = 'i';
   sprintf (string, f, (long int)lua_getnumber(o));
  break;
  case 'f': case 'g': case 'e': t = 'f';
   sprintf (string, f, (float)lua_getnumber(o));
  break;
  case 's': t = 's';
   sprintf (string, f, lua_getstring(o));
  break;
  default: return "";
 }
 l = strlen(string);
 if (m!=0 && l>m)
 {
  int i;
  for (i=0; i<m; i++)
   string[i] = '*';
  string[i] = 0;
 }
 else if (m!=0 && j=='|')
 {
  int i=l-1;
  while (isspace(string[i])) i--;
  string -= (m-i) / 2;
  i=0;
  while (string[i]==0) string[i++] = ' ';
  string[l] = 0;
 }
 return string;
}
static void io_write (void)
{
 lua_Object o1 = lua_getparam (1);
 lua_Object o2 = lua_getparam (2);
 if (o1 == NULL)			/* new line */
 {
  fprintf (out, "\n");
  lua_pushnumber(1);
 }
 else if (o2 == NULL)   		/* free format */
 {
  int status=0;
  if (lua_isnumber(o1))
   status = fprintf (out, "%g", lua_getnumber(o1));
  else if (lua_isstring(o1))
   status = fprintf (out, "%s", lua_getstring(o1));
  lua_pushnumber(status);
 }
 else					/* formated */
 {
  if (!lua_isstring(o2))
  { 
   lua_error ("incorrect format to function `write'"); 
   lua_pushnumber(0);
   return;
  }
  lua_pushnumber(fprintf (out, "%s", buildformat(lua_getstring(o2),o1)));
 }
}

/*
** Execute a executable program using "sustem".
** On error put 0 on stack, otherwise put 1.
*/
void io_execute (void)
{
 lua_Object o = lua_getparam (1);
 if (o == NULL || !lua_isstring (o))
 {
  lua_error ("incorrect arguments to function `execute'");
  lua_pushnumber (0);
 }
 else
 {
  system(lua_getstring(o));
  lua_pushnumber (1);
 }
 return;
}

/*
** Remove a file.
** On error put 0 on stack, otherwise put 1.
*/
void io_remove  (void)
{
 lua_Object o = lua_getparam (1);
 if (o == NULL || !lua_isstring (o))
 {
  lua_error ("incorrect arguments to function `remove'");
  lua_pushnumber (0);
 }
 else
 {
  if (remove(lua_getstring(o)) == 0)
   lua_pushnumber (1);
  else
   lua_pushnumber (0);
 }
 return;
}

/*
** Open io library
*/
void iolib_open (void)
{
 in=stdin; out=stdout;
 lua_register ("readfrom", io_readfrom);
 lua_register ("writeto",  io_writeto);
 lua_register ("read",     io_read);
 lua_register ("write",    io_write);
 lua_register ("execute",  io_execute);
 lua_register ("remove",   io_remove);
}
