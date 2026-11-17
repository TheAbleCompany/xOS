/*
** LUA - 面向应用用户的语言
** 图形计算技术组
** TeCGraf - PUC-Rio
** 1993年5月19日
**
** Lua 语言主头文件
** 定义了Lua API接口、对象类型和核心函数声明
** 提供Lua与C语言交互的所有函数接口
*/


#ifndef lua_h
#define lua_h

typedef void (*lua_CFunction) (void);  /* C函数指针类型，指向Lua可调用的C函数 */
typedef struct Object *lua_Object;  /* Lua对象指针类型，指向Lua中的对象 */

#define lua_register(n,f)	(lua_pushcfunction(f), lua_storeglobal(n))  /* 注册C函数到Lua全局环境 */


void           lua_errorfunction    	(void (*fn) (char *s));
void           lua_error		(char *s);
int            lua_dofile 		(char *filename);
int            lua_dostring 		(char *string);
int            lua_call 		(char *functionname, int nparam);

lua_Object     lua_getparam 		(int number);
float          lua_getnumber 		(lua_Object object);
char          *lua_getstring 		(lua_Object object);
char 	      *lua_copystring 		(lua_Object object);
lua_CFunction  lua_getcfunction 	(lua_Object object);
void          *lua_getuserdata  	(lua_Object object);
lua_Object     lua_getfield         	(lua_Object object, char *field);
lua_Object     lua_getindexed         	(lua_Object object, float index);
lua_Object     lua_getglobal 		(char *name);

lua_Object     lua_pop 			(void);

int 	       lua_pushnil 		(void);
int            lua_pushnumber 		(float n);
int            lua_pushstring 		(char *s);
int            lua_pushcfunction	(lua_CFunction fn);
int            lua_pushuserdata     	(void *u);
int            lua_pushobject       	(lua_Object object);

int            lua_storeglobal		(char *name);
int            lua_storefield		(lua_Object object, char *field);
int            lua_storeindexed		(lua_Object object, float index);

int            lua_isnil 		(lua_Object object);
int            lua_isnumber 		(lua_Object object);
int            lua_isstring 		(lua_Object object);
int            lua_istable          	(lua_Object object);
int            lua_iscfunction 		(lua_Object object);
int            lua_isuserdata 		(lua_Object object);

#endif
