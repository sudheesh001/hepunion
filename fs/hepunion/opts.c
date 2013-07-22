/**
 * \file opts.c
 * \brief Exported functions by the HEPunion file system
 * \author Pierre Schweitzer <pierre.jean.schweitzer@cern.ch>
 * \version 1.0
 * \date 10-Dec-2011
 * \copyright GNU General Public License - GPL
 * \todo Disallow .me. and .wh. files creation
 * \todo Identical files on RO/RW after mod
 */

#include "hepunion.h"

static int hepunion_close(struct inode *inode, struct file *filp) {
	struct file *real_file = (struct file *)filp->private_data;

	pr_info("hepunion_close: %p, %p\n", inode, filp);

	validate_inode(inode);

	return filp_close(real_file, NULL);
}

static int hepunion_closedir(struct inode *inode, struct file *filp) {
	struct readdir_file *entry;
	struct opendir_context *ctx = (struct opendir_context *)filp->private_data;

	pr_info("hepunion_closedir: %p, %p\n", inode, filp);

	validate_inode(inode);

	/* First, clean all the lists */
	while (!list_empty(&ctx->whiteouts_head)) {
		entry = list_entry(ctx->whiteouts_head.next, struct readdir_file, files_entry);
		list_del(&entry->files_entry);
		kfree(entry);
	}

	while (!list_empty(&ctx->files_head)) {
		entry = list_entry(ctx->files_head.next, struct readdir_file, files_entry);
		list_del(&entry->files_entry);
		kfree(entry);
	}

	/* Then, release the context itself */
	kfree(ctx);

	return 0;
}

static int hepunion_create(struct inode *dir, struct dentry *dentry, int mode, struct nameidata *nameidata) {
	int err;
	struct hepunion_sb_info *context = get_context_i(dir);
	char *path = context->global1;
	char *real_path = context->global2;
	struct file* filp;
	struct iattr attr;
	struct inode *inode;

	pr_info("hepunion_create: %p, %p, %x, %p\n", dir, dentry, mode, nameidata);

	will_use_buffers(context);
	validate_inode(dir);
	validate_dentry(dentry);

	/* Try to find the file first */
	err = get_relative_path_for_file(dir, dentry, context, path, 1);
	if (err < 0) {
		release_buffers(context);
		return err;
	}

	/* And ensure it doesn't exist */
	err = find_file(path, real_path, context, 0);
	if (err >= 0) {
		release_buffers(context);
		return -EEXIST;
	}

	/* Once we are here, we know that the file does not exist
	 * And that we can create it (thanks to lookup)
	 */
	/* Create path if needed */
	err = find_path(path, real_path, context);
	if (err < 0) {
		release_buffers(context);
		return err;
	}

	/* Be paranoid, check access */
	err = can_create(path, real_path, context);
	if (err < 0) {
		release_buffers(context);
		return err;
	}

	/* Open the file */
	filp = creat_worker(real_path, context, mode);
	if (IS_ERR(filp)) {
		release_buffers(context);
		return PTR_ERR(filp);
	}

	/* Set its correct owner in case of creation */
	attr.ia_uid = current_fsuid();
	attr.ia_gid = current_fsgid();
	attr.ia_valid = ATTR_UID | ATTR_GID;

	push_root();
	err = notify_change(filp->f_dentry, &attr);
	filp_close(filp, NULL);
	pop_root();

	if (err < 0) {
		unlink(real_path, context);
		release_buffers(context);
		return err;
	}

	/* Now we're done, create the inode */
	inode = new_inode(dir->i_sb);
	if (!inode) {
		unlink(real_path, context);
		release_buffers(context);
		return -ENOMEM;
	}

	/* And fill it in */
	dir->__i_nlink++;
	inode->i_uid = current_fsuid();
	inode->i_gid = current_fsgid();
	inode->i_mtime = CURRENT_TIME;
	inode->i_atime = CURRENT_TIME;
	inode->i_ctime = CURRENT_TIME;
	inode->i_blocks = 0;
	inode->i_blkbits = 0;
	inode->i_op = &hepunion_iops;
	inode->i_fop = &hepunion_fops;
	inode->i_mode = mode;
	inode->__i_nlink = 1;
	inode->i_ino = name_to_ino(path);
#ifdef _DEBUG_
	inode->i_private = (void *)HEPUNION_MAGIC;
#endif
	insert_inode_hash(inode); 

	d_instantiate(dentry, inode);
	mark_inode_dirty(dir);
	mark_inode_dirty(inode);

	/* Remove whiteout if any */
	unlink_whiteout(path, context);

	release_buffers(context);
	return 0;
}

static int hepunion_getattr(struct vfsmount *mnt, struct dentry *dentry, struct kstat *kstbuf) {
	int err;
	struct hepunion_sb_info *context = get_context_d(dentry);
	char *path = context->global1;

	pr_info("hepunion_getattr: %p, %p, %p\n", mnt, dentry, kstbuf);

	will_use_buffers(context);
	validate_dentry(dentry);

	/* Get path */
	err = get_relative_path(NULL, dentry, context, path, 1);
	if (err < 0) {
		release_buffers(context);
		return err;
	}

	/* Call worker */
	err = get_file_attr(path, context, kstbuf);
	if (err >= 0) {
		/* Set our inode number */
		kstbuf->ino = dentry->d_inode->i_ino;
	}

	release_buffers(context);
	return err;
}

static int hepunion_link(struct dentry *old_dentry, struct inode *dir, struct dentry *dentry) {
	int err, origin;
	struct hepunion_sb_info *context = get_context_d(old_dentry);
	char *from = context->global1;
	char *to = context->global2;
	char *real_from = kmalloc(PATH_MAX, GFP_KERNEL);//dynamic array to solve stack problem
	char *real_to = kmalloc(PATH_MAX, GFP_KERNEL);//dynamic array to solve stack problem 
       
	pr_info("hepunion_link: %p, %p, %p\n", old_dentry, dir, dentry);

	will_use_buffers(context);
	validate_inode(dir);
	validate_dentry(old_dentry);
	validate_dentry(dentry);

	/* First, find file */
	err = get_relative_path(NULL, old_dentry, context, from, 1);
	if (err < 0) {
		release_buffers(context);
		return err;
	}

	origin = find_file(from, real_from, context, 0);
	if (origin < 0) {
		release_buffers(context);
		return origin;
	}

	/* Find destination */
	err = get_relative_path_for_file(dir, dentry, context, to, 1);
	if (err < 0) {
		release_buffers(context);
		return err;
	}

	/* And ensure it doesn't exist */
	err = find_file(to, real_to, context, 0);
	if (err >= 0) {
		release_buffers(context);
		return -EEXIST;
	}

	/* Check access */
	err = can_create(to, real_to, context);
	if (err < 0) {
		release_buffers(context);
		return err;
	}

	/* Create path if needed */
	err = find_path(to, real_to, context);
	if (err < 0) {
		release_buffers(context);
		return err;
	}

	if (origin == READ_ONLY) {
		/* Here, fallback to a symlink */
		err = symlink_worker(real_from, real_to, context);
		if (err < 0) {
			release_buffers(context);
			return err;
		}
	}
	else {
		/* Get RW name */
		if (make_rw_path(to, real_to) > PATH_MAX) {
			release_buffers(context);
			return -ENAMETOOLONG;
		}

		err = link_worker(real_from, real_to, context);
		if (err < 0) {
			release_buffers(context);
			return err;
		}
	}

	/* Remove possible whiteout */
	unlink_whiteout(to, context);
        kfree(real_from);
        kfree(real_to); 
	release_buffers(context);
	return 0;
}

static loff_t hepunion_llseek(struct file *file, loff_t offset, int origin) {
	struct file *real_file = (struct file *)file->private_data;
	loff_t ret;

	pr_info("hepunion_llseek: %p, %llx, %x\n", file, offset, origin);

	ret = vfs_llseek(real_file, offset, origin);
	file->f_pos = real_file->f_pos;

	return ret;
}

static struct dentry * hepunion_lookup(struct inode *dir, struct dentry *dentry, struct nameidata *nameidata) {
	/* We are looking for "dentry" in "dir" */
	int err;
	struct hepunion_sb_info *context = get_context_i(dir);
	char *path = context->global1;
	char *real_path = context->global2;
	struct inode *inode = NULL;
	struct read_inode_context * ctx;
	size_t namelen;
	unsigned long ino;

	pr_info("hepunion_lookup: %p, %p, %p\n", dir, dentry, nameidata);

	will_use_buffers(context);
	validate_inode(dir);

#ifdef _DEBUG_
	dentry->d_fsdata = (void *)HEPUNION_MAGIC;
#endif

	/* First get path of the file */
	err = get_relative_path_for_file(dir, dentry, context, path, 1);
	if (err < 0) {
		release_buffers(context);
		return ERR_PTR(err);
	}

	pr_info("Looking for: %s\n", path);

	/* Set our operations before we continue */
	dentry->d_op = &hepunion_dops;

	/* Now, look for the file */
	err = find_file(path, real_path, context, 0);
	if (err < 0) {
		if (err == -ENOENT) {
			pr_info("Null inode\n");
			d_add(dentry, inode);
			release_buffers(context);
			return NULL;
		} else {
			pr_info("Err: %d\n", err);
			release_buffers(context);
			return ERR_PTR(err);
		}
	}

	/* We've got it!
	 * Prepare a read_inode context for further read
	 */
	namelen = strlen(path); 
	ino = name_to_ino(path);
	ctx = kmalloc(sizeof(struct read_inode_context) + (namelen + 1) * sizeof(path[0]), GFP_KERNEL);
	ctx->ino = ino;
	memcpy(ctx->name, path, namelen * sizeof(path[0]));
	ctx->name[namelen] = 0;
	list_add(&ctx->read_inode_entry, &context->read_inode_head);

	/* Get inode */
	inode = iget_locked(dir->i_sb, ino);
	if (!inode) {
		inode = ERR_PTR(-EACCES);
	} else {
		/* Set our inode */
		d_add(dentry, inode);
	}

	/* Release the context, whatever happened
	 * If inode was new, read_inode has been called and the context used
	 * otherwise it was just useless
	 */
	list_del(&ctx->read_inode_entry);
	kfree(ctx);

	release_buffers(context);
	return NULL;
}

static int hepunion_mkdir(struct inode *dir, struct dentry *dentry, int mode) {
	int err;
	struct inode *inode;
	struct hepunion_sb_info *context = get_context_i(dir);
	char *path = context->global1;
	char *real_path = context->global2;

	pr_info("hepunion_mkdir: %p, %p, %x\n", dir, dentry, mode);

	will_use_buffers(context);
	validate_inode(dir);
	validate_dentry(dentry);

	/* Try to find the directory first */
	err = get_relative_path_for_file(dir, dentry, context, path, 1);
	if (err < 0) {
		release_buffers(context);
		return err;
	}

	/* And ensure it doesn't exist */
	err = find_file(path, real_path, context, 0);
	if (err >= 0) {
		release_buffers(context);
		return -EEXIST;
	}

	/* Get full path for destination */
	if (make_rw_path(path, real_path) > PATH_MAX) {
		release_buffers(context);
		return -ENAMETOOLONG;
	}

	/* Check access */
	err = can_create(path, real_path, context);
	if (err < 0) {
		release_buffers(context);
		return err;
	}

	/* Now, create/reuse arborescence */
	err = find_path(path, real_path, context);
	if (err < 0) {
		release_buffers(context);
		return err;
	}

	/* Ensure we have good mode */
	mode |= S_IFDIR;

	/* Just create dir now */
	err = mkdir_worker(real_path, context, mode);
	if (err < 0) {
		release_buffers(context);
		return err;
	}

	/* Hide contents */
	err = hide_directory_contents(path, context);
	if (err < 0) {
		rmdir(real_path, context);

		release_buffers(context);
		return err;
	}

	/* Now we're done, create the inode */
	inode = new_inode(dir->i_sb);
	if (!inode) {
		rmdir(real_path, context);

		release_buffers(context);
		return -ENOMEM;
	}

	/* And fill it in */
	dir->__i_nlink++;
	inode->i_uid = current_fsuid();
	inode->i_gid = current_fsgid();
	inode->i_mtime = CURRENT_TIME;
	inode->i_atime = CURRENT_TIME;
	inode->i_ctime = CURRENT_TIME;
	inode->i_blocks = 0;
	inode->i_blkbits = 0;
	inode->i_op = &hepunion_dir_iops;
	inode->i_fop = &hepunion_dir_fops;
	inode->i_mode = mode;
	inode->__i_nlink = 1;
	inode->i_ino = name_to_ino(path);
#ifdef _DEBUG_
	inode->i_private = (void *)HEPUNION_MAGIC;
#endif
	insert_inode_hash(inode); 

	d_instantiate(dentry, inode);
	mark_inode_dirty(dir);
	mark_inode_dirty(inode);

	/* Remove possible .wh. */
	unlink_whiteout(path, context);

	release_buffers(context);
	return 0;
}

static int hepunion_mknod(struct inode *dir, struct dentry *dentry, int mode, dev_t rdev) {
	int err;
	struct hepunion_sb_info *context = get_context_i(dir);
	char *path = context->global1;
	char *real_path = context->global2;

	pr_info("hepunion_mknod: %p, %p, %x, %x\n", dir, dentry, mode, rdev);

	will_use_buffers(context);
	validate_inode(dir);
	validate_dentry(dentry);

	/* Try to find the node first */
	err = get_relative_path_for_file(dir, dentry, context, path, 1);
	if (err < 0) {
		release_buffers(context);
		return err;
	}

	/* And ensure it doesn't exist */
	err = find_file(path, real_path, context, 0);
	if (err >= 0) {
		release_buffers(context);
		return -EEXIST;
	}

	/* Now, create/reuse arborescence */
	err = find_path(path, real_path, context);
	if (err < 0) {
		release_buffers(context);
		return err;
	}

	/* Just create file now */
	if (S_ISFIFO(mode)) {
		err = mkfifo_worker(real_path, context, mode);
		if (err < 0) {
			release_buffers(context);
			return err;
		}
	}
	else {
		err = mknod_worker(real_path, context, mode, rdev);
		if (err < 0) {
			release_buffers(context);
			return err;
		}
	}

	/* Remove possible whiteout */
	unlink_whiteout(path, context);

	release_buffers(context);
	return 0;
}

static int hepunion_open(struct inode *inode, struct file *file) {
	int err, origin;
	struct hepunion_sb_info *context = get_context_i(inode);
	char *path = context->global1;
	char *real_path = context->global2;
	short is_write_op = (file->f_flags & (O_WRONLY | O_RDWR));

	pr_info("hepunion_open: %p, %p\n", inode, file);

	will_use_buffers(context);
	validate_inode(inode);

	/* Don't check for flags here, if we are down here
	 * the user is allowed to read/write the file, the
	 * file was created if required (and allowed).
	 * Here, the only operation required is to open the
	 * file on the underlying file system
	 */

	/* Get our file path */
	err = get_relative_path(inode, file->f_dentry, context, path, 1);

	/* Get real file path */
	origin = find_file(path, real_path, context, (is_write_op ? CREATE_COPYUP : 0));
	if (origin < 0) {
		pr_info("Failed!\n");
		release_buffers(context);
		return origin;
	}

	/* If copyup created, check access */
	if (origin == READ_WRITE_COPYUP) {
		err = can_create(path, real_path, context);
		if (err < 0) {
			unlink_copyup(path, real_path, context);
			release_buffers(context);
			return err;
		}
	}

	/* Really open the file.
	 * The associated file object on real file system is stored
	 * as private data of the HEPunion file object. This is used
	 * to maintain data consistency and to forward requests on
	 * the file to the lower file system.
	 */
	pr_info("Will open... %s\n", real_path);
	file->private_data = open_worker_2(real_path, context, file->f_flags, file->f_mode);
	if (IS_ERR(file->private_data)) {
		err = PTR_ERR(file->private_data);
		file->private_data = NULL;

		if (origin == READ_WRITE_COPYUP) {
			unlink_copyup(path, real_path, context);
		}

		release_buffers(context);
		return err;
	}

	release_buffers(context);
	return 0;
}

static int hepunion_opendir(struct inode *inode, struct file *file) {
	int err;
	struct hepunion_sb_info *context = get_context_i(inode);
	char *path = context->global1;
	char *real_path = context->global2;
	struct opendir_context *ctx;
	char *ro_path = kmalloc(PATH_MAX, GFP_KERNEL);
	char *rw_path = kmalloc(PATH_MAX, GFP_KERNEL);
	size_t ro_len = 0;
	size_t rw_len = 0;

	pr_info("hepunion_opendir: %p, %p\n", inode, file);

	will_use_buffers(context);
	validate_inode(inode);

	/* Don't check for flags here, if we are down here
	 * the user is allowed to read/write the dir, the
	 * dir was created if required (and allowed).
	 * Here, the only operation required is to open the
	 * dir on the underlying file system
	 */

	/* Get our directory path */
	err = get_relative_path(inode, file->f_dentry, context, path, 1);

	/* Get real directory path */
	err = find_file(path, real_path, context, 0);
	if (err < 0) {
		release_buffers(context);
		return err;
	}

	if (find_file(path, rw_path, context, MUST_READ_WRITE) >= 0) {
		rw_len = strlen(rw_path);
	}

	if (find_file(path, ro_path, context, MUST_READ_ONLY) >= 0) {
		ro_len = strlen(ro_path);
	}

	/* Allocate readdir context */
	ctx = kmalloc(sizeof(struct opendir_context) + rw_len + ro_len + 2 * sizeof(char), GFP_KERNEL);
	if (!ctx) {
		release_buffers(context);
		return -ENOMEM;
	}

	/* Copy strings - RO first */
	if (ro_len) {
		ctx->ro_len = ro_len;
		ctx->ro_off = sizeof(struct opendir_context);

		strncpy((char *)(ctx->ro_off + (unsigned long)ctx), ro_path, ro_len);
		*((char *)(ctx->ro_off + ro_len + (unsigned long)ctx)) = '\0';
	}
	else {
		ctx->ro_len =
		ctx->ro_off = 0;
	}

	/* Then RW */
	if (rw_len) {
		ctx->rw_len = rw_len;
		ctx->rw_off = sizeof(struct opendir_context) + ro_len;
		/* Don't forget \0 */
		if (ro_len) {
			ctx->rw_off += sizeof(char);
		}

		strncpy((char *)(ctx->rw_off + (unsigned long)ctx), rw_path, rw_len);
		*((char *)(ctx->rw_off + rw_len + (unsigned long)ctx)) = '\0';
	}
	else {
		ctx->rw_len =
		ctx->rw_off = 0;
	}

	/* Keep inode */
	ctx->context = context;

	/* Init list heads */
	INIT_LIST_HEAD(&ctx->files_head);
	INIT_LIST_HEAD(&ctx->whiteouts_head);

	file->private_data = ctx;

	release_buffers(context);
        kfree(ro_path);
        kfree(rw_path); 
	return 0;
}

static int hepunion_permission(struct inode *inode, int mask, struct nameidata *nd) {
	int err;
	struct hepunion_sb_info *context = get_context_i(inode);
	char *path = context->global1;
	char *real_path = context->global2;

	pr_info("hepunion_permission: %p, %x, %p\n", inode, mask, nd);

	will_use_buffers(context);
	validate_inode(inode);
	if (nd && nd->path.dentry) {
		validate_dentry(nd->path.dentry);
	}

	/* Get path */
	err = get_relative_path(inode, (nd ? nd->path.dentry : NULL), context, path, 1);
	if (err) {
		release_buffers(context);
		return err;
	}

	/* Get file */
	err = find_file(path, real_path, context, 0);
	if (err < 0) {
		release_buffers(context);
		return err;
	}

	/* And call worker */
	err = can_access(path, real_path, context, mask);

	release_buffers(context);
	return err;
}

static ssize_t hepunion_read(struct file *file, char __user *buf, size_t count, loff_t *offset) {
	struct file *real_file = (struct file *)file->private_data;
	ssize_t ret;

	ret = vfs_read(real_file, buf, count, offset);
	file->f_pos = real_file->f_pos;

	return ret;
}



static int read_rw_branch(void *buf, const char *name, int namlen, loff_t offset, u64 ino, unsigned d_type) {
	struct readdir_file *entry;
	struct opendir_context *ctx = (struct opendir_context *)buf;
	struct hepunion_sb_info *context = ctx->context;
	char *complete_path = kmalloc(PATH_MAX, GFP_KERNEL);
	char *path;
	size_t len;

	pr_info("read_rw_branch: %p, %s, %d, %llx, %llx, %d\n", buf, name, namlen, offset, ino, d_type);

#if 0
	/* Skip special */
	if (is_special(name, namlen)) {
		return 0;
	}
#endif

	/* Ignore metadata */
	if (is_me(name, namlen)) {
		return 0;
	}

	/* Handle whiteouts */
	if (is_whiteout(name, namlen)) {
		/* Just work if there's a RO branch */
		if (ctx->ro_len) {
			/* Fix name len. Don't take .wh. into account
			 * It will be removed
			 * Prefix isn't mandatory, since context makes it obvious
			 */
			namlen -= 4; /* strlen(".wh."); */

			/* Allocate a list big enough to contain data and null terminated name */
			entry = kmalloc(sizeof(struct readdir_file) + (namlen + 1) * sizeof(char), GFP_KERNEL);
			if (!entry) {
				return -ENOMEM;
			}

			/* Add it to list */
			list_add(&entry->files_entry, &ctx->whiteouts_head);

			/* Fill in data */
			entry->d_reclen = namlen;
			strncpy(entry->d_name, name + 4, namlen);
			entry->d_name[namlen] = '\0';
		}
	}
	else {
		/* This is a normal entry
		 * Just add it to the list
		 */
		entry = kmalloc(sizeof(struct readdir_file) + namlen + sizeof(char), GFP_KERNEL);
		if (!entry) {
			return -ENOMEM;
		}

		/* Add it to list */
		list_add(&entry->files_entry, &ctx->files_head);

		/* Fill in data */
		entry->d_reclen = namlen;
		entry->type = d_type;
		strncpy(entry->d_name, name, namlen);
		entry->d_name[namlen] = '\0';

		/* Get its ino */
		path = (char *)(ctx->rw_off + (unsigned long)ctx);
		len = ctx->rw_len - context->rw_len;
		if (len + namlen + 1 > PATH_MAX) {
			return -ENAMETOOLONG;
		}
		memcpy(complete_path, path + context->rw_len, len);
		memcpy(complete_path + len, name, namlen);
		complete_path[len + namlen] = '\0';

		entry->ino = name_to_ino(complete_path);
	}
        kfree(complete_path);
	return 0;
}

static int read_ro_branch(void *buf, const char *name, int namlen, loff_t offset, u64 ino, unsigned d_type) {
	struct opendir_context *ctx = (struct opendir_context *)buf;
	struct hepunion_sb_info *context = ctx->context;
	char *complete_path = kmalloc(PATH_MAX, GFP_KERNEL);
	char *path;
	size_t len;
	struct readdir_file *entry;
	struct list_head *lentry;

	pr_info("read_ro_branch: %p, %s, %d, %llx, %llx, %d\n", buf, name, namlen, offset, ino, d_type);

#if 0
	/* Skip special */
	if (is_special(name, namlen)) {
		return 0;
	}
#endif

	/* Check if there is any matching whiteout */
	lentry = ctx->whiteouts_head.next;
	while (lentry != &ctx->whiteouts_head) {
		entry = list_entry(lentry, struct readdir_file, files_entry);
		if (namlen == entry->d_reclen &&
			strncmp(name, entry->d_name, namlen) == 0) {
			/* There's a whiteout, forget the entry */
			return 0;
		}

		lentry = lentry->next;
	}

	/* Check if it matches a RW entry */
	lentry = ctx->files_head.next;
	while (lentry != &ctx->files_head) {
		entry = list_entry(lentry, struct readdir_file, files_entry);
		if (namlen == entry->d_reclen &&
			strncmp(name, entry->d_name, namlen) == 0) {
			/* There's a RW entry, forget the entry */
			return 0;
		}

		lentry = lentry->next;
	}

	/* Finally, add the entry in list */
	entry = kmalloc(sizeof(struct readdir_file) + namlen + sizeof(char), GFP_KERNEL);
	if (!entry) {
		return -ENOMEM;
	}

	/* Add it to list */
	list_add(&entry->files_entry, &ctx->files_head);

	/* Fill in data */
	entry->d_reclen = namlen;
	entry->type = d_type;
	strncpy(entry->d_name, name, namlen);
	entry->d_name[namlen] = '\0';

	/* Get its ino */
	path = (char *)(ctx->ro_off + (unsigned long)ctx);
	len = ctx->ro_len - context->ro_len;
	if (len + namlen + 1 > PATH_MAX) {
		return -ENAMETOOLONG;
	}
	memcpy(complete_path, path + context->ro_len, len);
	memcpy(complete_path + len, name, namlen);
	complete_path[len + namlen] = '\0';

	entry->ino = name_to_ino(complete_path);
        kfree(complete_path); 
	return 0;
}

static int hepunion_readdir(struct file *filp, void *dirent, filldir_t filldir) {
	int err = 0;
	int i = 0;
	struct readdir_file *entry;
	struct list_head *lentry;
	struct opendir_context *ctx = (struct opendir_context *)filp->private_data;
#ifdef _DEBUG_
	struct hepunion_sb_info *context = ctx->context;
#endif

	pr_info("hepunion_readdir: %p, %p, %p\n", filp, dirent, filldir);

	if (list_empty(&ctx->files_head)) {
		/* Here fun begins.... */
		struct file *rw_dir;
		struct file *ro_dir;

		/* Check if there is an associated RW dir */
		if (ctx->rw_len) {
			char *rw_dir_path = (char *)(ctx->rw_off + (unsigned long)ctx);

			/* Start browsing RW dir */
			rw_dir = open_worker(rw_dir_path, context, O_RDONLY);
			if (IS_ERR(rw_dir)) {
				err = PTR_ERR(rw_dir);
				goto cleanup;
			}

			err = vfs_readdir(rw_dir, read_rw_branch, ctx);
			filp_close(rw_dir, NULL);

			if (err < 0) {
				goto cleanup;
			}
		}

		/* Work on RO branch */
		if (ctx->ro_len) {
			char *ro_dir_path = (char *)(ctx->ro_off + (unsigned long)ctx);

			/* Start browsing RO dir */
			ro_dir = open_worker(ro_dir_path, context, O_RDONLY);
			if (IS_ERR(ro_dir)) {
				err = PTR_ERR(ro_dir);
				goto cleanup;
			}

			err = vfs_readdir(ro_dir, read_ro_branch, ctx);
			filp_close(ro_dir, NULL);

			if (err < 0) {
				goto cleanup;
			}
		}

		/* Now we have files list, clean whiteouts */
		while (!list_empty(&ctx->whiteouts_head)) {
			entry = list_entry(ctx->whiteouts_head.next, struct readdir_file, files_entry);
			list_del(&entry->files_entry);
			kfree(entry);
		}
	}

	/* Reset error */
	err = 0;

	pr_info("Looking for entry: %lld\n", filp->f_pos);

	/* Try to find the requested entry now */
	lentry = ctx->files_head.next;
	while (lentry != &ctx->files_head) {
		/* Found the entry - return it */
		if (i == filp->f_pos) {
			entry = list_entry(lentry, struct readdir_file, files_entry);
			pr_info("Found: %s\n", entry->d_name);
			filldir(dirent, entry->d_name, entry->d_reclen, i, entry->ino, entry->type);
			/* Update position */
			++filp->f_pos;
			break;
		}

		++i;
		lentry = lentry->next;
	}

cleanup:
	/* There was an error, clean everything */
	if (err < 0) {
		while (!list_empty(&ctx->whiteouts_head)) {
			entry = list_entry(ctx->whiteouts_head.next, struct readdir_file, files_entry);
			list_del(&entry->files_entry);
			kfree(entry);
		}

		while (!list_empty(&ctx->files_head)) {
			entry = list_entry(ctx->files_head.next, struct readdir_file, files_entry);
			list_del(&entry->files_entry);
			kfree(entry);
		}
	}

	return err;
}

static int hepunion_revalidate(struct dentry *dentry, struct nameidata *nd) {
	pr_info("hepunion_revalidate: %p, %p\n", dentry, nd);

	if (dentry->d_inode == NULL) {
		return 0;
	}

	return 1;
}

static int hepunion_rmdir(struct inode *dir, struct dentry *dentry) {
	int err;
	struct kstat kstbuf;
	char *me_path = kmalloc(PATH_MAX, GFP_KERNEL);
	char *wh_path = kmalloc(PATH_MAX, GFP_KERNEL);
	char *ro_path = kmalloc(PATH_MAX, GFP_KERNEL);
        char has_ro = 0;
	struct hepunion_sb_info *context = get_context_i(dir);
	char *path = context->global1;
	char *real_path = context->global2;

	pr_info("hepunion_rmdir: %p, %p\n", dir, dentry);

	will_use_buffers(context);
	validate_inode(dir);
	validate_dentry(dentry);

	/* Try to find the dir first */
	err = get_relative_path_for_file(dir, dentry, context, path, 1);
	if (err < 0) {
		release_buffers(context);
		return err;
	}

	/* Then, find dir */
	err = find_file(path, real_path, context, 0);
	switch (err) {
		int has_me = 0;
		/* On RW, just remove it */
		case READ_WRITE_COPYUP: // Can't happen
		case READ_WRITE:
			/* Check if RO exists */
			if (find_file(path, ro_path, context, MUST_READ_ONLY) >= 0) {
				has_ro = 1;
			}

			/* Check if user can remove dir */
			err = can_remove(path, real_path, context);
			if (err < 0) {
				break;
			}

			/* If RO is present, check for emptyness */
			err = is_empty_dir(path, (has_ro ? ro_path : NULL), real_path, context);
			if (err < 0) {
				err = -ENOTEMPTY;
				break;
			}

			/* If with have RO, first create whiteout */
			if (has_ro && create_whiteout(path, wh_path, context) < 0) {
				break;
			}

			/* Remove dir */
			err = rmdir(real_path, context);
			if (err < 0) {
				unlink(wh_path, context);
			}
			break;

		/* On RO, create a whiteout */
		case READ_ONLY:
			/* Check if user can remove dir */
			if (!can_remove(path, real_path, context)) {
				break;
			}

			/* Check for directory emptyness */
			err = is_empty_dir(path, real_path, NULL, context);
			if (err < 0) {
				err = -ENOTEMPTY;
				break;
			}

			/* Get me first */
			if (find_me(path, context, me_path, &kstbuf) >= 0) {
				has_me = 1;
				/* Unlink it */
				err = unlink(me_path, context);
				if (err < 0) {
					break;
				}
			}

			/* Now, create whiteout */
			err = create_whiteout(path, wh_path, context);
			if (err < 0 && has_me) {
				create_me(me_path, &kstbuf, context);
			}
			break;

		default:
			/* Nothing to do */
			break;
	}

	release_buffers(context);
        kfree(me_path); 
	kfree(wh_path); 
	kfree(ro_path); 
	return err;
}

static int hepunion_setattr(struct dentry *dentry, struct iattr *attr) {
	int err;
	struct dentry *real_dentry;
	struct hepunion_sb_info *context = get_context_d(dentry);
	char *path = context->global1;
	char *real_path = context->global2;

	pr_info("hepunion_setattr: %p, %p\n", dentry, attr);

	will_use_buffers(context);
	validate_dentry(dentry);

	/* Get path */
	err = get_relative_path(NULL, dentry, context, path, 1);
	if (err) {
		release_buffers(context);
		return err;
	}

	/* Get file */
	err = find_file(path, real_path, context, 0);
	if (err < 0) {
		release_buffers(context);
		return err;
	}

	if (err == READ_WRITE || err == READ_WRITE_COPYUP) {
		/* Get dentry for the file to update */
		real_dentry = get_path_dentry(real_path, context, LOOKUP_REVAL);
		if (IS_ERR(real_dentry)) {
			release_buffers(context);
			return PTR_ERR(real_dentry);
		}

		/* Just update file attributes */
		push_root();
		err = notify_change(real_dentry, attr);
		pop_root();
		dput(real_dentry);

		release_buffers(context);
		return err;
    }

	/* Update me
	 * Don't clear flags, set_me_worker will do
	 * So, only call the worker
	 */
	err = set_me_worker(path, real_path, attr, context);

	release_buffers(context);
	return err;
}

static int hepunion_symlink(struct inode *dir, struct dentry *dentry, const char *symname) {
	/* Create the link on the RW branch */
	int err;
	struct hepunion_sb_info *context = get_context_i(dir);
	char *to = context->global1;
	char *real_to = context->global2;

	pr_info("hepunion_symlink: %p, %p, %s\n", dir, dentry, symname);

	will_use_buffers(context);
	validate_inode(dir);
	validate_dentry(dentry);

	/* Find destination */
	err = get_relative_path_for_file(dir, dentry, context, to, 1);
	if (err < 0) {
		release_buffers(context);
		return err;
	}

	/* And ensure it doesn't exist */
	err = find_file(to, real_to, context, 0);
	if (err >= 0) {
		release_buffers(context);
		return -EEXIST;
	}

	/* Get full path for destination */
	if (make_rw_path(to, real_to) > PATH_MAX) {
		release_buffers(context);
		return -ENAMETOOLONG;
	}

	/* Check access */
	err = can_create(to, real_to, context);
	if (err < 0) {
		release_buffers(context);
		return err;
	}

	/* Create path if needed */
	err = find_path(to, real_to, context);
	if (err < 0) {
		release_buffers(context);
		return err;
	}

	/* Now it's sure the link does not exist, create it */
	err = symlink_worker(symname, real_to, context);
	if (err < 0) {
		release_buffers(context);
		return err;
	}

	/* Remove possible whiteout */
	unlink_whiteout(to, context);

	release_buffers(context);
	return 0;
}

/* used by df to show it up */
static int hepunion_statfs(struct dentry *dentry, struct kstatfs *buf) {
	struct super_block *sb = dentry->d_sb;
	struct hepunion_sb_info * sb_info = sb->s_fs_info;
	struct file *filp;
	int err;

	pr_info("hepunion_statfs: %p, %p\n", dentry, buf);

	validate_dentry(dentry);

	memset(buf, 0, sizeof(*buf));

	/* First, get RO data */
	filp = filp_open(sb_info->read_only_branch, O_RDONLY, 0);
	if (IS_ERR(filp)) {
		pr_err("Failed opening RO branch!\n");
		return PTR_ERR(filp);
	}

	err = vfs_statfs(&filp->f_path, buf);
	filp_close(filp, NULL);

	if (unlikely(err)) {
		return err;
	}

	/* Return them, but ensure we mark our stuff */
	buf->f_type = sb->s_magic;
	buf->f_fsid.val[0] = (u32)HEPUNION_SEED;
	buf->f_fsid.val[1] = (u32)(HEPUNION_SEED >> 32);

	return 0;
}

static int hepunion_unlink(struct inode *dir, struct dentry *dentry) {
	int err;
	struct hepunion_sb_info *context = get_context_i(dir);
	char *path = context->global1;
	char *real_path = context->global2;
	struct kstat kstbuf;
	char *me_path = kmalloc(PATH_MAX, GFP_KERNEL);//dynamic array to solve stack problem
        char *wh_path = kmalloc(PATH_MAX, GFP_KERNEL);//dynamic array to solve stack problem        

	pr_info("hepunion_unlink: %p, %p\n", dir, dentry);

	will_use_buffers(context);
	validate_inode(dir);
	validate_dentry(dentry);

	/* Try to find the file first */
	err = get_relative_path_for_file(dir, dentry, context, path, 1);
	if (err < 0) {
		release_buffers(context);
		return err;
	}

	/* Then, find file */
	err = find_file(path, real_path, context, 0);
	switch (err) {
		int has_me = 0;
		/* On RW, just remove it */
		case READ_WRITE_COPYUP: /* Can't happen */
		case READ_WRITE:
			err = unlink_rw_file(path, real_path, context, 0);
			break;

		/* On RO, create a whiteout */
		case READ_ONLY:
			/* Check if user can unlink file */
			err = can_remove(path, real_path, context);
			if (err < 0) {
				break;
			}

			/* Get me first */
			if (find_me(path, context, me_path, &kstbuf) >= 0) {
				has_me = 1;
				/* Delete it */
				err = unlink(me_path, context);
				if (err < 0) {
					break;
				}
			}

			/* Now, create whiteout */
			err = create_whiteout(path, wh_path, context);
			if (err < 0 && has_me) {
				create_me(me_path, &kstbuf, context);
			}
			break;

		default:
			/* Nothing to do */
			break;
	}

	/* Kill the inode now */
	if (err == 0) {
		dir->__i_nlink--;
		mark_inode_dirty(dir);
        dentry->d_inode->__i_nlink--;
        mark_inode_dirty(dentry->d_inode);
	}

	release_buffers(context); 
        kfree(me_path);
        kfree(wh_path);       
	return err;
}

static ssize_t hepunion_write(struct file *file, const char __user *buf, size_t count, loff_t *offset) {
	struct file *real_file = (struct file *)file->private_data;
	ssize_t ret;

	pr_info("hepunion_write: %p, %p, %zu, %p(%llx)\n", file, buf, count, offset, *offset);

	ret = vfs_write(real_file, buf, count, offset);
	file->f_pos = real_file->f_pos;

	return ret;
}

static int hepunion_readpage(struct file *file, struct page *page)
{
	/*called by generic_read happens in fops_read operation*/
        /*we are instead calling vfs_read which calls do_sync_read*/
        /*so wont be called*/
        pr_info("hepunion_readpage\n");
	return 1;
}

static int hepunion_writepage(struct page *page, struct writeback_control *wbc)
{
	/*only by when generic_write happens in fops_write operation*/
        /*we are instead calling vfs_write which calls do_sync_write*/
        /*so wont be called*/
        pr_info("hepunion_writepage\n");
	return 1;
}

static void hepunion_put_super(struct super_block *sb)
{
       /* this function used for umounting the fs*/	
       struct hepunion_sb_info * sb_info = sb->s_fs_info; 
	
        pr_info("hepunion_put_super\n");
	if (sb_info)
	{
		kfree(sb_info);
		sb->s_fs_info = NULL;
	}
}


static int hepunion_update(struct hepunion_sb_info *info, int vfs_ino, int *size, int *timestamp, int *perms)
{
//      /*need to implemnt hepunion_read_file_entry andhepunion_write_file_entry*/
//	int i;
//	int retval;

//      /*need to figure out what is the structure for file entry*/
//      /*suppose it is file_entry*/

//	if ((retval = hepunion_read_file_entry(info, vfs_ino, &file_entry)) < 0)
//	{
//		return retval;
//	}

//      /*update the changed parameters at the read inode*/	
        return 1;//return hepunion_write_file_entry(info, vfs_ino, &fe);

}


static int hepunion_write_inode(struct inode *inode, struct writeback_control *wbc)
{
        /*function will enable changing the meta data of file.*/
        /*useful for operations like chown, chmod*/
        /*At present it is generic, need to implement it only for RW branch*/
        struct hepunion_sb_info * sb_info = inode->i_sb->s_fs_info;
        int size, timestamp, perms;

	pr_info("hepunion_write_inode (i_ino = %ld)\n", inode->i_ino);

	if (!(S_ISREG(inode->i_mode))) // checking for regular files
		return 0;

	size = i_size_read(inode);
	timestamp = inode->i_mtime.tv_sec > inode->i_ctime.tv_sec ? inode->i_mtime.tv_sec : inode->i_ctime.tv_sec;
	perms = 0;
	perms |= (inode->i_mode & (S_IRUSR | S_IRGRP | S_IROTH)) ? 4 : 0;
	perms |= (inode->i_mode & (S_IWUSR | S_IWGRP | S_IWOTH)) ? 2 : 0;
	perms |= (inode->i_mode & (S_IXUSR | S_IXGRP | S_IXOTH)) ? 1 : 0;

	pr_info(" Writing inode with %d bytes @ %d secs w/ %o\n", size, timestamp, perms);
        return hepunion_update(sb_info, inode->i_ino, &size, &timestamp, &perms); // TODO: Fill up the dfs_update
        return 1;
}

struct inode_operations hepunion_iops = {
	.getattr	= hepunion_getattr,
	.permission	= hepunion_permission,
#if 0
	.readlink	= generic_readlink, /* dentry will already point on the right file */
#endif
	.setattr	= hepunion_setattr
};

struct inode_operations hepunion_dir_iops = {
	.create		= hepunion_create,
	.getattr	= hepunion_getattr,
	.link		= hepunion_link,
	.lookup		= hepunion_lookup,
	.mkdir		= hepunion_mkdir,
	.mknod		= hepunion_mknod,
	.permission	= hepunion_permission,
	.rmdir		= hepunion_rmdir,
	.setattr	= hepunion_setattr,
	.symlink	= hepunion_symlink,
	.unlink		= hepunion_unlink
};

struct super_operations hepunion_sops = {
	//.read_inode	= hepunion_read_inode,//system-call no longer supported
	.statfs		= hepunion_statfs,
        .put_super      = hepunion_put_super,
	.write_inode    = hepunion_write_inode
};

struct dentry_operations hepunion_dops = {
	.d_revalidate	= hepunion_revalidate
};

struct file_operations hepunion_fops = {
	.llseek		= hepunion_llseek,
	.open		= hepunion_open,
	.read		= hepunion_read,
	//.readv		= hepunion_readv,//system-call no longer supported
	.release	= hepunion_close,
	.write		= hepunion_write
	//.writev		= hepunion_writev,//system-call no longer supported
};

struct file_operations hepunion_dir_fops = {
	.open		= hepunion_opendir,
	.readdir	= hepunion_readdir,
	.release	= hepunion_closedir
};

struct address_space_operations hepunion_aops =
{
	.readpage = hepunion_readpage,
	.writepage = hepunion_writepage

};