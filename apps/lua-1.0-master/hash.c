/*
** hash.c
** Lua的哈希表管理器
** Luiz Henrique de Figueiredo - 1990年8月17日
** 由Waldemar Celes Filho修改
** 1993年5月12日
**
** Lua 哈希表管理器
** 负责实现 Lua 语言中的哈希表（关联数组）数据结构
** 提供哈希表的创建、查找、插入、删除等基本操作
** 支持数字和字符串作为键，实现高效的键值对存储
*/

#include <string.h>
#include <stdlib.h>

#include "opcode.h"
#include "hash.h"
#include "inout.h"
#include "table.h"
#include "lua.h"

/* 字符串比较宏定义 */
#define streq(s1,s2)	(strcmp(s1,s2)==0)  /* 判断两个字符串是否相等 */
#define strneq(s1,s2)	(strcmp(s1,s2)!=0)  /* 判断两个字符串是否不相等 */

/* 内存分配宏定义 */
#define new(s)		((s *)malloc(sizeof(s)))      /* 为单个结构体分配内存 */
#define newvector(n,s)	((s *)calloc(n,sizeof(s)))  /* 为结构体数组分配内存并初始化为0 */

/* 哈希表访问宏定义 */
#define nhash(t)	((t)->nhash)        /* 获取哈希表的桶数量 */
#define nodelist(t)	((t)->list)        /* 获取哈希表的节点链表数组 */
#define list(t,i)	((t)->list[i])      /* 获取第i个桶的链表头 */
#define ref_tag(n)	(tag(&(n)->ref))    /* 获取节点引用的类型标签 */
#define ref_nvalue(n)	(nvalue(&(n)->ref))  /* 获取节点引用的数值 */
#define ref_svalue(n)	(svalue(&(n)->ref))  /* 获取节点引用的字符串值 */

/*
** 哈希函数
** 根据给定的引用对象计算哈希值
** 参数：t - 哈希表，ref - 引用对象
** 返回：计算得到的哈希桶索引
*/
static int head (Hash *t, Object *ref)
{
 /* 如果引用是数字类型，直接使用数字值取模 */
 if (tag(ref) == T_NUMBER) return (((int)nvalue(ref))%nhash(t));
 /* 如果引用是字符串类型，计算字符串的哈希值 */
 else if (tag(ref) == T_STRING)
 {
  int h;
  char *name = svalue(ref);
  /* 将字符串解释为二进制数来计算哈希值 */
  for (h=0; *name!=0; name++)
  {
   h <<= 8;                           /* 左移8位 */
   h  += (unsigned char) *name;     /* 加上当前字符的ASCII值，避免符号扩展 */
   h  %= nhash(t);				/* 取模使其成为有效的桶索引 */
  }
  return h;
 }
 /* 其他类型不支持作为哈希表的键 */
 else
 {
  lua_reportbug ("unexpected type to index table");  /* 报告错误 */
  return -1;                                         /* 返回错误值 */
 }
}

static Node *present(Hash *t, Object *ref, int h)
{
 Node *n=NULL, *p;
 if (tag(ref) == T_NUMBER)
 {
  for (p=NULL,n=list(t,h); n!=NULL; p=n, n=n->next)
   if (ref_tag(n) == T_NUMBER && nvalue(ref) == ref_nvalue(n)) break;
 }  
 else if (tag(ref) == T_STRING)
 {
  for (p=NULL,n=list(t,h); n!=NULL; p=n, n=n->next)
   if (ref_tag(n) == T_STRING && streq(svalue(ref),ref_svalue(n))) break;
 }  
 if (n==NULL)				/* 名称不存在 */
  return NULL;
#if 0
 if (p!=NULL)				/* 名称存在但不是第一个 */
 {
  p->next=n->next;			/* 移动到前端的自组织 */
  n->next=list(t,h);
  list(t,h)=n;
 }
#endif
 return n;
}

static void freelist (Node *n)
{
 while (n)
 {
  Node *next = n->next;
  free (n);
  n = next;
 }
}

/*
** 创建新的哈希表
** 分配并初始化哈希表结构，创建指定数量的哈希桶
** 参数：nhash - 哈希桶的数量
** 返回：哈希表指针，失败返回NULL
*/
Hash *lua_hashcreate (unsigned int nhash)
{
 Hash *t = new (Hash);  /* 为哈希表结构分配内存 */
 if (t == NULL)  /* 内存分配失败 */
 {
  lua_error ("not enough memory");  /* 报告内存不足错误 */
  return NULL;  /* 返回NULL */
 }
 nhash(t) = nhash;  /* 设置哈希桶数量 */
 markarray(t) = 0;  /* 初始化标记数组标志为0 */
 nodelist(t) = newvector (nhash, Node*);  /* 为哈希桶数组分配内存 */
 if (nodelist(t) == NULL)  /* 内存分配失败 */
 {
  lua_error ("not enough memory");  /* 报告内存不足错误 */
  return NULL;  /* 返回NULL */
 }
 return t;  /* 返回创建的哈希表 */
}

/*
** 删除哈希表
*/
void lua_hashdelete (Hash *h)
{
 int i;
 for (i=0; i<nhash(h); i++)
  freelist (list(h,i));
 free (nodelist(h));
 free(h);
}

/*
** 定义哈希表节点
** 如果节点存在则返回其指针，否则创建新节点并返回指针
** 参数：t - 哈希表，ref - 引用对象（键）
** 返回：节点值的指针，错误返回NULL
*/
Object *lua_hashdefine (Hash *t, Object *ref)
{
 int   h;  /* 哈希桶索引 */
 Node *n;  /* 节点指针 */
 h = head (t, ref);  /* 计算哈希值 */
 if (h < 0) return NULL;  /* 哈希计算失败 */
 
 n = present(t, ref, h);  /* 查找节点是否存在 */
 if (n == NULL)  /* 节点不存在，创建新节点 */
 {
  n = new(Node);  /* 分配节点内存 */
  if (n == NULL)  /* 内存分配失败 */
  {
   lua_error ("not enough memory");  /* 报告内存不足错误 */
   return NULL;  /* 返回NULL */
  }
  n->ref = *ref;  /* 复制引用对象 */
  tag(&n->val) = T_NIL;  /* 初始化节点值为nil */
  n->next = list(t,h);  /* 将新节点插入链表头部 */
  list(t,h) = n;  /* 更新桶的头指针 */
 }
 return (&n->val);  /* 返回节点值的指针 */
}

/*
** Mark a hash and check its elements 
*/
void lua_hashmark (Hash *h)
{
 int i;
 
 markarray(h) = 1;
 
 for (i=0; i<nhash(h); i++)
 {
  Node *n;
  for (n = list(h,i); n != NULL; n = n->next)
  {
   lua_markobject (&n->ref);
   lua_markobject (&n->val);
  }
 }
}


/*
** 操作数组的内部函数
** 给定一个数组对象和一个引用值，返回哈希表中的下一个元素
** 该函数将元素值及其引用压入栈中
*/
#include "lua.h"
static void firstnode (Hash *a, int h)
{
 if (h < nhash(a))
 {  
  int i;
  for (i=h; i<nhash(a); i++)
  {
   if (list(a,i) != NULL && tag(&list(a,i)->val) != T_NIL)
   {
    lua_pushobject (&list(a,i)->ref);
    lua_pushobject (&list(a,i)->val);
    return;
   }
  }
 }
 lua_pushnil();
 lua_pushnil();
}
void lua_next (void)
{
 Hash   *a;
 Object *o = lua_getparam (1);
 Object *r = lua_getparam (2);
 if (o == NULL || r == NULL)
 { lua_error ("too few arguments to function `next'"); return; }
 if (lua_getparam (3) != NULL)
 { lua_error ("too many arguments to function `next'"); return; }
 if (tag(o) != T_ARRAY)
 { lua_error ("first argument to function `next' is not a table"); return; }
 a = avalue(o);
 if (tag(r) == T_NIL)
 {
  firstnode (a, 0);
  return;
 }
 else
 {
  int h = head (a, r);
  if (h >= 0)
  {
   Node *n = list(a,h);
   while (n)
   {
    if (memcmp(&n->ref,r,sizeof(Object)) == 0)
    {
     if (n->next == NULL)
     {
      firstnode (a, h+1);
      return;
     }
     else if (tag(&n->next->val) != T_NIL)
     {
      lua_pushobject (&n->next->ref);
      lua_pushobject (&n->next->val);
      return;
     }
     else
     {
      Node *next = n->next->next;
      while (next != NULL && tag(&next->val) == T_NIL) next = next->next;
      if (next == NULL)
      {
       firstnode (a, h+1);
       return;
      }
      else
      {
       lua_pushobject (&next->ref);
       lua_pushobject (&next->val);
      }
      return;
     }
    }
    n = n->next;
   }
   if (n == NULL)
    lua_error ("function `next' error: reference not found");
  }
 }
}
