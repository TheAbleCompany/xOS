/*
** hash.h
** Lua的哈希表管理器
** Luiz Henrique de Figueiredo - 1990年8月17日
** 由Waldemar Celes Filho修改
** 1993年4月26日
**
** Lua 哈希表管理头文件
** 定义了哈希表节点结构和哈希表管理函数
** 提供哈希表的基本数据结构和操作接口
*/

#ifndef hash_h
#define hash_h

/* 哈希表节点结构 */
typedef struct node
{
 Object ref;  /* 引用对象（键） */
 Object val;  /* 值对象 */
 struct node *next;  /* 下一个节点的指针 */
} Node;

/* 哈希表结构 */
typedef struct Hash
{
 char           mark;  /* 标记数组标志 */
 unsigned int   nhash;  /* 哈希桶的数量 */
 Node         **list;  /* 哈希桶数组，每个桶是一个链表 */
} Hash;

/* 访问哈希表标记的宏 */
#define markarray(t)		((t)->mark)

Hash 	*lua_hashcreate (unsigned int nhash);
void 	 lua_hashdelete (Hash *h);
Object 	*lua_hashdefine (Hash *t, Object *ref);
void 	 lua_hashmark   (Hash *h);

void     lua_next (void);

#endif
