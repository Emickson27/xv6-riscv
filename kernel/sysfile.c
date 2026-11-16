//
// File-system system calls.
// Mostly argument checking, since we don't trust
// user code, and calls into file.c and fs.c.
//

#include "types.h"
#include "riscv.h"
#include "defs.h"
#include "param.h"
#include "stat.h"
#include "spinlock.h"
#include "proc.h"
#include "fs.h"
#include "sleeplock.h"
#include "file.h"
#include "fcntl.h"

// Fetch the nth word-sized system call argument as a file descriptor
// and return both the descriptor and the corresponding struct file.
static int
argfd(int n, int *pfd, struct file **pf)
{
  int fd;
  struct file *f;

  argint(n, &fd);
  if (fd < 0 || fd >= NOFILE || (f = myproc()->ofile[fd]) == 0)
    return -1;
  if (pfd)
    *pfd = fd;
  if (pf)
    *pf = f;
  return 0;
}

// Allocate a file descriptor for the given file.
// Takes over file reference from caller on success.
static int
fdalloc(struct file *f)
{
  int fd;
  struct proc *p = myproc();

  for (fd = 0; fd < NOFILE; fd++) {
    if (p->ofile[fd] == 0) {
      p->ofile[fd] = f;
      return fd;
    }
  }
  return -1;
}

uint64
sys_dup(void)
{
  struct file *f;
  int fd;

  if (argfd(0, 0, &f) < 0)
    return -1;
  if ((fd = fdalloc(f)) < 0)
    return -1;
  filedup(f);
  return fd;
}

uint64
sys_read(void)
{
  struct file *f;
  int n;
  uint64 p;

  argaddr(1, &p);
  argint(2, &n);
  if (argfd(0, 0, &f) < 0)
    return -1;
  return fileread(f, p, n);
}

uint64
sys_write(void)
{
  struct file *f;
  int n;
  uint64 p;

  argaddr(1, &p);
  argint(2, &n);
  if (argfd(0, 0, &f) < 0)
    return -1;

  return filewrite(f, p, n);
}

uint64
sys_close(void)
{
  int fd;
  struct file *f;

  if (argfd(0, &fd, &f) < 0)
    return -1;
  myproc()->ofile[fd] = 0;
  fileclose(f);
  return 0;
}

uint64
sys_fstat(void)
{
  struct file *f;
  uint64 st; // user pointer to struct stat

  argaddr(1, &st);
  if (argfd(0, 0, &f) < 0)
    return -1;
  return filestat(f, st);
}

// Create the path new as a link to the same inode as old.
uint64
sys_link(void)
{
  char name[DIRSIZ], new[MAXPATH], old[MAXPATH];
  struct inode *dp, *ip;

  if (argstr(0, old, MAXPATH) < 0 || argstr(1, new, MAXPATH) < 0)
    return -1;

  begin_op();
  if ((ip = namei(old)) == 0) {
    end_op();
    return -1;
  }

  ilock(ip);
  if (ip->type == T_DIR) {
    iunlockput(ip);
    end_op();
    return -1;
  }

  if (ip->nlink >= NLINK_MAX) {
    iunlockput(ip);
    end_op();
    return -1;
  }

  ip->nlink++;
  iupdate(ip);
  iunlock(ip);

  if ((dp = nameiparent(new, name)) == 0)
    goto bad;
  ilock(dp);
  // dp may have been unlinked while we resolved it; linking into an
  // orphaned directory leaks ip (itrunc discards the record without
  // dropping ip->nlink).  create() has the same guard.
  if (dp->nlink == 0) {
    iunlockput(dp);
    goto bad;
  }
  if (dp->dev != ip->dev || dirlink(dp, name, ip->inum) < 0) {
    iunlockput(dp);
    goto bad;
  }
  iunlockput(dp);
  iput(ip);

  end_op();

  return 0;

bad:
  ilock(ip);
  ip->nlink--;
  iupdate(ip);
  iunlockput(ip);
  end_op();
  return -1;
}

// Is the directory dp empty except for "." and ".." ?
static int
isdirempty(struct inode *dp)
{
  int off;
  struct dirent de;

  for (off = 2 * sizeof(de); off < dp->size; off += sizeof(de)) {
    if (readi(dp, 0, (uint64)&de, off, sizeof(de)) != sizeof(de))
      panic("isdirempty: readi");
    if (de.inum != 0)
      return 0;
  }
  return 1;
}

uint64
sys_unlink(void)
{
  struct inode *ip, *dp;
  struct dirent de;
  char name[DIRSIZ], path[MAXPATH];
  uint off;

  if (argstr(0, path, MAXPATH) < 0)
    return -1;

  begin_op();
  if ((dp = nameiparent(path, name)) == 0) {
    end_op();
    return -1;
  }

  ilock(dp);

  // Cannot unlink "." or "..".
  if (namecmp(name, ".") == 0 || namecmp(name, "..") == 0)
    goto bad;

  if ((ip = dirlookup(dp, name, &off)) == 0)
    goto bad;
  ilock(ip);

  if (ip->nlink < 1)
    panic("unlink: nlink < 1");
  if (ip->type == T_DIR && !isdirempty(ip)) {
    iunlockput(ip);
    goto bad;
  }

  memset(&de, 0, sizeof(de));
  if (writei(dp, 0, (uint64)&de, off, sizeof(de)) != sizeof(de))
    panic("unlink: writei");
  if (ip->type == T_DIR) {
    dp->nlink--;
    iupdate(dp);
  }
  iunlockput(dp);

  ip->nlink--;
  iupdate(ip);
  iunlockput(ip);

  end_op();

  return 0;

bad:
  iunlockput(dp);
  end_op();
  return -1;
}

static struct inode *
create(char *path, short type, short major, short minor)
{
  struct inode *ip, *dp;
  char name[DIRSIZ];

  if ((dp = nameiparent(path, name)) == 0)
    return 0;

  ilock(dp);

  if (dp->nlink == 0) {
    iunlockput(dp);
    return 0;
  }

  // a new directory's ".." would push dp->nlink past its maximum
  if (type == T_DIR && dp->nlink >= NLINK_MAX) {
    iunlockput(dp);
    return 0;
  }

  if ((ip = dirlookup(dp, name, 0)) != 0) {
    iunlockput(dp);
    ilock(ip);
    if (type == T_FILE && (ip->type == T_FILE || ip->type == T_DEVICE))
      return ip;
    iunlockput(ip);
    return 0;
  }

  if ((ip = ialloc(dp->dev, type)) == 0) {
    iunlockput(dp);
    return 0;
  }

  ilock(ip);
  ip->major = major;
  ip->minor = minor;
  ip->nlink = 1;
  iupdate(ip);

  if (type == T_DIR) { // Create . and .. entries.
    // No ip->nlink++ for ".": avoid cyclic ref count.
    if (dirlink(ip, ".", ip->inum) < 0 || dirlink(ip, "..", dp->inum) < 0)
      goto fail;
  }

  if (dirlink(dp, name, ip->inum) < 0)
    goto fail;

  if (type == T_DIR) {
    // now that success is guaranteed:
    dp->nlink++; // for ".."
    iupdate(dp);
  }

  iunlockput(dp);

  return ip;

fail:
  // something went wrong. de-allocate ip.
  ip->nlink = 0;
  iupdate(ip);
  iunlockput(ip);
  iunlockput(dp);
  return 0;
}

uint64
sys_open(void)
{
  char path[MAXPATH];
  int fd, omode;
  struct file *f;
  struct inode *ip;
  int n;

  argint(1, &omode);
  if ((n = argstr(0, path, MAXPATH)) < 0)
    return -1;

  begin_op();

  if (omode & O_CREATE) {
    ip = create(path, T_FILE, 0, 0);
    if (ip == 0) {
      end_op();
      return -1;
    }
  } else {
    if ((ip = namei(path)) == 0) {
      end_op();
      return -1;
    }
    ilock(ip);
    if (ip->type == T_DIR && omode != O_RDONLY) {
      iunlockput(ip);
      end_op();
      return -1;
    }
  }

  if (ip->type == T_DEVICE && (ip->major < 0 || ip->major >= NDEV)) {
    iunlockput(ip);
    end_op();
    return -1;
  }

  if ((f = filealloc()) == 0 || (fd = fdalloc(f)) < 0) {
    if (f)
      fileclose(f);
    iunlockput(ip);
    end_op();
    return -1;
  }

  if (ip->type == T_DEVICE) {
    f->type = FD_DEVICE;
    f->major = ip->major;
  } else {
    f->type = FD_INODE;
    f->off = 0;
  }
  f->ip = ip;
  f->readable = !(omode & O_WRONLY);
  f->writable = (omode & O_WRONLY) || (omode & O_RDWR);
  f->append = (omode & O_APPEND) != 0;

  if ((omode & O_TRUNC) && ip->type == T_FILE) {
    itrunc(ip);
  }

  iunlock(ip);
  end_op();

  return fd;
}

uint64
sys_mkdir(void)
{
  char path[MAXPATH];
  struct inode *ip;

  begin_op();
  if (argstr(0, path, MAXPATH) < 0 || (ip = create(path, T_DIR, 0, 0)) == 0) {
    end_op();
    return -1;
  }
  iunlockput(ip);
  end_op();
  return 0;
}

uint64
sys_mknod(void)
{
  struct inode *ip;
  char path[MAXPATH];
  int major, minor;

  begin_op();
  argint(1, &major);
  argint(2, &minor);
  if ((argstr(0, path, MAXPATH)) < 0 ||
      (ip = create(path, T_DEVICE, major, minor)) == 0) {
    end_op();
    return -1;
  }
  iunlockput(ip);
  end_op();
  return 0;
}

uint64
sys_chdir(void)
{
  char path[MAXPATH];
  struct inode *ip;
  struct proc *p = myproc();

  begin_op();
  if (argstr(0, path, MAXPATH) < 0 || (ip = namei(path)) == 0) {
    end_op();
    return -1;
  }
  ilock(ip);
  if (ip->type != T_DIR) {
    iunlockput(ip);
    end_op();
    return -1;
  }
  iunlock(ip);
  iput(p->cwd);
  end_op();
  p->cwd = ip;
  return 0;
}

uint64
sys_exec(void)
{
  char path[MAXPATH], *argv[MAXARG];
  int i;
  uint64 uargv, uarg;

  argaddr(1, &uargv);
  if (argstr(0, path, MAXPATH) < 0) {
    return -1;
  }
  memset(argv, 0, sizeof(argv));
  for (i = 0;; i++) {
    if (i >= NELEM(argv)) {
      goto bad;
    }
    if (fetchaddr(uargv + sizeof(uint64) * i, (uint64 *)&uarg) < 0) {
      goto bad;
    }
    if (uarg == 0) {
      argv[i] = 0;
      break;
    }
    argv[i] = kalloc();
    if (argv[i] == 0)
      goto bad;
    if (fetchstr(uarg, argv[i], PGSIZE) < 0)
      goto bad;
  }

  int ret = kexec(path, argv);

  for (i = 0; i < NELEM(argv) && argv[i] != 0; i++)
    kfree(argv[i]);

  return ret;

bad:
  for (i = 0; i < NELEM(argv) && argv[i] != 0; i++)
    kfree(argv[i]);
  return -1;
}

uint64
sys_pipe(void)
{
  uint64 fdarray; // user pointer to array of two integers
  struct file *rf, *wf;
  int fd0, fd1;
  struct proc *p = myproc();

  argaddr(0, &fdarray);
  if (pipealloc(&rf, &wf) < 0)
    return -1;
  fd0 = -1;
  if ((fd0 = fdalloc(rf)) < 0 || (fd1 = fdalloc(wf)) < 0) {
    if (fd0 >= 0)
      p->ofile[fd0] = 0;
    fileclose(rf);
    fileclose(wf);
    return -1;
  }
  if (copyout(p->pagetable, p->sz, fdarray, (char *)&fd0, sizeof(fd0)) < 0 ||
      copyout(p->pagetable, p->sz, fdarray + sizeof(fd0), (char *)&fd1,
              sizeof(fd1)) < 0) {
    p->ofile[fd0] = 0;
    p->ofile[fd1] = 0;
    fileclose(rf);
    fileclose(wf);
    return -1;
  }
  return 0;
}


// Helper: check whether target directory dp2 is equal to or inside src directory ip1.
// Caller holds locks on dp1 and dp2 (and ip1 is valid with ref >= 1).
static int
is_descendant(struct inode *ip1, struct inode *dp1, struct inode *dp2)
{
  if (dp2 == ip1)
    return 1;
  if (dp2->inum == ROOTINO)
    return 0;

  struct inode *cur = idup(dp2);
  while (cur->inum != ROOTINO) {
    struct inode *parent;
    if (cur != dp1 && cur != dp2) {
      ilock(cur);
      parent = dirlookup(cur, "..", 0);
      iunlock(cur);
    } else {
      parent = dirlookup(cur, "..", 0);
    }
    iput(cur);
    if (parent == 0)
      return 1; // conservative fail on corrupt dir
    if (parent == ip1) {
      iput(parent);
      return 1;
    }
    cur = parent;
  }
  iput(cur);
  return 0;
}

uint64
sys_rename(void)
{
  char oldpath[MAXPATH], newpath[MAXPATH];
  char oldname[DIRSIZ], newname[DIRSIZ];
  struct inode *dp1 = 0, *dp2 = 0, *ip1 = 0, *ip2 = 0;
  uint off1 = 0, off2 = 0;
  struct dirent de;

  if (argstr(0, oldpath, MAXPATH) < 0 || argstr(1, newpath, MAXPATH) < 0)
    return -1;

  begin_op();

  if ((dp1 = nameiparent(oldpath, oldname)) == 0) {
    end_op();
    return -1;
  }
  if ((dp2 = nameiparent(newpath, newname)) == 0) {
    iput(dp1);
    end_op();
    return -1;
  }

  // Cannot rename "." or ".."
  if (namecmp(oldname, ".") == 0 || namecmp(oldname, "..") == 0 ||
      namecmp(newname, ".") == 0 || namecmp(newname, "..") == 0) {
    iput(dp1);
    iput(dp2);
    end_op();
    return -1;
  }

  // Lock parent directories in ascending inum order to prevent deadlock
  if (dp1 == dp2) {
    ilock(dp1);
  } else if (dp1->inum < dp2->inum) {
    ilock(dp1);
    ilock(dp2);
  } else {
    ilock(dp2);
    ilock(dp1);
  }

  // Look up source entry in dp1
  if ((ip1 = dirlookup(dp1, oldname, &off1)) == 0)
    goto bad;

  // Look up destination entry in dp2 (may or may not exist)
  ip2 = dirlookup(dp2, newname, &off2);

  // Same inode (same name or hard link to same file): no-op, return 0
  if (ip1 == ip2) {
    iput(ip1);
    iput(ip2);
    if (dp1 != dp2)
      iunlock(dp2);
    iunlock(dp1);
    iput(dp1);
    iput(dp2);
    end_op();
    return 0;
  }

  // Cannot move a directory directly into itself (rename("d", "d/z"))
  // or overwrite a parent directory (rename("d/f", "d") or rename("a/b", "d"))
  if (ip1 == dp2 || ip2 == dp1 || ip2 == dp2) {
    iput(ip1);
    if (ip2 != 0)
      iput(ip2);
    goto bad;
  }

  // Lock child inodes in ascending inum order
  if (ip2 == 0) {
    ilock(ip1);
  } else if (ip1->inum < ip2->inum) {
    ilock(ip1);
    ilock(ip2);
  } else {
    ilock(ip2);
    ilock(ip1);
  }
  // Prevent moving a directory into its own subtree (e.g., rename("a", "a/b/c"))
  if (ip1->type == T_DIR && is_descendant(ip1, dp1, dp2))
    goto bad_locked;

  // Validate target type and emptiness if destination already exists
  if (ip2 != 0) {
    if (ip1->type == T_DIR && ip2->type != T_DIR)
      goto bad_locked;
    if (ip1->type != T_DIR && ip2->type == T_DIR)
      goto bad_locked;
    if (ip2->type == T_DIR && !isdirempty(ip2))
      goto bad_locked;
  }

  // --- All checks passed: perform atomic modifications ---

  // 1. Add or overwrite entry in dp2 pointing to ip1
  if (ip2 != 0) {
    // Overwrite existing dirent at off2 in dp2
    memset(&de, 0, sizeof(de));
    de.inum = ip1->inum;
    strncpy(de.name, newname, DIRSIZ);
    if (writei(dp2, 0, (uint64)&de, off2, sizeof(de)) != sizeof(de))
      panic("rename: writei dp2");
  } else {
    // Add new entry to dp2
    if (dirlink(dp2, newname, ip1->inum) < 0)
      goto bad_locked;
  }

  // 2. Remove old entry from dp1
  memset(&de, 0, sizeof(de));
  if (writei(dp1, 0, (uint64)&de, off1, sizeof(de)) != sizeof(de))
    panic("rename: writei dp1");

  // 3. Decrement link count on replaced target ip2
  if (ip2 != 0) {
    ip2->nlink--;
    iupdate(ip2);
  }

  // 4. If moving a directory across different parents, update ".." and parent nlinks
  if (ip1->type == T_DIR && dp1 != dp2) {
    uint dotdotoff;
    struct inode *dotdot = dirlookup(ip1, "..", &dotdotoff);
    if (dotdot == 0)
      panic("rename: no ..");
    iput(dotdot);

    memset(&de, 0, sizeof(de));
    de.inum = dp2->inum;
    strncpy(de.name, "..", DIRSIZ);
    if (writei(ip1, 0, (uint64)&de, dotdotoff, sizeof(de)) != sizeof(de))
      panic("rename: writei ..");

    dp1->nlink--;
    iupdate(dp1);
    dp2->nlink++;
    iupdate(dp2);
  }

  if (ip2 != 0)
    iunlockput(ip2);
  iunlockput(ip1);
  if (dp1 != dp2)
    iunlock(dp2);
  iunlock(dp1);
  iput(dp1);
  iput(dp2);
  end_op();
  return 0;

bad_locked:
  if (ip2 != 0)
    iunlockput(ip2);
  iunlockput(ip1);
bad:
  if (ip1 != 0 && ip2 == 0) {
    // ip1 was unlocked in bad: path only if we jumped before ilock(ip1)
  }
  if (dp1 != dp2)
    iunlock(dp2);
  iunlock(dp1);
  iput(dp1);
  iput(dp2);
  end_op();
  return -1;
}
