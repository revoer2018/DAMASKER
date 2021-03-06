/*********************************************************************************************\
 *
 *  Produce a script to compute overlaps for all block pairs of a DB, and then sort and merge
 *    them into as many .las files as their are blocks.
 *
 *  Author:  Gene Myers
 *  Date  :  June 1, 2014
 *********************************************************************************************/
 
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <math.h>
#include <ctype.h>
#include <unistd.h>
#include <sys/types.h>
#include <sys/stat.h>

#include "DB.h"

#undef  LSF  //  define if want a directly executable LSF script

static char *Usage[] =
  { "[-vbd] [-t<int>] [-w<int(6)>] [-l<int(1000)>] [-s<int(100)>] [-P<dir(/tmp)>]", 
    "       [-M<int>] [-B<int(4)>] [-D<int( 250)>] [T<int(4)>] [-f<name>]",
    "       [-k<int(14)>] [-h<int(35)>] [-e<double(.70)>] [-m<track>]+",
    "       -g<int> -c<int> <reads:db|dam> [<block:int>[-<range:int>]"
  };

#define LSF_ALIGN \
            "bsub -q medium -n 4 -o DAL.REP%d.out -e DAL.REP%d.err -R span[hosts=1] -J align#%d"
#define LSF_MERGE \
    "bsub -q short -n 12 -o MERGE%d.REP%d.out -e MERGE%d.REP%d.err -R span[hosts=1] -J merge#%d"
#define LSF_CHECK \
          "bsub -q short -n 12 -o CHECK%d.DAL.out -e CHECK%d.DAL.err -R span[hosts=1] -J check#%d"
#define LSF_MASK \
    "bsub -q short -n 12 -o MASK.REP%d.out -e MASK.REP%d.err -R span[hosts=1] -J maskrep#%d"

int main(int argc, char *argv[])
{ int   nblocks;
  int   usepath;
  int   useblock;
  int   fblock, lblock;
#ifdef LSF
  int   jobid;
#endif

  FILE *out;
  char  name[100];
  int   stage;
  char *pwd, *root;

  int    CINT, SPAN;
  int    DUNIT, BUNIT;
  int    VON, BON, DON;
  int    WINT, TINT, HINT, KINT, SINT, LINT, MINT;
  int    NTHREADS;
  double EREL;
  int    MMAX, MTOP;
  char **MASK;
  char  *ONAME;
  char  *PDIR;

  { int    i, j, k;         //  Process options
    int    flags[128];
    char  *eptr;

    ARG_INIT("HPC.REPmask")

    BUNIT = 4;
    DUNIT = 250;
    KINT  = 14;
    WINT  = 6;
    HINT  = 35;
    TINT  = 0;
    EREL  = 0.;
    LINT  = 1000;
    SINT  = 100;
    MINT  = -1;
    PDIR  = NULL;

    MTOP = 0;
    MMAX = 10;
    MASK = (char **) Malloc(MMAX*sizeof(char *),"Allocating mask track array");
    if (MASK == NULL)
      exit (1);
    ONAME = NULL;
    out   = stdout;

    NTHREADS = 4;

    SPAN = -1;
    CINT = -1;

    j = 1;
    for (i = 1; i < argc; i++)
      if (argv[i][0] == '-')
        switch (argv[i][1])
        { default:
            ARG_FLAGS("vbd");
            break;
          case 'c':
            ARG_POSITIVE(CINT,"Repeat coverage threshold")
            break;
          case 'e':
            ARG_REAL(EREL)
            if (EREL < .7 || EREL >= 1.)
              { fprintf(stderr,"%s: Average correlation must be in [.7,1.) (%g)\n",Prog_Name,EREL);
                exit (1);
              }
            break;
          case 'f':
            ONAME = argv[i]+2;
            break;
          case 'g':
            ARG_POSITIVE(SPAN,"Block span")
            break;
          case 'h':
            ARG_POSITIVE(HINT,"Hit threshold (in bp.s)")
            break;
          case 'k':
            ARG_POSITIVE(KINT,"K-mer length")
            if (KINT > 32)
              { fprintf(stderr,"%s: K-mer length must be 32 or less\n",Prog_Name);
                exit (1);
              }
            break;
          case 'l':
            ARG_POSITIVE(LINT,"Minimum ovlerap length")
            break;
          case 't':
            ARG_POSITIVE(TINT,"Tuple suppression frequency")
            break;
          case 's':
            ARG_POSITIVE(SINT,"Trace spacing")
            break;
          case 'w':
            ARG_POSITIVE(WINT,"Log of bin width")
            break;
          case 'm':
            if (MTOP >= MMAX)
              { MMAX = 1.2*MTOP + 10;
                MASK = (char **) Realloc(MASK,MMAX*sizeof(char *),"Reallocating mask track array");
                if (MASK == NULL)
                  exit (1);
              }
            MASK[MTOP++] = argv[i]+2;
            break;
          case 'B':
            ARG_NON_NEGATIVE(BUNIT,"Blocks per command")
            break;
          case 'D':
            ARG_NON_NEGATIVE(DUNIT,"Files per merge")
            if (DUNIT < 3)
              { fprintf(stderr,"%s: Files per merge must be at least 3 (%d)\n",
                               Prog_Name,DUNIT);
                exit (1);
              }
            break;
          case 'M':
            ARG_NON_NEGATIVE(MINT,"Memory allocation (in Gb)")
            break;
          case 'P':
            PDIR = argv[i]+2;
            break;
          case 'T':
            ARG_POSITIVE(NTHREADS,"Number of threads")
            break;
        }
      else
        argv[j++] = argv[i];
    argc = j;

    VON = flags['v'];
    BON = flags['b'];
    DON = flags['d'];

    if (argc < 2 || argc > 3)
      { fprintf(stderr,"Usage: %s %s\n",Prog_Name,Usage[0]);
        fprintf(stderr,"       %*s %s\n",(int) strlen(Prog_Name),"",Usage[1]);
        fprintf(stderr,"       %*s %s\n",(int) strlen(Prog_Name),"",Usage[2]);
        fprintf(stderr,"       %*s %s\n",(int) strlen(Prog_Name),"",Usage[3]);
        exit (1);
      }
    if (SPAN < 0)
      { fprintf(stderr,"%s: Must supply -g parameter\n",Prog_Name);
        exit (1);
      }
    if (CINT < 0)
      { fprintf(stderr,"%s: Must supply -c parameter\n",Prog_Name);
        exit (1);
      }

    for (j = 1; 2*j <= NTHREADS; j *= 2)
      ;
    NTHREADS = j;
  }

  //  Make sure DB exists and is partitioned, get number of blocks in partition

  pwd = PathTo(argv[1]);
  if (strcmp(argv[1]+(strlen(argv[1])-4),".dam") == 0)
    root = Root(argv[1],".dam");
  else
    root = Root(argv[1],".db");

  { int i, nfiles;
    FILE *dbvis;

    dbvis = fopen(Catenate(pwd,"/",root,".dam"),"r");
    if (dbvis == NULL)
      { dbvis = Fopen(Catenate(pwd,"/",root,".db"),"r");
        if (dbvis == NULL)
          exit (1);
      }

    if (fscanf(dbvis,"files = %d\n",&nfiles) != 1)
      SYSTEM_READ_ERROR
    for (i = 0; i < nfiles; i++)
      { char buffer[30001];

        if (fgets(buffer,30000,dbvis) == NULL)
          SYSTEM_READ_ERROR
      }

    useblock = 1;
    if (fscanf(dbvis,"blocks = %d\n",&nblocks) != 1 || nblocks == 1)
      { useblock = 0;
        nblocks  = 1;
      }

    if (nblocks < SPAN)
      { fprintf(stderr,"%s: There a fewer than -g = %d blocks in the DB!\n",Prog_Name,SPAN);
        exit (1);
      }

    usepath = (strcmp(pwd,".") != 0);
  }

  //  Set range fblock-lblock checking that DB.<fblock-1>.las exists & DB.<fblock>.las does not

  { char *eptr, *fptr, *sfx;
    FILE *file;

    if (argc == 3)
      { fblock = strtol(argv[2],&eptr,10);
        if ((*eptr != '\0' && *eptr != '-') || eptr <= argv[2])
          { fprintf(stderr,"%s: final argument '%s' does not start with an integer\n",
                           Prog_Name,argv[2]);
            exit (1);
          }
        useblock = 1;
        if (*eptr == '-')
          { lblock = strtol(eptr+1,&fptr,10);
            if (*fptr != '\0' || fptr <= eptr+1)
              { fprintf(stderr,"%s: second part of range '%s' is not an integer\n",
                               Prog_Name,eptr+1);
                exit (1);
              }
            if (lblock < SPAN)
              { fprintf(stderr,"%s: End of range %d must be >= group span -g = %d\n",
                               Prog_Name,lblock,SPAN);
                exit (1);
              }
          }
        else
          lblock = fblock;
        if (fblock < 1 || lblock > nblocks || fblock > lblock)
          { fprintf(stderr,"%s: range %d-%d is empty or out of bounds\n",Prog_Name,fblock,lblock);
            exit (1);
          }
      }
    else
      { fblock = 1;
        lblock = nblocks;
      }

    sfx = Strdup(Numbered_Suffix(".rep",SPAN,".anno"),"Allocating small string!");
    if (sfx == NULL)
      exit (1);

    if (fblock > 1)
      { file = fopen(Catenate(pwd,"/.",root,Numbered_Suffix(".",fblock-1,sfx)),"r");
        if (file == NULL)
          { if (usepath)
              fprintf(stderr,"%s: File %s/%s.%d%s should already be present!\n",
                             Prog_Name,pwd,root,fblock-1,sfx);
            else
              fprintf(stderr,"%s: File %s.%d%s should already be present!\n",
                             Prog_Name,root,fblock-1,sfx);
            exit (1);
          }
        else
          fclose(file);
      }
    if (useblock)
      file = fopen(Catenate(pwd,"/.",root,Numbered_Suffix(".",fblock,sfx)),"r");
    else
      file = fopen(Catenate(pwd,"/.",root,sfx),"r");
    if (file != NULL)
      { if (usepath)
          if (useblock)
            fprintf(stderr,"%s: File %s/.%s.%d%s should not yet exist!\n",
                           Prog_Name,pwd,root,fblock,sfx);
          else
            fprintf(stderr,"%s: File %s/.%s%s as should not yet exist!\n",Prog_Name,pwd,root,sfx);
        else
          if (useblock)
            fprintf(stderr,"%s: File .%s.%d%s should not yet exist!\n",Prog_Name,root,fblock,sfx);
          else
            fprintf(stderr,"%s: File .%s%s should not yet exist!\n",Prog_Name,root,sfx);
        exit (1);
      }

    DON = (DON && (SPAN > 1));
  }

  { int level, njobs;
    int i, j, k;

    if (DON)
      { if (ONAME != NULL)
          { sprintf(name,"%s.00.MKDIR",ONAME);
            out = fopen(name,"w");
          }

        fprintf(out,"# Create work subdirectories\n");
        for (i = fblock; i <= lblock; i++)
          fprintf(out,"mkdir temp%d\n",i);

        if (ONAME != NULL)
          fclose(out);
      }

    //  Produce all necessary daligner jobs ...

    if (ONAME != NULL)
      { sprintf(name,"%s.01.OVL",ONAME);
        out = fopen(name,"w");
      }

    njobs = 0;
    for (i = fblock; i <= lblock; i++)
      { int base;

        base = fblock + ((i-fblock)/SPAN)*SPAN;
        if (base + SPAN > lblock+1)
          base = (lblock+1) - SPAN;
        njobs += (i-base)/BUNIT+1;
      }

    fprintf(out,"# Daligner jobs (%d)\n",njobs);

#ifdef LSF
    jobid = 1;
#endif
    for (i = fblock; i <= lblock; i++)
      { int bits, base;
        int low, hgh;

        base = fblock + ((i-fblock)/SPAN)*SPAN;
        if (base + SPAN > lblock+1)
          base = (lblock+1) - SPAN;
        bits = (i-base)/BUNIT+1;
        low  = base;
        for (j = 1; j <= bits; j++)
          {
#ifdef LSF
            fprintf(out,LSF_ALIGN,SPAN,SPAN,jobid++);
            fprintf(out," \"");
#endif
            fprintf(out,"daligner");
            if (VON)
              fprintf(out," -v");
            if (BON)
              fprintf(out," -b");
            if (KINT != 14)
              fprintf(out," -k%d",KINT);
            if (WINT != 6)
              fprintf(out," -w%d",WINT);
            if (HINT != 35)
              fprintf(out," -h%d",HINT);
            if (TINT > 0)
              fprintf(out," -t%d",TINT);
            if (EREL > .1)
              fprintf(out," -e%g",EREL);
            if (LINT != 1000)
              fprintf(out," -l%d",LINT);
            if (SINT != 100)
              fprintf(out," -s%d",SINT);
            if (MINT >= 0)
              fprintf(out," -M%d",MINT);
            if (PDIR != NULL)
              fprintf(out," -P%s",PDIR);
            if (NTHREADS != 4)
              fprintf(out," -T%d",NTHREADS);
            for (k = 0; k < MTOP; k++)
              fprintf(out," -m%s",MASK[k]);
            if (useblock)
              if (usepath)
                fprintf(out," %s/%s.%d",pwd,root,i);
              else
                fprintf(out," %s.%d",root,i);
            else
              if (usepath)
                fprintf(out," %s/%s",pwd,root);
              else
                fprintf(out," %s",root);
            hgh = ((i-base+1)*j)/bits + base;
            for (k = low; k < hgh; k++)
              if (useblock)
                if (usepath)
                  fprintf(out," %s/%s.%d",pwd,root,k);
                else
                  fprintf(out," %s.%d",root,k);
              else
                if (usepath)
                  fprintf(out," %s/%s",pwd,root);
                else
                  fprintf(out," %s",root);

            if (SPAN == 1)   // ==> [low,hgh) = [i,i+1)
              if (useblock)
                fprintf(out," && mv %s.%d.%s.%d.las %s.R1.%d.las",root,i,root,i,root,i);
              else
                fprintf(out," && mv %s.%s.las %s.R1.%d.las",root,root,root,i);
            else if (DON)
              { fprintf(out," && mv");
                for (k = low; k < hgh; k++)
                  fprintf(out," %s.%d.%s.%d.las",root,i,root,k);
                fprintf(out," temp%d",i);
                for (k = low; k < hgh; k++)
                  if (k != i)
                    fprintf(out," && mv %s.%d.%s.%d.las temp%d",root,k,root,i,k);
              }

#ifdef LSF
            fprintf(out,"\"");
#endif
            fprintf(out,"\n");
            low = hgh;
          }
      }

    //  Check .las files (optional)

    if (ONAME != NULL)
      { fclose(out);
        sprintf(name,"%s.02.CHECK.OPT",ONAME);
        out = fopen(name,"w");
      }

    njobs = ((lblock-fblock)+1) * ((SPAN-1)/(BUNIT+1) + 1);

    fprintf(out,"# Check inital .las jobs (%d) (optional but recommended)\n",njobs);

#ifdef LSF
    jobid = 1;
#endif
    for (i = fblock; i <= lblock; i++)
      { int base;

        base = fblock + ((i-fblock)/SPAN)*SPAN;
        if (base + SPAN > lblock+1)
          base = (lblock+1) - SPAN;
        for (j = base; j <= (base+SPAN)-1; )
          { k = j+BUNIT;
            if (k > (base+SPAN)-1)
              k = (base+SPAN)-1;
#ifdef LSF
            fprintf(out,LSF_CHECK,0,0,jobid++);
            fprintf(out," \"");
#endif
            fprintf(out,"LAcheck -vS");
            if (usepath)
              fprintf(out," %s/%s",pwd,root);
            else
              fprintf(out," %s",root);
            while (j <= k)
              { if (SPAN == 1)
                  fprintf(out," %s.R1.%d",root,i);
                else if (DON)
                  fprintf(out," temp%d/%s.%d.%s.%d",i,root,i,root,j);
                else
                  fprintf(out," %s.%d.%s.%d",root,i,root,j);
                j += 1;
              }
#ifdef LSF
            fprintf(out,"\"");
#endif
            fprintf(out,"\n");
          }
      }

    if (ONAME != NULL)
      fclose(out);
    stage = 3;

    //  Higher level merges (if SPAN > 1)

    if (SPAN > 1)
      { int pow;

        //  Determine numbe of merge levels

        pow = 1;
        for (level = 0; pow < SPAN; level++)
          pow *= DUNIT;

        //  Issue the commands for each merge level

        { int  p, cnt;

          cnt = SPAN;
          for (i = 1; i <= level; i++)
            { int cits;
              int low, hgh;

              if (ONAME != NULL)
                { sprintf(name,"%s.%02d.MERGE",ONAME,stage++);
                  out = fopen(name,"w");
                }

              cits = (cnt-1)/DUNIT+1;

              //  Incremental update merges

              njobs = ((lblock-fblock)+1)*cits;
              fprintf(out,"# Level %d jobs (%d)\n",i,njobs);

#ifdef LSF
              jobid = 1;
#endif

              //  New block merges

              for (j = fblock; j <= lblock; j++) 
                { int base;

                  base = fblock + ((j-fblock)/SPAN)*SPAN;
                  if (base + SPAN > lblock+1)
                    base = (lblock+1) - SPAN;
                  base -= 1;

                  low = 1;
                  for (p = 1; p <= cits; p++)
                    { hgh = (cnt*p)/cits;
#ifdef LSF
                      fprintf(out,LSF_MERGE,i,SPAN,i,SPAN,jobid++);
                      fprintf(out," \"");
#endif
                      fprintf(out,"LAmerge");
                      if (VON)
                        fprintf(out," -v");
                      if (DON)
                        if (cits == 1)
                          fprintf(out," temp%d/%s.R%d.%d",j,root,SPAN,j);
                        else
                          fprintf(out," temp%d/L%d.%d.%d",j,i+1,j,p);
                      else
                        if (cits == 1)
                          fprintf(out," %s.R%d.%d",root,SPAN,j);
                        else
                          fprintf(out," L%d.%d.%d",i+1,j,p);
                      for (k = low; k <= hgh; k++)
                        if (i == 1)
                          if (DON)
                            fprintf(out," temp%d/%s.%d.%s.%d",j,root,j,root,base+k);
                          else
                            fprintf(out," %s.%d.%s.%d",root,j,root,base+k);
                        else
                          if (DON)
                            fprintf(out," temp%d/L%d.%d.%d",j,i,j,k);
                          else
                            fprintf(out," L%d.%d.%d",i,j,k);
#ifdef LSF
                      fprintf(out,"\"");
#endif
                      fprintf(out,"\n");
                      low = hgh+1;
                    }
                }

              //  Check new .las (optional)

              if (ONAME != NULL)
                { fclose(out);
                  sprintf(name,"%s.%02d.CHECK.OPT",ONAME,stage++);
                  out = fopen(name,"w");
                }

              fprintf(out,"# Check level %d .las files jobs (%d) (optional but recommended)\n",
                          i+1,((lblock-fblock)+1)*((cits-1)/(BUNIT+1)+1));

#ifdef LSF
              jobid = 1;
#endif
              for (j = fblock; j <= lblock; j++) 
                for (p = 1; p <= cits; )
                  { k = p+BUNIT;
                    if (k > cits)
                      k = cits;
#ifdef LSF
                    fprintf(out,LSF_CHECK,i,i,jobid++);
                    fprintf(out," \"");
#endif
                    fprintf(out,"LAcheck -vS");
                    if (usepath)
                      fprintf(out," %s/%s",pwd,root);
                    else
                      fprintf(out," %s",root);
                    while (p <= k)
                      { if (DON)
                          if (cits == 1)
                            fprintf(out," temp%d/%s.R%d.%d",j,root,SPAN,j);
                          else
                            fprintf(out," temp%d/L%d.%d.%d",j,i+1,j,p);
                        else
                          if (cits == 1)
                            fprintf(out," %s.R%d.%d",root,SPAN,j);
                          else
                            fprintf(out," L%d.%d.%d",i+1,j,p);
                        p += 1;
                      }
#ifdef LSF
                    fprintf(out,"\"");
#endif
                    fprintf(out,"\n");
                  }

              //  Cleanup (optional)

              if (ONAME != NULL)
                { fclose(out);
                  sprintf(name,"%s.%02d.RM",ONAME,stage++);
                  out = fopen(name,"w");
                }

              fprintf(out,"# Remove level %d .las files)\n",i);

              for (j = fblock; j <= lblock; j++) 
                { int base;

                  base = fblock + ((j-fblock)/SPAN)*SPAN;
                  if (base + SPAN > lblock+1)
                    { int n, u;
                      int mnt, mits;

                      n = (lblock+1) - SPAN;
                      u = j - (base-n);
                      if (i == 1 && u < base)
                        { mnt  = (lblock+1)-base;
                          mits = (mnt-1)/DUNIT+1;
                          low  = 1;
                          base -= 1;
                          for (p = 1; p <= mits; p++)
                            { hgh = (mnt*p)/mits;
                              fprintf(out,"rm");
                              for (k = low; k <= hgh; k++)
                                if (DON)
                                  fprintf(out," temp%d/%s.%d.%s.%d.las",u,root,u,root,base+k);
                                else
                                  fprintf(out," %s.%d.%s.%d.las",root,u,root,base+k);
                              fprintf(out,"\n");
                              low = hgh+1;
                            }
                        }
                      base = n;
                    }
                  base -= 1;

                  low = 1;
                  for (p = 1; p <= cits; p++)
                    { hgh = (cnt*p)/cits;
                      fprintf(out,"rm");
                      for (k = low; k <= hgh; k++)
                        if (i == 1)
                          if (DON)
                            fprintf(out," temp%d/%s.%d.%s.%d.las",j,root,j,root,base+k);
                          else
                            fprintf(out," %s.%d.%s.%d.las",root,j,root,base+k);
                        else
                          if (DON)
                            fprintf(out," temp%d/L%d.%d.%d.las",j,i,j,k);
                          else
                            fprintf(out," L%d.%d.%d.las",i,j,k);
                      fprintf(out,"\n");
                      low = hgh+1;
                    }
                }

              if (ONAME != NULL)
                fclose(out);

              cnt = cits;
            }
        }
      }

    //  Finish with MASKrep

    { int h, step;

      if (ONAME != NULL)
        { sprintf(name,"%s.%02d.MASK",ONAME,stage++);
          out = fopen(name,"w");
        }
 
#ifdef LSF
    jobid = 1;
#endif
      step   = (SPAN-1)/BUNIT+1;
      njobs  = (((lblock-fblock)+1) / SPAN)*step;
      step   = (SPAN-1)/step+1;
      njobs += ((((lblock-fblock)+1) % SPAN)-1)/step + 1;

      fprintf(out,"# REPmask jobs (%d)\n",njobs);

      for (i = fblock; i <= lblock; i += SPAN)
       for (h = i; h < i+SPAN && h <= lblock; h += step)
        {
#ifdef LSF
          fprintf(out,LSF_MASK,SPAN,SPAN,jobid++);
          fprintf(out," \"");
#endif
          fprintf(out,"REPmask");
          if (VON)
            fprintf(out," -v");
          fprintf(out," -c%d -mrep%d",CINT,SPAN);
          if (usepath)
            fprintf(out," %s/%s",pwd,root);
          else
            fprintf(out," %s",root);
          j = h+step;
          if (j > i+SPAN)
            j = i+SPAN;
          if (j > lblock)
            j = lblock+1;
          for (k = h; k < j; k++)
            if (DON)
              fprintf(out," temp%d/%s.R%d.%d",k,root,SPAN,k);
            else
              fprintf(out," %s.R%d.%d",root,SPAN,k);
#ifdef LSF
          fprintf(out,"\"");
#endif
          fprintf(out,"\n");
        }

      //  Cleanup (optinoal)

      if (ONAME != NULL)
        { sprintf(name,"%s.%02d.RM",ONAME,stage++);
          out = fopen(name,"w");
        }

      if (DON)
        fprintf(out,"# Cleanup all temporary directories\n");
      else
        fprintf(out,"# Cleanup all R%d.las files\n",SPAN);

      if (DON)
        for (i = fblock; i <= lblock; i++)
          fprintf(out,"rm -r temp%d\n",i);
      else
        for (i = fblock; i <= lblock; i += SPAN)
         for (h = i; h < i+SPAN && h <= lblock; h += step)
          { fprintf(out,"rm");
            j = h+step;
            if (j > i+SPAN)
              j = i+SPAN;
            if (j > lblock)
              j = lblock+1;
            for (k = h; k < j; k++)
              fprintf(out," %s.R%d.%d.las",root,SPAN,k);
            fprintf(out,"\n");
          }
  
      if (ONAME != NULL)
        fclose(out);
    }
  }

  printf("# Once all the .rep masks have been computed for every block\n");
  printf("#   you should call 'Catrack' to merge them, and then you should\n");
  printf("#   remove the individual block tracks, e.g.:\n");
  if (usepath)
    { printf("#      Catrack -v %s/%s rep%d\n",pwd,root,SPAN);
      printf("#      rm %s/.%s.*.rep%d.*\n",pwd,root,SPAN);
    }
  else
    { printf("#      Catrack -v %s rep%d\n",root,SPAN);
      printf("#      rm .%s.*.rep%d.*\n",root,SPAN);
    }


  free(root);
  free(pwd);

  exit (0);
}
