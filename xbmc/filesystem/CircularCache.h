/*
 *  Copyright (C) 2005-2018 Team Kodi
 *  This file is part of Kodi - https://kodi.tv
 *
 *  SPDX-License-Identifier: GPL-2.0-or-later
 *  See LICENSES/README.md for more information.
 */

#pragma once

#include "CacheStrategy.h"
#include "threads/CriticalSection.h"
#include "threads/Event.h"

#if !defined(TARGET_WINDOWS)
#include <memory>
#include <vector>
#endif

namespace XFILE {

class CCircularCache : public CCacheStrategy
{
public:
    CCircularCache(size_t front, size_t back);
    ~CCircularCache() override;

    int Open() override;
    void Close() override;

    size_t GetMaxWriteSize(const size_t& iRequestSize) override;
    int WriteToCache(const char *buf, size_t len) override;
    int ReadFromCache(char *buf, size_t len) override;
    int64_t WaitForData(uint32_t minimum, std::chrono::milliseconds timeout) override;

    int64_t Seek(int64_t pos) override;
    bool Reset(int64_t pos) override;

    int64_t CachedDataEndPosIfSeekTo(int64_t iFilePosition) override;
    int64_t CachedDataStartPos() override;
    int64_t CachedDataEndPos() override;
    bool IsCachedPosition(int64_t iFilePosition) override;

    CCacheStrategy *CreateNew() override;
protected:
  int64_t m_beg = 0; /**< index in file (not buffer) of beginning of valid data */
  int64_t m_end = 0; /**< index in file (not buffer) of end of valid data */
  int64_t m_cur = 0; /**< current reading index in file */
    size_t            m_size;      /**< total cache capacity */
    size_t            m_size_back; /**< guaranteed size of back buffer (actual size can be smaller, or larger if front buffer doesn't need it) */
    CCriticalSection  m_sync;
    CEvent            m_written;
#ifdef TARGET_WINDOWS
    uint8_t* m_buf{nullptr};
    HANDLE m_handle;
#else
    static constexpr size_t SEGMENT_SIZE = 8 * 1024 * 1024;
    std::vector<std::unique_ptr<uint8_t[]>> m_segments;
    void CopyToSegments(size_t offset, const char* src, size_t len);
    void CopyFromSegments(size_t offset, char* dst, size_t len) const;
#endif
};

} // namespace XFILE

