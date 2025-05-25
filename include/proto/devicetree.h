/* Automatically generated header (sfdc 1.11f)! Do not edit! */

#ifndef PROTO_DEVICETREE_H
#define PROTO_DEVICETREE_H

#include <clib/devicetree_protos.h>

#ifndef _NO_INLINE
# if defined(__GNUC__)
#  ifdef __AROS__
#   include <defines/devicetree.h>
#  else
#   include <inline/devicetree.h>
#  endif
# else
#  include <pragmas/devicetree_pragmas.h>
# endif
#endif /* _NO_INLINE */

#ifdef __amigaos4__
# include <interfaces/devicetree.h>
# ifndef __NOGLOBALIFACE__
   extern struct DeviceTreeIFace *IDeviceTree;
# endif /* __NOGLOBALIFACE__*/
#endif /* !__amigaos4__ */
#ifndef __NOLIBBASE__
  extern APTR
# ifdef __CONSTLIBBASEDECL__
   __CONSTLIBBASEDECL__
# endif /* __CONSTLIBBASEDECL__ */
  DeviceTreeBase;
#endif /* !__NOLIBBASE__ */

#endif /* !PROTO_DEVICETREE_H */
