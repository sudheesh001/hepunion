/**
 * \file me.c
 * \brief Metadata (ME) support for the HEPunion file system
 * \author Pierre Schweitzer <pierre.jean.schweitzer@cern.ch>
 * \version 1.0
 * \date 10-Dec-2011
 * \copyright GNU General Public License - GPL
 *
 * Metadata support in HEPunion file system is different that
 * in the other union file systems.
 *
 * Here, a clear difference is made between data and metadata.
 * This is why the concept of metadata support has been added
 * to this file system. It clearly mirors the idea of COW (
 * read cow.c header) but adapts it to the metadata of a file
 * or even a directory.
 *
 * That way, when an attempt to modify a file metadata is made
 * (owner, time or mode), instead of copying the whole file,
 * a copyup of its metadata is made in a separate file. This
 * contains no data, it just carries the metadata.
 *
 * In order to make this possible, deported metadata are made
 * of a file called .me.{original file} which is at the same
 * place than the original file, but on read-write branch.
 * This mechanism is of course not used when the file is on the
 * read-write branch.
 *
 * This also means that if a metadata file is first created,
 * and then a copyup is done, the metadata file will be deleted
 * and its contents merged to the copyup file.
 *
 * On the other hand, on copyup deletion when original file
 * still exists, a metadata file will be recreated.
 * .me. files don't appear during files listing (thanks to
 * unioning).
 *
 * Metadata handling present some particularities since we there
 * is a need to merge some metadata instead of just using metadata
 * file. Indeed, since you can change mode for every object on the
 * system, but metadata is always a simple file, there is a need
 * to merge mode than can be modified with metadata files and
 * non alterable metadata.
 */

#include "hepunion.h"

int create_me(const char *me_path, struct kstat *kstbuf, struct hepunion_sb_info *context) {
	int err;
	struct file *fd;
	struct iattr attr;

	/* Get creation modes */
	umode_t mode = kstbuf->mode;

	pr_info("create_me: %s, %p, %p\n", me_path, kstbuf, context);

	clear_mode_flags(mode);

	/* Create file */
	fd = creat_worker(me_path, context, mode);
	if (IS_ERR(fd)) {
		return PTR_ERR(fd);
	}

	attr.ia_valid = ATTR_MODE | ATTR_UID | ATTR_GID | ATTR_ATIME | ATTR_MTIME;
	attr.ia_mode = kstbuf->mode;
	attr.ia_uid = kstbuf->uid;
	attr.ia_gid = kstbuf->gid;
	attr.ia_size = 0;
	attr.ia_atime = kstbuf->atime;
	attr.ia_mtime = kstbuf->mtime;
	attr.ia_ctime = kstbuf->ctime;

	/* Set all the attributes */
	push_root();
	err = notify_change(fd->f_dentry, &attr);
	filp_close(fd, NULL);
	pop_root();

	return err;
}

int find_me(const char *path, struct hepunion_sb_info *context, char *me_path, struct kstat *kstbuf) {
	int err;

	pr_info("find_me: %s, %p, %p, %p\n", path, context, me_path, kstbuf);

	/* Get me path */
	err = path_to_special(path, ME, context, me_path);
	if (err < 0) {
		return err;
	}

	/* Now, try to get properties */
	err = lstat(me_path, context, kstbuf);

	return err;
}

int get_file_attr(const char *path, struct hepunion_sb_info *context, struct kstat *kstbuf) {
	char *real_path;
	int err;
	pr_info("get_file_attr: %s, %p, %p\n", path, context, kstbuf);

	real_path = kmalloc(PATH_MAX, GFP_KERNEL);
	if (!real_path) {
		return -ENOMEM;
	}

	/* First, find file */
	err = find_file(path, real_path, context, 0);
	if (err < 0) {
		kfree(real_path);
		return err;
	}

	/* Call worker */
	err = get_file_attr_worker(path, real_path, context, kstbuf);
	kfree(real_path);
	return err;
}

int get_file_attr_worker(const char *path, const char *real_path, struct hepunion_sb_info *context, struct kstat *kstbuf) {
	int err;
	char me;
	struct kstat kstme;
	char *me_file;

	pr_info("get_file_attr_worker: %s, %s, %p, %p\n", path, real_path, context, kstbuf);

	me_file = kmalloc(PATH_MAX, GFP_KERNEL);//dynamic array to solve stack problem
	if (!me_file) {
		return -ENOMEM;
	}

	/* Look for a me file */
	me = (find_me(path, context, me_file, &kstme) >= 0);

	pr_info("me file status: %d\n", me);

	/* We don't need its name info about */
	kfree(me_file);

	/* Get attributes */
	err = lstat(real_path, context, kstbuf);
	if (err < 0) {
		return err;
	}

	/* If me file was present, merge results */
	if (me) {
		kstbuf->uid = kstme.uid;
		kstbuf->gid = kstme.gid;
		kstbuf->atime = kstme.atime;
		kstbuf->mtime = kstme.mtime;
		kstbuf->ctime = kstme.ctime;
		/* Now we need to merge modes */
		/* First clean .me. modes */
		kstme.mode = clear_mode_flags(kstme.mode);
		/* Then, clean real file modes */
		kstbuf->mode &= ~VALID_MODES_MASK;
		/* Finally, apply .me. modes */
		kstbuf->mode |= kstme.mode;
	}

	return 0;
}

int set_me(const char *path, const char *real_path, struct kstat *kstbuf, struct hepunion_sb_info *context, int flags) {
	struct iattr attr;

	pr_info("set_me: %s, %s, %p, %p, %x\n", path, real_path, kstbuf, context, flags);

	/* Convert the kstbuf to a iattr struct */
	attr.ia_valid = 0;
	attr.ia_mode = kstbuf->mode;
	attr.ia_atime = kstbuf->atime;
	attr.ia_mtime = kstbuf->mtime;
	attr.ia_uid = kstbuf->uid;
	attr.ia_gid = kstbuf->gid;

	if (is_flag_set(flags, MODE)) {
		attr.ia_valid |= ATTR_MODE;
	}

	if (is_flag_set(flags, TIME)) {
		attr.ia_valid |= ATTR_ATIME | ATTR_MTIME;
	}

	if (is_flag_set(flags, OWNER)) {
		attr.ia_valid |= ATTR_UID | ATTR_GID;
	}

	/* Call the real worker */
	return set_me_worker(path, real_path, &attr, context);
}

int set_me_worker(const char *path, const char *real_path, struct iattr *attr, struct hepunion_sb_info *context) {
	int err;
	char me;
	char *me_path;
	struct kstat kstme;
	struct file *fd;
	umode_t mode;

	pr_info("set_me: %s, %s, %p, %p\n", path, real_path, attr, context);

	/* Ensure input is correct */
	attr->ia_valid &= ATTR_UID | ATTR_GID | ATTR_ATIME | ATTR_MTIME | ATTR_MODE;

	me_path = kmalloc(PATH_MAX, GFP_KERNEL);//dynamic array to solve stack problem
	if (!me_path) {
		return -ENOMEM;
	}

	/* Look for a me file */
	me = (find_me(path, context, me_path, &kstme) >= 0);

	if (!me) {
		/* Read real file info */
		err = lstat(real_path, context, &kstme);
		if (err < 0) {
			goto cleanup;
		}

		/* Recreate path up to the .me. file */
		err = find_path(path, NULL, context);
		if (err < 0) {
			goto cleanup;
		}

		/* .me. does not exist, create it with appropriate mode */
		mode = (attr->ia_valid & ATTR_MODE) ? attr->ia_mode : kstme.mode;
		clear_mode_flags(mode);

		fd = creat_worker(me_path, context, mode);
		if (IS_ERR(fd)) {
			err = PTR_ERR(fd);
			goto cleanup;
		}

		/* Remove mode if it was set */
		attr->ia_valid &= ~ATTR_MODE;

		/* Set its time */
		if (!is_flag_set(attr->ia_valid, (ATTR_ATIME | ATTR_MTIME))) {
			attr->ia_atime = kstme.atime;
			attr->ia_mtime = kstme.mtime;
			attr->ia_valid |= ATTR_ATIME | ATTR_MTIME;
		}

		/* Set its owner */
		if (!is_flag_set(attr->ia_valid, (ATTR_UID | ATTR_GID))) {
			attr->ia_uid = kstme.uid;
			attr->ia_gid = kstme.gid;
			attr->ia_valid |= ATTR_UID | ATTR_GID;
		}

		push_root();
		err = notify_change(fd->f_dentry, attr);

		filp_close(fd, NULL);
		pop_root();
	}
	else {
		fd = open_worker(me_path, context, O_RDWR);
		if (IS_ERR(fd)) {
			err = PTR_ERR(fd);
			goto cleanup;
		}

		/* Only change if there are changes */
		if (attr->ia_valid) {
			push_root();
			err = notify_change(fd->f_dentry, attr);
			pop_root();
		}
		else {
			err = 0;
		}

		push_root();
		filp_close(fd, NULL);
		pop_root();
	}

cleanup:
	kfree(me_path); 
	return err;
}
