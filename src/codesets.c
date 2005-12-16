/***************************************************************************

 codesets.library - Amiga shared library for handling different codesets
 Copyright (C) 2001-2005 by Alfonso [alfie] Ranieri <alforan@tin.it>.
 Copyright (C) 2005      by codesets.library Open Source Team

 This library is free software; you can redistribute it and/or
 modify it under the terms of the GNU Lesser General Public
 License as published by the Free Software Foundation; either
 version 2.1 of the License, or (at your option) any later version.

 This library is distributed in the hope that it will be useful,
 but WITHOUT ANY WARRANTY; without even the implied warranty of
 MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU
 Lesser General Public License for more details.

 codesets.library project: http://sourceforge.net/projects/codesetslib/

 Most of the code included in this file was relicensed from GPL to LGPL
 from the source code of SimpleMail (http://www.sf.net/projects/simplemail)
 with full permissions by its authors.

 $Id$

***************************************************************************/

#include "lib.h"

#include <diskfont/glyph.h>
#include <diskfont/diskfonttag.h>
#include <proto/diskfont.h>
#include <ctype.h>
#include <limits.h>

#include "codesets_table.h"
#include "convertUTF.h"

#include "SDI_stdarg.h"

#include "debug.h"

/***********************************************************************/

/* search a sorted array in O(log n) e.g.
   BIN_SEARCH(strings,0,sizeof(strings)/sizeof(strings[0]),strcmp(key,array[mid]),res); */
#define BIN_SEARCH(array,low,high,compare,result) \
	{\
		int l = low;\
		int h = high;\
		int m = (low+high)/2;\
		result = NULL;\
		while (l<=h)\
		{\
			int c = compare;\
			if (!c){ result = &array[m]; break; }\
			if (c < 0) h = m - 1;\
			else l = m + 1;\
			m = (l + h)/2;\
		}\
	}

/***********************************************************************/

static STRPTR
mystrdup(STRPTR str)
{
    STRPTR new;
    int   len;

    if (!str) return NULL;

    len = strlen(str);
    if (!len) return NULL;

    if ((new = allocArbitrateVecPooled(len+1)))
        strcpy(new,str);

    return new;
}

/***********************************************************************/

static STRPTR
mystrndup(STRPTR str1,int n)
{
    STRPTR dest;

    if ((dest = allocArbitrateVecPooled(n+1)))
    {
        if (str1) strncpy(dest,str1,n);
        else dest[0] = 0;

        dest[n] = 0;
    }

    return dest;
}

/***********************************************************************/

static ULONG
readLine(BPTR fh,STRPTR buf,int size)
{
    STRPTR c;

    if (!FGets(fh,buf,size)) return FALSE;

    for (c = buf; *c; c++)
    {
        if (*c=='\n' || *c=='\r')
        {
            *c = 0;
            break;
        }
    }

    return TRUE;
}

/***********************************************************************/

static STRPTR
getConfigItem(STRPTR buf,STRPTR item,int len)
{
    if(!strnicmp(buf, item, len))
    {
        UBYTE c;

        buf += len;

        /* skip spaces */
        while ((c = *buf) && isspace(c)) buf++;

        if (*buf!='=') return NULL;
        buf++;

        /* skip spaces */
        while ((c = *buf) && isspace(c)) buf++;

        return buf;
    }

    return NULL;
}

/***********************************************************************/

STRPTR *LIBFUNC
CodesetsSupportedA(REG(a0, UNUSED struct TagItem * attrs))
{
  STRPTR *array;

  ENTER();

  if((array = allocArbitrateVecPooled(sizeof(STRPTR)*(countNodes(&CodesetsBase->codesets)+1))))
  {
    struct codeset *code, *succ;
    int            i;

    ObtainSemaphoreShared(&CodesetsBase->libSem);

    for(i = 0, code = (struct codeset *)CodesetsBase->codesets.mlh_Head; (succ = (struct codeset *)code->node.mln_Succ); code = succ, i++)
      array[i] = code->name;

    array[i] = NULL;

    ReleaseSemaphore(&CodesetsBase->libSem);
  }

  RETURN(array);
  return array;
}

LIBSTUB(CodesetsSupportedA, STRPTR*, REG(a0, struct TagItem *attrs))
{
  #ifdef __MORPHOS__
  return CodesetsSupportedA((struct TagItem *)REG_A0);
  #else
  return CodesetsSupportedA(attrs);
  #endif
}

#ifdef __amigaos4__
LIBSTUBVA(CodesetsSupported, STRPTR*, ...)
{
  STRPTR* res;
  VA_LIST args;

  VA_START(args, self);
  res = CodesetsSupportedA(VA_ARG(args, struct TagItem *));
  VA_END(args);

  return res;
}
#endif

/**************************************************************************/

void LIBFUNC
CodesetsFreeA(REG(a0, APTR obj),
              REG(a1, UNUSED struct TagItem *attrs))
{
  ENTER();

  if(obj)
    freeArbitrateVecPooled(obj);

  LEAVE();
}

LIBSTUB(CodesetsFreeA, void, REG(a0, APTR obj), REG(a1, struct TagItem *attrs))
{
  #ifdef __MORPHOS__
  return CodesetsFreeA((APTR)REG_A0,(struct TagItem *)REG_A1);
  #else
  return CodesetsFreeA(obj, attrs);
  #endif
}

#ifdef __amigaos4__
LIBSTUBVA(CodesetsFree, void, REG(a0, APTR obj), ...)
{
  VA_LIST args;

  VA_START(args, obj);
  CodesetsFreeA(obj, VA_ARG(args, struct TagItem *));
  VA_END(args);
}
#endif

/**************************************************************************/

/*
 * The compare function
 */

static int
codesetsCmpUnicode(struct single_convert *arg1,struct single_convert *arg2)
{
  return strcmp((char*)arg1->utf8+1, (char*)arg2->utf8+1);
}

/**************************************************************************/
/*
 * Reads a coding table and adds it
 */


#define ITEM_STANDARD           "Standard"
#define ITEM_ALTSTANDARD        "AltStandard"
#define ITEM_READONLY           "ReadOnly"
#define ITEM_CHARACTERIZATION   "Characterization"

static ULONG
codesetsReadTable(struct MinList *codesetsList,STRPTR name)
{
  char buf[512];
  BPTR  fh;
  ULONG res = FALSE;

  ENTER();

  if((fh = Open(name, MODE_OLDFILE)))
  {
    struct codeset *codeset;

    if((codeset = (struct codeset *)allocVecPooled(CodesetsBase->pool,sizeof(struct codeset))))
    {
      int i;

      memset(codeset,0,sizeof(struct codeset));

      for(i = 0; i<256; i++)
        codeset->table[i].code = codeset->table[i].ucs4 = i;

      while(readLine(fh, buf, sizeof(buf)))
      {
        STRPTR result;

        if(*buf=='#')
          continue;

        if((result = getConfigItem(buf,ITEM_STANDARD,strlen(ITEM_STANDARD))))
          codeset->name = mystrdup(result);
        else if((result = getConfigItem(buf,ITEM_ALTSTANDARD,strlen(ITEM_ALTSTANDARD))))
          codeset->alt_name = mystrdup(result);
        else if((result = getConfigItem(buf,ITEM_READONLY,strlen(ITEM_READONLY))))
          codeset->read_only = !!atoi(result);
        else if((result = getConfigItem(buf,ITEM_CHARACTERIZATION,strlen(ITEM_CHARACTERIZATION))))
        {
          if((result[0]=='_') && (result[1]=='(') && (result[2]=='"'))
          {
            STRPTR end = strchr(result + 3, '"');

            if(end)
              codeset->characterization = mystrndup(result+3,end-(result+3));
          }
        }
        else
        {
          STRPTR p = buf;
          int fmt2 = 0;

          if((*p=='=') || (fmt2 = ((*p=='0') || (*(p+1)=='x'))))
          {
            p++;
            p += fmt2;

            i = strtol((const char *)p,(char **)&p,16);
            if(i>0 && i<256)
            {
              while(isspace(*p)) p++;

              if(!strnicmp(p, "U+", 2))
              {
                p += 2;
                codeset->table[i].ucs4 = strtol((const char *)p,(char **)&p,16);
              }
              else
              {
                if(*p!='#')
                  codeset->table[i].ucs4 = strtol((const char *)p,(char **)&p,0);
              }
            }
          }
        }
      }

      // check if there is not already codeset with the same name in here
      if(!(codesetsFind(codesetsList, codeset->name)))
      {
        for(i=0; i<256; i++)
        {
          UTF32 src = codeset->table[i].ucs4, *src_ptr = &src;
          UTF8  *dest_ptr = &codeset->table[i].utf8[1];

          CodesetsConvertUTF32toUTF8((const UTF32 **)&src_ptr,src_ptr+1,&dest_ptr,dest_ptr+6,CONVFLG_StrictConversion);
          *dest_ptr = 0;
          codeset->table[i].utf8[0] = (ULONG)dest_ptr-(ULONG)(&codeset->table[i].utf8[1]);
        }

        memcpy(codeset->table_sorted, codeset->table, sizeof(codeset->table));
        qsort(codeset->table_sorted, 256, sizeof(codeset->table[0]), (int (*)(const void *arg1,const void *arg2))codesetsCmpUnicode);
        AddTail((struct List *)codesetsList, (struct Node *)&codeset->node);

        res = TRUE;
      }
      else
      {
        // cleanup
        if(codeset->name)             freeArbitrateVecPooled(codeset->name);
        if(codeset->alt_name)         freeArbitrateVecPooled(codeset->alt_name);
        if(codeset->characterization) freeArbitrateVecPooled(codeset->characterization);
        freeArbitrateVecPooled(codeset);
      }
    }

    Close(fh);
  }

  RETURN(res);
  return res;
}

/**************************************************************************/

/*
 * Initialized and loads the codesets
 */

ULONG
codesetsInit(struct MinList * codesetsList)
{
  struct codeset       *codeset = NULL;
  UTF32                src;
  struct FileInfoBlock *fib;
  int                  i;
  #if defined(__amigaos4__)
  int                  nextMIB = 3;
  #endif

  ENTER();

  ObtainSemaphore(&CodesetsBase->poolSem);

  // on AmigaOS4 we can use diskfont.library to inquire charset information as
  // it comes with a quite rich implementation of different charsets.
  #if defined(__amigaos4__)
  do
  {
    char *mimename;
    char *ianaName;
    ULONG *mapTable;
    int curMIB = nextMIB;

    nextMIB = (ULONG)ObtainCharsetInfo(DFCS_NUMBER, curMIB, DFCS_NEXTNUMBER);
    if(nextMIB == 0)
      break;

    mapTable = (ULONG *)ObtainCharsetInfo(DFCS_NUMBER, curMIB, DFCS_MAPTABLE);
    mimename = (char *)ObtainCharsetInfo(DFCS_NUMBER, curMIB, DFCS_MIMENAME);
    ianaName = (char *)ObtainCharsetInfo(DFCS_NUMBER, curMIB, DFCS_NAME);
    if(mapTable && mimename)
    {
      D(DBF_STARTUP, "loading charset '%s' from diskfont.library...", mimename);

      if(!(codeset = allocVecPooled(CodesetsBase->pool, sizeof(struct codeset)))) goto end;
      codeset->name 	          = mystrdup(mimename);
      codeset->alt_name 	      = NULL;
      codeset->characterization = mystrdup(ianaName);
      codeset->read_only 	      = 0;

      for(i=0; i<256; i++)
      {
        UTF32 *src_ptr = &src;
        UTF8  *dest_ptr = &codeset->table[i].utf8[1];

        src = mapTable[i];

        codeset->table[i].code = i;
        codeset->table[i].ucs4 = src;
        CodesetsConvertUTF32toUTF8((const UTF32 **)&src_ptr, src_ptr+1, &dest_ptr, dest_ptr+6, CONVFLG_StrictConversion);
        *dest_ptr = 0;
        codeset->table[i].utf8[0] = (ULONG)dest_ptr-(ULONG)&codeset->table[i].utf8[1];
      }
    	memcpy(codeset->table_sorted,codeset->table,sizeof(codeset->table));
      qsort(codeset->table_sorted,256,sizeof(codeset->table[0]),(int (*)(const void *arg1, const void *arg2))codesetsCmpUnicode);
      AddTail((struct List *)codesetsList, (struct Node *)&codeset->node);
    }
  }
  while(TRUE);
  #endif

  D(DBF_STARTUP, "loading charsets from Libs:Charsets...");

  // we try to walk to the LIBS:Charsets directory on our own and readin our
  // own charset tables
  if((fib = AllocDosObject(DOS_FIB,NULL)))
  {
    BPTR dir;

    if((dir = Lock("LIBS:Charsets",SHARED_LOCK)) && Examine(dir,fib) && (fib->fib_DirEntryType>=0))
    {
      BPTR oldDir = CurrentDir(dir);

      while(ExNext(dir,fib))
      {
        if(fib->fib_DirEntryType>=0)
          continue;

        codesetsReadTable(codesetsList, fib->fib_FileName);
      }

      CurrentDir(oldDir);
    }

    if(dir)
      UnLock(dir);

    FreeDosObject(DOS_FIB,fib);
  }

  //
  // now we go and initialize our internally supported codesets but only if
  // we have not already loaded a charset with the same name
  //
  D(DBF_STARTUP, "initializing internal charsets...");

  // ISO-8859-1 + EURO
  if(!(codesetsFind(codesetsList, "ISO-8859-1 + Euro")))
  {
    if(!(codeset = allocVecPooled(CodesetsBase->pool, sizeof(struct codeset)))) goto end;
    codeset->name 	          = mystrdup("ISO-8859-1 + Euro");
    codeset->alt_name 	      = NULL;
    codeset->characterization = mystrdup("West European (with EURO)");
    codeset->read_only 	      = 1;
    for(i = 0; i<256; i++)
    {
      UTF32 *src_ptr = &src;
      UTF8  *dest_ptr = &codeset->table[i].utf8[1];

      if(i==164)
        src = 0x20AC; /* the EURO sign */
      else
        src = i;

      codeset->table[i].code = i;
      codeset->table[i].ucs4 = src;
      CodesetsConvertUTF32toUTF8((const UTF32 **)&src_ptr, src_ptr+1, &dest_ptr, dest_ptr+6, CONVFLG_StrictConversion);
      *dest_ptr = 0;
      codeset->table[i].utf8[0] = (ULONG)dest_ptr-(ULONG)&codeset->table[i].utf8[1];
    }
	  memcpy(codeset->table_sorted,codeset->table,sizeof(codeset->table));
    qsort(codeset->table_sorted,256,sizeof(codeset->table[0]),(int (*)(const void *arg1, const void *arg2))codesetsCmpUnicode);
    AddTail((struct List *)codesetsList, (struct Node *)&codeset->node);
  }

  // ISO-8859-1
  if(!(codesetsFind(codesetsList, "ISO-8859-1")))
  {
    if(!(codeset = allocVecPooled(CodesetsBase->pool, sizeof(struct codeset)))) goto end;
    codeset->name 	          = mystrdup("ISO-8859-1");
    codeset->alt_name 	      = NULL;
    codeset->characterization = mystrdup("West European");
    codeset->read_only 	      = 0;
    for(i = 0; i<256; i++)
    {
      UTF32 *src_ptr = &src;
      UTF8 *dest_ptr = &codeset->table[i].utf8[1];

      src = i;

      codeset->table[i].code = i;
      codeset->table[i].ucs4 = src;
      CodesetsConvertUTF32toUTF8((const UTF32 **)&src_ptr, src_ptr+1, &dest_ptr, dest_ptr+6, CONVFLG_StrictConversion);
      *dest_ptr = 0;
      codeset->table[i].utf8[0] = (ULONG)dest_ptr-(ULONG)&codeset->table[i].utf8[1];
    }
    memcpy(codeset->table_sorted,codeset->table,sizeof (codeset->table));
    qsort(codeset->table_sorted,256,sizeof(codeset->table[0]),(int (*)(const void *arg1,const void *arg2))codesetsCmpUnicode);
    AddTail((struct List *)codesetsList, (struct Node *)&codeset->node);
  }

  // ISO-8859-2
  if(!(codesetsFind(codesetsList, "ISO-8859-2")))
  {
    if(!(codeset = allocVecPooled(CodesetsBase->pool, sizeof(struct codeset)))) goto end;
    codeset->name 	          = mystrdup("ISO-8859-2");
    codeset->alt_name 	      = NULL;
    codeset->characterization = mystrdup("Central/East European");
    codeset->read_only 	      = 0;
    for(i = 0; i<256; i++)
    {
      UTF32 *src_ptr = &src;
      UTF8  *dest_ptr = &codeset->table[i].utf8[1];

      if(i<0xa0)
        src = i;
      else
        src = iso_8859_2_to_ucs4[i-0xa0];

      codeset->table[i].code = i;
      codeset->table[i].ucs4 = src;
      CodesetsConvertUTF32toUTF8((const UTF32 **)&src_ptr, src_ptr+1, &dest_ptr,dest_ptr+6, CONVFLG_StrictConversion);
      *dest_ptr = 0;
      codeset->table[i].utf8[0] = (ULONG)dest_ptr-(ULONG)&codeset->table[i].utf8[1];
    }
    memcpy(codeset->table_sorted, codeset->table, sizeof(codeset->table));
    qsort(codeset->table_sorted,256,sizeof(codeset->table[0]),(int (*)(const void *arg1,const void *arg2))codesetsCmpUnicode);
    AddTail((struct List *)codesetsList, (struct Node *)&codeset->node);
  }

  // ISO-8859-3
  if(!(codesetsFind(codesetsList, "ISO-8859-3")))
  {
    if(!(codeset = allocVecPooled(CodesetsBase->pool, sizeof(struct codeset)))) goto end;
    codeset->name 	          = mystrdup("ISO-8859-3");
    codeset->alt_name 	      = NULL;
    codeset->characterization = mystrdup("South European");
    codeset->read_only 	      = 0;
    for(i = 0; i<256; i++)
    {
      UTF32 *src_ptr = &src;
      UTF8  *dest_ptr = &codeset->table[i].utf8[1];

      if(i<0xa0)
        src = i;
      else
        src = iso_8859_3_to_ucs4[i-0xa0];

      codeset->table[i].code = i;
      codeset->table[i].ucs4 = src;
      CodesetsConvertUTF32toUTF8((const UTF32 **)&src_ptr,src_ptr+1,&dest_ptr,dest_ptr+6,CONVFLG_StrictConversion);
      *dest_ptr = 0;
      codeset->table[i].utf8[0] = (ULONG)dest_ptr-(ULONG)&codeset->table[i].utf8[1];
    }
    memcpy(codeset->table_sorted,codeset->table,sizeof(codeset->table));
    qsort(codeset->table_sorted,256,sizeof(codeset->table[0]),(int (*)(const void *arg1,const void *arg2))codesetsCmpUnicode);
    AddTail((struct List *)codesetsList, (struct Node *)&codeset->node);
  }

  // ISO-8859-4
  if(!(codesetsFind(codesetsList, "ISO-8859-4")))
  {
    if(!(codeset = allocVecPooled(CodesetsBase->pool,sizeof(struct codeset)))) goto end;
    codeset->name 	          = mystrdup("ISO-8859-4");
    codeset->alt_name 	      = NULL;
    codeset->characterization = mystrdup("North European");
    codeset->read_only 	      = 0;
    for(i = 0; i<256; i++)
    {
      UTF32 *src_ptr = &src;
      UTF8  *dest_ptr = &codeset->table[i].utf8[1];

      if(i<0xa0)
        src = i;
      else
        src = iso_8859_4_to_ucs4[i-0xa0];

      codeset->table[i].code = i;
      codeset->table[i].ucs4 = src;
      CodesetsConvertUTF32toUTF8((const UTF32 **)&src_ptr,src_ptr+1,&dest_ptr,dest_ptr+6,CONVFLG_StrictConversion);
      *dest_ptr = 0;
      codeset->table[i].utf8[0] = (ULONG)dest_ptr-(ULONG)&codeset->table[i].utf8[1];
    }
    memcpy(codeset->table_sorted,codeset->table,sizeof(codeset->table));
    qsort(codeset->table_sorted,256,sizeof(codeset->table[0]),(int (*)(const void *arg1, const void *arg2))codesetsCmpUnicode);
    AddTail((struct List *)codesetsList, (struct Node *)&codeset->node);
  }

  // ISO-8859-5
  if(!(codesetsFind(codesetsList, "ISO-8859-5")))
  {
    if(!(codeset = allocVecPooled(CodesetsBase->pool,sizeof(struct codeset)))) goto end;
    codeset->name 	          = mystrdup("ISO-8859-5");
    codeset->alt_name 	      = NULL;
    codeset->characterization = mystrdup("Slavic languages");
    codeset->read_only 	      = 0;
    for(i = 0; i<256; i++)
    {
      UTF32 *src_ptr = &src;
      UTF8  *dest_ptr = &codeset->table[i].utf8[1];

      if(i<0xa0)
        src = i;
      else
        src = iso_8859_5_to_ucs4[i-0xa0];

      codeset->table[i].code = i;
      codeset->table[i].ucs4 = src;
      CodesetsConvertUTF32toUTF8((const UTF32 **)&src_ptr,src_ptr+1,&dest_ptr,dest_ptr+6,CONVFLG_StrictConversion);
      *dest_ptr = 0;
      codeset->table[i].utf8[0] = (ULONG)dest_ptr-(ULONG)&codeset->table[i].utf8[1];
    }
    memcpy(codeset->table_sorted,codeset->table,sizeof(codeset->table));
    qsort(codeset->table_sorted,256,sizeof(codeset->table[0]),(int (*)(const void *arg1,const void *arg2))codesetsCmpUnicode);
    AddTail((struct List *)codesetsList, (struct Node *)&codeset->node);
  }

  // ISO-8859-9
  if(!(codesetsFind(codesetsList, "ISO-8859-9")))
  {
    if(!(codeset = allocVecPooled(CodesetsBase->pool,sizeof(struct codeset)))) goto end;
    codeset->name 	      = mystrdup("ISO-8859-9");
    codeset->alt_name 	      = NULL;
    codeset->characterization = mystrdup("Turkish");
    codeset->read_only 	      = 0;
    for(i = 0; i<256; i++)
    {
      UTF32 *src_ptr = &src;
      UTF8  *dest_ptr = &codeset->table[i].utf8[1];

      if(i<0xa0)
        src = i;
      else
        src = iso_8859_9_to_ucs4[i-0xa0];

      codeset->table[i].code = i;
      codeset->table[i].ucs4 = src;
      CodesetsConvertUTF32toUTF8((const UTF32 **)&src_ptr,src_ptr+1,&dest_ptr,dest_ptr+6,CONVFLG_StrictConversion);
      *dest_ptr = 0;
      codeset->table[i].utf8[0] = (ULONG)dest_ptr-(ULONG)&codeset->table[i].utf8[1];
    }
    memcpy(codeset->table_sorted,codeset->table,sizeof(codeset->table));
    qsort(codeset->table_sorted,256,sizeof(codeset->table[0]),(int (*)(const void *arg1,const void *arg2))codesetsCmpUnicode);
    AddTail((struct List *)codesetsList, (struct Node *)&codeset->node);
  }

  // ISO-8859-15
  if(!(codesetsFind(codesetsList, "ISO-8859-15")))
  {
    if(!(codeset = allocVecPooled(CodesetsBase->pool,sizeof(struct codeset)))) goto end;
    codeset->name 	          = mystrdup("ISO-8859-15");
    codeset->alt_name 	      = NULL;
    codeset->characterization = mystrdup("West European II");
    codeset->read_only 	      = 0;
    for(i = 0; i<256; i++)
    {
      UTF32 *src_ptr = &src;
      UTF8  *dest_ptr = &codeset->table[i].utf8[1];

      if(i<0xa0)
        src = i;
      else
        src = iso_8859_15_to_ucs4[i-0xa0];

      codeset->table[i].code = i;
      codeset->table[i].ucs4 = src;
      CodesetsConvertUTF32toUTF8((const UTF32 **)&src_ptr,src_ptr+1,&dest_ptr,dest_ptr+6,CONVFLG_StrictConversion);
      *dest_ptr = 0;
      codeset->table[i].utf8[0] = (ULONG)dest_ptr-(ULONG)&codeset->table[i].utf8[1];
    }
    memcpy(codeset->table_sorted,codeset->table,sizeof (codeset->table));
    qsort(codeset->table_sorted,256,sizeof(codeset->table[0]),(int (*)(const void *arg1,const void *arg2))codesetsCmpUnicode);
    AddTail((struct List *)codesetsList, (struct Node *)&codeset->node);
  }

  // ISO-8859-16
  if(!(codesetsFind(codesetsList, "ISO-8859-16")))
  {
    if(!(codeset = allocVecPooled(CodesetsBase->pool,sizeof(struct codeset)))) goto end;
	  codeset->name             = mystrdup("ISO-8859-16");
  	codeset->alt_name         = NULL;
	  codeset->characterization = mystrdup("South-Eastern European");
  	codeset->read_only        = 0;
	  for(i=0;i<256;i++)
  	{
      UTF32 *src_ptr = &src;
      UTF8 *dest_ptr = &codeset->table[i].utf8[1];

      if(i < 0xa0)
        src = i;
		  else
        src = iso_8859_16_to_ucs4[i-0xa0];

      codeset->table[i].code = i;
      codeset->table[i].ucs4 = src;
      CodesetsConvertUTF32toUTF8((const UTF32 **)&src_ptr, src_ptr+1, &dest_ptr, dest_ptr+6, CONVFLG_StrictConversion);
      *dest_ptr = 0;
	  	codeset->table[i].utf8[0] = (ULONG)dest_ptr - (ULONG)&codeset->table[i].utf8[1];
  	}
	  memcpy(codeset->table_sorted, codeset->table, sizeof(codeset->table));
  	qsort(codeset->table_sorted, 256, sizeof(codeset->table[0]), (int (*)(const void *arg1, const void *arg2))codesetsCmpUnicode);
    AddTail((struct List *)codesetsList, (struct Node *)&codeset->node);
  }

  // KOI8-R
  if(!(codesetsFind(codesetsList, "KOI8-R")))
  {
    if(!(codeset = allocVecPooled(CodesetsBase->pool,sizeof(struct codeset)))) goto end;
    codeset->name 	            = mystrdup("KOI8-R");
    codeset->alt_name 	        = NULL;
    codeset->characterization   = mystrdup("Russian");
    codeset->read_only 	        = 0;
    for(i = 0; i<256; i++)
    {
      UTF32 *src_ptr = &src;
      UTF8  *dest_ptr = &codeset->table[i].utf8[1];

      if(i<0x80)
        src = i;
      else
        src = koi8r_to_ucs4[i-0x80];

      codeset->table[i].code = i;
      codeset->table[i].ucs4 = src;
      CodesetsConvertUTF32toUTF8((const UTF32 **)&src_ptr,src_ptr+1,&dest_ptr,dest_ptr+6,CONVFLG_StrictConversion);
      *dest_ptr = 0;
      codeset->table[i].utf8[0] = (ULONG)dest_ptr-(ULONG)&codeset->table[i].utf8[1];
    }
    memcpy(codeset->table_sorted,codeset->table,sizeof(codeset->table));
    qsort(codeset->table_sorted,256,sizeof(codeset->table[0]),(int (*)(const void *arg1,const void *arg2))codesetsCmpUnicode);
    AddTail((struct List *)codesetsList, (struct Node *)&codeset->node);
  }

  // AmigaPL
  if(!(codesetsFind(codesetsList, "AmigaPL")))
  {
    if(!(codeset = allocVecPooled(CodesetsBase->pool,sizeof(struct codeset)))) goto end;
    codeset->name 	      = mystrdup("AmigaPL");
    codeset->alt_name 	      = NULL;
    codeset->characterization = mystrdup("AmigaPL");
    codeset->read_only 	      = 1;
    for(i=0; i<256; i++)
    {
      UTF32 *src_ptr = &src;
      UTF8  *dest_ptr = &codeset->table[i].utf8[1];

      if(i<0xa0)
        src = i;
      else
        src = amigapl_to_ucs4[i-0xa0];

      codeset->table[i].code = i;
      codeset->table[i].ucs4 = src;
      CodesetsConvertUTF32toUTF8((const UTF32 **)&src_ptr,src_ptr+1,&dest_ptr,dest_ptr+6,CONVFLG_StrictConversion);
      *dest_ptr = 0;
      codeset->table[i].utf8[0] = (ULONG)dest_ptr-(ULONG)&codeset->table[i].utf8[1];
    }
    memcpy(codeset->table_sorted,codeset->table,sizeof(codeset->table));
    qsort(codeset->table_sorted,256,sizeof(codeset->table[0]),(int (*)(const void *arg1,const void *arg2))codesetsCmpUnicode);
    AddTail((struct List *)codesetsList, (struct Node *)&codeset->node);
  }

  // Amiga-1251
  if(!(codesetsFind(codesetsList, "Amiga-1251")))
  {
    if(!(codeset = allocVecPooled(CodesetsBase->pool,sizeof(struct codeset)))) goto end;
	  codeset->name             = mystrdup("Amiga-1251");
  	codeset->alt_name         = NULL;
	  codeset->characterization = mystrdup("Amiga-1251");
  	codeset->read_only        = 1;
	  for(i=0; i<256; i++)
  	{
      UTF32 *src_ptr = &src;
      UTF8 *dest_ptr = &codeset->table[i].utf8[1];

      if(i < 0xa0)
        src = i;
      else
        src = amiga1251_to_ucs4[i-0xa0];
		
      codeset->table[i].code = i;
      codeset->table[i].ucs4 = src;
      CodesetsConvertUTF32toUTF8((const UTF32 **)&src_ptr, src_ptr+1, &dest_ptr, dest_ptr+6, CONVFLG_StrictConversion);
      *dest_ptr = 0;
      codeset->table[i].utf8[0] = (char*)dest_ptr - (char*)&codeset->table[i].utf8[1];
  	}
	  memcpy(codeset->table_sorted,codeset->table,sizeof(codeset->table));
  	qsort(codeset->table_sorted,256,sizeof(codeset->table[0]),(int (*)(const void *arg1, const void *arg2))codesetsCmpUnicode);
	  AddTail((struct List *)codesetsList, (struct Node *)&codeset->node);
  }

end:
  ReleaseSemaphore(&CodesetsBase->poolSem);

  RETURN(codeset != 0);
  return codeset != NULL;
}

/**************************************************************************/
/*
 * Cleanup the memory for the codeset
 */

void
codesetsCleanup(struct MinList *codesetsList)
{
  struct codeset *code;

  ENTER();

  while((code = (struct codeset *)RemHead((struct List *)codesetsList)))
  {
    if(code->name) freeArbitrateVecPooled(code->name);
    if(code->alt_name) freeArbitrateVecPooled(code->alt_name);
    if(code->characterization) freeArbitrateVecPooled(code->characterization);

    freeArbitrateVecPooled(code);
  }

  LEAVE();
}

/**************************************************************************/

static struct codeset *
defaultCodeset(ULONG sem)
{
  char buf[256];
  struct codeset *codeset;

  ENTER();

  if(sem)
    ObtainSemaphoreShared(&CodesetsBase->libSem);

  *buf = 0;
  GetVar("codeset_default",buf,sizeof(buf),GVF_GLOBAL_ONLY);

  if(!*buf || !(codeset = codesetsFind(&CodesetsBase->codesets,buf)))
    codeset = CodesetsBase->systemCodeset;

  if(sem)
    ReleaseSemaphore(&CodesetsBase->libSem);

  RETURN(codeset);
  return codeset;
}

/**************************************************************************/

/*
 * Returns the given codeset.
 */

struct codeset *
codesetsFind(struct MinList *codesetsList,STRPTR name)
{
  struct codeset *res = NULL;

  ENTER();

  if(name && *name)
  {
    struct codeset *mstate, *succ;

    for(mstate = (struct codeset *)codesetsList->mlh_Head; (succ = (struct codeset *)mstate->node.mln_Succ); mstate = succ)
    {
      if(!stricmp(name, mstate->name) || (mstate->alt_name != NULL && !stricmp(name, mstate->alt_name)))
        break;
    }

    if(succ)
      res = mstate;
  }

  RETURN(res);
  return res;
}

/**************************************************************************/

struct codeset *LIBFUNC
CodesetsSetDefaultA(REG(a0, STRPTR name),
                    REG(a1, struct TagItem *attrs))
{
  struct codeset *codeset;

  ENTER();

  ObtainSemaphoreShared(&CodesetsBase->libSem);

  if((codeset = codesetsFind(&CodesetsBase->codesets,name)))
  {
    ULONG flags;

    flags = GVF_SAVE_VAR | (GetTagData(CODESETSA_Save,FALSE,attrs) ? GVF_GLOBAL_ONLY : 0);

    SetVar("codeset_default",codeset->name,strlen(codeset->name),flags);
  }

  ReleaseSemaphore(&CodesetsBase->libSem);

  RETURN(codeset);
  return codeset;
}

LIBSTUB(CodesetsSetDefaultA, struct codeset *, REG(a0, STRPTR name), REG(a1, struct TagItem *attrs))
{
  #ifdef __MORPHOS__
  return CodesetsSetDefaultA((STRPTR)REG_A0,(struct TagItem *)REG_A1);
  #else
  return CodesetsSetDefaultA(name, attrs);
  #endif
}

#ifdef __amigaos4__
LIBSTUBVA(CodesetsSetDefault, struct codeset *, REG(a0, STRPTR name), ...)
{
  struct codeset *cs;
  VA_LIST args;

  VA_START(args, name);
  cs = CodesetsSetDefaultA(name, VA_ARG(args, struct TagItem *));
  VA_END(args);

  return cs;
}
#endif

/**************************************************************************/

struct codeset *LIBFUNC
CodesetsFindA(REG(a0, STRPTR name), REG(a1, struct TagItem *attrs))
{
  struct codeset *codeset;

  ENTER();

  ObtainSemaphoreShared(&CodesetsBase->libSem);

  codeset = codesetsFind(&CodesetsBase->codesets,name);

  if(!codeset && GetTagData(CODESETSA_NoFail,TRUE,attrs))
    codeset = defaultCodeset(FALSE);

  ReleaseSemaphore(&CodesetsBase->libSem);

  RETURN(codeset);
  return codeset;
}

LIBSTUB(CodesetsFindA, struct codeset *, REG(a0, STRPTR name), REG(a1, struct TagItem *attrs))
{
  #ifdef __MORPHOS__
  return CodesetsFindA((STRPTR)REG_A0,(struct TagItem *)REG_A1);
  #else
  return CodesetsFindA(name, attrs);
  #endif
}

#ifdef __amigaos4__
LIBSTUBVA(CodesetsFind, struct codeset *, REG(a0, STRPTR name), ...)
{
  struct codeset *cs;
  VA_LIST args;

  VA_START(args, name);
  cs = CodesetsFindA(name, VA_ARG(args, struct TagItem *));
  VA_END(args);

  return cs;
}
#endif

/**************************************************************************/

/*
 * Returns the best codeset for the given text
 */

struct codeset *
codesetsFindBest(struct MinList *codesetsList,STRPTR text,int text_len,int *error_ptr)
{
  struct codeset *codeset, *best_codeset = NULL;
  int            best_errors = text_len;

  codeset = (struct codeset *)codesetsList->mlh_Head;

  while(codeset)
  {
    if(!codeset->read_only)
    {
      struct single_convert conv;
      STRPTR       text_ptr = text;
      int          i, errors = 0;

      for(i = 0; i<text_len; i++)
      {
        unsigned char c = *text_ptr++;

        if(c)
        {
          int len = trailingBytesForUTF8[c];

          conv.utf8[1] = c;
          strncpy((char*)&conv.utf8[2], text_ptr, len);
          conv.utf8[2+len] = 0;
          text_ptr += len;

          if(!bsearch(&conv,codeset->table_sorted,256,sizeof(codeset->table_sorted[0]),(APTR)codesetsCmpUnicode))
            errors++;
        }
        else
          break;
      }

      if(errors<best_errors)
      {
        best_codeset = codeset;
        best_errors = errors;
      }

      if(!best_errors)
        break;
    }

    codeset = (struct codeset *)codeset->node.mln_Succ;
  }

  if(!best_codeset)
    best_codeset = defaultCodeset(FALSE);

  if(error_ptr)
    *error_ptr = best_errors;

  RETURN(best_codeset);
  return best_codeset;
}

/**************************************************************************/

struct codeset *LIBFUNC
CodesetsFindBestA(REG(a0, STRPTR text),
                  REG(d0, ULONG text_len),
                  REG(a1, ULONG *error_ptr),
                  REG(a2, UNUSED struct TagItem *attrs))
{
  struct codeset *codeset;

  ENTER();

  ObtainSemaphoreShared(&CodesetsBase->libSem);

  codeset = codesetsFindBest(&CodesetsBase->codesets,text,text_len,(int *)error_ptr);

  ReleaseSemaphore(&CodesetsBase->libSem);

  RETURN(codeset);
  return codeset;
}

LIBSTUB(CodesetsFindBestA, struct codeset *, REG(a0, STRPTR text),
                                             REG(d0, ULONG text_len),
                                             REG(a1, ULONG *error_ptr),
                                             REG(a2, struct TagItem *attrs))
{
  #ifdef __MORPHOS__
  return CodesetsFindBestA((STRPTR)REG_A0,(ULONG)REG_D0,(ULONG *)REG_A1,(struct TagItem *)REG_A2);
  #else
  return CodesetsFindBestA(text, text_len, error_ptr, attrs);
  #endif
}

#ifdef __amigaos4__
LIBSTUBVA(CodesetsFindBest, struct codeset *, REG(a0, STRPTR text),
                                              REG(d0, ULONG text_len),
                                              REG(a1, ULONG *error_ptr), ...)
{
  struct codeset *cs;
  VA_LIST args;

  VA_START(args, error_ptr);
  cs = CodesetsFindBestA(text, text_len, error_ptr, VA_ARG(args, struct TagItem *));
  VA_END(args);

  return cs;
}
#endif

/**************************************************************************/

/*
 * Returns the number of characters a utf8 string has. This is not
 * identically with the size of memory is required to hold the string.
 */

ULONG LIBFUNC
CodesetsUTF8Len(REG(a0, UTF8 *str))
{
  int           len;
  unsigned char c;

  ENTER();

  if(!str)
    return 0;

  len = 0;

  while((c = *str++))
  {
    len++;
    str += trailingBytesForUTF8[c];
  }

  RETURN((ULONG)len);
  return (ULONG)len;
}

LIBSTUB(CodesetsUTF8Len, ULONG, REG(a0, UTF8* str))
{
  #ifdef __MORPHOS__
  return CodesetsUTF8Len((UTF8 *)REG_A0);
  #else
  return CodesetsUTF8Len(str);
  #endif
}

/**************************************************************************/

ULONG LIBFUNC
CodesetsStrLenA(REG(a0, STRPTR str),
                REG(a1, struct TagItem *attrs))
{
  struct codeset *codeset;
  int            len, res;
  STRPTR         src;
  UBYTE          c;

  ENTER();

  if(!str)
    return 0;

  if(!(codeset = (struct codeset *)GetTagData(CODESETSA_Codeset, 0, attrs)))
    codeset = defaultCodeset(TRUE);

  len = GetTagData(CODESETSA_SourceLen,UINT_MAX,attrs);

  src = str;
  res = 0;

  while(((c = *src++) && (len--)))
    res += codeset->table[c].utf8[0];

  RETURN((ULONG)res);
  return (ULONG)res;
}

LIBSTUB(CodesetsStrLenA, ULONG, REG(a0, STRPTR str),
                                REG(a1, struct TagItem *attrs))
{
  #ifdef __MORPHOS__
  return CodesetsStrLenA((STRPTR)REG_A0,(struct TagItem *)REG_A1);
  #else
  return CodesetsStrLenA(str, attrs);
  #endif
}

#ifdef __amigaos4__
LIBSTUBVA(CodesetsStrLen, ULONG, REG(a0, STRPTR str), ...)
{
  ULONG res;
  VA_LIST args;

  VA_START(args, str);
  res = CodesetsStrLenA(str, VA_ARG(args, struct TagItem *));
  VA_END(args);

  return res;
}
#endif

/**************************************************************************/
/*
 * Converts a UTF8 string to a given charset. Return the number of bytes
 * written to dest excluding the NULL byte (which is always ensured by this
 * function; it means a NULL str will produce "" as dest; anyway you should
 * check NULL str to not waste your time!).
 */

STRPTR LIBFUNC
CodesetsUTF8ToStrA(REG(a0, struct TagItem *attrs))
{
  UTF8   *str;
  STRPTR dest;
  ULONG  *destLenPtr;
  ULONG  n;

  ENTER();

  str = (UTF8 *)GetTagData(CODESETSA_Source, 0, attrs);
  if(!str)
    return NULL;

  dest = NULL;
  n    = 0;

  if(str)
  {
    struct convertMsg              msg;
    struct codeset        *codeset;
    struct Hook           *hook;
    struct single_convert *f;
    char                 	         buf[256];
    STRPTR                destIter = NULL, b = NULL;
    ULONG                 destLen;
    int                   i = 0;

    hook    = (struct Hook *)GetTagData(CODESETSA_DestHook, 0, attrs);
    destLen = GetTagData(CODESETSA_DestLen,0,attrs);

    if(hook)
    {
      if(destLen<16 || destLen>sizeof(buf)) destLen = sizeof(buf);

      msg.state = CODESETV_Translating;
      b = buf;
      i = 0;
    }
    else
    {
      APTR                   pool;
      struct SignalSemaphore *sem;

      //if (destLen==0) return NULL;

      if(!(dest = (STRPTR)GetTagData(CODESETSA_Dest, 0, attrs)) ||
        GetTagData(CODESETSA_AllocIfNeeded,TRUE,attrs))
      {
        ULONG len;
        UBYTE c, *s;

        len = 0;
        s   = str;

        while ((c = *s++))
        {
            len++;
            s += trailingBytesForUTF8[c];
        }

        if(!dest || (destLen<len+1))
        {
          if((pool = (APTR)GetTagData(CODESETSA_Pool, 0, attrs)))
          {
            if((sem = (struct SignalSemaphore *)GetTagData(CODESETSA_PoolSem, 0, attrs)))
            {
              ObtainSemaphore(sem);
            }

            dest = allocVecPooled(pool,len+1);

            if(sem)
              ReleaseSemaphore(sem);
          }
          else
            dest = allocArbitrateVecPooled(len+1);

          destLen  = len+1;
        }

        if(!dest)
          return NULL;
      }

      destIter = dest;
    }

    if(!(codeset = (struct codeset *)GetTagData(CODESETSA_Codeset, 0, attrs)))
      codeset = defaultCodeset(TRUE);

    for(;;n++)
    {
      UBYTE c, d;

      if(!hook)
      {
        if(n>=destLen-1)
          break;
      }

      if((c = *str))
      {
        if(c>127)
        {
          int lenAdd = trailingBytesForUTF8[c], lenStr = lenAdd+1;

          BIN_SEARCH(codeset->table_sorted, 0, 255, strncmp((char *)str, (char *)codeset->table_sorted[m].utf8+1, lenStr), f);

          if(f)
            d = f->code;
          else
            d = '_';

          str += lenAdd;
        }
        else
          d = c;

        if(hook)
        {
          *b++ = d;
          i++;

          if(i%(destLen-1)==0)
          {
            *b = 0;
            msg.len = i;
            CallHookPkt(hook, &msg, buf);

            b  = buf;
            *b = 0;
            i  = 0;
          }
        }
        else
        {
          *destIter++ = d;
        }

        str++;
      }
      else
        break;
    }

    if(hook)
    {
      msg.state = CODESETV_End;
      msg.len   = i;
      *b        = 0;
      CallHookPkt(hook,&msg,buf);
    }
    else
    {
      *destIter = 0;
    }
  }

  if((destLenPtr = (ULONG *)GetTagData(CODESETSA_DestLenPtr, 0, attrs)))
    *destLenPtr = n;

  RETURN(dest);
  return dest;
}

LIBSTUB(CodesetsUTF8ToStrA, STRPTR, REG(a0, struct TagItem *attrs))
{
  #ifdef __MORPHOS__
  return CodesetsUTF8ToStrA((struct TagItem *)REG_A0);
  #else
  return CodesetsUTF8ToStrA(attrs);
  #endif
}

#ifdef __amigaos4__
LIBSTUBVA(CodesetsUTF8ToStr, STRPTR, ...)
{
  STRPTR res;
  VA_LIST args;

  VA_START(args, self);
  res = CodesetsUTF8ToStrA(VA_ARG(args, struct TagItem *));
  VA_END(args);

  return res;
}
#endif

/**************************************************************************/
/*
 * Converts a string and a charset to an UTF8. Returns the UTF8.
 * If a destination hook is supplied always return 0.
 * If from is NULL, it returns NULL and doesn't call the hook.
 */

UTF8 *LIBFUNC
CodesetsUTF8CreateA(REG(a0, struct TagItem *attrs))
{
  UTF8   *from;
  UTF8   *dest;
  ULONG  fromLen, *destLenPtr;
  ULONG  n;

  ENTER();

  dest = NULL;
  n    = 0;

  from = (UTF8*)GetTagData(CODESETSA_Source, 0, attrs);
  fromLen = GetTagData(CODESETSA_SourceLen, UINT_MAX, attrs);

  if(from && fromLen)
  {
    struct convertMsg       msg;
    struct codeset *codeset;
    struct Hook    *hook;
    ULONG          destLen;
    int            i = 0;
    UBYTE                		buf[256];
    UBYTE          *src, *destPtr = NULL, *b = NULL, c;

    if(!(codeset = (struct codeset *)GetTagData(CODESETSA_Codeset, 0, attrs)))
      codeset = defaultCodeset(TRUE);

    hook    = (struct Hook *)GetTagData(CODESETSA_DestHook, 0, attrs);
    destLen = GetTagData(CODESETSA_DestLen,0,attrs);

    if(hook)
    {
      if(destLen<16 || destLen>sizeof(buf))
        destLen = sizeof(buf);

      msg.state = CODESETV_Translating;
      b = buf;
      i = 0;
    }
    else
    {
      if(!(dest = (UTF8*)GetTagData(CODESETSA_Dest, 0, attrs)) ||
        GetTagData(CODESETSA_AllocIfNeeded,TRUE,attrs))
      {
        ULONG len, flen;

        flen = fromLen;
        len  = 0;
        src  = from;

        while(((c = *src++) && (flen--)))
          len += codeset->table[c].utf8[0];

        if(!dest || (destLen<len+1))
        {
          APTR                   pool;
          struct SignalSemaphore *sem;

          if((pool = (APTR)GetTagData(CODESETSA_Pool, 0, attrs)))
          {
            if((sem = (struct SignalSemaphore *)GetTagData(CODESETSA_PoolSem, 0, attrs)))
            {
              ObtainSemaphore(sem);
            }

            dest = allocVecPooled(pool,len+1);

            if(sem)
              ReleaseSemaphore(sem);
          }
          else
            dest = allocArbitrateVecPooled(len+1);

          destLen  = len;
        }

        if(!dest)
          return NULL;
      }

      destPtr = (UBYTE*)dest;
    }

    for(src = from; fromLen && (c = *src); src++, fromLen--)
    {
      UTF8* utf8_seq;

      for(utf8_seq = &codeset->table[c].utf8[1]; (c = *utf8_seq); utf8_seq++)
      {
        if(!hook)
        {
          if(n>=destLen)
            break;
        }

        if(hook)
        {
          *b++ = c;
          i++;

          if(i%(destLen-1)==0)
          {
            *b = 0;
            msg.len = i;
            CallHookPkt(hook,&msg,buf);

            b  = buf;
            *b = 0;
            i  = 0;
          }
        }
        else
        {
          *destPtr++ = c;
        }

        n++;
      }
    }

    if(hook)
    {
      msg.state = CODESETV_End;
      msg.len   = i;
      *b = 0;
      CallHookPkt(hook,&msg,buf);
    }
    else
    {
      *destPtr = 0;
    }
  }

  if((destLenPtr = (ULONG *)GetTagData(CODESETSA_DestLenPtr, 0, attrs)))
    *destLenPtr = n;

  RETURN(dest);
  return dest;
}

LIBSTUB(CodesetsUTF8CreateA, UTF8*, REG(a0, struct TagItem *attrs))
{
  #ifdef __MORPHOS__
  return CodesetsUTF8CreateA((struct TagItem *)REG_A0);
  #else
  return CodesetsUTF8CreateA(attrs);
  #endif
}

#ifdef __amigaos4__
LIBSTUBVA(CodesetsUTF8Create, UTF8*, ...)
{
  UTF8 *res;
  VA_LIST args;

  VA_START(args, self);
  res = CodesetsUTF8CreateA(VA_ARG(args, struct TagItem *));
  VA_END(args);

  return res;
}
#endif

/**************************************************************************/

static int
parseUtf8(STRPTR *ps)
{
  STRPTR s = *ps;
  int    wc, n, i;

  ENTER();

  if(*s<0x80)
  {
    *ps = s+1;

    RETURN(*s);
    return *s;
  }

  if(*s<0xc2)
  {
    RETURN(-1);
    return -1;
  }
  else
  {
    if(*s<0xe0)
    {
      if((s[1] & 0xc0)!=0x80)
      {
        RETURN(-1);
        return -1;
      }

      *ps = s+2;

      RETURN(((s[0] & 0x1f)<<6) | (s[1] & 0x3f));
      return ((s[0] & 0x1f)<<6) | (s[1] & 0x3f);
    }
    else
    {
      if(*s<0xf0)
      {
        n = 3;
      }
      else
      {
        if(*s<0xf8)
        {
          n = 4;
        }
        else
        {
          if(*s<0xfc)
          {
            n = 5;
          }
          else
          {
            if(*s<0xfe)
            {
              n = 6;
            }
            else
            {
              RETURN(-1);
              return -1;
            }
          }
        }
      }
    }
  }

  wc = *s++ & ((1<<(7-n))-1);

  for(i = 1; i<n; i++)
  {
    if((*s & 0xc0) != 0x80)
    {
      RETURN(-1);
      return -1;
    }

    wc = (wc << 6) | (*s++ & 0x3f);
  }

  if(wc < (1 << (5 * n - 4)))
  {
    RETURN(-1);
    return -1;
  }

  *ps = s;

  RETURN(wc);
  return wc;
}

#define GOOD_UCS(c) \
     ((c) >= 160 && ((c) & ~0x3ff) != 0xd800 && \
      (c) != 0xfeff && (c) != 0xfffe && (c) != 0xffff)

ULONG LIBFUNC
CodesetsIsValidUTF8(REG(a0, STRPTR s))
{
  ENTER();

  STRPTR  t = s;
  int n;

  while((n = parseUtf8(&t)))
  {
    if(!GOOD_UCS(n))
    {
      RETURN(FALSE);
      return FALSE;
    }
  }

  RETURN(TRUE);
  return TRUE;
}

LIBSTUB(CodesetsIsValidUTF8, ULONG, REG(a0, STRPTR s))
{
  #ifdef __MORPHOS__
  return CodesetsIsValidUTF8((STRPTR)REG_A0);
  #else
  return CodesetsIsValidUTF8(s);
  #endif
}

/***********************************************************************/

void LIBFUNC
CodesetsFreeVecPooledA(REG(a0, APTR pool),
                       REG(a1, APTR mem),
                       REG(a2, struct TagItem *attrs))
{
  ENTER();

  if(pool && mem)
  {
    struct SignalSemaphore *sem;

	  if((sem = (struct SignalSemaphore *)GetTagData(CODESETSA_PoolSem, 0, attrs)))
      ObtainSemaphore(sem);

    freeVecPooled(pool,mem);

    if(sem)
      ReleaseSemaphore(sem);
  }

  LEAVE();
}

LIBSTUB(CodesetsFreeVecPooledA, void, REG(a0, APTR pool),
                                      REG(a1, APTR mem),
                                      REG(a2, struct TagItem *attrs))
{
  #ifdef __MORPHOS__
  return CodesetsFreeVecPooledA((APTR)REG_A0,(APTR)REG_A1,(struct TagItem *)REG_A2);
  #else
  return CodesetsFreeVecPooledA(pool, mem, attrs);
  #endif
}

#ifdef __amigaos4__
LIBSTUBVA(CodesetsFreeVecPooled, void, REG(a0, APTR pool),
                                       REG(a1, APTR mem), ...)
{
  VA_LIST args;

  VA_START(args, mem);
  CodesetsFreeVecPooledA(pool, mem, VA_ARG(args, struct TagItem *));
  VA_END(args);
}
#endif

/***********************************************************************/
