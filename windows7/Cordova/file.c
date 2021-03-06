// Copyright 2012 Intel Corporation
//
// Licensed to the Apache Software Foundation (ASF) under one
// or more contributor license agreements.  See the NOTICE file
// distributed with this work for additional information
// regarding copyright ownership.  The ASF licenses this file
// to you under the Apache License, Version 2.0 (the
// "License"); you may not use this file except in compliance
// with the License.  You may obtain a copy of the License at
// 
//    http://www.apache.org/licenses/LICENSE-2.0
// 
// Unless required by applicable law or agreed to in writing,
// software distributed under the License is distributed on an
// "AS IS" BASIS, WITHOUT WARRANTIES OR CONDITIONS OF ANY
// KIND, either express or implied.  See the License for the
// specific language governing permissions and limitations
// under the License.

#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <Shlwapi.h>
#include <Shlobj.h>
#include <Shellapi.h>
#include <wininet.h>
#include <WinReg.h>
#include <Wincrypt.h>

#include "file.h"
#include "common.h"
#include "device.h"
#include "json.h"


#ifdef CORDOVA_FILE_ENABLED

#pragma comment(lib, "shlwapi.lib")
#pragma comment(lib, "crypt32.lib")

static DWORD full_path_size;
static wchar_t full_path[MAX_PATH + 1];
static wchar_t url[INTERNET_MAX_URL_LENGTH + 1];

#define MAX_CORDOVA_FS_NAME 32
#define URL_START L"file:///"
#define DRIVE_COLUMN_POS 9

typedef enum {
	TEMPORARY_FS = 0,
	PERSISTENT_FS
} CordovaFsType;

static wchar_t temporary_root_path[MAX_PATH + 1];
static wchar_t persistent_root_path[MAX_PATH + 1];

// Tools
// ---------------------------------------------------------------------------

wchar_t *make_entry(BOOL is_dir, wchar_t *root_path, wchar_t *rel_path)
{
	wchar_t *response;
	wchar_t name[MAX_PATH + 1];
	const wchar_t *format = L"{isFile:%s,isDirectory:%s,name:'%s',fullPath:'%s'}";
	size_t root_len = wcslen(root_path);
	DWORD url_size = INTERNET_MAX_URL_LENGTH;

	wcscpy_s(full_path, MAX_PATH, root_path);
	if (rel_path && wcscmp(rel_path, L"\\"))
		wcscat_s(full_path + root_len, MAX_PATH - root_len, rel_path);
	if (UrlCreateFromPath(full_path, url, &url_size, 0) != S_OK)
		return NULL;
	url[DRIVE_COLUMN_POS] = L'_'; // Replace drive ':' due to wrong Cordova check in common/plugin/resolveLocalFileSystemURI.js file

	// Comply with wrong unit test: replace %20 with space char
	if (UrlUnescapeInPlace(url, 0) != S_OK)
		return NULL;

	if (rel_path && !wcscmp(rel_path, L"\\"))
		wcscpy_s(name, MAX_PATH, L"/");
	else
		wcscpy_s(name, MAX_PATH, wcsrchr(full_path, L'\\') + 1);

	response = (wchar_t *)malloc(sizeof(wchar_t) * (1 + wcslen(format) + 5 + 5 + wcslen(name) + wcslen(url)));
	wsprintf(response, format, is_dir?L"false":L"true", is_dir?L"true":L"false", name, url);

	return response;
}

static BOOL path_is_valid(wchar_t *path)
{
	size_t x = wcscspn(path, L"<>:\"/|?*");
	if (x == wcslen(path))
		return TRUE;

	return FALSE;
}

CordovaFsError path_from_uri(wchar_t *uri, wchar_t *path, DWORD *len, BOOL must_exist, BOOL *is_dir)
{
	uri[DRIVE_COLUMN_POS] = L':'; // Restore drive ':'

	if (PathCreateFromUrl(uri, path, len, 0) != S_OK)
		return ENCODING_ERR;

	if (!path_is_valid(path + 2)) // skip drive letter
		return ENCODING_ERR;

	if (wcsncmp(path, temporary_root_path, wcslen(temporary_root_path)) &&
						wcsncmp(path, persistent_root_path, wcslen(persistent_root_path)))
		return NOT_FOUND_ERR;

	if (must_exist) {
		DWORD attributes;

		if ((attributes = GetFileAttributes(path)) == INVALID_FILE_ATTRIBUTES) {
			return NOT_FOUND_ERR;
		}

		if (is_dir != NULL)
			*is_dir = (attributes & FILE_ATTRIBUTE_DIRECTORY) ? TRUE : FALSE;
	}

	return FILE_NO_ERR;
}

static void file_fail_callback(wchar_t *callback_id, CordovaFsError err)
{
	wchar_t *error_text;

	switch (err) {
	case NOT_FOUND_ERR:
		error_text = L"FileError.NOT_FOUND_ERR";
		break;
	case SECURITY_ERR:
		error_text = L"FileError.SECURITY_ERR";
		break;
	case ABORT_ERR:
		error_text = L"FileError.ABORT_ERR";
		break;
	case NOT_READABLE_ERR:
		error_text = L"FileError.NOT_READABLE_ERR";
		break;
	case ENCODING_ERR:
		error_text = L"FileError.ENCODING_ERR";
		break;
	case NO_MODIFICATION_ALLOWED_ERR:
		error_text = L"FileError.NO_MODIFICATION_ALLOWED_ERR";
		break;
	case INVALID_STATE_ERR:
		error_text = L"FileError.INVALID_STATE_ERR";
		break;
	case SYNTAX_ERR:
		error_text = L"FileError.SYNTAX_ERR";
		break;
	case INVALID_MODIFICATION_ERR:
		error_text = L"FileError.INVALID_MODIFICATION_ERR";
		break;
	case QUOTA_EXCEEDED_ERR:
		error_text = L"FileError.QUOTA_EXCEEDED_ERR";
		break;
	case TYPE_MISMATCH_ERR:
		error_text = L"FileError.TYPE_MISMATCH_ERR";
		break;
	case PATH_EXISTS_ERR:
		error_text = L"FileError.PATH_EXISTS_ERR";
		break;
	default:
		ASSERT(FALSE);
	}

	cordova_fail_callback(callback_id, FALSE, CB_GENERIC_ERROR, error_text);
}

time_t filetime_to_timet(FILETIME *ft) 
{
	ULARGE_INTEGER ull;

	ull.LowPart = ft->dwLowDateTime;
	ull.HighPart = ft->dwHighDateTime;

	return ull.QuadPart / 10000000ULL - 11644473600ULL;
}

wchar_t *type_from_file_name(wchar_t *file_name)
{
	wchar_t *extension = wcsrchr(file_name, L'.');

	if(extension) {
		HKEY key;
		DWORD data_size;
		wchar_t *type;

		if (RegOpenKeyEx(HKEY_CLASSES_ROOT, extension, 0, KEY_READ, &key) != ERROR_SUCCESS)
			goto on_default;

		if (RegQueryValueEx(key, L"Content Type", NULL, NULL, NULL, &data_size) != ERROR_SUCCESS)
			goto on_default;

		type = (wchar_t *)malloc(data_size + 1);
		if (RegQueryValueEx(key, L"Content Type", NULL, NULL, (LPBYTE)type, &data_size) != ERROR_SUCCESS) {
			free(type);
			goto on_default;
		}

		RegCloseKey(key);
		return type;
	}

on_default:
	return _wcsdup(L"application/octetstream");
}

// FileSystem
// ---------------------------------------------------------------------------

static HRESULT request_file_system(BSTR callback_id, BSTR args)
{
	HRESULT res = S_OK;
	JsonArray array;
	JsonItem item;
	CordovaFsType type;
	UINT64 size;
	wchar_t *response;
	wchar_t *entry;
	wchar_t *root;
	const wchar_t *format = L"{name:'%s',root:%s}";
	ULARGE_INTEGER available;

	// Validate array contents
	if (!json_parse_and_validate_args(args, &array, JSON_VALUE_INT,
											JSON_VALUE_INT64,
											JSON_VALUE_INVALID)) {
		res = E_FAIL;
		goto out;
	}

	item = json_array_get_first(array);
	type = (CordovaFsType) json_get_int_value(item);
	item = json_array_get_next(item);
	size = (UINT64) json_get_int64_value(item);

	root = (type == TEMPORARY_FS) ? temporary_root_path : persistent_root_path;

	if (GetDiskFreeSpaceEx(root, &available, NULL, NULL)) {
		if (size > available.QuadPart) {
			file_fail_callback(callback_id, QUOTA_EXCEEDED_ERR);
			goto out;
		}
	}

	entry = make_entry(TRUE, root, L"\\");
	if (!entry) {
		res = E_FAIL;
		goto out;
	}

	response = (wchar_t *)malloc(sizeof(wchar_t) * (1 + MAX_CORDOVA_FS_NAME + wcslen(entry)));
	wsprintf(response, format, (type == TEMPORARY_FS)?L"temporary":L"persistent", entry);
	cordova_success_callback(callback_id, FALSE, response);

	free(entry);
	free(response);

out:
	json_free_args(array);

	return res;
}

static HRESULT resolve_file_system_uri(BSTR callback_id, BSTR args)
{
	HRESULT res = S_OK;
	JsonArray array;
	wchar_t *uri = NULL;
	wchar_t *entry;
	BOOL is_dir;
	CordovaFsError err;

	if (!json_parse_and_validate_args(args, &array, JSON_VALUE_STRING,
												JSON_VALUE_INVALID)) {
		res = E_FAIL;
		goto out;
	}

	uri = json_get_string_value(array);
	full_path_size = MAX_PATH;
	err = path_from_uri(uri, full_path, &full_path_size, TRUE, &is_dir);
	if (err != FILE_NO_ERR) {
		file_fail_callback(callback_id, err);
		goto out;
	}

	entry = make_entry(is_dir, full_path, NULL);
	if (!entry) {
		res = E_FAIL;
		goto out;
	}

	cordova_success_callback(callback_id, FALSE, entry);
	free(entry);

out:
	json_free_args(array);
	if (uri)
		free(uri);

	return res;
}

// DirectoryReader
// ---------------------------------------------------------------------------

static HRESULT read_entries(BSTR callback_id, BSTR args)
{
	HRESULT res = S_OK;
	JsonArray array;
	wchar_t *uri = NULL;
	BOOL is_dir;
	CordovaFsError err;
	HANDLE hFind = INVALID_HANDLE_VALUE;
	WIN32_FIND_DATA ffd;
	TextBuf response;
	BOOL next;
	wchar_t cur_path[MAX_PATH + 1];
	wchar_t base_path[MAX_PATH + 1];
	DWORD base_path_size = MAX_PATH;

	response = text_buf_new();

	// Check args
	if (!json_parse_and_validate_args(args, &array, JSON_VALUE_STRING,
												JSON_VALUE_INVALID)) {
		res = E_FAIL;
		goto out;
	}
	
	// Convert to path
	uri = json_get_string_value(array);
	full_path_size = MAX_PATH;
	err = path_from_uri(uri, base_path, &base_path_size, TRUE, &is_dir);
	if (err == FILE_NO_ERR && !is_dir)
		err = TYPE_MISMATCH_ERR;
	if (err != FILE_NO_ERR) {
		file_fail_callback(callback_id, err);
		goto out;
	}

	// Read dir entries
	wsprintf(cur_path, L"%s\\%s", base_path, L"*");

	text_buf_append(response, L"[");

	hFind = FindFirstFile(cur_path, &ffd);
	if (INVALID_HANDLE_VALUE == hFind) 
	{
		if (GetLastError() == ERROR_FILE_NOT_FOUND) {
			text_buf_append(response, L"]");
			goto success;
		}

		res = E_FAIL;
		goto out;
	}

	do {
		wchar_t *entry = NULL;

		wsprintf(cur_path, L"%s\\%s", base_path, ffd.cFileName);
		if (ffd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) {
			if(wcscmp(ffd.cFileName, L".") && wcscmp(ffd.cFileName, L".."))
				entry = make_entry(TRUE, cur_path, NULL);
		}
		else
			entry = make_entry(FALSE, cur_path, NULL);

		if (entry) {
			text_buf_append(response, entry);
			free(entry);
		}

		next = FindNextFile(hFind, &ffd);
		if (next && entry) {
			text_buf_append(response, L",");
			entry = NULL;
		}
	} while (next);

	FindClose(hFind);

	text_buf_append(response, L"]");

success:
	FindClose(hFind);
	cordova_success_callback(callback_id, FALSE, text_buf_get(response));

out:
	json_free_args(array);
	if (uri)
		free(uri);
	text_buf_free(response);

	return res;
}

// Entry
// ---------------------------------------------------------------------------

static HRESULT get_metadata(BSTR callback_id, BSTR args)
{
	HRESULT res = S_OK;
	JsonArray array;
	wchar_t *uri = NULL;

	CordovaFsError err;
	HANDLE hFind = INVALID_HANDLE_VALUE;
	WIN32_FIND_DATA ffd;
	wchar_t response[20];

	// Check args
	if (!json_parse_and_validate_args(args, &array, JSON_VALUE_STRING,
												JSON_VALUE_INVALID)) {
		res = E_FAIL;
		goto out;
	}
	
	// Convert to path
	uri = json_get_string_value(array);
	full_path_size = MAX_PATH;
	err = path_from_uri(uri, full_path, &full_path_size, TRUE, NULL);
	if (err != FILE_NO_ERR) {
		file_fail_callback(callback_id, err);
		goto out;
	}

	// Get file info
	hFind = FindFirstFile(full_path, &ffd);
	if (INVALID_HANDLE_VALUE == hFind) {
		if (GetLastError() == ERROR_FILE_NOT_FOUND) {
			file_fail_callback(callback_id, NOT_FOUND_ERR);
		} else {
			res = E_FAIL;
			goto out;
		}
	}

	wsprintf(response, L"%I64d", filetime_to_timet(&ffd.ftLastWriteTime));
	cordova_success_callback(callback_id, FALSE, response);

	FindClose(hFind);

out:
	json_free_args(array);
	if (uri)
		free(uri);

	return res;
}

static HRESULT move_or_copy_to(BSTR callback_id, BSTR args, BOOL is_move)
{
	HRESULT res = S_OK;
	JsonArray array;
	JsonItem item;
	wchar_t *uri = NULL;
	TextBuf dst_uri = NULL;
	wchar_t *str;
	BOOL src_is_dir;
	BOOL dst_is_dir;
	CordovaFsError err;
	wchar_t dest_path[MAX_PATH + 1];
	wchar_t *entry;
	SHFILEOPSTRUCT fileOp;

	// Check args
	if (!json_parse_and_validate_args(args, &array, JSON_VALUE_STRING,
										JSON_VALUE_STRING,
										JSON_VALUE_STRING,
										JSON_VALUE_INVALID)) {
		res = E_FAIL;
		goto out;
	}
	item = json_array_get_first(array);

	// Convert source to path
	uri = json_get_string_value(item);
	full_path_size = MAX_PATH;
	err = path_from_uri(uri, full_path, &full_path_size, TRUE, &src_is_dir);
	if (err != FILE_NO_ERR) {
		file_fail_callback(callback_id, err);
		goto out;
	}
	if (!wcscmp(full_path, temporary_root_path) || !wcscmp(full_path, persistent_root_path)) {
		file_fail_callback(callback_id, NO_MODIFICATION_ALLOWED_ERR);
		goto out;
	}

	// Convert parent to path
	item = json_array_get_next(item);
	dst_uri = text_buf_new();

	str = json_get_string_value(item);
	text_buf_append(dst_uri, str);
	free(str);

	full_path_size = MAX_PATH;
	err = path_from_uri(text_buf_get(dst_uri), dest_path, &full_path_size, TRUE, &dst_is_dir);
	if (err != FILE_NO_ERR) {
		file_fail_callback(callback_id, err);
		goto out;
	}
	if (!dst_is_dir) {
		file_fail_callback(callback_id, TYPE_MISMATCH_ERR);
		goto out;
	}

	item = json_array_get_next(item);
	text_buf_append(dst_uri, L"/");
	str = json_get_string_value(item);
	text_buf_append(dst_uri, str); // dest name

	full_path_size = MAX_PATH;
	err = path_from_uri(text_buf_get(dst_uri), dest_path, &full_path_size, FALSE, NULL);
	if (err != FILE_NO_ERR) {
		file_fail_callback(callback_id, err);
		goto out;
	}

	if (!wcscmp(full_path, dest_path)) {
		file_fail_callback(callback_id, INVALID_MODIFICATION_ERR);
		goto out;
	}

	if ((!src_is_dir && PathFileExists(dest_path) && PathIsDirectory(dest_path)) ||
				(src_is_dir && PathFileExists(dest_path) && PathIsDirectory(dest_path) && !PathIsDirectoryEmpty(dest_path)) ||
				(src_is_dir && PathFileExists(dest_path) && !PathIsDirectory(dest_path))) {
		file_fail_callback(callback_id, INVALID_MODIFICATION_ERR);
		goto out;
	}

	// Special handling for moving dir to empty dir
	if (src_is_dir && PathFileExists(dest_path) && PathIsDirectory(dest_path) && PathIsDirectoryEmpty(dest_path)) {
		if (!RemoveDirectory(dest_path)) {
			file_fail_callback(callback_id, INVALID_MODIFICATION_ERR);
			goto out;
		}
	}

	// Move or Copy
	full_path[wcslen(full_path) + 1] = 0; // double-null terminated required by SHFileOperation()
	dest_path[wcslen(dest_path) + 1] = 0; // double-null terminated required by SHFileOperation()
	memset(&fileOp, 0, sizeof(SHFILEOPSTRUCT));
	fileOp.wFunc = is_move?FO_MOVE:FO_COPY;
	fileOp.pFrom = full_path;
	fileOp.pTo = dest_path;
	fileOp.fFlags = FOF_NO_UI;

	if (SHFileOperation(&fileOp) != 0) {
		file_fail_callback(callback_id, INVALID_MODIFICATION_ERR);
		goto out;
	}

	entry = make_entry(src_is_dir, dest_path, NULL);
	cordova_success_callback(callback_id, FALSE, entry);
	free(entry);

out:
	json_free_args(array);
	if (uri)
		free(uri);
	if (dst_uri)
		text_buf_free(dst_uri);

	return res;
}

static HRESULT move_to(BSTR callback_id, BSTR args)
{
	return move_or_copy_to(callback_id, args, TRUE);
}

static HRESULT copy_to(BSTR callback_id, BSTR args)
{
	return move_or_copy_to(callback_id, args, FALSE);
}

static HRESULT private_remove(BSTR callback_id, BSTR args, BOOL recursive)
{
	HRESULT res = S_OK;
	JsonArray array;
	wchar_t *uri = NULL;
	CordovaFsError err;
	SHFILEOPSTRUCT fileOp;
	BOOL is_dir;

	// Check args
	if (!json_parse_and_validate_args(args, &array, JSON_VALUE_STRING,
												JSON_VALUE_INVALID)) {
		res = E_FAIL;
		goto out;
	}
	
	// Convert to path
	uri = json_get_string_value(array);
	full_path_size = MAX_PATH;
	err = path_from_uri(uri, full_path, &full_path_size, TRUE, &is_dir);
	if (err != FILE_NO_ERR) {
		file_fail_callback(callback_id, err);
		goto out;
	}
	if (!wcscmp(full_path, temporary_root_path) || !wcscmp(full_path, persistent_root_path)) {
		file_fail_callback(callback_id, NO_MODIFICATION_ALLOWED_ERR);
		goto out;
	}

	if (!recursive && is_dir && !PathIsDirectoryEmpty(full_path)) {
		file_fail_callback(callback_id, INVALID_MODIFICATION_ERR);
		goto out;
	}

	// Remove
	full_path[wcslen(full_path) + 1] = 0; // double-null terminated required by SHFileOperation()
	memset(&fileOp, 0, sizeof(SHFILEOPSTRUCT));
	fileOp.wFunc = FO_DELETE;
	fileOp.pFrom = full_path;
	fileOp.fFlags = FOF_NO_UI;

	if (SHFileOperation(&fileOp)) {
		file_fail_callback(callback_id, NOT_FOUND_ERR);
	}

	cordova_success_callback(callback_id, FALSE, NULL_MESSAGE);

out:
	json_free_args(array);
	if (uri)
		free(uri);

	return res;
}

static HRESULT remove(BSTR callback_id, BSTR args)
{
	return private_remove(callback_id, args, FALSE);
}

static HRESULT get_parent(BSTR callback_id, BSTR args)
{
	HRESULT res = S_OK;
	JsonArray array;
	wchar_t *uri = NULL;
	CordovaFsError err;
	wchar_t *entry;

	// Check args
	if (!json_parse_and_validate_args(args, &array, JSON_VALUE_STRING,
												JSON_VALUE_INVALID)) {
		res = E_FAIL;
		goto out;
	}
	
	// Convert to path
	uri = json_get_string_value(array);
	full_path_size = MAX_PATH;
	err = path_from_uri(uri, full_path, &full_path_size, TRUE, NULL);
	if (err != FILE_NO_ERR) {
		file_fail_callback(callback_id, err);
		goto out;
	}
	if (wcscmp(full_path, temporary_root_path) && wcscmp(full_path, persistent_root_path))
		*wcsrchr(full_path, L'\\') = 0;

	// Get parent
	entry = make_entry(TRUE, full_path, NULL);
	cordova_success_callback(callback_id, FALSE, entry);
	free(entry);

out:
	json_free_args(array);
	if (uri)
		free(uri);

	return res;
}

// FileEntry
// ---------------------------------------------------------------------------

static HRESULT get_file_metadata(BSTR callback_id, BSTR args)
{
	HRESULT res = S_OK;
	JsonArray array;
	wchar_t *uri = NULL;
	CordovaFsError err;
	HANDLE hFind = INVALID_HANDLE_VALUE;
	WIN32_FIND_DATA ffd;
	wchar_t *format = L"{name:'%s',fullPath:'%s',type:'%s',lastModifiedDate:new Date(%I64d),size:%I64d}";
	wchar_t *response;
	ULARGE_INTEGER ull;
	wchar_t *type = NULL;

	// Check args
	if (!json_parse_and_validate_args(args, &array, JSON_VALUE_STRING,
												JSON_VALUE_INVALID)) {
		res = E_FAIL;
		goto out;
	}
	
	// Convert to path
	uri = json_get_string_value(array);
	full_path_size = MAX_PATH;
	err = path_from_uri(uri, full_path, &full_path_size, TRUE, NULL);
	if (err != FILE_NO_ERR) {
		file_fail_callback(callback_id, err);
		goto out;
	}

	// Get file info
	hFind = FindFirstFile(full_path, &ffd);
	if (INVALID_HANDLE_VALUE == hFind) {
		if (GetLastError() == ERROR_FILE_NOT_FOUND) {
			file_fail_callback(callback_id, NOT_FOUND_ERR);
		} else {
			res = E_FAIL;
			goto out;
		}
	}

	ull.LowPart = ffd.nFileSizeLow;
	ull.HighPart = ffd.nFileSizeHigh;
	type = type_from_file_name(ffd.cFileName);
	uri[DRIVE_COLUMN_POS] = L'_'; // Replace drive ':'

	// Build & send response
	response = (wchar_t *)malloc(sizeof(wchar_t) * (1 + wcslen(format) + wcslen(ffd.cFileName) +
									wcslen(uri) + wcslen(type) + 40));
	wsprintf(response, format, ffd.cFileName, uri, type,
								filetime_to_timet(&ffd.ftLastWriteTime), ull.QuadPart);
	cordova_success_callback(callback_id, FALSE, response);

	FindClose(hFind);
	free(response);

out:
	json_free_args(array);
	if (uri)
		free(uri);
	if (type)
		free(type);

	return res;
}

// DirectoryEntry
// ---------------------------------------------------------------------------

static HRESULT get_fs_object(BSTR callback_id, BSTR args, BOOL is_dir)
{
	HRESULT res = S_OK;
	JsonArray array;
	JsonItem item;
	wchar_t *uri = NULL;
	CordovaFsError err;
	JsonObjectItem options;
	HANDLE hFind = INVALID_HANDLE_VALUE;
	WIN32_FIND_DATA ffd;
	BOOL option_create = FALSE;
	BOOL option_exclusive = FALSE;
	JsonObjectItem object;
	wchar_t *entry;

	// Check args
	if (!json_parse_and_validate_args(args, &array, JSON_VALUE_STRING,
												JSON_VALUE_STRING,
												JSON_VALUE_OBJECT | JSON_VALUE_NULL,
												JSON_VALUE_INVALID)) {
		res = E_FAIL;
		goto out;
	}
	item = json_array_get_first(array);
	
	// Convert src uri to path
	uri = json_get_string_value(item);
	full_path_size = MAX_PATH;
	err = path_from_uri(uri, full_path, &full_path_size, TRUE, NULL);
	if (err != FILE_NO_ERR) {
		file_fail_callback(callback_id, err);
		goto out;
	}

	// Convert dst uri to path
	item = json_array_get_next(item);
	free(uri);

	uri = json_get_string_value(item);

	if (!wcsncmp(uri, URL_START, wcslen(URL_START))) { // absolute path
		full_path_size = MAX_PATH;
		err = path_from_uri(uri, full_path, &full_path_size, FALSE, NULL);
		if (err != FILE_NO_ERR) {
			file_fail_callback(callback_id, err);
			goto out;
		}

		if (!wcscmp(full_path, temporary_root_path) || !wcscmp(full_path, persistent_root_path)) {
			file_fail_callback(callback_id, NO_MODIFICATION_ALLOWED_ERR);
			goto out;
		}
	} else { // relative to src path
		wchar_t *ptr = uri;

		while (*ptr++) {
			if (*ptr == L'/')
				*ptr = L'\\';
		}

		if (!path_is_valid(uri)) {
			file_fail_callback(callback_id, ENCODING_ERR);
			goto out;
		}

		if (!PathAppend(full_path, uri)) {
			file_fail_callback(callback_id, ENCODING_ERR);
			goto out;
		}
	}

	item = json_array_get_next(item);
	if (json_get_value_type(item) != JSON_VALUE_NULL) {
		options = json_object_get_first(json_get_object_value(item));
		object = json_object_find_prop(options, L"create", JSON_VALUE_BOOL);
		if (object)
			option_create = json_get_bool_value(to_item(object));

		object = json_object_find_prop(options, L"exclusive", JSON_VALUE_BOOL);
		if (object)
			option_exclusive = json_get_bool_value(to_item(object));
	}

	// Get file info
	hFind = FindFirstFile(full_path, &ffd);
	if (INVALID_HANDLE_VALUE == hFind) {
		DWORD err = GetLastError();
		if (err == ERROR_INVALID_NAME) {
			file_fail_callback(callback_id, ENCODING_ERR);
			goto out;
		}

		if (option_create) {
			if (is_dir) {
				if (!SUCCEEDED(SHCreateDirectory(NULL,full_path))) {
					res = E_FAIL;
					goto out;
				}
			} else {
				 HANDLE file;
				if ((file = CreateFile(full_path, GENERIC_WRITE, 0, NULL, CREATE_NEW, FILE_ATTRIBUTE_NORMAL, NULL)) == INVALID_HANDLE_VALUE) {
					res = E_FAIL;
					goto out;
				}
				CloseHandle(file);
			}
		} else {
			file_fail_callback(callback_id, NOT_FOUND_ERR);
			goto out;
		}
	} else { // exists
		if (option_create && option_exclusive) {
			file_fail_callback(callback_id, PATH_EXISTS_ERR);
			goto out;
		}
		if (((ffd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY)?TRUE:FALSE) != is_dir) {
			file_fail_callback(callback_id, TYPE_MISMATCH_ERR);
			goto out;
		}
	}

	// Return entry
	entry = make_entry(TRUE, full_path, NULL);
	cordova_success_callback(callback_id, FALSE, entry);
	free(entry);

out:
	json_free_args(array);
	if (uri)
		free(uri);

	return res;
}

static HRESULT get_directory(BSTR callback_id, BSTR args)
{
	return get_fs_object(callback_id, args, TRUE);
}

static HRESULT get_file(BSTR callback_id, BSTR args)
{
	return get_fs_object(callback_id, args, FALSE);
}

static HRESULT remove_recursively(BSTR callback_id, BSTR args)
{
	return private_remove(callback_id, args, TRUE);
}

// FileReader
// ---------------------------------------------------------------------------

static HRESULT read_file(BSTR callback_id, wchar_t *uri, wchar_t *encoding, LARGE_INTEGER startpos, LARGE_INTEGER endpos, BOOL is_base64)
{
	HRESULT res = S_OK;
	CordovaFsError err;
	HANDLE file = INVALID_HANDLE_VALUE;
	ULARGE_INTEGER bytes_to_read;
	BOOL is_dir;
	UINT codepage;
	char *buf = NULL;
	DWORD utf16_len;
	wchar_t *utf16_text = NULL;
	TextBuf response;
	DWORD read;
	wchar_t *base64_prefix = L"data:text/plain;base64,";
	
	// Convert src uri to path
	full_path_size = MAX_PATH;
	err = path_from_uri(uri, full_path, &full_path_size, TRUE, &is_dir);
	if (err != FILE_NO_ERR) {
		file_fail_callback(callback_id, err);
		goto out;
	}

	if (is_dir) {
		file_fail_callback(callback_id, TYPE_MISMATCH_ERR);
		goto out;
	}

	// Open file
	file = CreateFile(full_path, GENERIC_READ, 0, NULL, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, NULL);
	if (file == INVALID_HANDLE_VALUE) {
		file_fail_callback(callback_id, INVALID_MODIFICATION_ERR);
		goto out;
	}

	if (!SetFilePointerEx(file, startpos, NULL, FILE_BEGIN)) {
		file_fail_callback(callback_id, INVALID_MODIFICATION_ERR);
		goto out;
	}

	// Read file contents
	bytes_to_read.QuadPart = endpos.QuadPart - startpos.QuadPart;
	buf = (char *) malloc(bytes_to_read.LowPart + 1);
	buf[bytes_to_read.LowPart] = 0;
	if (!ReadFile(file, buf, bytes_to_read.LowPart, &read, NULL)) {
		file_fail_callback(callback_id, ABORT_ERR);
		goto out;
	}

	if(!read) {
		cordova_success_callback(callback_id, FALSE, L"''");
		goto out;
	}

	// Convert buffer
	if (is_base64) {
		if (!CryptBinaryToString((const BYTE *)buf, read+1, CRYPT_STRING_BASE64 | CRYPT_STRING_NOCRLF, NULL, &utf16_len)) { // First call to get length
			file_fail_callback(callback_id, ABORT_ERR);
			goto out;
		}
		utf16_text = (wchar_t *) malloc(sizeof(wchar_t) * utf16_len);
		if (!CryptBinaryToString((const BYTE *)buf, read+1, CRYPT_STRING_BASE64 | CRYPT_STRING_NOCRLF, utf16_text, &utf16_len)) {
			file_fail_callback(callback_id, ABORT_ERR);
			goto out;
		}
	} else {
		// see list of codepage identifiers at http://msdn.microsoft.com/en-us/library/windows/desktop/dd317756(v=vs.85).aspx
		if (!_wcsicmp(encoding, L"UTF-8")) {
			codepage = CP_UTF8;
		} else if (!_wcsicmp(encoding, L"ISO-8859-1")) {
			codepage = 28591;
		} else {
			file_fail_callback(callback_id, ABORT_ERR);
			goto out;
		}

		utf16_len = MultiByteToWideChar(codepage, 0, buf, read, NULL, 0); // First call to get length
		utf16_text = (wchar_t *) malloc(sizeof(wchar_t) * utf16_len);
		if (!MultiByteToWideChar(codepage, 0, buf, read, utf16_text, utf16_len)) {
			file_fail_callback(callback_id, ABORT_ERR);
			goto out;
		}
//		utf16_len--;
	}

	response = text_buf_new();
	text_buf_append(response, L"\"");
	if (is_base64) {
		text_buf_append(response, base64_prefix);
	}
	text_buf_append_with_json_escaping_len(response, utf16_text, utf16_len);
	text_buf_append(response, L"\"");
	cordova_success_callback(callback_id, FALSE, text_buf_get(response));
	text_buf_free(response);

out:
	if (buf)
		free(buf);
	if (utf16_text)
		free(utf16_text);
	if (file != INVALID_HANDLE_VALUE)
		CloseHandle(file);

	return res;
}

static HRESULT read_as_text(BSTR callback_id, BSTR args)
{
	HRESULT res = S_OK;
	JsonArray array;
	JsonItem item;
	wchar_t *uri = NULL;
	wchar_t *encoding = NULL;
	LARGE_INTEGER startpos;
	LARGE_INTEGER endpos;

		// Check args
	if (!json_parse_and_validate_args(args, &array, JSON_VALUE_STRING,
												JSON_VALUE_STRING,
												JSON_VALUE_INT64,
												JSON_VALUE_INT64,
												JSON_VALUE_INVALID)) {
		res = E_FAIL;
		goto out;
	}

	item = json_array_get_first(array);
	uri = json_get_string_value(item);
	item = json_array_get_next(item);
	encoding = json_get_string_value(item);
	item = json_array_get_next(item);
	startpos.QuadPart = json_get_int64_value(item);
	item = json_array_get_next(item);
	endpos.QuadPart = json_get_int64_value(item);

	res = read_file(callback_id, uri, encoding, startpos, endpos, FALSE);

out:
	json_free_args(array);
	if (uri)
		free(uri);
	if (encoding)
		free(encoding);

	return res;
}

static HRESULT read_as_binary_string(BSTR callback_id, BSTR args)
{
	HRESULT res = S_OK;
	JsonArray array;
	JsonItem item;
	wchar_t *uri = NULL;
	LARGE_INTEGER startpos;
	LARGE_INTEGER endpos;

		// Check args
	if (!json_parse_and_validate_args(args, &array, JSON_VALUE_STRING,
												JSON_VALUE_INT64,
												JSON_VALUE_INT64,
												JSON_VALUE_INVALID)) {
		res = E_FAIL;
		goto out;
	}

	item = json_array_get_first(array);
	uri = json_get_string_value(item);
	item = json_array_get_next(item);
	startpos.QuadPart = json_get_int64_value(item);
	item = json_array_get_next(item);
	endpos.QuadPart = json_get_int64_value(item);

	res = read_file(callback_id, uri, L"ISO-8859-1", startpos, endpos, FALSE);

out:
	json_free_args(array);
	if (uri)
		free(uri);

	return res;
}

static HRESULT read_as_data_url(BSTR callback_id, BSTR args)
{
	HRESULT res = S_OK;
	JsonArray array;
	JsonItem item;
	wchar_t *uri = NULL;
	LARGE_INTEGER startpos;
	LARGE_INTEGER endpos;

		// Check args
	if (!json_parse_and_validate_args(args, &array, JSON_VALUE_STRING,
												JSON_VALUE_INT64,
												JSON_VALUE_INT64,
												JSON_VALUE_INVALID)) {
		res = E_FAIL;
		goto out;
	}

	item = json_array_get_first(array);
	uri = json_get_string_value(item);
	item = json_array_get_next(item);
	startpos.QuadPart = json_get_int64_value(item);
	item = json_array_get_next(item);
	endpos.QuadPart = json_get_int64_value(item);
	
	res = read_file(callback_id, uri, L"UTF-8", startpos, endpos, TRUE);

out:
	json_free_args(array);
	if (uri)
		free(uri);

	return res;
}

// FileWriter
// ---------------------------------------------------------------------------

static HRESULT write(BSTR callback_id, BSTR args)
{
	HRESULT res = S_OK;
	JsonArray array;
	JsonItem item;
	wchar_t *uri = NULL;
	CordovaFsError err;
	HANDLE file = INVALID_HANDLE_VALUE;
	char *output_text = NULL;
	int output_len;
	DWORD written;
	LARGE_INTEGER seek;
	wchar_t response[20];
	wchar_t *data = NULL;
	size_t data_len;
	BOOL is_binary;

	// Check args
	if (!json_parse_and_validate_args(args, &array, JSON_VALUE_STRING,
												JSON_VALUE_STRING,
												JSON_VALUE_INT64,
												JSON_VALUE_BOOL,
												JSON_VALUE_INVALID)) {
		res = E_FAIL;
		goto out;
	}
	item = json_array_get_first(array);
	uri = json_get_string_value(item);
	item = json_array_get_next(item);
	data = json_get_string_value_and_length(item, &data_len);
	item = json_array_get_next(item);
	seek.QuadPart = json_get_int64_value(item);
	item = json_array_get_next(item);
	is_binary = json_get_bool_value(item);
	
	// Convert src uri to path
	full_path_size = MAX_PATH;
	err = path_from_uri(uri, full_path, &full_path_size, FALSE, NULL);
	if (err != FILE_NO_ERR) {
		file_fail_callback(callback_id, err);
		goto out;
	}

	// Open file
	file = CreateFile(full_path, GENERIC_WRITE, 0, NULL, OPEN_ALWAYS, FILE_ATTRIBUTE_NORMAL, NULL);
	if (file == INVALID_HANDLE_VALUE) {
		file_fail_callback(callback_id, INVALID_MODIFICATION_ERR);
		goto out;
	}

	// Write file
	if(is_binary) {
		output_len = data_len;
		data_len = 0;
		output_text = (char *)data;
		data = NULL;
	} else {
		output_len = WideCharToMultiByte(CP_UTF8, 0, data, data_len, NULL, 0, NULL, NULL); // First call to get length
		output_text = (char *) malloc(output_len + 1);
		if (!WideCharToMultiByte(CP_UTF8, 0, data, data_len, output_text, output_len, NULL, NULL)) {
			file_fail_callback(callback_id, ABORT_ERR);
			goto out;
		}
	}

	if (!SetFilePointerEx(file, seek, NULL, FILE_BEGIN)) {
		file_fail_callback(callback_id, INVALID_MODIFICATION_ERR);
		goto out;
	}

	if (!WriteFile(file, output_text, output_len, &written, NULL)) {
		file_fail_callback(callback_id, ABORT_ERR);
		goto out;
	}

	wsprintf(response, L"%d", written);
	cordova_success_callback(callback_id, FALSE, response);

out:
	json_free_args(array);
	if (uri)
		free(uri);
	if (data)
		free(data);
	if (output_text)
		free(output_text);
	if (file != INVALID_HANDLE_VALUE)
		CloseHandle(file);

	return res;
}

static HRESULT truncate(BSTR callback_id, BSTR args)
{
	HRESULT res = S_OK;
	JsonArray array;
	JsonItem item;
	wchar_t *uri = NULL;
	CordovaFsError err;
	HANDLE file = INVALID_HANDLE_VALUE;
	LARGE_INTEGER seek;
	wchar_t response[20];
	BOOL is_dir;

	// Check args
	if (!json_parse_and_validate_args(args, &array, JSON_VALUE_STRING,
												JSON_VALUE_INT64,
												JSON_VALUE_INVALID)) {
		res = E_FAIL;
		goto out;
	}
	item = json_array_get_first(array);
	
	// Convert src uri to path
	uri = json_get_string_value(item);
	full_path_size = MAX_PATH;
	err = path_from_uri(uri, full_path, &full_path_size, TRUE, &is_dir);
	if (err != FILE_NO_ERR) {
		file_fail_callback(callback_id, err);
		goto out;
	}

	if (is_dir) {
		file_fail_callback(callback_id, TYPE_MISMATCH_ERR);
		goto out;
	}

	// Open file
	file = CreateFile(full_path, GENERIC_WRITE, 0, NULL, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, NULL);
	if (file == INVALID_HANDLE_VALUE) {
		file_fail_callback(callback_id, INVALID_MODIFICATION_ERR);
		goto out;
	}

	// Truncate file
	item = json_array_get_next(item);
	seek.QuadPart = json_get_int64_value(item);
	if (!SetFilePointerEx(file, seek, NULL, FILE_BEGIN)) {
		file_fail_callback(callback_id, INVALID_MODIFICATION_ERR);
		goto out;
	}

	if (!SetEndOfFile(file)) {
		file_fail_callback(callback_id, INVALID_MODIFICATION_ERR);
		goto out;
	}

	wsprintf(response, L"%I64d", seek.QuadPart);
	cordova_success_callback(callback_id, FALSE, response);
out:
	json_free_args(array);
	if (uri)
		free(uri);
	if (file != INVALID_HANDLE_VALUE)
		CloseHandle(file);

	return res;
}

// Module
// ---------------------------------------------------------------------------

static HRESULT file_module_exec(BSTR callback_id, BSTR action, BSTR args, VARIANT *result)
{
	// FileSystem
	if (!wcscmp(action, L"requestFileSystem"))
			return request_file_system(callback_id, args);
	if (!wcscmp(action, L"resolveLocalFileSystemURI"))
		return resolve_file_system_uri(callback_id, args);
	// DirectoryReader
	if (!wcscmp(action, L"readEntries"))
			return read_entries(callback_id, args);
	// Entry
	if (!wcscmp(action, L"getMetadata"))
			return get_metadata(callback_id, args);
	if (!wcscmp(action, L"moveTo"))
			return move_to(callback_id, args);
	if (!wcscmp(action, L"copyTo"))
			return copy_to(callback_id, args);
	if (!wcscmp(action, L"remove"))
			return remove(callback_id, args);
	if (!wcscmp(action, L"getParent"))
			return get_parent(callback_id, args);
	// FileEntry
	if (!wcscmp(action, L"getFileMetadata"))
			return get_file_metadata(callback_id, args);
	// DirectoryEntry
	if (!wcscmp(action, L"getDirectory"))
			return get_directory(callback_id, args);
	if (!wcscmp(action, L"removeRecursively"))
			return remove_recursively(callback_id, args);
	if (!wcscmp(action, L"getFile"))
			return get_file(callback_id, args);
	// FileReader
	if (!wcscmp(action, L"readAsText"))
			return read_as_text(callback_id, args);
	if (!wcscmp(action, L"readAsBinaryString"))
			return read_as_binary_string(callback_id, args);
	if (!wcscmp(action, L"readAsDataURL"))
			return read_as_data_url(callback_id, args);
	// FileWriter
	if (!wcscmp(action, L"write"))
			return write(callback_id, args);
	if (!wcscmp(action, L"truncate"))
			return truncate(callback_id, args);

	return DISP_E_MEMBERNOTFOUND;
}

static void file_module_init(void)
{
	int result;
	wchar_t *folderpath;
	wchar_t *temporary_dir;

	// Initialize root paths
	*temporary_root_path = 0;
	*persistent_root_path = 0;

	if(SUCCEEDED(SHGetKnownFolderPath(&CORDOVA_FILE_PERSISTENT_STORAGE_FOLDERID, 0, NULL, &folderpath))) {
		wcscpy_s(persistent_root_path, MAX_PATH, folderpath);
		PathAppend(persistent_root_path, CORDOVA_FILE_PERSISTENT_STORAGE_SUBDIR);
		CoTaskMemFree(folderpath);

		result = SHCreateDirectory(NULL, persistent_root_path);
		if(!SUCCEEDED(result) && (result != ERROR_FILE_EXISTS) && (result != ERROR_ALREADY_EXISTS))
			*persistent_root_path = 0;
	}
	if(SUCCEEDED(GetTempPath(MAX_PATH, temporary_root_path))) {
		temporary_dir = get_device_uuid();
		PathAppend(temporary_root_path, temporary_dir);

		result = SHCreateDirectory(NULL, temporary_root_path);
		if(!SUCCEEDED(result) && (result != ERROR_FILE_EXISTS) && (result != ERROR_ALREADY_EXISTS))
			*temporary_root_path = 0;
	}
}

static void file_module_close(void)
{
	int res;
	SHFILEOPSTRUCT fileOp;

	// Remove temporary filesystem (uuid based)
	wcscpy_s(full_path, MAX_PATH, temporary_root_path);
	full_path[wcslen(temporary_root_path) + 1] = 0; // double-null terminated required by SHFileOperation()
	memset(&fileOp, 0, sizeof(SHFILEOPSTRUCT));
	fileOp.wFunc = FO_DELETE;
	fileOp.pFrom = full_path;
	fileOp.fFlags = FOF_NO_UI | FOF_NOCONFIRMATION;

	res = SHFileOperation(&fileOp);
}

DEFINE_CORDOVA_MODULE(File, L"File", file_module_exec, file_module_init, file_module_close)

#endif