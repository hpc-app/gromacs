#include <ctype.h>
#include "fatal.h"
#include "macros.h"
#include "smalloc.h"
#include "futil.h"
#include "filenm.h"
#include "string2.h"
#include "gmxfio.h"

typedef struct {
  int  iFTP;
  bool bOpen,bRead,bDouble,bDebug,bStdio;
  char *fn;
  FILE *fp;
  XDR  *xdr;
} t_fileio;

/* These simple lists define the I/O type for these files */
static int ftpXDR[] = { efTPR, efTRR, efEDR, efXTC };
static int ftpASC[] = { efTPA, efGRO, efPDB };
static int ftpBIN[] = { efTPB, efTRJ, efMTX, efENE };

bool in_ftpset(int ftp,int nset,int set[])
{
  int i;
  bool bResult;
  
  bResult = FALSE;
  for(i=0; (i<nset); i++)
    if (ftp == set[i])
      bResult = TRUE;
  
  return bResult;    
}

static bool do_dummy(void *item,int nitem,int eio,
		     char *desc,char *srcfile,int line)
{
  fatal_error(0,"fio_select not called!");
  
  return FALSE;
}

/* Global variables */
do_func *do_read  = do_dummy;
do_func *do_write = do_dummy;
char *itemstr[eitemNR] = {
  "[header]",      "[inputrec]",   "[box]",         "[topology]", 
  "[coordinates]", "[velocities]", "[forces]"
};
/* Comment strings for TPA only */
char *comment_str[eitemNR] = {
  "; The header holds information on the number of atoms etc. and on whether\n"
  "; certain items are present in the file or not.\n"
  "; \n"
  ";                             WARNING\n"
  ";                   DO NOT EDIT THIS FILE BY HAND\n"
  "; The GROMACS preprocessor performs a lot of checks on your input that\n"
  "; you ignore when editing this. Your simulation may crash because of this\n",
  
  "; The inputrec holds the parameters for MD such as the number of steps,\n"
  "; the timestep and the cut-offs.\n",
  "; The simulation box in nm.\n",
  "; The topology section describes the topology of the molcecules\n"
  "; i.e. bonds, angles and dihedrals etc. and also holds the force field\n"
  "; parameters.\n",
  "; The atomic coordinates in nm\n",
  "; The atomic velocities in nm/ps\n",
  "; The forces on the atoms in nm/ps^2\n"
};


/* Local variables */
static t_fileio *FIO = NULL;
static t_fileio *curfio = NULL;
static int  nFIO = 0;
static char *eioNames[eioNR] = { "REAL", "INT", "NUCHAR", "USHORT", 
				 "RVEC", "NRVEC", "IVEC", "STRING" };
static char *add_comment = NULL;

static char *dbgstr(char *desc)
{
  static char *null_str="";
  static char buf[STRLEN];
  
  if (!curfio->bDebug)
    return null_str;
  else {
    sprintf(buf,"  ; %s %s",add_comment ? add_comment : "",desc);
    return buf;
  }
}

void set_comment(char *comment)
{
  if (comment)
    add_comment = strdup(comment);
}

void unset_comment(void)
{
  if (add_comment)
    sfree(add_comment);
  add_comment = NULL;
}


static void _check_nitem(int eio,int nitem,char *file,int line)
{
  if ((nitem != 1) && !((eio == eioNRVEC) || (eio == eioNUCHAR)))
    fatal_error(0,"nitem (%d) may differ from 1 only for %s or %s, not for %s"
		"(%s, %d)",nitem,eioNames[eioNUCHAR],eioNames[eioNRVEC],
		eioNames[eio],file,line);
}

#define check_nitem() _check_nitem(eio,nitem,__FILE__,__LINE__)

static void fe(int eio,char *desc,char *srcfile,int line)
{
  fatal_error(0,"Trying to %s %s type %d (%s), src %s, line %d",
	      curfio->bRead ? "read" : "write",desc,eio,
	      ((eio >= 0) && (eio < eioNR)) ? eioNames[eio] : "unknown",
	      srcfile,line);
}

#define FE() fe(eio,desc,__FILE__,__LINE__)

static bool do_ascwrite(void *item,int nitem,int eio,
			char *desc,char *srcfile,int line)
{
  int  i;
  int  res,*iptr;
  real *ptr;
  unsigned char *ucptr;
  
  check_nitem();
  switch (eio) {
  case eioREAL:
    res = fprintf(curfio->fp,"%18.10e%s\n",*((real *)item),dbgstr(desc));
    break;
  case eioINT:
    res = fprintf(curfio->fp,"%18d%s\n",*((int *)item),dbgstr(desc));
    break;
  case eioNUCHAR:
    ucptr = (unsigned char *)item;
    for(i=0; (i<nitem); i++)
      res = fprintf(curfio->fp,"%4d",(int)ucptr[i]);
    fprintf(curfio->fp,"%s\n",dbgstr(desc));
    break;
  case eioUSHORT:
    res = fprintf(curfio->fp,"%18d%s\n",*((unsigned short *)item),
		  dbgstr(desc));
    break;
  case eioRVEC:
    ptr = (real *)item;
    res = fprintf(curfio->fp,"%18.10e%18.10e%18.10e%s\n",
		  ptr[XX],ptr[YY],ptr[ZZ],dbgstr(desc));
    break;
  case eioNRVEC:
    for(i=0; (i<nitem); i++) {
      ptr = ((rvec *)item)[i];
      res = fprintf(curfio->fp,"%18.10e%18.10e%18.10e%s\n",
		    ptr[XX],ptr[YY],ptr[ZZ],dbgstr(desc));
    }
    break;
  case eioIVEC:
    iptr= (int *)item;
    res = fprintf(curfio->fp,"%18d%18d%18d%s\n",
		  iptr[XX],iptr[YY],iptr[ZZ],dbgstr(desc));
    break;
  case eioSTRING:
    res = fprintf(curfio->fp,"%-18s%s\n",(char *)item,dbgstr(desc));
    break;
  default:
    FE();
  }
  if ((res <= 0) && curfio->bDebug)
    fprintf(stderr,"Error writing %s %s to file %s (source %s, line %d)\n",
	    eioNames[eio],desc,curfio->fn,srcfile,line);
  return (res > 0);
}

/* This is a global variable that is reset when a file is opened. */
static  int  nbuf=0;

static char *next_item(FILE *fp)
{
  /* This routine reads strings from the file fp, strips comment
   * and buffers. If there are multiple strings on a line, they will
   * be stored here, and indices in the line buffer (buf) will be
   * stored in bufindex. This way we can uncomment on the fly,
   * without too much double work. Each string is first read through
   * fscanf in this routine, and then through sscanf in do_read.
   * No unnecessary string copying is done.
   */
#define MAXBUF 20
  static  char buf[STRLEN];
  static  int  bufindex[MAXBUF];
  int     i,j0;
  char    ccc;
  
  if (nbuf) {
    j0 = bufindex[0];
    for(i=1; (i<nbuf); i++)
      bufindex[i-1] = bufindex[i];
    nbuf--;

    return buf+j0;
  }
  else {
    /* First read until we find something that is not comment */
    if (fgets2(buf,STRLEN-1,fp) == NULL)
      fatal_error(0,"End of file");
      
    i = 0;
    do {
      /* Skip over leading spaces */
      while ((buf[i] != '\0') && (buf[i] != ';') && isspace(buf[i]))
	i++;

      /* Store start of something non-space */
      j0 = i;
      
      /* Look for next spaces */
      while ((buf[i] != '\0') && (buf[i] != ';') && !isspace(buf[i]))
	i++;
	
      /* Store the last character in the string */
      ccc = buf[i];
      
      /* If the string is non-empty, add it to the list */
      if (i > j0) {
	buf[i] = '\0';
	bufindex[nbuf++] = j0;
	
	/* We increment i here; otherwise the next test for buf[i] would be 
	 * '\0', since we test the main loop for ccc anyway, we cant go SEGV
	 */
	i++;
      }
    } while ((ccc != '\0') && (ccc != ';'));

    return next_item(fp);
  }
}

static bool do_ascread(void *item,int nitem,int eio,
		       char *desc,char *srcfile,int line)
{
  FILE   *fp = curfio->fp;
  int    i,m,res,*iptr,ix;
  double d,x;
  real   *ptr;
  unsigned char *ucptr;
  char   *cptr;
  
  check_nitem();  
  switch (eio) {
  case eioREAL:
    res = sscanf(next_item(fp),"%lf",&d);
    if (item) *((real *)item) = d;
    break;
  case eioINT:
    res = sscanf(next_item(fp),"%d",&i);
    if (item) *((int *)item) = i;
    break;
  case eioNUCHAR:
    ucptr = (unsigned char *)item;
    for(i=0; (i<nitem); i++) {
      res = sscanf(next_item(fp),"%d",&ix);
      if (item) ucptr[i] = ix;
    }
    break;
  case eioUSHORT:
    res = sscanf(next_item(fp),"%d",&i);
    if (item) *((unsigned short *)item) = i;
    break;
  case eioRVEC:
    ptr = (real *)item;
    for(m=0; (m<DIM); m++) {
      res = sscanf(next_item(fp),"%lf\n",&x);
      ptr[m] = x;
    }
    break;
  case eioNRVEC:
    for(i=0; (i<nitem); i++) {
      ptr = ((rvec *)item)[i];
      for(m=0; (m<DIM); m++) {
	res = sscanf(next_item(fp),"%lf\n",&x);
	if (item) ptr[m] = x;
      }
    }
    break;
  case eioIVEC:
    iptr = (int *)item;
    for(m=0; (m<DIM); m++) {
      res = sscanf(next_item(fp),"%d\n",&ix);
      if (item) iptr[m] = ix;
    }
    break;
  case eioSTRING:
    cptr = next_item(fp);
    if (item) res = sscanf(cptr,"%s",(char *)item);
    break;
  default:
    FE();
  }
  if ((res <= 0) && curfio->bDebug)
    fprintf(stderr,"Error reading %s %s from file %s (source %s, line %d)\n",
	    eioNames[eio],desc,curfio->fn,srcfile,line);
  
  return (res > 0);
}

static bool do_binwrite(void *item,int nitem,int eio,
			char *desc,char *srcfile,int line)
{
  size_t size,wsize;
  int    ssize;
  
  check_nitem();
  switch (eio) {
  case eioREAL:
    size = sizeof(real);
    break;
  case eioINT:
    size = sizeof(int);
    break;
  case eioNUCHAR:
    size = sizeof(unsigned char);
    break;
  case eioUSHORT:
    size = sizeof(unsigned short);
    break;
  case eioRVEC:
    size = sizeof(rvec);
    break;
  case eioNRVEC:
    size = sizeof(rvec);
    break;
  case eioIVEC:
    size = sizeof(ivec);
    break;
  case eioSTRING:
    size = ssize = strlen((char *)item)+1;
    do_binwrite(&ssize,1,eioINT,desc,srcfile,line);
    break;
  default:
    FE();
  }
  wsize = fwrite(item,size,nitem,curfio->fp);
  
  if ((wsize != nitem) && curfio->bDebug) {
    fprintf(stderr,"Error writing %s %s to file %s (source %s, line %d)\n",
	    eioNames[eio],desc,curfio->fn,srcfile,line);
    fprintf(stderr,"written size %u bytes, source size %u bytes\n",
	    wsize,size);
  }
  return (wsize == nitem);
}

static bool do_binread(void *item,int nitem,int eio,
		       char *desc,char *srcfile,int line)
{
  size_t size,rsize;
  int    ssize;
  
  check_nitem();
  switch (eio) {
  case eioREAL:
    if (curfio->bDouble)
      size = sizeof(double);
    else
      size = sizeof(float);
    break;
  case eioINT:
    size = sizeof(int);
    break;
  case eioNUCHAR:
    size = sizeof(unsigned char);
    break;
  case eioUSHORT:
    size = sizeof(unsigned short);
    break;
  case eioRVEC:
  case eioNRVEC:
    if (curfio->bDouble)
      size = sizeof(double)*DIM;
    else
      size = sizeof(float)*DIM;
    break;
  case eioIVEC:
    size = sizeof(ivec);
    break;
  case eioSTRING:
    do_binread(&ssize,1,eioINT,desc,srcfile,line);
    size = ssize;
    break;
  default:
    FE();
  }
  if (item)
    rsize = fread(item,size,nitem,curfio->fp);
  else {
    /* Skip over it if we have a NULL pointer here */
    fseek(curfio->fp,(long) (size*nitem),SEEK_CUR);
    rsize = nitem;
  }
  if ((rsize != nitem) && (curfio->bDebug))
    fprintf(stderr,"Error reading %s %s from file %s (source %s, line %d)\n",
	    eioNames[eio],desc,curfio->fn,srcfile,line);
	    
  return (rsize == nitem);
}

#ifdef USE_XDR
static bool do_xdr(void *item,int nitem,int eio,
		   char *desc,char *srcfile,int line)
{
  unsigned char *ucptr;
  bool_t res;
  float  fvec[DIM];
  double dvec[DIM];
  int    j,m,*iptr,idum;
  real   *ptr;
  unsigned short us;
  double d=0;
  float  f=0;
  
  check_nitem();
  switch (eio) {
  case eioREAL:
    if (curfio->bDouble) {
      if (item) d = *((real *)item);
      res = xdr_double(curfio->xdr,&d);
      if (item) *((real *)item) = d;
    }
    else {
      if (item) f = *((real *)item);
      res = xdr_float(curfio->xdr,&f);
      if (item) *((real *)item) = f;
    }
    break;
  case eioINT:
    if (item) idum = *(int *)item;
    res = xdr_int(curfio->xdr,&idum);
    if (item) *(int *)item = idum;
    break;
  case eioNUCHAR:
    ucptr = (unsigned char *)item;
    res   = 1;
    for(j=0; (j<nitem) && res; j++) {
      res = xdr_u_char(curfio->xdr,&(ucptr[j]));
    }
    
    break;
  case eioUSHORT:
    if (item) us = *(unsigned short *)item;
    res = xdr_u_short(curfio->xdr,(u_short *)&us);
    if (item) *(unsigned short *)item = us;
    break;
  case eioRVEC:
    if (curfio->bDouble) {
      if (item)
	for(m=0; (m<DIM); m++) 
	  dvec[m] = ((real *)item)[m];
      res=xdr_vector(curfio->xdr,(char *)dvec,DIM,(u_int)sizeof(double),
		     (xdrproc_t)xdr_double);
      if (item)
	for(m=0; (m<DIM); m++) 
	  ((real *)item)[m] = dvec[m];
    }
    else {
      if (item)
	for(m=0; (m<DIM); m++) 
	  fvec[m] = ((real *)item)[m];
      res=xdr_vector(curfio->xdr,(char *)fvec,DIM,(u_int)sizeof(float),
		     (xdrproc_t)xdr_float);
      if (item)
	for(m=0; (m<DIM); m++) 
	  ((real *)item)[m] = fvec[m];
    }
    break;
  case eioNRVEC:
    ptr = NULL;
    res = 1;
    for(j=0; (j<nitem) && res; j++) {
      if (item)
	ptr = ((rvec *)item)[j];
      res = do_xdr(ptr,1,eioRVEC,desc,srcfile,line);
    }
    break;
  case eioIVEC:
    iptr = (int *)item;
    res  = 1;
    for(m=0; (m<DIM) && res; m++) {
      if (item) idum = iptr[m];
      res = xdr_int(curfio->xdr,&idum);
      if (item) iptr[m] = idum;
    }
    break;
  case eioSTRING: {
    char *cptr;
    int  slen;
    
    if (item) {
      if (!curfio->bRead) 
	slen = strlen((char *)item)+1;
      else
	slen = 0;
    }
    else
      slen = 0;
    
    if (xdr_int(curfio->xdr,&slen) <= 0)
      fatal_error(0,"Error in string length for string %s (source %s, line %d)",
		  desc,srcfile,line);
    if (!item && curfio->bRead)
      snew(cptr,slen);
    else
      cptr=(char *)item;
    if (cptr) 
      res = xdr_string(curfio->xdr,&cptr,slen);
    else
      res = 1;
    if (!item && curfio->bRead)
      sfree(cptr);
    break;
  }
  default:
    FE();
  }
  if ((res == 0) && (curfio->bDebug))
    fprintf(stderr,"Error in xdr I/O %s %s to file %s (source %s, line %d)\n",
	    eioNames[eio],desc,curfio->fn,srcfile,line);
  return (res != 0);
}
#endif

static void _fio_check(int fio,char *file,int line)
{
  if ((fio < 0) || (fio >= nFIO)) 
    fatal_error(0,"Trying to access non-open file %d, in %s, line %d",
		fio,file,line);
}
#define fio_check(fio) _fio_check(fio,__FILE__,__LINE__)

/*****************************************************************
 *
 *                     EXPORTED SECTION
 *
 *****************************************************************/
int fio_open(char *fn,char *mode)
{
  t_fileio *fio;
  int      i,nfio;
  char     *bf,*m;
  bool     bRead;

  if (fn2ftp(fn)==efTPA)
    m=mode;
  else {
    if (mode[0]=='r')
      m="rb";
    else if (mode[0]=='w')
      m="wb";
    else
      m="ab";
  }

  /* Determine whether we have to make a new one */
  for(i=0; (i<nFIO); i++)
    if (!FIO[i].bOpen) {
      fio  = &(FIO[i]);
      nfio = i;
      break;
    }
  if (i == nFIO) {
    srenew(FIO,++nFIO);
    fio  = &(FIO[nFIO-1]);
    nfio = nFIO-1;
  }

  bRead = (m[0]=='r');
  fio->fp  = NULL;
  fio->xdr = NULL;
  if (fn) {
    fio->iFTP   = fn2ftp(fn);
    fio->fn     = strdup(fn);
    fio->bStdio = FALSE;
    
    /* If this file type is in the list of XDR files, open it like that */
    if (in_ftpset(fio->iFTP,asize(ftpXDR),ftpXDR)) {
      /* First check whether we have to make a backup,
       * only for writing, not for read or append.
       */
      if (m[0]=='w') {
	if (fexist(fn)) {
	  bf=(char *)backup_fn(fn);
	  if (rename(fn,bf) == 0) {
	    fprintf(stderr,"\nBack Off! I just backed up %s to %s\n",fn,bf);
	  }
	  else
	    fprintf(stderr,"Sorry, I couldn't backup %s to %s\n",fn,bf);
	}
      }
      else {
	/* Check whether file exists */
	if (!fexist(fn))
	  fatal_error(0,"File %s not found",fn);
      }
      snew(fio->xdr,1);
      xdropen(fio->xdr,fn,m);
    }
    else {
      /* If it is not, open it as a regular file */
      fio->fp = ffopen(fn,m);
    }
  }
  else {
    /* Use stdin/stdout for I/O */
    fio->iFTP   = efTPA;
    fio->fp     = bRead ? stdin : stdout;
    fio->fn     = strdup("STDIO");
    fio->bStdio = TRUE;
  }
  fio->bRead  = bRead;
  fio->bDouble= (sizeof(real) == sizeof(double));
  fio->bDebug = FALSE;
  fio->bOpen  = TRUE;

  return nfio;
}

void fio_close(int fio)
{
  fio_check(fio);
  
  if (in_ftpset(FIO[fio].iFTP,asize(ftpXDR),ftpXDR)) {
    xdrclose(FIO[fio].xdr);
    sfree(FIO[fio].xdr);
  }
  else {
    /* Don't close stdin and stdout! */
    if (!FIO[fio].bStdio)
      fclose(FIO[fio].fp);
  }

  sfree(FIO[fio].fn);
  FIO[fio].bOpen = FALSE;
  do_read  = do_dummy;
  do_write = do_dummy;
}

void fio_select(int fio)
{
  fio_check(fio);
#ifdef DEBUG
  fprintf(stderr,"Select fio called with type %d for file %s\n",
	  FIO[fio].iFTP,FIO[fio].fn);
#endif

  if (in_ftpset(FIO[fio].iFTP,asize(ftpXDR),ftpXDR)) {
#ifdef USE_XDR    
    do_read  = do_xdr;
    do_write = do_xdr;
#else
    fatal_error(0,"Sorry, no XDR");
#endif
  }
  else if (in_ftpset(FIO[fio].iFTP,asize(ftpASC),ftpASC)) {
    do_read  = do_ascread;
    do_write = do_ascwrite;
  }
  else if (in_ftpset(FIO[fio].iFTP,asize(ftpBIN),ftpBIN)) {
    do_read  = do_binread;
    do_write = do_binwrite;
  }
  else 
    fatal_error(0,"Can not read/write topologies to file type %s",
		ftp2ext(curfio->iFTP));
  
  curfio = &(FIO[fio]);
}

void fio_setprecision(int fio,bool bDouble)
{
  fio_check(fio);
  FIO[fio].bDouble = bDouble;
}

bool fio_getdebug(int fio)
{
  fio_check(fio);
  return FIO[fio].bDebug;
}

void fio_setdebug(int fio,bool bDebug)
{
  fio_check(fio);
  FIO[fio].bDebug = bDebug;
}

char *fio_getname(int fio)
{
  fio_check(fio);
  return curfio->fn;
}

void fio_setftp(int fio,int ftp)
{
  fio_check(fio);
  FIO[fio].iFTP = ftp;
}

int fio_getftp(int fio)
{
  fio_check(fio);
  return FIO[fio].iFTP;
}
 
void fio_rewind(int fio)
{
  fio_check(fio);
  if (FIO[fio].xdr) {
    xdrclose(FIO[fio].xdr);
    xdropen(FIO[fio].xdr,FIO[fio].fn,FIO[fio].bRead ? "r" : "w");
  }
  else
    frewind(FIO[fio].fp);
}

void fio_flush(int fio)
{
  fio_check(fio);
  if (FIO[fio].fp)
    fflush(FIO[fio].fp);
}
  
long fio_ftell(int fio)
{
  fio_check(fio);
  if (FIO[fio].fp)
    return ftell(FIO[fio].fp);
  else
    return 0;
}

void fio_seek(int fio,long fpos)
{
  fio_check(fio);
  if (FIO[fio].fp) 
    fseek(FIO[fio].fp,fpos,SEEK_SET);
  else
    fatal_error(0,"Can not seek on file %s",FIO[fio].fn);
}

FILE *fio_getfp(int fio)
{
  fio_check(fio);
  if (FIO[fio].fp)
    return FIO[fio].fp;
  else
    return NULL;
}

XDR *fio_getxdr(int fio)
{
  fio_check(fio);
  if (FIO[fio].xdr) 
    return FIO[fio].xdr;
  else
    return NULL;
}

bool fio_getread(int fio)
{
  fio_check(fio);
  
  return FIO[fio].bRead;
}
