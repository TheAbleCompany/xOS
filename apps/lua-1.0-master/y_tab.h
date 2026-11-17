
/*
** Lua 语法分析器生成的头文件
** 定义了词法分析器的token类型和值类型
** 包含所有Lua关键字的token定义
*/

typedef union  
{
 int   vInt;  /* 整数值 */
 long  vLong;  /* 长整数值 */
 float vFloat;  /* 浮点数值 */
 Word  vWord;  /* 字值 */
 Byte *pByte;  /* 字节指针 */
} YYSTYPE;  /* Yacc值类型 */
extern YYSTYPE yylval;  /* Yacc的全局值变量 */
/* Lua关键字和token定义 */
# define NIL 257  /* nil关键字 */
# define IF 258  /* if关键字 */
# define THEN 259  /* then关键字 */
# define ELSE 260  /* else关键字 */
# define ELSEIF 261  /* elseif关键字 */
# define WHILE 262  /* while关键字 */
# define DO 263  /* do关键字 */
# define REPEAT 264  /* repeat关键字 */
# define UNTIL 265  /* until关键字 */
# define END 266  /* end关键字 */
# define RETURN 267  /* return关键字 */
# define LOCAL 268  /* local关键字 */
# define NUMBER 269  /* 数字字面量 */
# define FUNCTION 270  /* function关键字 */
# define NAME 271  /* 标识符 */
# define STRING 272  /* 字符串字面量 */
# define DEBUG 273  /* debug关键字 */
# define NOT 274  /* not操作符 */
# define AND 275  /* and操作符 */
# define OR 276  /* or操作符 */
# define NE 277  /* 不等于操作符 */
# define LE 278  /* 小于等于操作符 */
# define GE 279  /* 大于等于操作符 */
# define CONC 280  /* 连接操作符 */
# define UNARY 281  /* 一元操作符 */
