/* $Id$
 *
 * isync - IMAP4 to maildir mailbox synchronizer
 * Copyright (C) 2000-1 Michael R. Elkins <me@mutt.org>
 *
 *  This program is free software; you can redistribute it and/or modify
 *  it under the terms of the GNU General Public License as published by
 *  the Free Software Foundation; either version 2 of the License, or
 *  (at your option) any later version.
 *
 *  This program is distributed in the hope that it will be useful,
 *  but WITHOUT ANY WARRANTY; without even the implied warranty of
 *  MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 *  GNU General Public License for more details.
 *
 *  You should have received a copy of the GNU General Public License
 *  along with this program; if not, write to the Free Software
 *  Foundation, Inc., 59 Temple Place, Suite 330, Boston, MA  02111-1307  USA
 */

#include <stdio.h>
#include <limits.h>
#include <stdlib.h>
#include <unistd.h>
#include <time.h>
#include <fcntl.h>
#include <string.h>
#include <errno.h>
#include <sys/stat.h>
#include "isync.h"

static unsigned int MaildirCount = 0;

message_t *
find_msg (message_t * list, unsigned int uid)
{
    for (; list; list = list->next)
	if (list->uid == uid)
	    return list;
    return 0;
}

int
sync_mailbox (mailbox_t * mbox, imap_t * imap, int flags,
	      unsigned int max_size)
{
    message_t *cur;
    message_t *tmp;
    char path[_POSIX_PATH_MAX];
    char newpath[_POSIX_PATH_MAX];
    char suffix[_POSIX_PATH_MAX];
    char *p;
    int fd;
    int ret;
    int fetched = 0;

    if (mbox->uidvalidity > 0)
    {
	if (mbox->uidvalidity != imap->uidvalidity)
	{
	    /* if the UIDVALIDITY value has changed, it means all our
	     * local UIDs are invalid, so we can't sync.
	     */
	    puts ("Error, UIDVALIDITY changed on server (fatal)");
	    return -1;
	}
    }
    else if (maildir_set_uidvalidity (mbox, imap->uidvalidity))
    {
	puts ("Error, unable to store UIDVALIDITY");
	return -1;
    }

    if (mbox->maxuid == 0 || imap->maxuid > mbox->maxuid)
    {
	mbox->maxuid = imap->maxuid;
	mbox->maxuidchanged = 1;
    }

    /* if we are --fast mode, the mailbox wont have been loaded, so
     * this next step is skipped.
     */
    for (cur = mbox->msgs; cur; cur = cur->next)
    {
	tmp = find_msg (imap->msgs, cur->uid);
	if (!tmp)
	{
	    /* if this message wasn't fetched from the server, attempt to
	     * upload it
	     */
	    if (cur->uid == (unsigned int) -1)
	    {
		struct stat sb;
		int fd;
		int uid;

		/* upload the message if its not too big */
		snprintf (path, sizeof (path), "%s/%s/%s", mbox->path,
			  cur->new ? "new" : "cur", cur->file);
		if (stat (path, &sb))
		{
		    printf ("Error, unable to stat %s: %s (errno %d)\n",
			    path, strerror (errno), errno);

		    continue;	/* not fatal */
		}
		if (sb.st_size > imap->box->max_size)
		{
		    printf
			("Warning, local message is too large (%ld), skipping...\n",
			 sb.st_size);
		    continue;
		}
		fd = open (path, O_RDONLY);
		if (fd == -1)
		{
		    printf ("Error, unable to open %s: %s (errno %d)\n",
			    path, strerror (errno), errno);
		    continue;
		}

		cur->size = sb.st_size;

		uid = imap_append_message (imap, fd, cur);

		close (fd);

		/* if the server gave us back a uid, rename the file so
		 * we remember for next time
		 */
		if (uid != -1)
		{
		    char newpath[_POSIX_PATH_MAX];
		    char *p;

		    strfcpy (newpath, path, sizeof (newpath));
		    /* kill :info field */
		    p = strchr (newpath, ':');
		    if (p)
			*p = 0;

		    /* XXX not quite right, should really always put the
		     * msg in "cur/", but i'm too tired right now.
		     */
		    snprintf (newpath + strlen (newpath),
			      sizeof (newpath) - strlen (newpath),
			      ",U=%d:2,%s%s%s%s", uid,
			      (cur->flags & D_FLAGGED) ? "F" : "",
			      (cur->flags & D_ANSWERED) ? "R" : "",
			      (cur->flags & D_SEEN) ? "S" : "",
			      (cur->flags & D_DELETED) ? "T" : "");
		    if (rename (path, newpath))
			perror ("rename");
		}
	    }
	    else
	    {
		printf ("Warning, uid %u doesn't exist on server\n",
			cur->uid);
		if (flags & SYNC_DELETE)
		{
		    cur->flags |= D_DELETED;
		    cur->dead = 1;
		    mbox->deleted++;
		}
	    }
	    continue;
	}
	tmp->processed = 1;

	/* if the message is deleted, and CopyDeletedTo is set, and we
	 * are expunging, make a copy of the message now.
	 */
	if (((cur->flags | tmp->flags) & D_DELETED) != 0 &&
	    (flags & SYNC_EXPUNGE) && imap->box->copy_deleted_to)
	{
	    if (imap_copy_message (imap, cur->uid,
				   imap->box->copy_deleted_to))
	    {
		printf ("Error, unable to copy deleted message to \"%s\"\n",
			imap->box->copy_deleted_to);
		return -1;
	    }
	}

	/* check if local flags are different from server flags.
	 * ignore \Recent and \Draft
	 */
	if (cur->flags != (tmp->flags & ~(D_RECENT | D_DRAFT)))
	{
	    /* set local flags that don't exist on the server */
	    if (!(tmp->flags & D_DELETED) && (cur->flags & D_DELETED))
		imap->deleted++;

	    imap_set_flags (imap, cur->uid, cur->flags & ~tmp->flags);

	    /* update local flags */
	    if ((cur->flags & D_DELETED) == 0 && (tmp->flags & D_DELETED))
		mbox->deleted++;
	    cur->flags |= (tmp->flags & ~(D_RECENT | D_DRAFT));
	    cur->changed = 1;
	    mbox->changed = 1;
	}
    }

    fputs ("Fetching new messages", stdout);
    fflush (stdout);
    for (cur = imap->msgs; cur; cur = cur->next)
    {
	if (!cur->processed)
	{
	    /* new message on server */

	    if ((flags & SYNC_EXPUNGE) && (cur->flags & D_DELETED))
	    {
		/* this message has been marked for deletion and
		 * we are currently expunging a mailbox.  don't
		 * bother downloading this message
		 */
		continue;
	    }

	    if (max_size && cur->size > max_size)
	    {
		printf
		    ("Warning, message skipped because it is too big (%u)\n",
		     cur->size);
		continue;
	    }

	    /* construct the flags part of the file name. */

	    *suffix = 0;
	    if (cur->flags)
	    {
		snprintf (suffix, sizeof (suffix), ":2,%s%s%s%s",
			  (cur->flags & D_FLAGGED) ? "F" : "",
			  (cur->flags & D_ANSWERED) ? "R" : "",
			  (cur->flags & D_SEEN) ? "S" : "",
			  (cur->flags & D_DELETED) ? "T" : "");
	    }

	    for (;;)
	    {
		/* create new file */
		snprintf (path, sizeof (path), "%s/tmp/%s.%ld_%d.%d,U=%d%s",
			  mbox->path, Hostname, time (0), MaildirCount++,
			  getpid (), cur->uid, suffix);

		if ((fd = open (path, O_WRONLY | O_CREAT | O_EXCL, 0600)) > 0)
		    break;
		if (errno != EEXIST)
		{
		    perror ("open");
		    break;
		}

		sleep (2);
	    }

	    if (fd < 0)
		continue;

	    /* give some visual feedback that something is happening */
	    fputs (".", stdout);
	    fflush (stdout);
	    fetched++;

	    ret = imap_fetch_message (imap, cur->uid, fd);

	    if (close (fd))
		perror ("close");
	    else if (!ret)
	    {
		p = strrchr (path, '/');

		snprintf (newpath, sizeof (newpath), "%s/%s%s", mbox->path,
			  cur->flags ? "cur" : "new", p);

		/* its ok if this fails, the next time we sync the message
		 * will get pulled down
		 */
		if (link (path, newpath))
		    perror ("link");
	    }

	    /* always remove the temp file */
	    unlink (path);
	}
    }
    printf ("  %d messages\n", fetched);

    return 0;
}