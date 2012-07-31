/*
	plus3dosfs: filesystem for mounting +3DOS partitions from an idedosfs
	Copyright (C) 2012 Edward Cree <ec429@cantab.net>

	This program is free software; you can redistribute it and/or modify
    it under the terms of the GNU General Public License as published by
    the Free Software Foundation; either version 2 of the License, or
    (at your option) any later version.

    This program is distributed in the hope that it will be useful,
    but WITHOUT ANY WARRANTY; without even the implied warranty of
    MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
    GNU General Public License for more details.

    You should have received a copy of the GNU General Public License
    along with this program; if not, write to the Free Software
    Foundation, Inc., 59 Temple Place, Suite 330, Boston, MA  02111-1307  USA
*/

#define FUSE_USE_VERSION 26

#include <fuse.h>
#include <stdio.h>
#include <stdlib.h>
#include <stdbool.h>
#include <stdint.h>
#include <string.h>
#include <errno.h>
#include <fcntl.h>
#include <unistd.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <sys/file.h> // for flock()
#include <attr/xattr.h>
#include <pthread.h> // for mutexen

typedef struct
{
	uint8_t status;
	char name[8];
	char ext[3];
	bool ro,sys,ar;
	uint16_t xnum; // extent number
	uint8_t rcount; // count of records in last used logical extent
	uint8_t bcount; // count of bytes in last used record.  +3DOS apparently doesn't use this (so you have to rely on the header)
	/*
		Files without headers can
		only record their EOF position to the start of the next 128 byte
		record, i.e. ceiling(EOF/128). Files with headers have their EOF
		position recorded exactly.
		-- The Sinclair ZX Spectrum +3 manual, Chapter 8, Part 27: "Guide to +3DOS"
	*/
	uint16_t al[16]; // block pointers
}
plus3_dirent;

pthread_rwlock_t dmex; // disk mutex
char *dm=NULL; // disk mmap
off_t d_sz; // mmap region size
uint32_t d_ndirent; // number of directory entries (DRM+1)
uint32_t d_nblocks; // number of blocks (DSM+1)
uint8_t d_bsh; // BSH (Block SHift)
off_t d_offset; // offset of the start of the data
bool d_manyblocks; // if true, there are only 8 block pointers in a dirent as each block pointer is 2 bytes
plus3_dirent *d_list=NULL;

static void dread(char *buf, size_t bytes, off_t offset);
plus3_dirent d_decode(const char buf[0x20]);
int32_t lookup(const char *path);

#define read16(p)	(((unsigned char *)p)[0]|(((unsigned char *)p)[1]<<8))
#define read32(p)	(((unsigned char *)p)[0]|(((unsigned char *)p)[1]<<8)|(((unsigned char *)p)[2]<<16)|(((unsigned char *)p)[3]<<24))

static int plus3_getattr(const char *path, struct stat *st)
{
	memset(st, 0, sizeof(struct stat));
	if(strcmp(path, "/")==0)
	{
		st->st_mode=S_IFDIR | 0400;
		st->st_nlink=2;
		st->st_size=d_sz;
		return(0);
	}
	if(path[0]!='/') return(-ENOENT);
	int32_t i=lookup(path+1);
	if((i<0)||(i>=(int32_t)d_ndirent))
		return(-ENOENT);
	pthread_rwlock_rdlock(&dmex);
	st->st_mode=S_IFREG | (d_list[i].ro?0500:0700);
	st->st_size=128*d_list[i].rcount+d_list[i].bcount;
	// grovel for the header
	off_t where=d_offset+(((off_t)d_list[i].al[0])<<(7+d_bsh));
	if(memcmp(dm+where, "PLUS3DOS\032", 9)==0)
	{
		uint32_t size=read32(dm+where+11);
		//fprintf(stderr, "HEADER? %zu\n", (size_t)size);
		uint8_t ck=0; // header checksum
		for(size_t i=0;i<127;i++)
			ck+=dm[where+i];
		if(ck==(uint8_t)dm[where+127])
			st->st_size=size-128; // first 128 bytes are the header itself
		/*else
			fprintf(stderr, "BADHEADER %hhu %hhu\n", ck, dm[where+127]);*/
	}
	/*else
		fprintf(stderr, "NOHEADER\n");*/
	st->st_nlink=1;
	pthread_rwlock_unlock(&dmex);
	return(0);
}

static int plus3_readdir(const char *path, void *buf, fuse_fill_dir_t filler, off_t offset, struct fuse_file_info *fi)
{
	if(strcmp(path, "/")==0)
	{
		filler(buf, ".", NULL, 0);
		filler(buf, "..", NULL, 0);
		pthread_rwlock_rdlock(&dmex);
		for(uint32_t i=0;i<d_ndirent;i++)
		{
			if((d_list[i].status<16)&&!d_list[i].xnum)
			{
				char nm[13];
				char ex[4];
				memcpy(nm, d_list[i].name, 8);
				memcpy(ex, d_list[i].ext, 3);
				size_t ne=8, ee=3;
				nm[ne]=ex[ee]=0;
				while(ne&&(nm[ne-1]==' ')) nm[--ne]=0;
				while(ee&&(ex[ee-1]==' ')) ex[--ee]=0;
				if(!(ne||ee)) continue;
				if(ee) snprintf(nm+ne, 13-ne, ".%s", ex);
				filler(buf, nm, NULL, 0);
			}
		}
		pthread_rwlock_unlock(&dmex);
		return(0);
	}
	return(-ENOENT);
}

static int plus3_open(const char *path, struct fuse_file_info *fi)
{
	if(fi->flags&O_SYNC) return(-ENOSYS);
	if(fi->flags&O_TRUNC) return(-EACCES);
	if(strcmp(path, "/")==0)
	{
		return(-EISDIR);
	}
	if(path[0]!='/') return(-ENOENT);
	int32_t i=lookup(path+1);
	if((i<0)||(i>=(int32_t)d_ndirent))
		return(-ENOENT);
	fi->fh=i;
	return(0);
}

static int plus3_read(const char *path, char *buf, size_t size, off_t offset, struct fuse_file_info *fi)
{
	int32_t i=fi->fh;
	if((i<0)||(i>=(int32_t)d_ndirent))
		return(-ENOENT);
	pthread_rwlock_rdlock(&dmex);
	// grovel for the header
	off_t where=d_offset+(((off_t)d_list[i].al[0])<<(7+d_bsh));
	bool header=false;
	size_t len=128*d_list[i].rcount+d_list[i].bcount;
	if(memcmp(dm+where, "PLUS3DOS\032", 9)==0)
	{
		uint32_t size=read32(dm+where+11);
		uint8_t ck=0; // header checksum
		for(size_t i=0;i<127;i++)
			ck+=dm[where+i];
		if(ck==(uint8_t)dm[where+127])
		{
			header=true;
			len=size;
		}
	}
	if(offset>len)
	{
		pthread_rwlock_unlock(&dmex);
		return(0);
	}
	if(offset+size>len)
		size=len-offset;
	uint32_t blocknum=offset>>(7+d_bsh), endblocknum=(offset+size)>>(7+d_bsh);
	if(!(blocknum||endblocknum))
		memcpy(buf, dm+where+(header?128:0), size);
	else
	{
		uint32_t transferred=0, len;
		for(uint32_t b=blocknum;b<=endblocknum;b++)
		{
			if(b<(d_manyblocks?16:8))
				where=d_offset+(((off_t)d_list[i].al[b])<<(7+d_bsh));
			else
			{
				fprintf(stderr, "File covers more than one extent; can't handle this yet!\n");
				pthread_rwlock_unlock(&dmex);
				return(-EIO);
			}
			if(b==blocknum)
			{
				len=((blocknum+1)<<(7+d_bsh))-offset;
				memcpy(buf, dm+where+((header&&!b)?128:0), len);
			}
			else if(b==endblocknum)
			{
				len=offset+size-(endblocknum<<(7+d_bsh));
				memcpy(buf+transferred, dm+where, len);
			}
			else
			{
				len=1<<(7+d_bsh);
				memcpy(buf+transferred, dm+where, len);
			}
			transferred+=len;
		}
		size=transferred;
	}
	pthread_rwlock_unlock(&dmex);
	return(size);
}

static int plus3_getxattr(const char *path, const char *name, char *value, size_t vlen)
{
	if(path[0]!='/') return(-ENOENT);
	if(!path[1]) return(-ENOATTR);
	int32_t i=lookup(path+1);
	if((i<0)||(i>=(int32_t)d_ndirent))
		return(-ENOENT);
	pthread_rwlock_rdlock(&dmex);
	// grovel for the header
	off_t where=d_offset+(((off_t)d_list[i].al[0])<<(7+d_bsh));
	bool header=false;
	size_t len=128*d_list[i].rcount+d_list[i].bcount;
	if(memcmp(dm+where, "PLUS3DOS\032", 9)==0)
	{
		uint32_t size=read32(dm+where+11);
		uint8_t ck=0; // header checksum
		for(size_t i=0;i<127;i++)
			ck+=dm[where+i];
		if(ck==(uint8_t)dm[where+127])
		{
			header=true;
			len=size;
		}
	}
	int rlen=0;
	char result[256];
	if(header&&(strcmp(name, "user.plus3dos.plus3basic.filetype")==0))
		snprintf(result, 256, "%u%n", dm[where+15], &rlen);
	else if(header&&(dm[where+15]==0)&&(strcmp(name, "user.plus3dos.plus3basic.line")==0))
		snprintf(result, 256, "%u%n", read16(dm+where+18), &rlen);
	else if(header&&(dm[where+15]==0)&&(strcmp(name, "user.plus3dos.plus3basic.prog")==0))
		snprintf(result, 256, "%u%n", read16(dm+where+20), &rlen); /* start of the variable area relative to the start of the program */
	else if(header&&((dm[where+15]==1)||(dm[where+15]==2))&&(strcmp(name, "user.plus3dos.plus3basic.name")==0))
	{
		result[0]=dm[where+19];
		rlen=1;
	}
	else if(header&&(dm[where+15]==3)&&(strcmp(name, "user.plus3dos.plus3basic.addr")==0))
		snprintf(result, 256, "%u%n", read16(dm+where+18), &rlen);
	pthread_rwlock_unlock(&dmex);
	if(!rlen) return(-ENOATTR);
	if(vlen)
	{
		if((int)vlen<rlen)
			return(-ERANGE);
		memcpy(value, result, rlen);
	}
	return(rlen);
}

static int plus3_listxattr(const char *path, char *list, size_t size)
{
	if(path[0]!='/') return(-ENOENT);
	if(!path[1]) return(0);
	int32_t i=lookup(path+1);
	if((i<0)||(i>=(int32_t)d_ndirent))
		return(-ENOENT);
	pthread_rwlock_rdlock(&dmex);
	// grovel for the header
	off_t where=d_offset+(((off_t)d_list[i].al[0])<<(7+d_bsh));
	bool header=false;
	size_t len=128*d_list[i].rcount+d_list[i].bcount;
	if(memcmp(dm+where, "PLUS3DOS\032", 9)==0)
	{
		uint32_t size=read32(dm+where+11);
		uint8_t ck=0; // header checksum
		for(size_t i=0;i<127;i++)
			ck+=dm[where+i];
		if(ck==(uint8_t)dm[where+127])
		{
			header=true;
			len=size;
		}
	}
	size_t nxattrs=0;
	const char *xattrs[256];
	if(header)
	{
		xattrs[nxattrs++]="user.plus3dos.plus3basic.filetype";
		switch(dm[where+15]) // filetype
		{
			case 0:
				xattrs[nxattrs++]="user.plus3dos.plus3basic.line";
				xattrs[nxattrs++]="user.plus3dos.plus3basic.prog";
			break;
			case 1:
			case 2:
				xattrs[nxattrs++]="user.plus3dos.plus3basic.name";
			break;
			case 3:
				xattrs[nxattrs++]="user.plus3dos.plus3basic.addr";
			break;
		}
	}
	size_t listsize=0;
	for(size_t i=0;i<nxattrs;i++)
		listsize+=strlen(xattrs[i])+1;
	if(size)
	{
		if(size<listsize)
		{
			pthread_rwlock_unlock(&dmex);
			return(-ERANGE);
		}
		for(size_t i=0;i<nxattrs;i++)
		{
			size_t l=strlen(xattrs[i]);
			strcpy(list, xattrs[i]);
			list[l]=0;
			list+=l+1;
		}
	}
	pthread_rwlock_unlock(&dmex);
	return(listsize);
}

static struct fuse_operations plus3_oper = {
	.getattr	= plus3_getattr,
	/*.access		= plus3_access,
	.readlink	= plus3_readlink,*/
	.readdir	= plus3_readdir,
	/*.mknod		= plus3_mknod,
	.mkdir		= plus3_mkdir,
	.symlink	= plus3_symlink,
	.unlink		= plus3_unlink,
	.rmdir		= plus3_rmdir,
	.rename		= plus3_rename,
	.link		= plus3_link,
	.chmod		= plus3_chmod,
	.chown		= plus3_chown,
	.truncate	= plus3_truncate,
	.utimens	= plus3_utimens,*/
	.open		= plus3_open,
	.read		= plus3_read,
	/*.write		= plus3_write,
	.statfs		= plus3_statfs,
	.release	= plus3_release,
	.fsync		= plus3_fsync,*/
	/*.setxattr	= plus3_setxattr,*/
	.getxattr	= plus3_getxattr,
	.listxattr	= plus3_listxattr,
	/*.removexattr= plus3_removexattr,*/
};

int main(int argc, char *argv[])
{
	if(argc<3)
	{
		fprintf(stderr, "Usage: plus3dosfs <part-image> <mountpoint> [options]\n");
		return(1);
	}
	if(pthread_rwlock_init(&dmex, NULL))
	{
		perror("pthread_rwlock_init");
		return(1);
	}
	const char *df=argv[1];
	struct stat st;
	if(stat(df, &st))
	{
		fprintf(stderr, "Failed to stat %s\n", df);
		perror("\tstat");
		pthread_rwlock_destroy(&dmex);
		return(1);
	}
	char ptbuf[4];
	ssize_t ptlen;
	if((ptlen=getxattr(df, "user.idedos.pt", ptbuf, sizeof ptbuf))<0)
	{
		perror("getxattr");
		return(1);
	}
	if(strncmp(ptbuf, "3", ptlen))
	{
		fprintf(stderr, "%s is not a +3DOS partition\n\tuser.idedos.pt=%s\n", df, ptbuf);
		return(1);
	}
	d_sz=st.st_size;
	fprintf(stderr, "%s size is %jdB", df, (intmax_t)d_sz);
	if(d_sz>2048)
	{
		const char *u="k";
		size_t uf=1024;
		if(d_sz/uf>2048)
		{
			u="M";
			uf*=1024;
		}
		if(d_sz/uf>2048)
		{
			u="G";
			uf*=1024;
		}
		fprintf(stderr, " (%.3g%sB)", d_sz/(double)uf, u);
	}
	fprintf(stderr, "\n");
	int dfd=open(df, O_RDWR);
	if(dfd<0)
	{
		perror("open");
		pthread_rwlock_destroy(&dmex);
		return(1);
	}
	if(flock(dfd, LOCK_EX|LOCK_NB))
	{
		if(errno==EWOULDBLOCK)
		{
			fprintf(stderr, "%s is locked by another process (flock: EWOULDBLOCK)\n", df);
		}
		else
			perror("flock");
		pthread_rwlock_destroy(&dmex);
		return(1);
	}
	dm=mmap(NULL, d_sz, PROT_READ|PROT_WRITE, MAP_SHARED|MAP_POPULATE, dfd, 0);
	if(!dm)
	{
		perror("mmap");
		flock(dfd, LOCK_UN);
		close(dfd);
		pthread_rwlock_destroy(&dmex);
		return(1);
	}
	fprintf(stderr, "%s mmap()ed in\n", df);
	int rv=EXIT_FAILURE;
	
	char ndbuf[6];
	ssize_t ndlen;
	if((ndlen=getxattr(df, "user.plus3dos.xdpb.ndirent", ndbuf, sizeof ndbuf))<0)
	{
		perror("getxattr");
		goto shutdown;
	}
	ndbuf[ndlen]=0;
	if(sscanf(ndbuf, "%u", &d_ndirent)!=1)
	{
		fprintf(stderr, "Bad user.plus3dos.xdpb.ndirent = %s\n", ndbuf);
		goto shutdown;
	}
	if(!(d_list=malloc(d_ndirent*sizeof(plus3_dirent))))
	{
		perror("malloc");
		goto shutdown;
	}
	if((ndlen=getxattr(df, "user.plus3dos.xdpb.nblocks", ndbuf, sizeof ndbuf))<0)
	{
		perror("getxattr");
		goto shutdown;
	}
	ndbuf[ndlen]=0;
	if(sscanf(ndbuf, "%u", &d_nblocks)!=1)
	{
		fprintf(stderr, "Bad user.plus3dos.xdpb.nblocks = %s\n", ndbuf);
		goto shutdown;
	}
	char bshbuf[6];
	ssize_t bshlen;
	if((bshlen=getxattr(df, "user.plus3dos.xdpb.bsh", bshbuf, sizeof bshbuf))<0)
	{
		perror("getxattr");
		goto shutdown;
	}
	bshbuf[bshlen]=0;
	if(sscanf(bshbuf, "%hhu", &d_bsh)!=1)
	{
		fprintf(stderr, "Bad user.plus3dos.xdpb.bsh = %s\n", bshbuf);
		goto shutdown;
	}
	d_manyblocks=(d_nblocks>255);
	
	d_offset=0;
	/* Magic d_offset autodetection, because I can't work out which eXDPB params control it */
	/* We just keep looking until we get a valid dirent */
	while(!dm[d_offset+1])
	{
		d_offset+=0x20;
		if(d_offset+1>d_sz)
		{
			fprintf(stderr, "Failed to grovelise d_offset\n");
			goto shutdown;
		}
	}
	size_t uents=0;
	for(size_t i=0;i<d_ndirent;i++)
	{
		char dbuf[0x20];
		dread(dbuf, 0x20, d_offset+i*0x20);
		d_list[i]=d_decode(dbuf);
		if(d_list[i].status!=0xe5)
			uents++;
	}
	fprintf(stderr, "Used %zu of %zu dirents\n", uents, d_ndirent);
	
	int fargc=argc-1;
	char **fargv=(char **)malloc(fargc*sizeof(char *));
	fargv[0]=argv[0];
	for(int i=1;i<fargc;i++)
		fargv[i]=argv[i+1];
	
	rv=fuse_main(fargc, fargv, &plus3_oper, NULL);
	shutdown:
	pthread_rwlock_wrlock(&dmex);
	munmap(dm, d_sz);
	flock(dfd, LOCK_UN);
	close(dfd);
	pthread_rwlock_unlock(&dmex);
	pthread_rwlock_destroy(&dmex);
	return(rv);
}

void dread(char *buf, size_t bytes, off_t offset)
{
	if(!buf) return;
	pthread_rwlock_rdlock(&dmex);
	memcpy(buf, dm+offset, bytes);
	pthread_rwlock_unlock(&dmex);
}

/*
	St F0 F1 F2 F3 F4 F5 F6 F7 E0 E1 E2 Xl Bc Xh Rc
	Al Al Al Al Al Al Al Al Al Al Al Al Al Al Al Al
	uint8_t status;
	char name[8];
	char ext[3];
	bool ro,sys,ar;
	uint16_t xnum; // extent number
	uint8_t rcount; // count of records in last used logical extent
	uint8_t bcount; // count of bytes in last used record
	uint16_t al[16]; // block pointers
*/
plus3_dirent d_decode(const char buf[0x20])
{
	plus3_dirent rv;
	rv.status=buf[0];
	for(size_t i=0;i<8;i++)
		rv.name[i]=buf[i+1]&0x7f;
	for(size_t i=0;i<3;i++)
		rv.ext[i]=buf[i+9]&0x7f;
	rv.ro=buf[9]&0x80;
	rv.sys=buf[10]&0x80;
	rv.ar=buf[11]&0x80;
	rv.xnum=(buf[12]&0x1f)|((buf[14]&0x3f)<<5);
	rv.bcount=buf[13];
	rv.rcount=buf[15];
	if(d_manyblocks)
		for(size_t i=0;i<8;i++)
			rv.al[i]=read16(buf+0x10+(i<<1));
	else
		memcpy(rv.al, buf+0x10, 0x10);
	return(rv);
}

int32_t lookup(const char *path)
{
	char nm[9], ex[4];
	size_t ne,nl,ee=0;
	for(ne=0;ne<8;ne++)
	{
		if(path[ne]=='.') break;
		if(!path[ne]) break;
		nm[ne]=path[ne];
	}
	nl=ne;
	for(;ne<8;ne++)
		nm[ne]=' ';
	nm[8]=0;
	if(path[nl])
	{
		if(path[nl++]!='.') return(-1);
		for(;ee<3;ee++)
		{
			if(!path[nl+ee]) break;
			ex[ee]=path[nl+ee];
		}
	}
	for(;ee<3;ee++)
		ex[ee]=' ';
	ex[3]=0;
	pthread_rwlock_rdlock(&dmex);
	for(uint32_t i=0;i<d_ndirent;i++)
	{
		if((d_list[i].status<16)&&!d_list[i].xnum)
		{
			if(!memcmp(nm, d_list[i].name, 8))
				if(!memcmp(ex, d_list[i].ext, 3))
				{
					pthread_rwlock_unlock(&dmex);
					return(i);
				}
		}
	}
	pthread_rwlock_unlock(&dmex);
	return(-1);
}