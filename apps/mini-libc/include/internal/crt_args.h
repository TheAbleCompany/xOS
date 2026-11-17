#ifndef __CRT_ARGS_H__
#define __CRT_ARGS_H__

struct exec_args
{
    int argc;

    /*
     * На x86_64 компилятор добавит padding,
     * поэтому argv будет находиться по offset 8.
     */

    char **argv;
};

#endif