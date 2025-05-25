/* Automatically generated header (sfdc 1.11f)! Do not edit! */

#ifndef CLIB_DEVICETREE_PROTOS_H
#define CLIB_DEVICETREE_PROTOS_H

/*
**   $VER: devicetree_protos.h 0.1.0 $Id: devicetree_lib.sfd 0.1.0 $
**
**   C prototypes. For use with 32 bit integers only.
**
**   Copyright (c) 2001 Amiga, Inc.
**       All Rights Reserved
*/

#include <exec/types.h>

#ifdef __cplusplus
extern "C" {
#endif /* __cplusplus */


/* "devicetree.resource" */
APTR DT_OpenKey(CONST_STRPTR name);
void DT_CloseKey(APTR key);
APTR DT_GetChild(APTR key, APTR prev);
APTR DT_FindProperty(APTR key, CONST_STRPTR property);
APTR DT_GetProperty(APTR key, APTR prev);
ULONG DT_GetPropLen(APTR property);
CONST_STRPTR DT_GetPropName(APTR property);
CONST_APTR DT_GetPropValue(APTR property);
APTR DT_GetParent(APTR key);
CONST_STRPTR DT_GetKeyName(APTR key);
APTR DT_FindPropertyRecursive(APTR key, CONST_STRPTR property);

#ifdef __cplusplus
}
#endif /* __cplusplus */

#endif /* CLIB_DEVICETREE_PROTOS_H */
