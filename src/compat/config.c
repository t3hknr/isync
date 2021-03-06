/*
 * isync - mbsync wrapper: IMAP4 to maildir mailbox synchronizer
 * Copyright (C) 2000-2002 Michael R. Elkins <me@mutt.org>
 * Copyright (C) 2002-2004 Oswald Buddenhagen <ossi@users.sf.net>
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
 *  along with this program.  If not, see <http://www.gnu.org/licenses/>.
 */

#include "isync.h"

#include <unistd.h>
#include <limits.h>
#include <errno.h>
#include <pwd.h>
#include <sys/types.h>
#include <string.h>
#include <stdlib.h>
#include <stdio.h>
#include <ctype.h>

static char *
my_strndup( const char *s, size_t nchars )
{
	char *r = nfmalloc( sizeof(char) * (nchars + 1) );
	memcpy( r, s, nchars );
	r[nchars] = 0;
	return r;
}

char *
expand_strdup( const char *s )
{
	struct passwd *pw;
	const char *p, *q;
	char *r;

	if (*s == '~') {
		s++;
		if (!*s) {
			p = 0;
			q = Home;
		} else if (*s == '/') {
			p = s + 1;
			q = Home;
		} else {
			if ((p = strchr( s, '/' ))) {
				r = my_strndup( s, (int)(p - s) );
				pw = getpwnam( r );
				free( r );
				p++;
			} else
				pw = getpwnam( s );
			if (!pw)
				return 0;
			q = pw->pw_dir;
		}
		nfasprintf( &r, "%s/%s", q, p ? p : "" );
		return r;
	} else if (*s != '/' && xmaildir) {
		nfasprintf( &r, "%s/%s", xmaildir, s );
		return r;
	} else
		return nfstrdup( s );
}

static int
is_true( const char *val )
{
	return
	    !strcasecmp( val, "yes" ) ||
	    !strcasecmp( val, "true" ) ||
	    !strcasecmp( val, "on" ) ||
	    !strcmp( val, "1" );
}

void
load_config( const char *path, config_t ***stor )
{
	config_t **sstor, *cfg;
	FILE *fp;
	char *p, *cmd, *val;
	int line = 0;
	char buf[1024];

	if (!(fp = fopen( path, "r" ))) {
		if (errno != ENOENT)
			sys_error( "Cannot read config file '%s'", path );
		return;
	}
	if (!Quiet && !Debug && !Verbose)
		printf( "Reading configuration file %s\n", path );
	buf[sizeof(buf) - 1] = 0;
	cfg = &global;
	while (fgets( buf, sizeof(buf) - 1, fp )) {
		p = buf;
		cmd = next_arg( &p );
		val = next_arg( &p );
		line++;
		if (!cmd || *cmd == '#')
			continue;
		if (!val) {
			fprintf( stderr, "%s:%d: parameter missing\n", path, line );
			continue;
		}
		if (!strcasecmp( "Mailbox", cmd )) {
			if (o2o)
				break;
			cfg = **stor = nfmalloc( sizeof(config_t) );
			*stor = &cfg->next;
			memcpy( cfg, &global, sizeof(config_t) );
			/* not expanded at this point */
			cfg->path = nfstrdup( val );
		} else if (!strcasecmp( "OneToOne", cmd )) {
			if (boxes) {
			  forbid:
				fprintf( stderr,
				         "%s:%d: keyword '%s' allowed only in global section\n",
				         path, line, cmd );
				continue;
			}
			o2o = is_true( val );
		} else if (!strcasecmp( "Maildir", cmd )) {
			if (boxes)
				goto forbid;
			maildir = nfstrdup( val );
			xmaildir = expand_strdup( val );
		} else if (!strcasecmp( "Folder", cmd )) {
			if (boxes)
				goto forbid;
			folder = nfstrdup( val );
		} else if (!strcasecmp( "Inbox", cmd )) {
			if (boxes)
				goto forbid;
			inbox = nfstrdup( val );
		} else if (!strcasecmp( "Host", cmd )) {
			if (starts_with( val, -1, "imaps:", 6 )) {
				val += 6;
				cfg->use_imaps = 1;
				cfg->port = 993;
				cfg->use_sslv3 = 1;
			}
			cfg->host = nfstrdup( val );
		} else if (!strcasecmp( "User", cmd ))
			cfg->user = nfstrdup( val );
		else if (!strcasecmp( "Pass", cmd ))
			cfg->pass = nfstrdup( val );
		else if (!strcasecmp ( "Port", cmd ))
			cfg->port = atoi( val );
		else if (!strcasecmp ( "Box", cmd ))
			cfg->box = nfstrdup( val );
		else if (!strcasecmp ( "Alias", cmd )) {
			if (!boxes) {
				fprintf( stderr,
				         "%s:%d: keyword 'Alias' allowed only in mailbox specification\n",
				         path, line );
				continue;
			}
			cfg->alias = nfstrdup( val );
		} else if (!strcasecmp( "MaxSize", cmd ))
			cfg->max_size = atol( val );
		else if (!strcasecmp ( "MaxMessages", cmd ))
			cfg->max_messages = atol( val );
		else if (!strcasecmp ( "UseNamespace", cmd ))
			cfg->use_namespace = is_true( val );
		else if (!strcasecmp ( "CopyDeletedTo", cmd ))
			cfg->copy_deleted_to = nfstrdup( val );
		else if (!strcasecmp ( "Tunnel", cmd ))
			cfg->tunnel = nfstrdup( val );
		else if (!strcasecmp ( "Expunge", cmd ))
			cfg->expunge = is_true( val );
		else if (!strcasecmp( "Delete", cmd ))
			cfg->delete = is_true( val );
		else if (!strcasecmp( "CertificateFile", cmd ))
			cfg->cert_file = expand_strdup( val );
		else if (!strcasecmp( "RequireSSL", cmd ))
			cfg->require_ssl = is_true( val );
		else if (!strcasecmp( "UseSSLv2", cmd ))
			fprintf( stderr, "Warning: UseSSLv2 is no longer supported\n" );
		else if (!strcasecmp( "UseSSLv3", cmd ))
			cfg->use_sslv3 = is_true( val );
		else if (!strcasecmp( "UseTLSv1", cmd ))
			cfg->use_tlsv1 = is_true( val );
		else if (!strcasecmp( "RequireCRAM", cmd ))
			cfg->require_cram = is_true( val );
		else if (buf[0])
			fprintf( stderr, "%s:%d: unknown keyword '%s'\n", path, line, cmd );
	}
	fclose( fp );
	if (o2o) {
		if (!global.host && !global.tunnel) {
			fprintf( stderr, "Neither Host nor Tunnel given to OneToOne. Aborting.\n" );
			exit( 1 );
		}
	} else
		for (sstor = &boxes; (cfg = *sstor); ) {
			if (!cfg->host && !cfg->tunnel) {
				fprintf( stderr, "Mailbox '%s' has neither Host nor Tunnel. Skipping.\n",
				         cfg->alias ? cfg->alias : cfg->path );
				if (&cfg->next == *stor)
					*stor = sstor;
				*sstor = cfg->next;
				continue;
			}
			sstor = &cfg->next;
		}
}

static const char *
tb( int on )
{
	return on ? "yes" : "no";
}

static const char *
quotify( const char *str )
{
	char *ostr;
	int i;

	for (i = 0; str[i]; i++) {
		if (isspace( str[i] )) {
			nfasprintf( &ostr, "\"%s\"", str );
			return ostr;
		}
	}
	return str;
}

static void
write_imap_server( FILE *fp, config_t *cfg )
{
	config_t *pbox;
	char *p, *p2;
	int hl, a1, a2, a3, a4;
	char buf[128], ubuf[64];
	static int tunnels;

	/* The old server names determine the derived store names. They are kinda stupid,
	 * but can't be changed, because store names are encoded in state file names. */
	if (cfg->tunnel)
		nfasprintf( (char **)&cfg->old_server_name, "tunnel%d", ++tunnels );
	else {
		if (sscanf( cfg->host, "%d.%d.%d.%d", &a1, &a2, &a3, &a4 ) == 4)
			hl = nfsnprintf( buf, sizeof(buf), "%s", cfg->host );
		else {
			/* XXX this does not avoid clashes. add port? */
			p = strrchr( cfg->host, '.' );
			if (!p)
				hl = nfsnprintf( buf, sizeof(buf), "%s", cfg->host );
			else {
				hl = nfsnprintf( buf, sizeof(buf), "%.*s", (int)(p - cfg->host), cfg->host );
				p2 = strrchr( buf, '.' );
				if (p2)
					hl = sprintf( buf, "%s", p2 + 1 );
			}
		}
		if (boxes) /* !o2o */
			for (pbox = boxes; pbox != cfg; pbox = pbox->next)
				if (equals( pbox->old_server_name, -1, buf, hl )) {
					nfasprintf( (char **)&cfg->old_server_name, "%s-%d", buf, ++pbox->old_servers );
					goto gotsrv;
				}
		cfg->old_server_name = nfstrdup( buf );
		cfg->old_servers = 1;
	  gotsrv: ;
	}

	/* The "new" server names determine the names of the accounts themselves.
	 * They are optimized for descriptiveness, e.g. in password prompts. */
	if (cfg->user)
		nfsnprintf( ubuf, sizeof(ubuf), "%s@", cfg->user );
	else
		ubuf[0] = 0;
	if (!cfg->host)
		hl = nfsnprintf( buf, sizeof(buf), "%stunnel", ubuf );
	else {
		if (cfg->port != (cfg->use_imaps ? 993 : 143))
			hl = nfsnprintf( buf, sizeof(buf), "%s%s_%d", ubuf, cfg->host, cfg->port );
		else
			hl = nfsnprintf( buf, sizeof(buf), "%s%s", ubuf, cfg->host );
	}
	if (boxes) /* !o2o */
		for (pbox = boxes; pbox != cfg; pbox = pbox->next)
			if (equals( pbox->server_name, -1, buf, hl )) {
				nfasprintf( (char **)&cfg->server_name, "%s-%d", buf, ++pbox->servers );
				goto ngotsrv;
			}
	cfg->server_name = nfstrdup( buf );
	cfg->servers = 1;
  ngotsrv: ;

	fprintf( fp, "IMAPAccount %s\n", cfg->server_name );
	if (cfg->tunnel)
		fprintf( fp, "Tunnel \"%s\"\n", cfg->tunnel );
	else {
		if (cfg->use_imaps)
			fprintf( fp, "Host imaps:%s\n", cfg->host );
		else
			fprintf( fp, "Host %s\n", cfg->host );
		fprintf( fp, "Port %d\n", cfg->port );
	}
	if (cfg->user)
		fprintf( fp, "User %s\n", quotify( cfg->user ) );
	if (cfg->pass)
		fprintf( fp, "Pass %s\n", quotify( cfg->pass ) );
	fprintf( fp, "RequireCRAM %s\nRequireSSL %s\n"
	             "UseSSLv3 %s\nUseTLSv1 %s\nUseTLSv1.1 %s\nUseTLSv1.2 %s\n",
	             tb(cfg->require_cram), tb(cfg->require_ssl),
	             tb(cfg->use_sslv3), tb(cfg->use_tlsv1), tb(cfg->use_tlsv1), tb(cfg->use_tlsv1) );
	if ((cfg->use_imaps || cfg->use_sslv3 || cfg->use_tlsv1) && cfg->cert_file)
		fprintf( fp, "CertificateFile %s\n", quotify( cfg->cert_file ) );
	fputc( '\n', fp );
}

static void
write_imap_store( FILE *fp, config_t *cfg )
{
	if (cfg->stores > 1)
		nfasprintf( (char **)&cfg->store_name, "%s-%d", cfg->old_server_name, cfg->stores );
	else
		cfg->store_name = cfg->old_server_name;
	fprintf( fp, "IMAPStore %s\nAccount %s\n",
	         cfg->store_name, cfg->server_name );
	if (*folder)
		fprintf( fp, "Path %s\n", quotify( folder ) );
	else
		fprintf( fp, "UseNamespace %s\n", tb(cfg->use_namespace) );
	if (inbox)
		fprintf( fp, "MapInbox %s\n", quotify( inbox ) );
	if (cfg->copy_deleted_to)
		fprintf( fp, "Trash %s\n", quotify( cfg->copy_deleted_to ) );
	fputc( '\n', fp );
}

static void
write_channel_parm( FILE *fp, config_t *cfg )
{
	if (cfg->max_size)
		fprintf( fp, "MaxSize %d\n", cfg->max_size );
	if (cfg->max_messages)
		fprintf( fp, "MaxMessages %d\n", cfg->max_messages );
	if (cfg->delete && !global.delete && !delete)
		fputs( "Sync All\n", fp );
	if (cfg->expunge && !global.expunge && !expunge)
		fputs( "Expunge Both\n", fp );
	fputc( '\n', fp );
}

static int
mstrcmp( const char *s1, const char *s2 )
{
	if (s1 == s2)
		return 0;
	if (!s1 || !s2)
		return 1;
	return strcmp( s1, s2 );
}

void
write_config( int fd )
{
	FILE *fp;
	const char *cn, *scn;
	char *path, *local_box, *local_store;
	config_t *box, *sbox, *pbox;
	int pl;

	if (!(fp = fdopen( fd, "w" ))) {
		perror( "fdopen" );
		return;
	}

	fputs( "SyncState *\n", fp );
	if (!global.delete && !delete)
		fputs( "Sync New ReNew Flags\n", fp );
	if (global.expunge || expunge)
		fputs( "Expunge Both\n", fp );
	fputc( '\n', fp );
	if (o2o) {
		write_imap_server( fp, &global );
		write_imap_store( fp, &global );
		fprintf( fp, "MaildirStore local\nPath %s/\n", quotify( maildir ) );
		if (!inbox) { /* just in case listing actually produces an INBOX ... */
			nfasprintf( (char **)&inbox, "%s/INBOX", maildir );
			fprintf( fp, "Inbox %s\n", quotify( inbox ) );
		}
		if (altmap > 0)
			fputs( "AltMap yes\n", fp );
		fprintf( fp, "\nChannel o2o\nMaster :%s:\nSlave :local:\nPattern %%\n", global.store_name );
		write_channel_parm( fp, &global );
	} else {
		for (box = boxes; box; box = box->next) {
			for (pbox = boxes; pbox != box; pbox = pbox->next) {
				if (box->tunnel) {
					if (mstrcmp( pbox->tunnel, box->tunnel ))
						continue;
				} else {
					if (mstrcmp( pbox->host, box->host ) ||
					    pbox->use_imaps != box->use_imaps ||
					    pbox->port != box->port)
						continue;
				}
				if (mstrcmp( pbox->user, box->user ) ||
				    mstrcmp( pbox->pass, box->pass )) /* nonsense */
					continue;
				if ((box->use_imaps ||
				     box->use_sslv3 || box->use_tlsv1) &&
				    mstrcmp( pbox->cert_file, box->cert_file )) /* nonsense */
					continue;
				if (pbox->use_imaps != box->use_imaps ||
				    pbox->use_sslv3 != box->use_sslv3 ||
				    pbox->use_tlsv1 != box->use_tlsv1)
					continue;
				box->server_name = pbox->server_name;
				for (sbox = boxes; sbox != box; sbox = sbox->next) {
					if (sbox->server_name != box->server_name ||
					    mstrcmp( sbox->copy_deleted_to, box->copy_deleted_to ) ||
					    (!*folder && sbox->use_namespace != box->use_namespace))
						continue;
					box->store_name = sbox->store_name;
					goto gotall;
				}
				box->stores = ++pbox->stores;
				goto gotsrv;
			}
			write_imap_server( fp, box );
			box->stores = 1;
		  gotsrv:
			write_imap_store( fp, box );
		  gotall:

			path = expand_strdup( box->path );
			if (starts_with( path, -1, Home, HomeLen ) && path[HomeLen] == '/')
				nfasprintf( &path, "~%s", path + HomeLen );
			local_store = local_box = strrchr( path, '/' ) + 1;
			pl = local_store - path;
			/* try to re-use existing store */
			for (pbox = boxes; pbox != box; pbox = pbox->next)
				if (pbox->local_store_path && equals( pbox->local_store_path, -1, path, pl ))
					goto gotstor;
			box->local_store_path = my_strndup( path, pl );
			/* derive a suitable name */
			if (!strcmp( box->local_store_path, "/var/mail/" ) || !strcmp( box->local_store_path, "/var/spool/mail/" )) {
				local_store = nfstrdup( "spool" );
			} else if (!strcmp( box->local_store_path, "~/" )) {
				local_store = nfstrdup( "home" );
			} else {
				local_store = memrchr( box->local_store_path, '/', pl - 1 );
				if (local_store) {
					local_store = my_strndup( local_store + 1, pl - 2 - (local_store - box->local_store_path) );
					for (pl = 0; local_store[pl]; pl++)
						local_store[pl] = tolower( local_store[pl] );
				} else {
					local_store = nfstrdup( "local" );
				}
			}
			/* avoid name clashes with imap stores */
			for (pbox = boxes; pbox != box; pbox = pbox->next)
				if (!strcmp( pbox->store_name, local_store )) {
					nfasprintf( &local_store, "local_%s", local_store );
					goto gotsdup;
				}
		  gotsdup:
			/* avoid name clashes with other local stores */
			for (pbox = boxes; pbox != box; pbox = pbox->next)
				if (pbox->local_store_name && !strcmp( pbox->local_store_name, local_store )) {
					nfasprintf( (char **)&box->local_store_name, "%s-%d", local_store, ++pbox->local_stores );
					goto gotdup;
				}
			box->local_store_name = local_store;
			box->local_stores = 1;
		  gotdup:
			fprintf( fp, "MaildirStore %s\nPath %s\n", box->local_store_name, quotify( box->local_store_path ) );
			if (altmap > 0)
				fputs( "AltMap yes\n", fp );
			fputc( '\n', fp );
			pbox = box;
		  gotstor:

			if (box->alias)
				cn = box->alias;
			else {
				cn = strrchr( box->path, '/' );
				if (cn)
					cn++;
				else
					cn = box->path;
			}
			for (sbox = boxes; sbox != box; sbox = sbox->next) {
				if (sbox->alias)
					scn = sbox->alias;
				else {
					scn = strrchr( sbox->path, '/' );
					if (scn)
						scn++;
					else
						scn = sbox->path;
				}
				if (mstrcmp( cn, scn ))
					continue;
				nfasprintf( (char **)&box->channel_name, "%s-%d", cn, ++sbox->channels );
				goto gotchan;
			}
			box->channels = 1;
			box->channel_name = cn;
		  gotchan:

			fprintf( fp, "Channel %s\nMaster :%s:%s\nSlave :%s:%s\n",
			             box->channel_name, box->store_name, quotify( box->box ), pbox->local_store_name, quotify( local_box ) );
			write_channel_parm( fp, box );
		}
				
	}

	fclose( fp );
}

config_t *
find_box( const char *s )
{
	config_t *p;
	char *t;

	if (starts_with( s, -1, Home, HomeLen ) && s[HomeLen] == '/')
		s += HomeLen + 1;
	for (p = boxes; p; p = p->next) {
		if (!strcmp( s, p->path ) || (p->alias && !strcmp( s, p->alias )))
			return p;
		/* check to see if the full pathname was specified on the
		 * command line.
		 */
		t = expand_strdup( p->path );
		if (!strcmp( s, t )) {
			free( t );
			return p;
		}
		free( t );
	}
	return 0;
}
