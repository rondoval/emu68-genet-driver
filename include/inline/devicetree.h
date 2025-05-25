/* Automatically generated header (sfdc 1.11f)! Do not edit! */

#ifndef _INLINE_DEVICETREE_H
#define _INLINE_DEVICETREE_H

#ifndef _SFDC_VARARG_DEFINED
#define _SFDC_VARARG_DEFINED
#ifdef __HAVE_IPTR_ATTR__
typedef APTR _sfdc_vararg __attribute__((iptr));
#else
typedef ULONG _sfdc_vararg;
#endif /* __HAVE_IPTR_ATTR__ */
#endif /* _SFDC_VARARG_DEFINED */

#ifndef __INLINE_MACROS_H
#include <inline/macros.h>
#endif /* !__INLINE_MACROS_H */

#ifndef DEVICETREE_BASE_NAME
#define DEVICETREE_BASE_NAME DeviceTreeBase
#endif /* !DEVICETREE_BASE_NAME */

#define DT_OpenKey(___name) \
      LP1(0x6, APTR, DT_OpenKey , CONST_STRPTR, ___name, a0,\
      , DEVICETREE_BASE_NAME)

#define DT_CloseKey(___key) \
      LP1NR(0xc, DT_CloseKey , APTR, ___key, a0,\
      , DEVICETREE_BASE_NAME)

#define DT_GetChild(___key, ___prev) \
      LP2(0x12, APTR, DT_GetChild , APTR, ___key, a0, APTR, ___prev, a1,\
      , DEVICETREE_BASE_NAME)

#define DT_FindProperty(___key, ___property) \
      LP2(0x18, APTR, DT_FindProperty , APTR, ___key, a0, CONST_STRPTR, ___property, a1,\
      , DEVICETREE_BASE_NAME)

#define DT_GetProperty(___key, ___prev) \
      LP2(0x1e, APTR, DT_GetProperty , APTR, ___key, a0, APTR, ___prev, a1,\
      , DEVICETREE_BASE_NAME)

#define DT_GetPropLen(___property) \
      LP1(0x24, ULONG, DT_GetPropLen , APTR, ___property, a0,\
      , DEVICETREE_BASE_NAME)

#define DT_GetPropName(___property) \
      LP1(0x2a, CONST_STRPTR, DT_GetPropName , APTR, ___property, a0,\
      , DEVICETREE_BASE_NAME)

#define DT_GetPropValue(___property) \
      LP1(0x30, CONST_APTR, DT_GetPropValue , APTR, ___property, a0,\
      , DEVICETREE_BASE_NAME)

#define DT_GetParent(___key) \
      LP1(0x36, APTR, DT_GetParent , APTR, ___key, a0,\
      , DEVICETREE_BASE_NAME)

#define DT_GetKeyName(___key) \
      LP1(0x3c, CONST_STRPTR, DT_GetKeyName , APTR, ___key, a0,\
      , DEVICETREE_BASE_NAME)

#define DT_FindPropertyRecursive(___key, ___property) \
      LP2(0x42, APTR, DT_FindPropertyRecursive , APTR, ___key, a0, CONST_STRPTR, ___property, a1,\
      , DEVICETREE_BASE_NAME)

#endif /* !_INLINE_DEVICETREE_H */
