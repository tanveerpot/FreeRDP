/**
 * FreeRDP: A Remote Desktop Protocol Implementation
 * X11 Clipboard Redirection
 *
 * Copyright 2010-2011 Vic Lee
 * Copyright 2015 Thincast Technologies GmbH
 * Copyright 2015 DI (FH) Martin Haimberger <martin.haimberger@thincast.com>
 * Copyright 2023 Armin Novak <armin.novak@thincast.com>
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * You may obtain a copy of the License at
 *
 *     http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 */

#include <freerdp/config.h>

#include <stdlib.h>
#include <errno.h>

#ifdef WITH_FUSE3
#define FUSE_USE_VERSION 30
#include <fuse_lowlevel.h>
#elif WITH_FUSE2
#define FUSE_USE_VERSION 26
#include <fuse_lowlevel.h>
#endif

#if defined(WITH_FUSE2) || defined(WITH_FUSE3)
#include <sys/mount.h>
#include <sys/stat.h>
#include <errno.h>
#include <time.h>
#endif

#include <winpr/crt.h>
#include <winpr/assert.h>
#include <winpr/image.h>
#include <winpr/stream.h>
#include <winpr/clipboard.h>
#include <winpr/path.h>

#include <freerdp/utils/signal.h>
#include <freerdp/log.h>
#include <freerdp/client/cliprdr.h>
#include <freerdp/channels/channels.h>
#include <freerdp/channels/cliprdr.h>

#include <freerdp/client/client_cliprdr_file.h>

#define MAX_CLIPBOARD_FORMATS 255
#define WIN32_FILETIME_TO_UNIX_EPOCH_USEC UINT64_C(116444736000000000)

#ifdef WITH_DEBUG_CLIPRDR
#define DEBUG_CLIPRDR(log, ...) WLog_Print(log, WLOG_DEBUG, __VA_ARGS__)
#else
#define DEBUG_CLIPRDR(log, ...) \
	do                          \
	{                           \
	} while (0)
#endif

#if defined(WITH_FUSE2) || defined(WITH_FUSE3)
typedef struct
{
	UINT32 stream_id;
	/* must be one of FILECONTENTS_SIZE or FILECONTENTS_RANGE*/
	UINT32 req_type;
	fuse_req_t req;
	/*for FILECONTENTS_SIZE must be ino number* */
	size_t req_ino;
} CliprdrFuseStream;

typedef struct
{
	size_t parent_ino;
	size_t ino;
	size_t lindex;
	mode_t st_mode;
	off_t st_size;
	BOOL size_set;
	struct timespec st_mtim;
	char* name;
	wArrayList* child_inos;
} CliprdrFuseInode;
#endif

typedef struct
{
	char* name;
	FILE* fp;
} CliprdrLocalFile;

typedef struct
{
	UINT32 streamID;

	BOOL locked;
	size_t count;
	CliprdrLocalFile* files;
} CliprdrLocalStream;

struct cliprdr_file_context
{
#if defined(WITH_FUSE2) || defined(WITH_FUSE3)
	/* FUSE related**/
	HANDLE fuse_thread;
	struct fuse_session* fuse_sess;

	/* fuse reset per copy*/
	wHashTable* remote_streams;
	UINT32 current_stream_id;
	wArrayList* ino_list;
#endif

	/* File clipping */
	BOOL file_formats_registered;
	UINT32 file_capability_flags;

	UINT32 local_stream_id;
	UINT32 remote_stream_id;

	wHashTable* local_streams;
	wLog* log;
	void* clipboard;
	CliprdrClientContext* context;
	char* path;
	BYTE hash[WINPR_SHA256_DIGEST_LENGTH];
};

static CliprdrLocalStream* cliprdr_local_stream_new(UINT32 streamID, const char* data, size_t size);
static void cliprdr_file_session_terminate(CliprdrFileContext* file);
static BOOL local_stream_discard(const void* key, void* value, void* arg);

#if defined(WITH_FUSE2) || defined(WITH_FUSE3)
static BOOL xf_fuse_repopulate(CliprdrFileContext* file, wArrayList* list);
static UINT xf_cliprdr_send_client_file_contents(CliprdrFileContext* file, UINT32 streamId,
                                                 UINT32 listIndex, UINT32 dwFlags,
                                                 UINT32 nPositionLow, UINT32 nPositionHigh,
                                                 UINT32 cbRequested);

static void cliprdr_file_fuse_lookup(fuse_req_t req, fuse_ino_t parent, const char* name);
static void cliprdr_file_fuse_getattr(fuse_req_t req, fuse_ino_t ino, struct fuse_file_info* fi);
static void cliprdr_file_fuse_readdir(fuse_req_t req, fuse_ino_t ino, size_t size, off_t off,
                                      struct fuse_file_info* fi);
static void cliprdr_file_fuse_read(fuse_req_t req, fuse_ino_t ino, size_t size, off_t off,
                                   struct fuse_file_info* fi);
static void cliprdr_file_fuse_open(fuse_req_t req, fuse_ino_t ino, struct fuse_file_info* fi);
static void cliprdr_file_fuse_opendir(fuse_req_t req, fuse_ino_t ino, struct fuse_file_info* fi);

static const struct fuse_lowlevel_ops cliprdr_file_fuse_oper = {
	.lookup = cliprdr_file_fuse_lookup,
	.getattr = cliprdr_file_fuse_getattr,
	.readdir = cliprdr_file_fuse_readdir,
	.open = cliprdr_file_fuse_open,
	.read = cliprdr_file_fuse_read,
	.opendir = cliprdr_file_fuse_opendir,
};

static void cliprdr_fuse_stream_free(void* stream)
{
	free(stream);
}

static CliprdrFuseStream* cliprdr_fuse_stream_new(UINT32 streamID, fuse_req_t req, size_t ino,
                                                  UINT32 req_type)
{
	CliprdrFuseStream* stream = (CliprdrFuseStream*)calloc(1, sizeof(CliprdrFuseStream));
	if (!stream)
		return NULL;
	stream->req = req;
	stream->req_type = req_type;
	stream->stream_id = streamID;
	stream->req_ino = ino;
	return stream;
}

static void cliprdr_file_fuse_inode_free(void* obj)
{
	CliprdrFuseInode* inode = (CliprdrFuseInode*)obj;
	if (!inode)
		return;

	free(inode->name);
	ArrayList_Free(inode->child_inos);

	inode->name = NULL;
	inode->child_inos = NULL;
	free(inode);
}

/* For better understanding the relationship between ino and index of arraylist*/
static inline CliprdrFuseInode* cliprdr_file_fuse_util_get_inode(wArrayList* ino_list,
                                                                 fuse_ino_t ino)
{
	size_t list_index = ino - 1;
	return (CliprdrFuseInode*)ArrayList_GetItem(ino_list, list_index);
}

/* fuse helper functions*/
static int cliprdr_file_fuse_util_stat(CliprdrFileContext* file, fuse_ino_t ino, struct stat* stbuf)
{
	int err = 0;
	CliprdrFuseInode* node;

	WINPR_ASSERT(file);
	WINPR_ASSERT(stbuf);

	ArrayList_Lock(file->ino_list);

	node = cliprdr_file_fuse_util_get_inode(file->ino_list, ino);

	if (!node)
	{
		err = ENOENT;
		goto error;
	}
	memset(stbuf, 0, sizeof(*stbuf));
	stbuf->st_ino = ino;
	stbuf->st_mode = node->st_mode;
	stbuf->st_mtime = node->st_mtim.tv_sec;
	stbuf->st_nlink = 1;
	stbuf->st_size = node->st_size;
error:
	ArrayList_Unlock(file->ino_list);
	return err;
}

static int cliprdr_file_fuse_util_stmode(CliprdrFileContext* file, fuse_ino_t ino, mode_t* mode)
{
	int err = 0;
	CliprdrFuseInode* node;

	WINPR_ASSERT(file);
	WINPR_ASSERT(mode);

	ArrayList_Lock(file->ino_list);

	node = cliprdr_file_fuse_util_get_inode(file->ino_list, ino);
	if (!node)
	{
		err = ENOENT;
		goto error;
	}
	*mode = node->st_mode;
error:
	ArrayList_Unlock(file->ino_list);
	return err;
}

static int cliprdr_file_fuse_util_lindex(CliprdrFileContext* file, fuse_ino_t ino, UINT32* lindex)
{
	int err = 0;
	CliprdrFuseInode* node;

	WINPR_ASSERT(file);
	WINPR_ASSERT(lindex);

	ArrayList_Lock(file->ino_list);

	node = cliprdr_file_fuse_util_get_inode(file->ino_list, ino);
	if (!node)
	{
		err = ENOENT;
		goto error;
	}
	if ((node->st_mode & S_IFDIR) != 0)
	{
		err = EISDIR;
		goto error;
	}
	*lindex = node->lindex;

error:
	ArrayList_Unlock(file->ino_list);
	return err;
}

static int cliprdr_file_fuse_util_add_stream_list(CliprdrFileContext* file, fuse_req_t req,
                                                  UINT32* stream_id)
{
	int err = 0;

	WINPR_ASSERT(file);
	WINPR_ASSERT(stream_id);

	CliprdrFuseStream* stream =
	    cliprdr_fuse_stream_new(file->current_stream_id++, req, 0, FILECONTENTS_RANGE);
	if (!stream)
	{
		err = ENOMEM;
		return err;
	}

	const BOOL res =
	    HashTable_Insert(file->remote_streams, (void*)(uintptr_t)stream->stream_id, stream);
	if (res)
	{
		err = ENOMEM;
		goto error;
	}
error:
	return err;
}

void cliprdr_file_fuse_getattr(fuse_req_t req, fuse_ino_t ino, struct fuse_file_info* fi)
{
	int err;
	struct stat stbuf;

	CliprdrFileContext* file = (CliprdrFileContext*)fuse_req_userdata(req);
	WINPR_ASSERT(file);

	err = cliprdr_file_fuse_util_stat(file, ino, &stbuf);
	if (err)
	{
		fuse_reply_err(req, err);
		return;
	}

	fuse_reply_attr(req, &stbuf, 0);
}

void cliprdr_file_fuse_readdir(fuse_req_t req, fuse_ino_t ino, size_t size, off_t off,
                               struct fuse_file_info* fi)
{
	size_t count;
	size_t index;
	size_t child_ino;
	size_t direntry_len;
	char* buf;
	size_t pos = 0;
	CliprdrFuseInode* child_node;
	CliprdrFuseInode* node;
	CliprdrFileContext* file = (CliprdrFileContext*)fuse_req_userdata(req);

	WINPR_ASSERT(file);

	ArrayList_Lock(file->ino_list);
	node = cliprdr_file_fuse_util_get_inode(file->ino_list, ino);

	if (!node || !node->child_inos)
	{
		ArrayList_Unlock(file->ino_list);
		fuse_reply_err(req, ENOENT);
		return;
	}
	else if ((node->st_mode & S_IFDIR) == 0)
	{
		ArrayList_Unlock(file->ino_list);
		fuse_reply_err(req, ENOTDIR);
		return;
	}

	ArrayList_Lock(node->child_inos);
	count = ArrayList_Count(node->child_inos);
	if ((count == 0) || ((SSIZE_T)(count + 1) <= off))
	{
		ArrayList_Unlock(node->child_inos);
		ArrayList_Unlock(file->ino_list);
		fuse_reply_buf(req, NULL, 0);
		return;
	}
	else
	{
		buf = (char*)calloc(size, sizeof(char));
		if (!buf)
		{
			ArrayList_Unlock(node->child_inos);
			ArrayList_Unlock(file->ino_list);
			fuse_reply_err(req, ENOMEM);
			return;
		}
		for (index = off; index < count + 2; index++)
		{
			struct stat stbuf = { 0 };
			if (index == 0)
			{
				stbuf.st_ino = ino;
				direntry_len = fuse_add_direntry(req, buf + pos, size - pos, ".", &stbuf, index);
				if (direntry_len > size - pos)
					break;
				pos += direntry_len;
			}
			else if (index == 1)
			{
				stbuf.st_ino = node->parent_ino;
				direntry_len = fuse_add_direntry(req, buf + pos, size - pos, "..", &stbuf, index);
				if (direntry_len > size - pos)
					break;
				pos += direntry_len;
			}
			else
			{
				/* execlude . and .. */
				child_ino = (size_t)ArrayList_GetItem(node->child_inos, index - 2);
				/* previous lock for ino_list still work*/
				child_node = cliprdr_file_fuse_util_get_inode(file->ino_list, child_ino);
				if (!child_node)
					break;
				stbuf.st_ino = child_node->ino;
				direntry_len =
				    fuse_add_direntry(req, buf + pos, size - pos, child_node->name, &stbuf, index);
				if (direntry_len > size - pos)
					break;
				pos += direntry_len;
			}
		}

		ArrayList_Unlock(node->child_inos);
		ArrayList_Unlock(file->ino_list);
		fuse_reply_buf(req, buf, pos);
		free(buf);
		return;
	}
}

void cliprdr_file_fuse_open(fuse_req_t req, fuse_ino_t ino, struct fuse_file_info* fi)
{
	int err;
	mode_t mode = 0;
	CliprdrFileContext* file = (CliprdrFileContext*)fuse_req_userdata(req);

	WINPR_ASSERT(file);
	err = cliprdr_file_fuse_util_stmode(file, ino, &mode);
	if (err)
	{
		fuse_reply_err(req, err);
		return;
	}

	if ((mode & S_IFDIR) != 0)
	{
		fuse_reply_err(req, EISDIR);
	}
	else
	{
		/* Important for KDE to get file correctly*/
		fi->direct_io = 1;
		fuse_reply_open(req, fi);
	}
}

void cliprdr_file_fuse_read(fuse_req_t req, fuse_ino_t ino, size_t size, off_t off,
                            struct fuse_file_info* fi)
{
	if (ino < 2)
	{
		fuse_reply_err(req, ENOENT);
		return;
	}
	int err;
	CliprdrFileContext* file = (CliprdrFileContext*)fuse_req_userdata(req);
	UINT32 lindex;
	UINT32 stream_id;

	WINPR_ASSERT(file);

	err = cliprdr_file_fuse_util_lindex(file, ino, &lindex);
	if (err)
	{
		fuse_reply_err(req, err);
		return;
	}

	err = cliprdr_file_fuse_util_add_stream_list(file, req, &stream_id);
	if (err)
	{
		fuse_reply_err(req, err);
		return;
	}

	UINT32 nPositionLow = (off >> 0) & 0xFFFFFFFF;
	UINT32 nPositionHigh = (off >> 32) & 0xFFFFFFFF;

	xf_cliprdr_send_client_file_contents(file, stream_id, lindex, FILECONTENTS_RANGE, nPositionLow,
	                                     nPositionHigh, size);
}

void cliprdr_file_fuse_lookup(fuse_req_t req, fuse_ino_t parent, const char* name)
{
	size_t index;
	size_t count;
	size_t child_ino;
	BOOL found = FALSE;
	struct fuse_entry_param e = { 0 };
	CliprdrFuseInode* parent_node;
	CliprdrFuseInode* child_node = NULL;
	CliprdrFileContext* file = (CliprdrFileContext*)fuse_req_userdata(req);

	WINPR_ASSERT(file);

	ArrayList_Lock(file->ino_list);
	parent_node = cliprdr_file_fuse_util_get_inode(file->ino_list, parent);

	if (!parent_node || !parent_node->child_inos)
	{
		ArrayList_Unlock(file->ino_list);
		fuse_reply_err(req, ENOENT);
		return;
	}

	ArrayList_Lock(parent_node->child_inos);
	count = ArrayList_Count(parent_node->child_inos);
	for (index = 0; index < count; index++)
	{
		child_ino = (size_t)ArrayList_GetItem(parent_node->child_inos, index);
		child_node = cliprdr_file_fuse_util_get_inode(file->ino_list, child_ino);
		if (child_node && strcmp(name, child_node->name) == 0)
		{
			found = TRUE;
			break;
		}
	}
	ArrayList_Unlock(parent_node->child_inos);

	if (!found || !child_node)
	{
		ArrayList_Unlock(file->ino_list);
		fuse_reply_err(req, ENOENT);
		return;
	}

	BOOL res;
	BOOL size_set = child_node->size_set;
	size_t lindex = child_node->lindex;
	size_t ino = child_node->ino;
	mode_t st_mode = child_node->st_mode;
	off_t st_size = child_node->st_size;
	time_t tv_sec = child_node->st_mtim.tv_sec;
	ArrayList_Unlock(file->ino_list);

	if (!size_set)
	{
		CliprdrFuseStream* stream =
		    cliprdr_fuse_stream_new(file->current_stream_id++, req, ino, FILECONTENTS_SIZE);
		if (!stream)
		{
			fuse_reply_err(req, ENOMEM);
			return;
		}
		file->current_stream_id++;

		res = HashTable_Insert(file->remote_streams, (void*)(uintptr_t)stream->stream_id, stream);
		if (!res)
		{
			fuse_reply_err(req, ENOMEM);
			return;
		}
		xf_cliprdr_send_client_file_contents(file, stream->stream_id, lindex, FILECONTENTS_SIZE, 0,
		                                     0, 0);
		return;
	}
	e.ino = ino;
	e.attr_timeout = 1.0;
	e.entry_timeout = 1.0;
	e.attr.st_ino = ino;
	e.attr.st_mode = st_mode;
	e.attr.st_nlink = 1;

	e.attr.st_size = st_size;
	e.attr.st_mtime = tv_sec;
	fuse_reply_entry(req, &e);
}

void cliprdr_file_fuse_opendir(fuse_req_t req, fuse_ino_t ino, struct fuse_file_info* fi)
{
	int err;
	mode_t mode = 0;
	CliprdrFileContext* file = (CliprdrFileContext*)fuse_req_userdata(req);
	WINPR_ASSERT(file);

	err = cliprdr_file_fuse_util_stmode(file, ino, &mode);
	if (err)
	{
		fuse_reply_err(req, err);
		return;
	}

	if ((mode & S_IFDIR) == 0)
	{
		fuse_reply_err(req, ENOTDIR);
	}
	else
	{
		fuse_reply_open(req, fi);
	}
}

static void fuse_abort(int sig, const char* signame, void* context)
{
	CliprdrFileContext* file = (CliprdrFileContext*)context;

	if (file)
	{
		WLog_Print(file->log, WLOG_INFO, "signal %s [%d] aborting session", signame, sig);
		cliprdr_file_session_terminate(file);
	}
}

static DWORD WINAPI cliprdr_file_fuse_thread(LPVOID arg)
{
	CliprdrFileContext* file = (CliprdrFileContext*)arg;

	WINPR_ASSERT(file);

	DEBUG_CLIPRDR(file->log, "Starting fuse with mountpoint '%s'", file->path);

	struct fuse_args args = FUSE_ARGS_INIT(0, NULL);
#if FUSE_USE_VERSION >= 30
	fuse_opt_add_arg(&args, file->path);
	if ((file->fuse_sess = fuse_session_new(&args, &cliprdr_file_fuse_oper,
	                                        sizeof(cliprdr_file_fuse_oper), (void*)file)) != NULL)
	{
		freerdp_add_signal_cleanup_handler(file, fuse_abort);
		if (0 == fuse_session_mount(file->fuse_sess, file->path))
		{
			fuse_session_loop(file->fuse_sess);
			fuse_session_unmount(file->fuse_sess);
		}
		freerdp_del_signal_cleanup_handler(file, fuse_abort);
		fuse_session_destroy(file->fuse_sess);
	}
#else
	struct fuse_chan* ch = fuse_mount(file->path, &args);
	if (ch != NULL)
	{
		file->fuse_sess = fuse_lowlevel_new(&args, &cliprdr_file_fuse_oper,
		                                    sizeof(cliprdr_file_fuse_oper), (void*)file);
		if (file->fuse_sess != NULL)
		{
			freerdp_add_signal_cleanup_handler(file, fuse_abort);
			fuse_session_add_chan(file->fuse_sess, ch);
			const int err = fuse_session_loop(file->fuse_sess);
			if (err != 0)
				WLog_WARN(TAG, "fuse_session_loop failed with %d", err);
			fuse_session_remove_chan(ch);
			freerdp_del_signal_cleanup_handler(file, fuse_abort);
			fuse_session_destroy(file->fuse_sess);
		}
		fuse_unmount(file->path, ch);
	}
#endif
	fuse_opt_free_args(&args);

	DEBUG_CLIPRDR(file->log, "Quitting fuse with mountpoint '%s'", file->path);

	ExitThread(0);
	return 0;
}

static CliprdrFuseInode* cliprdr_file_fuse_create_root_node(CliprdrFileContext* file)
{
	WINPR_ASSERT(file);

	CliprdrFuseInode* rootNode = (CliprdrFuseInode*)calloc(1, sizeof(CliprdrFuseInode));
	if (!rootNode)
		return NULL;

	rootNode->ino = FUSE_ROOT_ID;
	rootNode->parent_ino = FUSE_ROOT_ID;
	rootNode->st_mode = S_IFDIR | 0755;
	rootNode->name = _strdup("/");
	rootNode->child_inos = ArrayList_New(TRUE);
	rootNode->st_mtim.tv_sec = time(NULL);
	rootNode->st_size = 0;
	rootNode->size_set = TRUE;

	if (!rootNode->child_inos || !rootNode->name)
	{
		cliprdr_file_fuse_inode_free(rootNode);
		WLog_Print(file->log, WLOG_ERROR, "fail to alloc rootNode's member");
		return NULL;
	}
	return rootNode;
}

static BOOL xf_fuse_repopulate(CliprdrFileContext* file, wArrayList* list)
{
	if (!list)
		return FALSE;

	ArrayList_Lock(list);
	ArrayList_Clear(list);
	ArrayList_Append(list, cliprdr_file_fuse_create_root_node(file));
	ArrayList_Unlock(list);
	return TRUE;
}

/**
 * Function description
 *
 * @return 0 on success, otherwise a Win32 error code
 */
static UINT xf_cliprdr_send_client_file_contents(CliprdrFileContext* file, UINT32 streamId,
                                                 UINT32 listIndex, UINT32 dwFlags,
                                                 UINT32 nPositionLow, UINT32 nPositionHigh,
                                                 UINT32 cbRequested)
{
	CLIPRDR_FILE_CONTENTS_REQUEST formatFileContentsRequest = { 0 };
	formatFileContentsRequest.streamId = streamId;
	formatFileContentsRequest.listIndex = listIndex;
	formatFileContentsRequest.dwFlags = dwFlags;

	WINPR_ASSERT(file);
	switch (dwFlags)
	{
		/*
		 * [MS-RDPECLIP] 2.2.5.3 File Contents Request PDU (CLIPRDR_FILECONTENTS_REQUEST).
		 *
		 * A request for the size of the file identified by the lindex field. The size MUST be
		 * returned as a 64-bit, unsigned integer. The cbRequested field MUST be set to
		 * 0x00000008 and both the nPositionLow and nPositionHigh fields MUST be
		 * set to 0x00000000.
		 */
		case FILECONTENTS_SIZE:
			formatFileContentsRequest.cbRequested = sizeof(UINT64);
			formatFileContentsRequest.nPositionHigh = 0;
			formatFileContentsRequest.nPositionLow = 0;
			break;
		case FILECONTENTS_RANGE:
			formatFileContentsRequest.cbRequested = cbRequested;
			formatFileContentsRequest.nPositionHigh = nPositionHigh;
			formatFileContentsRequest.nPositionLow = nPositionLow;
			break;
	}

	formatFileContentsRequest.haveClipDataId = FALSE;
	return file->context->ClientFileContentsRequest(file->context, &formatFileContentsRequest);
}

/**
 * Function description
 *
 * @return 0 on success, otherwise a Win32 error code
 */
static UINT cliprdr_file_context_server_file_contents_response(
    CliprdrClientContext* context, const CLIPRDR_FILE_CONTENTS_RESPONSE* fileContentsResponse)
{
	BOOL found = FALSE;
	CliprdrFuseInode* ino;
	UINT32 stream_id;
	const BYTE* data;
	size_t data_len;
	CliprdrFileContext* file;

	WINPR_ASSERT(context);
	WINPR_ASSERT(fileContentsResponse);

	file = context->custom;
	WINPR_ASSERT(file);

	stream_id = fileContentsResponse->streamId;
	data = fileContentsResponse->requestedData;
	data_len = fileContentsResponse->cbRequested;

	CliprdrFuseStream fstream = { 0 };
	HashTable_Lock(file->remote_streams);
	{
		const CliprdrFuseStream* stream =
		    HashTable_GetItemValue(file->remote_streams, (void*)(uintptr_t)stream_id);
		if (stream)
		{
			found = TRUE;
			fstream = *stream;
		}
		HashTable_Remove(file->remote_streams, (void*)(uintptr_t)stream_id);
	}
	HashTable_Unlock(file->remote_streams);
	if (!found)
		return CHANNEL_RC_OK;

	switch (fstream.req_type)
	{
		case FILECONTENTS_SIZE:
			/* fileContentsResponse->cbRequested should be 64bit*/
			if (data_len != sizeof(UINT64))
			{
				fuse_reply_err(fstream.req, EIO);
				break;
			}
			UINT64 size;
			wStream sbuffer = { 0 };
			wStream* s = Stream_StaticConstInit(&sbuffer, data, data_len);
			if (!s)
			{
				fuse_reply_err(fstream.req, ENOMEM);
				break;
			}
			Stream_Read_UINT64(s, size);

			ArrayList_Lock(file->ino_list);
			ino = cliprdr_file_fuse_util_get_inode(file->ino_list, fstream.req_ino);
			/* ino must be exists and  */
			if (!ino)
			{
				ArrayList_Unlock(file->ino_list);
				fuse_reply_err(fstream.req, EIO);
				break;
			}

			ino->st_size = size;
			ino->size_set = TRUE;
			struct fuse_entry_param e = { 0 };
			e.ino = ino->ino;
			e.attr_timeout = 1.0;
			e.entry_timeout = 1.0;
			e.attr.st_ino = ino->ino;
			e.attr.st_mode = ino->st_mode;
			e.attr.st_nlink = 1;
			e.attr.st_size = ino->st_size;
			e.attr.st_mtime = ino->st_mtim.tv_sec;
			ArrayList_Unlock(file->ino_list);
			fuse_reply_entry(fstream.req, &e);
			break;
		case FILECONTENTS_RANGE:
			fuse_reply_buf(fstream.req, (const char*)data, data_len);
			break;
	}
	return CHANNEL_RC_OK;
}

static const char* cliprdr_file_fuse_split_basename(const char* name, size_t len)
{
	WINPR_ASSERT(name || (len <= 0));
	for (size_t s = len; s > 0; s--)
	{
		char c = name[s - 1];
		if (c == '\\')
		{
			return &name[s - 1];
		}
	}
	return NULL;
}

static BOOL cliprdr_file_fuse_check_stream(CliprdrFileContext* file, wStream* s, size_t count)
{
	UINT32 nrDescriptors;

	WINPR_ASSERT(file);
	if (!Stream_CheckAndLogRequiredLengthWLog(file->log, s, sizeof(UINT32)))
		return FALSE;

	Stream_Read_UINT32(s, nrDescriptors);
	if (count != nrDescriptors)
	{
		WLog_Print(file->log, WLOG_WARN, "format data response mismatch");
		return FALSE;
	}
	return TRUE;
}

static BOOL cliprdr_file_fuse_create_nodes(CliprdrFileContext* file, wStream* s, size_t count,
                                           const CliprdrFuseInode* rootNode)
{
	BOOL status = FALSE;
	size_t lindex = 0;
	char* curName = NULL;
	CliprdrFuseInode* inode = NULL;
	wHashTable* mapDir;

	WINPR_ASSERT(file);
	WINPR_ASSERT(s);
	WINPR_ASSERT(rootNode);

	mapDir = HashTable_New(TRUE);
	if (!mapDir)
	{
		WLog_Print(file->log, WLOG_ERROR, "fail to alloc hashtable");
		goto error;
	}
	if (!HashTable_SetupForStringData(mapDir, FALSE))
		goto error;

	FILEDESCRIPTORW* descriptor = (FILEDESCRIPTORW*)calloc(1, sizeof(FILEDESCRIPTORW));
	if (!descriptor)
	{
		WLog_Print(file->log, WLOG_ERROR, "fail to alloc FILEDESCRIPTORW");
		goto error;
	}
	/* here we assume that parent folder always appears before its children */
	for (; lindex < count; lindex++)
	{
		Stream_Read(s, descriptor, sizeof(FILEDESCRIPTORW));
		inode = (CliprdrFuseInode*)calloc(1, sizeof(CliprdrFuseInode));
		if (!inode)
		{
			WLog_Print(file->log, WLOG_ERROR, "fail to alloc ino");
			break;
		}

		free(curName);
		curName =
		    ConvertWCharNToUtf8Alloc(descriptor->cFileName, ARRAYSIZE(descriptor->cFileName), NULL);
		if (!curName)
			break;

		const char* split_point = cliprdr_file_fuse_split_basename(
		    curName, strnlen(curName, ARRAYSIZE(descriptor->cFileName)));

		UINT64 ticks;
		CliprdrFuseInode* parent;

		inode->lindex = lindex;
		inode->ino = lindex + 2;

		if (split_point == NULL)
		{
			char* baseName = _strdup(curName);
			if (!baseName)
				break;
			inode->parent_ino = FUSE_ROOT_ID;
			inode->name = baseName;
			if (!ArrayList_Append(rootNode->child_inos, (void*)inode->ino))
				break;
		}
		else
		{
			char* dirName = calloc(split_point - curName + 1, sizeof(char));
			if (!dirName)
				break;
			_snprintf(dirName, split_point - curName + 1, "%s", curName);
			/* drop last '\\' */
			char* baseName = _strdup(split_point + 1);
			if (!baseName)
				break;

			parent = (CliprdrFuseInode*)HashTable_GetItemValue(mapDir, dirName);
			if (!parent)
				break;
			inode->parent_ino = parent->ino;
			inode->name = baseName;
			if (!ArrayList_Append(parent->child_inos, (void*)inode->ino))
				break;
			free(dirName);
		}
		/* TODO: check FD_ATTRIBUTES in dwFlags
		    However if this flag is not valid how can we determine file/folder?
		*/
		if ((descriptor->dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) != 0)
		{
			inode->st_mode = S_IFDIR | 0755;
			inode->child_inos = ArrayList_New(TRUE);
			if (!inode->child_inos)
				break;
			inode->st_size = 0;
			inode->size_set = TRUE;
			if (!HashTable_Insert(mapDir, curName, inode))
			{
				break;
			}
		}
		else
		{
			inode->st_mode = S_IFREG | 0644;
			if ((descriptor->dwFlags & FD_FILESIZE) != 0)
			{
				inode->st_size = (((UINT64)descriptor->nFileSizeHigh) << 32) |
				                 ((UINT64)descriptor->nFileSizeLow);
				inode->size_set = TRUE;
			}
			else
			{
				inode->size_set = FALSE;
			}
		}

		if ((descriptor->dwFlags & FD_WRITESTIME) != 0)
		{
			ticks = (((UINT64)descriptor->ftLastWriteTime.dwHighDateTime << 32) |
			         ((UINT64)descriptor->ftLastWriteTime.dwLowDateTime)) -
			        WIN32_FILETIME_TO_UNIX_EPOCH_USEC;
			inode->st_mtim.tv_sec = ticks / 10000000ULL;
			/* tv_nsec Not used for now */
			inode->st_mtim.tv_nsec = ticks % 10000000ULL;
		}
		else
		{
			inode->st_mtim.tv_sec = time(NULL);
			inode->st_mtim.tv_nsec = 0;
		}

		if (!ArrayList_Append(file->ino_list, inode))
			break;
	}
	/* clean up incomplete ino_list*/
	if (lindex != count)
	{
		/* baseName is freed in cliprdr_file_fuse_inode_free*/
		cliprdr_file_fuse_inode_free(inode);
		xf_fuse_repopulate(file, file->ino_list);
	}
	else
	{
		status = TRUE;
	}

	free(descriptor);

error:
	free(curName);
	HashTable_Free(mapDir);
	return status;
}

/**
 * Generate inode list for fuse
 *
 * @return TRUE on success, FALSE on fail
 */
static BOOL cliprdr_file_fuse_generate_list(CliprdrFileContext* file, const BYTE* data, UINT32 size)
{
	BOOL status = FALSE;
	wStream sbuffer = { 0 };
	wStream* s;

	WINPR_ASSERT(file);
	WINPR_ASSERT(data || (size == 0));

	if (size < 4)
	{
		WLog_Print(file->log, WLOG_ERROR, "size of format data response invalid : %" PRIu32, size);
		return FALSE;
	}
	size_t count = (size - 4) / sizeof(FILEDESCRIPTORW);
	if (count < 1)
		return FALSE;

	s = Stream_StaticConstInit(&sbuffer, data, size);
	if (!s || !cliprdr_file_fuse_check_stream(file, s, count))
	{
		WLog_Print(file->log, WLOG_ERROR, "Stream_New failed");
		goto error;
	}

	/* prevent conflict between fuse_thread and this */
	ArrayList_Lock(file->ino_list);
	CliprdrFuseInode* rootNode = cliprdr_file_fuse_util_get_inode(file->ino_list, FUSE_ROOT_ID);

	if (!rootNode)
	{
		cliprdr_file_fuse_inode_free(rootNode);
		WLog_Print(file->log, WLOG_ERROR, "fail to alloc rootNode to ino_list");
		goto error2;
	}

	status = cliprdr_file_fuse_create_nodes(file, s, count, rootNode);

error2:
	ArrayList_Unlock(file->ino_list);
error:
	return status;
}
#endif

static UINT cliprdr_file_context_send_file_contents_failure(
    CliprdrClientContext* context, const CLIPRDR_FILE_CONTENTS_REQUEST* fileContentsRequest)
{
	CLIPRDR_FILE_CONTENTS_RESPONSE response = { 0 };

	WINPR_ASSERT(fileContentsRequest);

	response.common.msgFlags = CB_RESPONSE_FAIL;
	response.streamId = fileContentsRequest->streamId;

	WINPR_ASSERT(context);
	WINPR_ASSERT(context->ClientFileContentsResponse);
	return context->ClientFileContentsResponse(context, &response);
}

static UINT cliprdr_file_context_send_contents(CliprdrClientContext* context,
                                               const CLIPRDR_FILE_CONTENTS_REQUEST* request,
                                               const void* data, size_t size)
{
	CLIPRDR_FILE_CONTENTS_RESPONSE response = { 0 };

	WINPR_ASSERT(request);

	response.common.msgFlags = CB_RESPONSE_OK;
	response.streamId = request->streamId;
	response.requestedData = data;
	response.cbRequested = size;

	WINPR_ASSERT(context);
	WINPR_ASSERT(context->ClientFileContentsResponse);
	return context->ClientFileContentsResponse(context, &response);
}

static FILE* file_for_request(CliprdrFileContext* file, UINT32 streamId, UINT32 listIndex)
{
	FILE* fp = NULL;
	HashTable_Lock(file->local_streams);

	CliprdrLocalStream* cur =
	    HashTable_GetItemValue(file->local_streams, (void*)(uintptr_t)streamId);
	if (cur)
	{
		if (listIndex < cur->count)
		{
			CliprdrLocalFile* f = &cur->files[listIndex];
			if (f->fp)
				fp = f->fp;
			else
			{
				if (strncmp("file://", f->name, 7) == 0)
					fp = fopen(&f->name[7], "rb");
				else
					fp = fopen(f->name, "rb");
				f->fp = fp;
			}
		}
	}

	HashTable_Unlock(file->local_streams);

	return fp;
}

static UINT cliprdr_file_context_server_file_size_request(
    CliprdrFileContext* file, const CLIPRDR_FILE_CONTENTS_REQUEST* fileContentsRequest)
{
	wClipboardFileSizeRequest request = { 0 };

	WINPR_ASSERT(fileContentsRequest);

	request.streamId = fileContentsRequest->streamId;
	request.listIndex = fileContentsRequest->listIndex;

	if (fileContentsRequest->cbRequested != sizeof(UINT64))
	{
		WLog_Print(file->log, WLOG_WARN, "unexpected FILECONTENTS_SIZE request: %" PRIu32 " bytes",
		           fileContentsRequest->cbRequested);
	}

	FILE* fp =
	    file_for_request(file, fileContentsRequest->streamId, fileContentsRequest->listIndex);
	if (!fp)
		return cliprdr_file_context_send_file_contents_failure(file->context, fileContentsRequest);

	if (_fseeki64(fp, 0, SEEK_END) < 0)
		return cliprdr_file_context_send_file_contents_failure(file->context, fileContentsRequest);

	const INT64 size = _ftelli64(fp);
	return cliprdr_file_context_send_contents(file->context, fileContentsRequest, &size,
	                                          sizeof(size));
}

static UINT cliprdr_file_context_server_file_range_request(
    CliprdrFileContext* file, const CLIPRDR_FILE_CONTENTS_REQUEST* fileContentsRequest)
{
	BYTE* data = NULL;
	wClipboardFileRangeRequest request = { 0 };

	WINPR_ASSERT(fileContentsRequest);

	request.streamId = fileContentsRequest->streamId;
	request.listIndex = fileContentsRequest->listIndex;
	request.nPositionLow = fileContentsRequest->nPositionLow;
	request.nPositionHigh = fileContentsRequest->nPositionHigh;
	request.cbRequested = fileContentsRequest->cbRequested;
	FILE* fp =
	    file_for_request(file, fileContentsRequest->streamId, fileContentsRequest->listIndex);
	if (!fp)
		goto fail;

	const UINT64 offset = (((UINT64)fileContentsRequest->nPositionHigh) << 32) |
	                      ((UINT64)fileContentsRequest->nPositionLow);
	if (_fseeki64(fp, offset, SEEK_SET) < 0)
		goto fail;

	data = malloc(fileContentsRequest->cbRequested);
	if (!data)
		goto fail;

	const size_t r = fread(data, 1, fileContentsRequest->cbRequested, fp);
	const UINT rc = cliprdr_file_context_send_contents(file->context, fileContentsRequest, data, r);
	free(data);
	return rc;
fail:
	free(data);
	return cliprdr_file_context_send_file_contents_failure(file->context, fileContentsRequest);
}

static UINT change_lock(CliprdrFileContext* file, UINT32 streamID, BOOL lock)
{
	WINPR_ASSERT(file);

	HashTable_Lock(file->local_streams);
	CliprdrLocalStream* stream =
	    HashTable_GetItemValue(file->local_streams, (void*)(uintptr_t)streamID);
	if (lock && !stream)
	{
		stream = cliprdr_local_stream_new(streamID, NULL, 0);
		HashTable_Insert(file->local_streams, (void*)(uintptr_t)streamID, stream);
		file->local_stream_id = streamID;
	}
	if (stream)
		stream->locked = lock;

	if (!lock)
		HashTable_Foreach(file->local_streams, local_stream_discard, file);
	HashTable_Unlock(file->local_streams);
	return CHANNEL_RC_OK;
}

static UINT cliprdr_file_context_lock(CliprdrClientContext* context,
                                      const CLIPRDR_LOCK_CLIPBOARD_DATA* lockClipboardData)
{
	WINPR_ASSERT(context);
	WINPR_ASSERT(lockClipboardData);
	CliprdrFileContext* file = (context->custom);
	return change_lock(file, lockClipboardData->clipDataId, TRUE);
}

static UINT cliprdr_file_context_unlock(CliprdrClientContext* context,
                                        const CLIPRDR_UNLOCK_CLIPBOARD_DATA* unlockClipboardData)
{
	WINPR_ASSERT(context);
	WINPR_ASSERT(unlockClipboardData);
	CliprdrFileContext* file = (context->custom);
	return change_lock(file, unlockClipboardData->clipDataId, FALSE);
}

static UINT cliprdr_file_context_server_file_contents_request(
    CliprdrClientContext* context, const CLIPRDR_FILE_CONTENTS_REQUEST* fileContentsRequest)
{
	UINT error = NO_ERROR;

	WINPR_ASSERT(context);
	WINPR_ASSERT(fileContentsRequest);

	CliprdrFileContext* file = (context->custom);
	WINPR_ASSERT(file);

	/*
	 * MS-RDPECLIP 2.2.5.3 File Contents Request PDU (CLIPRDR_FILECONTENTS_REQUEST):
	 * The FILECONTENTS_SIZE and FILECONTENTS_RANGE flags MUST NOT be set at the same time.
	 */
	if ((fileContentsRequest->dwFlags & (FILECONTENTS_SIZE | FILECONTENTS_RANGE)) ==
	    (FILECONTENTS_SIZE | FILECONTENTS_RANGE))
	{
		WLog_Print(file->log, WLOG_ERROR, "invalid CLIPRDR_FILECONTENTS_REQUEST.dwFlags");
		return cliprdr_file_context_send_file_contents_failure(context, fileContentsRequest);
	}

	if (fileContentsRequest->dwFlags & FILECONTENTS_SIZE)
		error = cliprdr_file_context_server_file_size_request(file, fileContentsRequest);

	if (fileContentsRequest->dwFlags & FILECONTENTS_RANGE)
		error = cliprdr_file_context_server_file_range_request(file, fileContentsRequest);

	if (error)
	{
		WLog_Print(file->log, WLOG_ERROR, "failed to handle CLIPRDR_FILECONTENTS_REQUEST: 0x%08X",
		           error);
		return cliprdr_file_context_send_file_contents_failure(context, fileContentsRequest);
	}

	return CHANNEL_RC_OK;
}

static BOOL xf_cliprdr_clipboard_is_valid_unix_filename(LPCWSTR filename)
{
	LPCWSTR c;

	if (!filename)
		return FALSE;

	if (filename[0] == L'\0')
		return FALSE;

	/* Reserved characters */
	for (c = filename; *c; ++c)
	{
		if (*c == L'/')
			return FALSE;
	}

	return TRUE;
}

BOOL cliprdr_file_context_init(CliprdrFileContext* file, CliprdrClientContext* cliprdr)
{
	WINPR_ASSERT(file);
	WINPR_ASSERT(cliprdr);

	cliprdr->custom = file;
	file->context = cliprdr;

	cliprdr->ServerLockClipboardData = cliprdr_file_context_lock;
	cliprdr->ServerUnlockClipboardData = cliprdr_file_context_unlock;
	cliprdr->ServerFileContentsRequest = cliprdr_file_context_server_file_contents_request;
#if defined(WITH_FUSE2) || defined(WITH_FUSE3)
	cliprdr->ServerFileContentsResponse = cliprdr_file_context_server_file_contents_response;
#endif

	return TRUE;
}

BOOL cliprdr_file_context_uninit(CliprdrFileContext* file, CliprdrClientContext* cliprdr)
{
	WINPR_ASSERT(file);
	WINPR_ASSERT(cliprdr);

	file->context = NULL;
#if defined(WITH_FUSE2) || defined(WITH_FUSE3)
	cliprdr->ServerFileContentsResponse = NULL;
#endif

	return TRUE;
}

static BOOL cliprdr_file_content_changed_and_update(CliprdrFileContext* file, const void* data,
                                                    size_t size)
{
	WINPR_ASSERT(file);

	BYTE hash[WINPR_SHA256_DIGEST_LENGTH] = { 0 };
	if (!winpr_Digest(WINPR_MD_SHA256, data, size, hash, sizeof(hash)))
		return FALSE;

	const BOOL changed = memcmp(hash, file->hash, sizeof(hash)) != 0;
	if (changed)
		memcpy(file->hash, hash, sizeof(file->hash));
	return changed;
}

BOOL cliprdr_file_context_update_server_data(CliprdrFileContext* file, const void* data,
                                             size_t size)
{
	WINPR_ASSERT(file);

	if (cliprdr_file_content_changed_and_update(file, data, size))
	{
#if defined(WITH_FUSE2) || defined(WITH_FUSE3)
		/* Build inode table for FILEDESCRIPTORW*/
		if (!cliprdr_file_fuse_generate_list(file, data, size))
			return FALSE;
#endif
	}

	return TRUE;
}

void* cliprdr_file_context_get_context(CliprdrFileContext* file)
{
	WINPR_ASSERT(file);
	return file->clipboard;
}

const char* cliprdr_file_context_base_path(CliprdrFileContext* file)
{
	WINPR_ASSERT(file);
	return file->path;
}

void cliprdr_file_session_terminate(CliprdrFileContext* file)
{
	if (!file)
		return;

#if defined(WITH_FUSE2) || defined(WITH_FUSE3)
	if (file->fuse_sess)
		fuse_session_exit(file->fuse_sess);
#endif
	/* 	not elegant but works for umounting FUSE
	    fuse_chan must receieve a oper buf to unblock fuse_session_receive_buf function.
	*/
	winpr_PathFileExists(file->path);
}

void cliprdr_file_context_free(CliprdrFileContext* file)
{
	if (!file)
		return;

#if defined(WITH_FUSE2) || defined(WITH_FUSE3)
	if (file->fuse_thread)
	{
		cliprdr_file_session_terminate(file);
		WaitForSingleObject(file->fuse_thread, INFINITE);
		CloseHandle(file->fuse_thread);
	}

	// fuse related
	HashTable_Free(file->remote_streams);
	ArrayList_Free(file->ino_list);
#endif
	HashTable_Free(file->local_streams);
	winpr_RemoveDirectory(file->path);
	free(file->path);
	free(file);
}

static BOOL create_base_path(CliprdrFileContext* file)
{
	WINPR_ASSERT(file);

	char base[64] = { 0 };
	_snprintf(base, sizeof(base), "/.xfreerdp.cliprdr.%" PRIu32, GetCurrentProcessId());

	file->path = GetKnownSubPath(KNOWN_PATH_TEMP, base);
	if (!file->path)
		return FALSE;

	if (!winpr_PathFileExists(file->path) && !winpr_PathMakePath(file->path, 0))
	{
		WLog_Print(file->log, WLOG_ERROR, "Failed to create directory '%s'", file->path);
		return FALSE;
	}
	return TRUE;
}

static void cliprdr_local_file_free(CliprdrLocalFile* file)
{
	if (!file)
		return;
	if (file->fp)
		fclose(file->fp);
	free(file->name);
}

static void cliprdr_local_stream_free(void* obj)
{
	CliprdrLocalStream* stream = (CliprdrLocalStream*)obj;
	if (stream)
	{
		for (size_t x = 0; x < stream->count; x++)
			cliprdr_local_file_free(&stream->files[x]);
		free(stream->files);
	}
	free(stream);
}

static BOOL cliprdr_local_stream_update(CliprdrLocalStream* stream, const char* data, size_t size)
{
	WINPR_ASSERT(stream);
	if (size == 0)
		return TRUE;

	free(stream->files);
	stream->files = calloc(size, sizeof(CliprdrLocalFile));
	if (!stream->files)
		return FALSE;

	char* copy = strndup(data, size);
	if (!copy)
		return FALSE;
	char* ptr = strtok(copy, "\r\n");
	while (ptr)
	{
		stream->files[stream->count++].name = _strdup(ptr);
		ptr = strtok(NULL, "\r\n");
	}
	free(copy);
	return TRUE;
}

CliprdrLocalStream* cliprdr_local_stream_new(UINT32 streamID, const char* data, size_t size)
{
	CliprdrLocalStream* stream = calloc(1, sizeof(CliprdrLocalStream));
	if (!stream)
		return NULL;

	if (!cliprdr_local_stream_update(stream, data, size))
		goto fail;

	stream->streamID = streamID;
	return stream;

fail:
	cliprdr_local_stream_free(stream);
	return NULL;
}

CliprdrFileContext* cliprdr_file_context_new(void* context)
{
	CliprdrFileContext* file = calloc(1, sizeof(CliprdrFileContext));
	if (!file)
		return NULL;

	file->log = WLog_Get(CLIENT_TAG("x11.cliprdr.file"));
	file->clipboard = context;
	if (!create_base_path(file))
		goto fail;

	file->local_streams = HashTable_New(FALSE);
	if (!file->local_streams)
		goto fail;

	wObject* hobj = HashTable_ValueObject(file->local_streams);
	WINPR_ASSERT(hobj);
	hobj->fnObjectFree = cliprdr_local_stream_free;

#if defined(WITH_FUSE2) || defined(WITH_FUSE3)
	file->current_stream_id = 0;
	file->remote_streams = HashTable_New(TRUE);
	if (!file->remote_streams)
	{
		WLog_Print(file->log, WLOG_ERROR, "failed to allocate stream_list");
		goto fail;
	}
	wObject* obj = HashTable_ValueObject(file->remote_streams);
	obj->fnObjectFree = cliprdr_fuse_stream_free;

	file->ino_list = ArrayList_New(TRUE);
	if (!file->ino_list)
	{
		WLog_Print(file->log, WLOG_ERROR, "failed to allocate stream_list");
		goto fail;
	}
	obj = ArrayList_Object(file->ino_list);
	obj->fnObjectFree = cliprdr_file_fuse_inode_free;

	if (!xf_fuse_repopulate(file, file->ino_list))
		goto fail;

	if (!(file->fuse_thread = CreateThread(NULL, 0, cliprdr_file_fuse_thread, file, 0, NULL)))
	{
		goto fail;
	}
#endif
	return file;

fail:
	cliprdr_file_context_free(file);
	return NULL;
}

BOOL local_stream_discard(const void* key, void* value, void* arg)
{
	CliprdrFileContext* file = arg;
	CliprdrLocalStream* stream = value;
	WINPR_ASSERT(file);
	WINPR_ASSERT(stream);

	if (!stream->locked)
		HashTable_Remove(file->local_streams, key);
	return TRUE;
}

static BOOL remote_stream_discard(const void* key, void* value, void* arg)
{
	CliprdrFuseStream* stream = (CliprdrFuseStream*)value;
	CliprdrFileContext* file = arg;
	WINPR_ASSERT(file);
	WINPR_ASSERT(value);
	WINPR_ASSERT(arg);
	fuse_reply_err(stream->req, EIO);
	return HashTable_Remove(file->remote_streams, key);
}

BOOL cliprdr_file_context_clear(CliprdrFileContext* file)
{
	HashTable_Foreach(file->local_streams, local_stream_discard, file);

#if defined(WITH_FUSE2) || defined(WITH_FUSE3)
	HashTable_Foreach(file->remote_streams, remote_stream_discard, file);

	xf_fuse_repopulate(file, file->ino_list);
#endif
	return TRUE;
}

BOOL cliprdr_file_context_update_client_data(CliprdrFileContext* file, const char* data,
                                             size_t size)
{
	BOOL rc = FALSE;
	UINT32 streamID = file->local_stream_id++;

	HashTable_Lock(file->local_streams);
	CliprdrLocalStream* stream =
	    HashTable_GetItemValue(file->local_streams, (void*)(uintptr_t)streamID);

	if (stream)
		rc = cliprdr_local_stream_update(stream, data, size);
	else
	{
		stream = cliprdr_local_stream_new(streamID, data, size);
		rc = HashTable_Insert(file->local_streams, (void*)(uintptr_t)streamID, stream);
	}
	HashTable_Unlock(file->local_streams);
	return rc;
}

UINT32 cliprdr_file_context_current_flags(CliprdrFileContext* file)
{
	WINPR_ASSERT(file);

	if ((file->file_capability_flags & CB_STREAM_FILECLIP_ENABLED) == 0)
		return 0;

	if (!file->file_formats_registered)
		return 0;

	return CB_STREAM_FILECLIP_ENABLED | CB_FILECLIP_NO_FILE_PATHS | CB_HUGE_FILE_SUPPORT_ENABLED |
	       CB_CAN_LOCK_CLIPDATA;
}

BOOL cliprdr_file_context_set_locally_available(CliprdrFileContext* file, BOOL available)
{
	WINPR_ASSERT(file);
	file->file_formats_registered = available;
	return TRUE;
}

BOOL cliprdr_file_context_remote_set_flags(CliprdrFileContext* file, UINT32 flags)
{
	WINPR_ASSERT(file);
	file->file_capability_flags = flags;
	return TRUE;
}

UINT32 cliprdr_file_context_remote_get_flags(CliprdrFileContext* file)
{
	WINPR_ASSERT(file);
	return file->file_capability_flags;
}