/*-
 * Copyright (c) 1980, 1993
 *	The Regents of the University of California.  All rights reserved.
 *
 * This module is believed to contain source code proprietary to AT&T.
 * Use and redistribution is subject to the Berkeley Software License
 * Agreement and your Software Agreement with AT&T (Western Electric).
 */

#if 0
static char copyright[] =
"@(#) Copyright (c) 1980, 1993\n\
	The Regents of the University of California.  All rights reserved.\n";
#endif /* not lint */

#if 0
static char sccsid[] = "@(#)ex3.7recover.c	8.1 (Berkeley) 6/9/93";
#endif /* not lint */

#include <stdlib.h>
#include <stdio.h>	/* mjm: BUFSIZ: stdio = 512, VMUNIX = 1024 */
#undef	BUFSIZ		/* mjm: BUFSIZ different */
#undef	EOF		/* mjm: EOF and NULL effectively the same */
#undef	NULL

#include "ex.h"
#include "ex_temp.h"
#include "ex_tty.h"
#include <sys/dir.h>
#if __STDC__
#include <stdarg.h>
#else
#include <varargs.h>
#endif

/*
 * Here we save the information about files, when
 * you ask us what files we have saved for you.
 * We buffer file name, number of lines, and the time
 * at which the file was saved.
 */
struct svfile {
	char	sf_name[FNSIZE + 1];
	int	sf_lines;
	char	sf_entry[MAXNAMLEN + 1];
	time_t	sf_time;
};

char xstr[1];		/* make loader happy */
short tfile = -1;	/* ditto */

#if __STDC__
void	fpr(const char *fmt, ...);
#else
void	fpr();
#endif
static void enter(struct svfile *, char *, int);
static int qucmp(const void *, const void *);
static int yeah(char *);
static void scrapbad(void);
static void blkio(int, char *, ssize_t (*)());

/*
 *
 * This program searches through the specified directory and then
 * the directory _PATH_USRPRESERVE looking for an instance of the specified
 * file from a crashed editor or a crashed system.
 * If this file is found, it is unscrambled and written to
 * the standard output.
 *
 * If this program terminates without a "broken pipe" diagnostic
 * (i.e. the editor doesn't die right away) then the buffer we are
 * writing from is removed when we finish.  This is potentially a mistake
 * as there is not enough handshaking to guarantee that the file has actually
 * been recovered, but should suffice for most cases.
 */

/*
 * For lint's sake...
 */
#ifndef lint
#define	ignorl(a)	a
#endif

/*
 * Limit on the number of printed entries
 * when an, e.g. ``ex -r'' command is given.
 */
#define	NENTRY	50

static void listfiles(char *);
static void findtmp(char *);
static void searchdir(char *);
char	*ctime();
char	nb[BUFSIZ];
int	vercnt;			/* Count number of versions of file found */

int
main(int argc, char **argv)
{
	register char *cp;
	register int b, i;

	/*
	 * Initialize as though the editor had just started.
	 */
	fendcore = (line *) sbrk(0);
	dot = zero = dol = fendcore;
	one = zero + 1;
	endcore = fendcore - 2;
	iblock = oblock = -1;

	/*
	 * If given only a -r argument, then list the saved files.
	 */
	if (argc == 2 && eq(argv[1], "-r")) {
		listfiles(_PATH_PRESERVE);
		exit(0);
	}
	if (argc != 3)
		error(" Wrong number of arguments to exrecover");

	CP(file, argv[2]);

	/*
	 * Search for this file.
	 */
	findtmp(argv[1]);

	/*
	 * Got (one of the versions of) it, write it back to the editor.
	 */
	cp = ctime(&H.Time);
	cp[19] = 0;
	fpr(" [Dated: %s", cp);
	fpr(vercnt > 1 ? ", newest of %d saved]" : "]", vercnt);
	H.Flines++;

	/*
	 * Allocate space for the line pointers from the temp file.
	 */
	if (sbrk(H.Flines * sizeof(line)) == (void *)-1)
		/*
		 * Good grief.
		 */
		error(" Not enough core for lines");
#ifdef DEBUG
	fpr("%d lines\n", H.Flines);
#endif

	/*
	 * Now go get the blocks of seek pointers which are scattered
	 * throughout the temp file, reconstructing the incore
	 * line pointers at point of crash.
	 */
	b = 0;
	while (H.Flines > 0) {
		ignorl(lseek(tfile, (long) blocks[b] * BUFSIZ, 0));
		i = H.Flines < (int)(BUFSIZ / sizeof (line)) ?
			H.Flines * sizeof (line) : BUFSIZ;
		if (read(tfile, (char *) dot, i) != i) {
			perror(nb);
			exit(1);
		}
		dot += i / sizeof (line);
		H.Flines -= i / sizeof (line);
		b++;
	}
	dot--; dol = dot;

	/*
	 * Sigh... due to sandbagging some lines may really not be there.
	 * Find and discard such.  This shouldn't happen much.
	 */
	scrapbad();

	/*
	 * Now if there were any lines in the recovered file
	 * write them to the standard output.
	 */
	if (dol > zero) {
		addr1 = one; addr2 = dol; io = 1;
		putfile(0);
	}

	/*
	 * Trash the saved buffer.
	 * Hopefully the system won't crash before the editor
	 * syncs the new recovered buffer; i.e. for an instant here
	 * you may lose if the system crashes because this file
	 * is gone, but the editor hasn't completed reading the recovered
	 * file from the pipe from us to it.
	 *
	 * This doesn't work if we are coming from an non-absolute path
	 * name since we may have chdir'ed but what the hay, noone really
	 * ever edits with temporaries in "." anyways.
	 */
	if (nb[0] == '/')
		ignore(unlink(nb));

	/*
	 * Adieu.
	 */
	return 0;
}

/*
 * Print an error message (notably not in error
 * message file).  If terminal is in RAW mode, then
 * we should be writing output for "vi", so don't print
 * a newline which would screw up the screen.
 */
/*VARARGS2*/
void
ierror(char *str, int inf)
{

	fpr(str, inf);
	if (!tcgetattr(2, &tty) && !(tty.c_oflag & OPOST))
		fpr("\n");
	exit(1);
}

void
error(char *s) {
	ierror(s, 0);
}

static void
listfiles(char *dirname)
{
	register DIR *dir;
	struct direct *dirent;
	int ecount;
	register int f;
	char *cp;
	struct svfile *fp, svbuf[NENTRY];

	/*
	 * Open _PATH_PRESERVE, and go there to make things quick.
	 */
	dir = opendir(dirname);
	if (dir == NULL) {
		perror(dirname);
		return;
	}
	if (chdir(dirname) < 0) {
		perror(dirname);
		return;
	}

	/*
	 * Look at the candidate files in _PATH_PRESERVE.
	 */
	fp = &svbuf[0];
	ecount = 0;
	while ((dirent = readdir(dir)) != NULL) {
		if (dirent->d_name[0] != 'E')
			continue;
#ifdef DEBUG
		fpr("considering %s\n", dirent->d_name);
#endif
		/*
		 * Name begins with E; open it and
		 * make sure the uid in the header is our uid.
		 * If not, then don't bother with this file, it can't
		 * be ours.
		 */
		f = open(dirent->d_name, 0);
		if (f < 0) {
#ifdef DEBUG
			fpr("open failed\n");
#endif
			continue;
		}
		if (read(f, (char *) &H, sizeof H) != sizeof H) {
#ifdef DEBUG
			fpr("culdnt read hedr\n");
#endif
			ignore(close(f));
			continue;
		}
		ignore(close(f));
		if (getuid() != H.Uid) {
#ifdef DEBUG
			fpr("uid wrong\n");
#endif
			continue;
		}

		/*
		 * Saved the day!
		 */
		enter(fp++, dirent->d_name, ecount);
		ecount++;
#ifdef DEBUG
		fpr("entered file %s\n", dirent->d_name);
#endif
	}
	ignore(closedir(dir));

	/*
	 * If any files were saved, then sort them and print
	 * them out.
	 */
	if (ecount == 0) {
		fpr("No files saved.\n");
		return;
	}
	qsort(&svbuf[0], ecount, sizeof svbuf[0], qucmp);
	for (fp = &svbuf[0]; fp < &svbuf[ecount]; fp++) {
		cp = ctime(&fp->sf_time);
		cp[10] = 0;
		fpr("On %s at ", cp);
 		cp[16] = 0;
		fpr(&cp[11]);
		fpr(" saved %d lines of file \"%s\"\n",
		    fp->sf_lines, fp->sf_name);
	}
}

/*
 * Enter a new file into the saved file information.
 */
static void
enter(struct svfile *fp, char *fname, int count)
{
	register char *cp, *cp2;
	register struct svfile *f, *fl;
	time_t curtime, itol();

	f = 0;
	if (count >= NENTRY) {
		/*
		 * My god, a huge number of saved files.
		 * Would you work on a system that crashed this
		 * often?  Hope not.  So lets trash the oldest
		 * as the most useless.
		 *
		 * (I wonder if this code has ever run?)
		 */
		fl = fp - count + NENTRY - 1;
		curtime = fl->sf_time;
		for (f = fl; --f > fp-count; )
			if (f->sf_time < curtime)
				curtime = f->sf_time;
		for (f = fl; --f > fp-count; )
			if (f->sf_time == curtime)
				break;
		fp = f;
	}

	/*
	 * Gotcha.
	 */
	fp->sf_time = H.Time;
	fp->sf_lines = H.Flines;
	for (cp2 = fp->sf_name, cp = savedfile; *cp;)
		*cp2++ = *cp++;
	for (cp2 = fp->sf_entry, cp = fname; *cp && cp-fname < 14;)
		*cp2++ = *cp++;
	*cp2++ = 0;
}

/*
 * Do the qsort compare to sort the entries first by file name,
 * then by modify time.
 */
static int
qucmp(const void *v1, const void *v2)
{
	register int t;
	struct svfile *p1 = (struct svfile *)v1,
		      *p2 = (struct svfile *)v2;

	if ((t = strcmp(p1->sf_name, p2->sf_name)))
		return(t);
	if (p1->sf_time > p2->sf_time)
		return(-1);
	return(p1->sf_time < p2->sf_time);
}

/*
 * Scratch for search.
 */
char	bestnb[BUFSIZ];		/* Name of the best one */
long	besttime;		/* Time at which the best file was saved */
int	bestfd;			/* Keep best file open so it dont vanish */

/*
 * Look for a file, both in the users directory option value
 * (i.e. usually /tmp) and in _PATH_PRESERVE.
 * Want to find the newest so we search on and on.
 */
static void
findtmp(char *dir)
{

	/*
	 * No name or file so far.
	 */
	bestnb[0] = 0;
	bestfd = -1;

	/*
	 * Search _PATH_PRESERVE and, if we can get there, /tmp
	 * (actually the users "directory" option).
	 */
	searchdir(dir);
	if (chdir(_PATH_PRESERVE) == 0)
		searchdir(_PATH_PRESERVE);
	if (bestfd != -1) {
		/*
		 * Gotcha.
		 * Put the file (which is already open) in the file
		 * used by the temp file routines, and save its
		 * name for later unlinking.
		 */
		tfile = bestfd;
		CP(nb, bestnb);
		ignorl(lseek(tfile, 0l, 0));

		/*
		 * Gotta be able to read the header or fall through
		 * to lossage.
		 */
		if (read(tfile, (char *) &H, sizeof H) == sizeof H)
			return;
	}

	/*
	 * Extreme lossage...
	 */
	error(" File not found");
}

/*
 * Search for the file in directory dirname.
 *
 * Don't chdir here, because the users directory
 * may be ".", and we would move away before we searched it.
 * Note that we actually chdir elsewhere (because it is too slow
 * to look around in _PATH_PRESERVE without chdir'ing there) so we
 * can't win, because we don't know the name of '.' and if the path
 * name of the file we want to unlink is relative, rather than absolute
 * we won't be able to find it again.
 */
static void
searchdir(char *dirname)
{
	struct direct *dirent;
	register DIR *dir;

	dir = opendir(dirname);
	if (dir == NULL)
		return;
	while ((dirent = readdir(dir)) != NULL) {
		if (dirent->d_name[0] != 'E')
			continue;
		/*
		 * Got a file in the directory starting with E...
		 * Save a consed up name for the file to unlink
		 * later, and check that this is really a file
		 * we are looking for.
		 */
		ignore(strcat(strcat(strcpy(nb, dirname), "/"), dirent->d_name));
		if (yeah(nb)) {
			/*
			 * Well, it is the file we are looking for.
			 * Is it more recent than any version we found before?
			 */
			if (H.Time > besttime) {
				/*
				 * A winner.
				 */
				ignore(close(bestfd));
				bestfd = dup(tfile);
				besttime = H.Time;
				CP(bestnb, nb);
			}
			/*
			 * Count versions so user can be told there are
			 * ``yet more pages to be turned''.
			 */
			vercnt++;
		}
		ignore(close(tfile));
	}
	ignore(closedir(dir));
}

/*
 * Given a candidate file to be recovered, see
 * if its really an editor temporary and of this
 * user and the file specified.
 */
static int
yeah(char *name)
{

	tfile = open(name, 2);
	if (tfile < 0)
		return (0);
	if (read(tfile, (char *) &H, sizeof H) != sizeof H) {
nope:
		ignore(close(tfile));
		return (0);
	}
	if (!eq(savedfile, file))
		goto nope;
	if (getuid() != H.Uid)
		goto nope;
	/*
	 * This is old and stupid code, which
	 * puts a word LOST in the header block, so that lost lines
	 * can be made to point at it.
	 */
	ignorl(lseek(tfile, (long)(BUFSIZ*HBLKS-8), 0));
	ignore(write(tfile, "LOST", 5));
	return (1);
}

/*
 * Find the true end of the scratch file, and ``LOSE''
 * lines which point into thin air.  This lossage occurs
 * due to the sandbagging of i/o which can cause blocks to
 * be written in a non-obvious order, different from the order
 * in which the editor tried to write them.
 *
 * Lines which are lost are replaced with the text LOST so
 * they are easy to find.  We work hard at pretty formatting here
 * as lines tend to be lost in blocks.
 *
 * This only seems to happen on very heavily loaded systems, and
 * not very often.
 */
static void
scrapbad(void)
{
	register line *ip;
	struct stat stbuf;
	off_t size, maxt;
	int bno, cnt, bad, was;
	char bk[BUFSIZ];

	ignore(fstat(tfile, &stbuf));
	size = stbuf.st_size;
	maxt = (size >> SHFT) | (BNDRY-1);
	bno = (maxt >> OFFBTS) & BLKMSK;
#ifdef DEBUG
	fpr("size %ld, maxt %o, bno %d\n", size, maxt, bno);
#endif

	/*
	 * Look for a null separating two lines in the temp file;
	 * if last line was split across blocks, then it is lost
	 * if the last block is.
	 */
	while (bno > 0) {
		ignorl(lseek(tfile, (long) BUFSIZ * bno, 0));
		cnt = read(tfile, (char *) bk, BUFSIZ);
		while (cnt > 0)
			if (bk[--cnt] == 0)
				goto null;
		bno--;
	}
null:

	/*
	 * Magically calculate the largest valid pointer in the temp file,
	 * consing it up from the block number and the count.
	 */
	maxt = ((bno << OFFBTS) | (cnt >> SHFT)) & ~1;
#ifdef DEBUG
	fpr("bno %d, cnt %d, maxt %o\n", bno, cnt, maxt);
#endif

	/*
	 * Now cycle through the line pointers,
	 * trashing the Lusers.
	 */
	was = bad = 0;
	for (ip = one; ip <= dol; ip++)
		if (*ip > maxt) {
#ifdef DEBUG
			fpr("%d bad, %o > %o\n", ip - zero, *ip, maxt);
#endif
			if (was == 0)
				was = ip - zero;
			*ip = ((HBLKS*BUFSIZ)-8) >> SHFT;
		} else if (was) {
			if (bad == 0)
				fpr(" [Lost line(s):");
			fpr(" %d", was);
			if ((ip - 1) - zero > was)
				fpr("-%d", (ip - 1) - zero);
			bad++;
			was = 0;
		}
	if (was != 0) {
		if (bad == 0)
			fpr(" [Lost line(s):");
		fpr(" %d", was);
		if (dol - zero != was)
			fpr("-%d", dol - zero);
		bad++;
	}
	if (bad)
		fpr("]");
}

/*
 * Aw shucks, if we only had a (void) cast.
 */
#ifdef lint
Ignorl(a)
	long a;
{

	a = a;
}

Ignore(a)
	char *a;
{

	a = a;
}

Ignorf(a)
	int (*a)();
{

	a = a;
}

ignorl(a)
	long a;
{

	a = a;
}
#endif

int	cntch, cntln, cntodd, cntnull;
/*
 * Following routines stolen mercilessly from ex.
 */
void
putfile(int i)
{
	line *a1;
	register char *fp, *lp;
	register int nib;

	(void)i;
	a1 = addr1;
	clrstats();
	cntln = addr2 - a1 + 1;
	if (cntln == 0)
		return;
	nib = BUFSIZ;
	fp = genbuf;
	do {
		ex_getline(*a1++);
		lp = linebuf;
		for (;;) {
			if (--nib < 0) {
				nib = fp - genbuf;
				if (write(io, genbuf, nib) != nib)
					wrerror();
				cntch += nib;
				nib = 511;
				fp = genbuf;
			}
			if ((*fp++ = *lp++) == 0) {
				fp[-1] = '\n';
				break;
			}
		}
	} while (a1 <= addr2);
	nib = fp - genbuf;
	if (write(io, genbuf, nib) != nib)
		wrerror();
	cntch += nib;
}

void
wrerror(void)
{

	syserror();
}

void
clrstats(void)
{

	ninbuf = 0;
	cntch = 0;
	cntln = 0;
	cntnull = 0;
	cntodd = 0;
}

#define	READ	0
#define	WRITE	1

void
ex_getline(line tl)
{
	register char *bp, *lp;
	register int nl;

	lp = linebuf;
	bp = getblock(tl, READ);
	nl = nleft;
	tl &= ~OFFMSK;
	while ((*lp++ = *bp++))
		if (--nl == 0) {
			bp = getblock(tl += INCRMT, READ);
			nl = nleft;
		}
}

char *
getblock(line atl, int iof)
{
	register int bno, off;
	
	bno = (atl >> OFFBTS) & BLKMSK;
	off = (atl << SHFT) & LBTMSK;
	if (bno >= NMBLKS)
		error(" Tmp file too large");
	nleft = BUFSIZ - off;
	if (bno == iblock) {
		ichanged |= iof;
		return (ibuff + off);
	}
	if (bno == oblock)
		return (obuff + off);
	if (iof == READ) {
		if (ichanged)
			blkio(iblock, ibuff, write);
		ichanged = 0;
		iblock = bno;
		blkio(bno, ibuff, read);
		return (ibuff + off);
	}
	if (oblock >= 0)
		blkio(oblock, obuff, write);
	oblock = bno;
	return (obuff + off);
}

static void
blkio(int b, char *buf, ssize_t (*iofcn)())
{

	lseek(tfile, b * BUFSIZ, 0);
	if ((*iofcn)(tfile, buf, BUFSIZ) != BUFSIZ)
		syserror();
}

void
syserror(void)
{
	char *strerror();

	dirtcnt = 0;
	write(2, " ", 1);
	error(strerror(errno));
	exit(1);
}

/*
 * Must avoid stdio because expreserve uses sbrk to do memory
 * allocation and stdio uses malloc.
 */
void
#if __STDC__
fpr(const char *fmt, ...)
#else
fpr(fmt, va_alist)
	char *fmt;
	va_dcl
#endif
{
	va_list ap;
	char buf[BUFSIZ];

#if __STDC__
	va_start(ap, fmt);
#else
	va_start(ap);
#endif
	(void)vsnprintf(buf, sizeof(buf), fmt, ap);
	va_end(ap);
	write(2, buf, strlen(buf));
}
