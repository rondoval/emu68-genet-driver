#ifndef _MINLIST_H
#define _MINLIST_H

#include <exec/lists.h>

/* This is to avoid KS v45 requirement*/
inline static void _NewMinList(struct MinList *list)
{
    list->mlh_Head = (struct MinNode *)&list->mlh_Tail;
    list->mlh_Tail = NULL;
    list->mlh_TailPred = (struct MinNode *)&list->mlh_Head;
}

#endif