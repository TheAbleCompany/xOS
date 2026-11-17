/*
** opcode.h
** TeCGraf - PUC-Rio
** 1992年4月16日
**
** Lua 操作码和类型定义头文件
** 定义了虚拟机操作码、数据类型、对象结构等核心数据结构
** 提供Lua虚拟机的基本类型系统和操作码枚举
*/

#ifndef opcode_h
#define opcode_h

#ifndef STACKGAP
#define STACKGAP	128  /* 栈间隙大小，用于栈空间管理 */
#endif 

#ifndef real
#define real float  /* 实数类型定义为float */
#endif

typedef unsigned char  Byte;  /* 字节类型，用于操作码 */

typedef unsigned short Word;  /* 字类型，用于16位数据 */

/* 虚拟机操作码枚举 */
typedef enum 
{ 
 NOP,  /* 空操作 */
 PUSHNIL,  /* 压入nil值 */
 PUSH0, PUSH1, PUSH2,  /* 压入常量0、1、2 */
 PUSHBYTE,  /* 压入字节常量 */
 PUSHWORD,  /* 压入字常量 */
 PUSHFLOAT,  /* 压入浮点数 */
 PUSHSTRING,  /* 压入字符串 */
 PUSHLOCAL0, PUSHLOCAL1, PUSHLOCAL2, PUSHLOCAL3, PUSHLOCAL4,  /* 压入局部变量0-4 */
 PUSHLOCAL5, PUSHLOCAL6, PUSHLOCAL7, PUSHLOCAL8, PUSHLOCAL9,  /* 压入局部变量5-9 */
 PUSHLOCAL,  /* 压入指定局部变量 */
 PUSHGLOBAL,  /* 压入全局变量 */
 PUSHINDEXED,  /* 压入索引值 */
 PUSHMARK,  /* 压入标记 */
 PUSHOBJECT,  /* 压入对象 */
 STORELOCAL0, STORELOCAL1, STORELOCAL2, STORELOCAL3, STORELOCAL4,  /* 存储到局部变量0-4 */
 STORELOCAL5, STORELOCAL6, STORELOCAL7, STORELOCAL8, STORELOCAL9,  /* 存储到局部变量5-9 */
 STORELOCAL,  /* 存储到指定局部变量 */
 STOREGLOBAL,  /* 存储到全局变量 */
 STOREINDEXED0,  /* 存储到索引0 */
 STOREINDEXED,  /* 存储到索引 */
 STOREFIELD,  /* 存储字段 */
 ADJUST,  /* 调整栈 */
 CREATEARRAY,  /* 创建数组 */
 EQOP,  /* 等于操作 */
 LTOP,  /* 小于操作 */
 LEOP,  /* 小于等于操作 */
 ADDOP,  /* 加法操作 */
 SUBOP,  /* 减法操作 */
 MULTOP,  /* 乘法操作 */
 DIVOP,  /* 除法操作 */
 CONCOP,  /* 连接操作 */
 MINUSOP,  /* 取负操作 */
 NOTOP,  /* 取反操作 */
 ONTJMP,  /* 为真跳转 */
 ONFJMP,  /* 为假跳转 */
 JMP,  /* 无条件跳转 */
 UPJMP,  /* 向上跳转 */
 IFFJMP,  /* 如果为假跳转 */
 IFFUPJMP,  /* 如果为假向上跳转 */
 POP,  /* 弹出栈顶 */
 CALLFUNC,  /* 调用函数 */
 RETCODE,  /* 返回 */
 HALT,  /* 停止 */
 SETFUNCTION,  /* 设置函数 */
 SETLINE,  /* 设置行号 */
 RESET  /* 重置 */
} OpCode;

typedef enum
{
 T_MARK,
 T_NIL,
 T_NUMBER,
 T_STRING,
 T_ARRAY,
 T_FUNCTION,
 T_CFUNCTION,
 T_USERDATA
} Type; 

typedef void (*Cfunction) (void);
typedef int  (*Input) (void);
typedef void (*Unput) (int );

typedef union
{
 Cfunction 	 f;
 real    	 n;
 char      	*s;
 Byte      	*b;
 struct Hash    *a;
 void           *u;
} Value;

typedef struct Object
{
 Type  tag;
 Value value;
} Object;

typedef struct
{
 char   *name;
 Object  object;
} Symbol;

/* Macros to access structure members */
#define tag(o)		((o)->tag)
#define nvalue(o)	((o)->value.n)
#define svalue(o)	((o)->value.s)
#define bvalue(o)	((o)->value.b)
#define avalue(o)	((o)->value.a)
#define fvalue(o)	((o)->value.f)
#define uvalue(o)	((o)->value.u)

/* Macros to access symbol table */
#define s_name(i)	(lua_table[i].name)
#define s_object(i)	(lua_table[i].object)
#define s_tag(i)	(tag(&s_object(i)))
#define s_nvalue(i)	(nvalue(&s_object(i)))
#define s_svalue(i)	(svalue(&s_object(i)))
#define s_bvalue(i)	(bvalue(&s_object(i)))
#define s_avalue(i)	(avalue(&s_object(i)))
#define s_fvalue(i)	(fvalue(&s_object(i)))
#define s_uvalue(i)	(uvalue(&s_object(i)))


/* Exported functions */
int     lua_execute   (Byte *pc);
void    lua_markstack (void);
char   *lua_strdup (char *l);

void    lua_setinput   (Input fn);	/* from "lua.lex" module */
void    lua_setunput   (Unput fn);	/* from "lua.lex" module */
char   *lua_lasttext   (void);		/* from "lua.lex" module */
int     lua_parse      (void); 		/* from "lua.stx" module */
void    lua_type       (void);
void 	lua_obj2number (void);
void 	lua_print      (void);

#endif
