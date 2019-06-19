/*
 * Tencent is pleased to support the open source community by making
 * WCDB available.
 *
 * Copyright (C) 2017 THL A29 Limited, a Tencent company.
 * All rights reserved.
 *
 * Licensed under the BSD 3-Clause License (the "License"); you may not use
 * this file except in compliance with the License. You may obtain a copy of
 * the License at
 *
 *       https://opensource.org/licenses/BSD-3-Clause
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 */

#ifndef SQLITE3_WCDB_H
#define SQLITE3_WCDB_H

#ifdef SQLITE_WCDB

#ifdef SQLITE_WCDB_LOCK_HOOK
/*
 ** Register handlers when lock state changed.
 */
SQLITE_API int sqlite3_lock_hook(void (*xWillLock)(void *pArg, const char* zPath, int eLock),
                      void (*xLockDidChange)(void *pArg, const char* zPath, int eLock),
                      void (*xWillShmLock)(void *pArg, const char* zPath, int flags, int mask),
                      void (*xShmLockDidChange)(void *pArg, const char* zPath, void* id, int sharedMask, int exclMask),
                      void *pArg);
#endif //SQLITE_WCDB_LOCK_HOOK


#ifdef SQLITE_WCDB_CHECKPOINT_HANDLER
/*
 ** Register a handler when checkpoint did happen.
 */
SQLITE_API void *sqlite3_wal_checkpoint_handler(sqlite3 *, void (*xCallback)(void*, sqlite3*, const char *), void*);
#endif // SQLITE_WCDB_CHECKPOINT_HANDLER


#ifdef SQLITE_WCDB_DIRTY_PAGE_COUNT
/*
 ** Return the number of dirty pages currently in the cache.
 */
SQLITE_API int sqlite3_dirty_page_count(sqlite3*);
#endif // SQLITE_WCDB_DIRTY_PAGE_COUNT

#endif // SQLITE_WCDB

#endif /* SQLITE3_WCDB_H */
